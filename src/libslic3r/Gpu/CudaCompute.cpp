#include "CudaCompute.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <type_traits>

#include <boost/log/trivial.hpp>

#include "libslic3r/StageTimer.hpp"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dlfcn.h>
#endif

// PTX text of Gpu/CudaKernels.ptx, generated at build time (cmake/EmbedText.cmake).
#include "CudaKernelsPtx.hpp"

namespace Slic3r::Gpu {

namespace {

// The few CUDA driver API types and entry points used here, declared by hand so the build needs no
// CUDA headers. The driver ABI is stable; the _v2 names are the 64-bit entry points of cuda.h.
using CUresult    = int;
using CUdevice    = int;
using CUdeviceptr = unsigned long long;
using CUcontext   = void *;
using CUmodule    = void *;
using CUfunction  = void *;
using CUstream    = void *;
constexpr CUresult CUDA_SUCCESS = 0;
constexpr int CU_JIT_ERROR_LOG_BUFFER = 5;
constexpr int CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES = 6;
#ifdef _WIN32
#define MAGPIE_CUDAAPI __stdcall
#else
#define MAGPIE_CUDAAPI
#endif

struct Driver
{
    CUresult (MAGPIE_CUDAAPI *cuInit)(unsigned int);
    CUresult (MAGPIE_CUDAAPI *cuDeviceGetCount)(int *);
    CUresult (MAGPIE_CUDAAPI *cuDeviceGet)(CUdevice *, int);
    CUresult (MAGPIE_CUDAAPI *cuDeviceGetName)(char *, int, CUdevice);
    CUresult (MAGPIE_CUDAAPI *cuDevicePrimaryCtxRetain)(CUcontext *, CUdevice);
    CUresult (MAGPIE_CUDAAPI *cuCtxPushCurrent)(CUcontext);
    CUresult (MAGPIE_CUDAAPI *cuCtxPopCurrent)(CUcontext *);
    CUresult (MAGPIE_CUDAAPI *cuCtxSynchronize)();
    CUresult (MAGPIE_CUDAAPI *cuModuleLoadDataEx)(CUmodule *, const void *, unsigned int, int *, void **);
    CUresult (MAGPIE_CUDAAPI *cuModuleGetFunction)(CUfunction *, CUmodule, const char *);
    CUresult (MAGPIE_CUDAAPI *cuMemAlloc)(CUdeviceptr *, size_t);
    CUresult (MAGPIE_CUDAAPI *cuMemFree)(CUdeviceptr);
    CUresult (MAGPIE_CUDAAPI *cuMemcpyHtoD)(CUdeviceptr, const void *, size_t);
    CUresult (MAGPIE_CUDAAPI *cuMemcpyDtoH)(void *, CUdeviceptr, size_t);
    CUresult (MAGPIE_CUDAAPI *cuLaunchKernel)(CUfunction, unsigned int, unsigned int, unsigned int, unsigned int,
                                              unsigned int, unsigned int, unsigned int, CUstream, void **, void **);
};

enum Op { opMeshLayerRanges, opVerticalIntersections, opMinDistances, opPointsInExPolygons, opCount };
constexpr const char *op_names[opCount] = { "mesh_layer_ranges", "vertical_intersections", "min_distances",
                                            "points_in_expolygons" };
constexpr const char *kernel_names[opCount] = { "magpie_mesh_layer_ranges", "magpie_vertical_intersections",
                                                "magpie_point_segment_distances", "magpie_points_in_expolygons" };

// Per launch caps. Windows stops a kernel that runs for about 2 s on a GPU that drives a display, so a
// launch holds at most this many inner-loop steps (items x edges or vertices tested): about 1e8 cheap
// integer or double steps, a few milliseconds on a current GPU.
constexpr uint64_t max_steps_per_launch = uint64_t(1) << 26;
// Items per launch (one thread each).
constexpr uint64_t max_items_per_launch = uint64_t(1) << 24;
// Device memory for one batch, in bytes (input + output).
constexpr uint64_t max_batch_bytes = uint64_t(1) << 30;
constexpr unsigned int threads_per_block = 256;

struct OpStats
{
    std::atomic<uint64_t> gpu_batches{ 0 }, gpu_items{ 0 }, gpu_us{ 0 };
    std::atomic<uint64_t> too_large{ 0 }, busy{ 0 }, failed{ 0 }, rejected{ 0 };
    void reset() { gpu_batches = gpu_items = gpu_us = too_large = busy = failed = rejected = 0; }
};

std::atomic<int>  g_mode{ int(CudaMode::Off) };
std::atomic<bool> g_force_dispatch{ false };
OpStats           g_stats[opCount];

class Runtime
{
public:
    // Held for a whole batch: the device buffers belong to one batch at a time.
    std::mutex mutex;

    // Under `mutex`. Loads the driver and the module once; a failure is final for the process.
    bool ready(std::string *reason)
    {
        if (!m_tried) {
            m_tried = true;
            m_ok    = this->load();
            if (m_ok)
                BOOST_LOG_TRIVIAL(info) << "CUDA compute: " << m_device_name << " ready";
            else
                BOOST_LOG_TRIVIAL(warning) << "CUDA compute unavailable: " << m_reason;
        }
        if (!m_ok && reason)
            *reason = m_reason;
        return m_ok;
    }

    // Under `mutex`, after ready(). Uploads `input`, runs the kernel of `op` over `count` records in
    // launches of at most `items_per_launch` and downloads `output_bytes`.
    bool run(Op op, const std::vector<uint8_t> &input, size_t input_stride, size_t count, uint32_t aux,
             uint64_t off1, uint64_t off2, size_t output_stride, size_t items_per_launch, std::vector<uint8_t> &output,
             std::string &error)
    {
        const size_t output_bytes = count * output_stride;
        output.assign(output_bytes, 0);
        if (m_d.cuCtxPushCurrent(m_context) != CUDA_SUCCESS) {
            error = "cuCtxPushCurrent";
            return false;
        }
        bool ok = this->reserve(input.size(), output_bytes, error) &&
                  check(m_d.cuMemcpyHtoD(m_in, input.data(), input.size()), "cuMemcpyHtoD", error);
        for (size_t begin = 0; ok && begin < count; begin += items_per_launch) {
            const size_t launch_count = std::min(items_per_launch, count - begin);
            // The kernel reads its records at in + i * stride and the shared arrays at in + off.
            // Shifting `in` by the launch's first record and the offsets back by as much keeps the
            // shared arrays where they are (unsigned wrap-around is exact).
            CUdeviceptr  in        = m_in + CUdeviceptr(begin * input_stride);
            CUdeviceptr  out       = m_out + CUdeviceptr(begin * output_stride);
            unsigned int n         = unsigned(launch_count);
            unsigned int a         = aux;
            uint64_t     o1        = off1 - uint64_t(begin * input_stride);
            uint64_t     o2        = off2 - uint64_t(begin * input_stride);
            void        *params[]  = { &in, &out, &n, &a, &o1, &o2 };
            const unsigned blocks  = unsigned((launch_count + threads_per_block - 1) / threads_per_block);
            ok = check(m_d.cuLaunchKernel(m_kernels[op], blocks, 1, 1, threads_per_block, 1, 1, 0, nullptr, params, nullptr),
                       "cuLaunchKernel", error) &&
                 check(m_d.cuCtxSynchronize(), "cuCtxSynchronize", error);
        }
        ok = ok && check(m_d.cuMemcpyDtoH(output.data(), m_out, output_bytes), "cuMemcpyDtoH", error);
        CUcontext previous = nullptr;
        m_d.cuCtxPopCurrent(&previous);
        return ok;
    }

private:
    static bool check(CUresult result, const char *what, std::string &error)
    {
        if (result == CUDA_SUCCESS)
            return true;
        error = std::string(what) + " failed (CUDA error " + std::to_string(result) + ")";
        return false;
    }

    bool reserve(size_t input_bytes, size_t output_bytes, std::string &error)
    {
        // Device buffers grow to the largest batch so far and are kept for the process.
        auto grow = [&](CUdeviceptr &ptr, size_t &capacity, size_t bytes, const char *what) {
            bytes = std::max<size_t>(bytes, 256);
            if (bytes <= capacity)
                return true;
            if (ptr != 0)
                m_d.cuMemFree(ptr);
            ptr      = 0;
            capacity = 0;
            if (!check(m_d.cuMemAlloc(&ptr, bytes), what, error))
                return false;
            capacity = bytes;
            return true;
        };
        return grow(m_in, m_in_capacity, input_bytes, "cuMemAlloc input") &&
               grow(m_out, m_out_capacity, output_bytes, "cuMemAlloc output");
    }

    bool load()
    {
#ifdef _WIN32
        // Only the driver from System32, never a DLL from the working directory.
        HMODULE library = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        auto    symbol  = [library](const char *name) { return reinterpret_cast<void *>(GetProcAddress(library, name)); };
#elif defined(__APPLE__)
        void *library = nullptr;
        auto  symbol  = [](const char *) -> void * { return nullptr; };
#else
        void *library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
        auto  symbol  = [library](const char *name) { return dlsym(library, name); };
#endif
        if (library == nullptr) {
            m_reason = "no NVIDIA CUDA driver";
            return false;
        }
        bool missing = false;
        auto bind = [&](auto &fn, const char *name) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(symbol(name));
            if (fn == nullptr) {
                missing  = true;
                m_reason = std::string("CUDA driver has no ") + name;
            }
        };
        bind(m_d.cuInit, "cuInit");
        bind(m_d.cuDeviceGetCount, "cuDeviceGetCount");
        bind(m_d.cuDeviceGet, "cuDeviceGet");
        bind(m_d.cuDeviceGetName, "cuDeviceGetName");
        bind(m_d.cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain");
        bind(m_d.cuCtxPushCurrent, "cuCtxPushCurrent_v2");
        bind(m_d.cuCtxPopCurrent, "cuCtxPopCurrent_v2");
        bind(m_d.cuCtxSynchronize, "cuCtxSynchronize");
        bind(m_d.cuModuleLoadDataEx, "cuModuleLoadDataEx");
        bind(m_d.cuModuleGetFunction, "cuModuleGetFunction");
        bind(m_d.cuMemAlloc, "cuMemAlloc_v2");
        bind(m_d.cuMemFree, "cuMemFree_v2");
        bind(m_d.cuMemcpyHtoD, "cuMemcpyHtoD_v2");
        bind(m_d.cuMemcpyDtoH, "cuMemcpyDtoH_v2");
        bind(m_d.cuLaunchKernel, "cuLaunchKernel");
        if (missing)
            return false;
        std::string error;
        int         devices = 0;
        if (!check(m_d.cuInit(0), "cuInit", error) || !check(m_d.cuDeviceGetCount(&devices), "cuDeviceGetCount", error)) {
            m_reason = error;
            return false;
        }
        if (devices < 1) {
            m_reason = "no CUDA device";
            return false;
        }
        char name[256] = {};
        if (!check(m_d.cuDeviceGet(&m_device, 0), "cuDeviceGet", error) ||
            !check(m_d.cuDeviceGetName(name, int(sizeof(name)) - 1, m_device), "cuDeviceGetName", error) ||
            !check(m_d.cuDevicePrimaryCtxRetain(&m_context, m_device), "cuDevicePrimaryCtxRetain", error) ||
            !check(m_d.cuCtxPushCurrent(m_context), "cuCtxPushCurrent", error)) {
            m_reason = error;
            return false;
        }
        m_device_name = name;
        // PERMANENT DIAGNOSTIC: the driver's compile log is the only record of why the PTX was refused.
        char  log[4096]  = {};
        int   options[]  = { CU_JIT_ERROR_LOG_BUFFER, CU_JIT_ERROR_LOG_BUFFER_SIZE_BYTES };
        void *values[]   = { log, reinterpret_cast<void *>(uintptr_t(sizeof(log) - 1)) };
        bool  ok         = check(m_d.cuModuleLoadDataEx(&m_module, magpie_cuda_kernels_ptx, 2, options, values),
                                 "cuModuleLoadDataEx", error);
        for (int op = 0; ok && op < opCount; ++op)
            ok = check(m_d.cuModuleGetFunction(&m_kernels[op], m_module, kernel_names[op]), kernel_names[op], error);
        CUcontext previous = nullptr;
        m_d.cuCtxPopCurrent(&previous);
        if (!ok)
            m_reason = error + (log[0] ? std::string(": ") + log : std::string());
        return ok;
    }

    Driver      m_d{};
    bool        m_tried{ false };
    bool        m_ok{ false };
    std::string m_reason;
    std::string m_device_name;
    CUdevice    m_device{ 0 };
    CUcontext   m_context{ nullptr };
    CUmodule    m_module{ nullptr };
    CUfunction  m_kernels[opCount]{};
    CUdeviceptr m_in{ 0 }, m_out{ 0 };
    size_t      m_in_capacity{ 0 }, m_out_capacity{ 0 };
};

// Never destroyed: the driver may already be unloading when static destructors run at exit.
Runtime &runtime()
{
    static Runtime *instance = new Runtime();
    return *instance;
}

template<class T> void put(std::vector<uint8_t> &buffer, const T &value)
{
    const size_t at = buffer.size();
    buffer.resize(at + sizeof(T));
    std::memcpy(buffer.data() + at, &value, sizeof(T));
}

template<class T> T get(const std::vector<uint8_t> &buffer, size_t at)
{
    T value;
    std::memcpy(&value, buffer.data() + at, sizeof(T));
    return value;
}

// Packs nothing itself: runs a packed batch through the GPU and, in On mode, compares every decoded
// item with the caller's CPU calculation. `decode(output, i)` reads item i of the downloaded output.
template<class T, class Decode>
bool dispatch(Op op, size_t count, const std::vector<uint8_t> &input, size_t input_stride, uint32_t aux, uint64_t off1,
              uint64_t off2, size_t output_stride, uint64_t steps_per_item, Decode decode,
              const std::function<T(size_t)> &cpu, std::vector<T> &out)
{
    OpStats &stats = g_stats[op];
    if (input.size() + count * output_stride > max_batch_bytes || count > std::numeric_limits<uint32_t>::max()) {
        ++stats.too_large;
        return false;
    }
    Runtime                     &rt = runtime();
    std::unique_lock<std::mutex> lock(rt.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        // Another thread's batch is on the GPU. Waiting would serialize the slicer's parallel loops.
        ++stats.busy;
        return false;
    }
    if (!rt.ready(nullptr)) {
        ++stats.failed;
        return false;
    }
    MAGPIE_STAGE_TIMER("cuda.dispatch");
    const auto           started          = std::chrono::steady_clock::now();
    const size_t         items_per_launch = size_t(std::max<uint64_t>(1, std::min(max_items_per_launch,
                                                                                max_steps_per_launch / std::max<uint64_t>(1, steps_per_item))));
    std::vector<uint8_t> output;
    std::string          error;
    if (!rt.run(op, input, input_stride, count, aux, off1, off2, output_stride, items_per_launch, output, error)) {
        // PERMANENT DIAGNOSTIC: names the driver call that failed; the slice itself silently uses the CPU.
        BOOST_LOG_TRIVIAL(warning) << "CUDA " << op_names[op] << ": " << error << "; CPU used";
        ++stats.failed;
        return false;
    }
    lock.unlock();
    stats.gpu_us += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
    out.resize(count);
    for (size_t i = 0; i < count; ++i)
        out[i] = decode(output, i);
    if (cuda_mode() == CudaMode::On) {
        for (size_t i = 0; i < count; ++i)
            if (!(out[i] == cpu(i))) {
                // PERMANENT DIAGNOSTIC: a kernel that drifted from its CPU function shows up here first.
                BOOST_LOG_TRIVIAL(error) << "CUDA " << op_names[op] << ": item " << i << " of " << count
                                         << " differs from the CPU; batch discarded";
                ++stats.rejected;
                out.clear();
                return false;
            }
    }
    ++stats.gpu_batches;
    stats.gpu_items += count;
    return true;
}

// Coordinates go to the GPU as int64 like coord_t, and the kernels do the CPU's int64 arithmetic,
// wrapping included, so there is nothing to range-check.
static_assert(sizeof(coord_t) == sizeof(int64_t), "the kernels read coordinates as int64");

} // namespace

CudaMode cuda_mode_from_string(const std::string &value)
{
    if (value == "on")
        return CudaMode::On;
    if (value == "max")
        return CudaMode::MaxGpu;
    return CudaMode::Off;
}

const char *cuda_mode_name(CudaMode mode)
{
    switch (mode) {
    case CudaMode::On: return "on";
    case CudaMode::MaxGpu: return "max";
    default: return "off";
    }
}

CudaMode cuda_mode() { return CudaMode(g_mode.load(std::memory_order_relaxed)); }

CudaSession::CudaSession(const std::string &configured_mode)
{
    CudaMode mode = cuda_mode_from_string(configured_mode);
    if (const char *env = std::getenv("MAGPIE_CUDA_SLICER_ENABLE"); env != nullptr && *env != 0)
        mode = std::string(env) == "1" ? CudaMode::On : cuda_mode_from_string(env);
    const char *force = std::getenv("MAGPIE_CUDA_SLICER_FORCE_DISPATCH");
    g_force_dispatch.store(force != nullptr && std::string(force) == "1");
    for (OpStats &stats : g_stats)
        stats.reset();
    g_mode.store(int(mode));
    if (mode != CudaMode::Off)
        BOOST_LOG_TRIVIAL(info) << "CUDA compute: " << cuda_mode_name(mode);
}

CudaSession::~CudaSession()
{
    if (cuda_mode() == CudaMode::Off)
        return;
    const std::string report = cuda_report();
    g_mode.store(int(CudaMode::Off));
    BOOST_LOG_TRIVIAL(info) << report;
    if (const char *env = std::getenv("MAGPIE_CUDA_REPORT"); env != nullptr && std::string(env) == "1")
        std::fprintf(stderr, "%s", report.c_str());
}

bool cuda_should_dispatch(size_t items)
{
    return cuda_mode() != CudaMode::Off && items > 0 &&
           (items >= cuda_minimum_batch_items || g_force_dispatch.load(std::memory_order_relaxed));
}

bool cuda_available(std::string *reason)
{
    Runtime                    &rt = runtime();
    std::lock_guard<std::mutex> lock(rt.mutex);
    return rt.ready(reason);
}

std::string cuda_report()
{
    std::ostringstream report;
    report << "CUDA compute (" << cuda_mode_name(cuda_mode()) << "):\n";
    for (int op = 0; op < opCount; ++op) {
        const OpStats &s = g_stats[op];
        report << "  " << op_names[op] << ": GPU " << s.gpu_batches << " batches, " << s.gpu_items << " items, "
               << (s.gpu_us / 1000.) << " ms; CPU: busy " << s.busy << ", too large " << s.too_large << ", failed "
               << s.failed << ", rejected " << s.rejected << "\n";
    }
    return report.str();
}

bool cuda_mesh_layer_ranges(const std::vector<std::array<float, 3>> &facet_z, const std::vector<float> &zs,
                            const std::function<CudaLayerRange(size_t)> &cpu, std::vector<CudaLayerRange> &out)
{
    if (!cuda_should_dispatch(facet_z.size()) || zs.empty() || zs.size() > std::numeric_limits<uint32_t>::max())
        return false;
    std::vector<uint8_t> input;
    input.reserve(facet_z.size() * 12 + zs.size() * 4);
    for (const std::array<float, 3> &z : facet_z)
        for (float v : z)
            put(input, v);
    const uint64_t zs_offset = input.size();
    for (float z : zs)
        put(input, z);
    // Two binary searches over the layers per facet.
    const uint64_t steps = 2 * uint64_t(std::ceil(std::log2(double(zs.size()) + 1.))) + 2;
    return dispatch<CudaLayerRange>(
        opMeshLayerRanges, facet_z.size(), input, 12, uint32_t(zs.size()), zs_offset, 0, 12, steps,
        [](const std::vector<uint8_t> &o, size_t i) {
            return CudaLayerRange{ get<uint32_t>(o, i * 12), get<uint32_t>(o, i * 12 + 4), get<uint32_t>(o, i * 12 + 8) };
        },
        cpu, out);
}

bool cuda_vertical_intersections(const std::vector<CudaVerticalCrossing> &crossings,
                                 const std::function<CudaRational(size_t)> &cpu, std::vector<CudaRational> &out)
{
    if (!cuda_should_dispatch(crossings.size()))
        return false;
    std::vector<uint8_t> input;
    input.reserve(crossings.size() * 40);
    for (const CudaVerticalCrossing &c : crossings) {
        put(input, int64_t(c.p1.x()));
        put(input, int64_t(c.p1.y()));
        put(input, int64_t(c.p2.x()));
        put(input, int64_t(c.p2.y()));
        put(input, int64_t(c.x));
    }
    return dispatch<CudaRational>(
        opVerticalIntersections, crossings.size(), input, 40, 0, 0, 0, 16, 1,
        [](const std::vector<uint8_t> &o, size_t i) {
            return CudaRational{ get<int64_t>(o, i * 16), uint32_t(get<uint64_t>(o, i * 16 + 8)) };
        },
        cpu, out);
}

bool cuda_min_distances_squared(const Points &points, const Lines &edges, const std::function<double(size_t)> &cpu,
                                std::vector<double> &out)
{
    if (!cuda_should_dispatch(points.size()) || edges.size() > std::numeric_limits<uint32_t>::max())
        return false;
    std::vector<uint8_t> input;
    input.reserve(points.size() * 16 + edges.size() * 32);
    for (const Point &p : points) {
        put(input, int64_t(p.x()));
        put(input, int64_t(p.y()));
    }
    const uint64_t edges_offset = input.size();
    for (const Line &e : edges) {
        put(input, int64_t(e.a.x()));
        put(input, int64_t(e.a.y()));
        put(input, int64_t(e.b.x()));
        put(input, int64_t(e.b.y()));
    }
    return dispatch<double>(
        opMinDistances, points.size(), input, 16, uint32_t(edges.size()), edges_offset, 0, 8, edges.size() + 1,
        [](const std::vector<uint8_t> &o, size_t i) { return get<double>(o, i * 8); }, cpu, out);
}

bool cuda_points_in_expolygons(const Points &points, const ExPolygons &expolygons, const std::function<bool(size_t)> &cpu,
                               std::vector<uint8_t> &out)
{
    if (!cuda_should_dispatch(points.size()))
        return false;
    // Ring table: {first vertex, vertex count, is contour, unused}; a contour starts each ExPolygon.
    std::vector<uint8_t> rings, vertices;
    uint64_t             vertex_count = 0, ring_count = 0;
    auto add_ring = [&](const Polygon &ring, bool contour) {
        put(rings, uint32_t(vertex_count));
        put(rings, uint32_t(ring.points.size()));
        put(rings, uint32_t(contour ? 1 : 0));
        put(rings, uint32_t(0));
        for (const Point &p : ring.points) {
            put(vertices, int64_t(p.x()));
            put(vertices, int64_t(p.y()));
        }
        vertex_count += ring.points.size();
        ++ring_count;
    };
    for (const ExPolygon &expolygon : expolygons) {
        add_ring(expolygon.contour, true);
        for (const Polygon &hole : expolygon.holes)
            add_ring(hole, false);
    }
    if (vertex_count > std::numeric_limits<uint32_t>::max() || ring_count > std::numeric_limits<uint32_t>::max())
        return false;
    std::vector<uint8_t> input;
    input.reserve(points.size() * 16 + rings.size() + vertices.size());
    for (const Point &p : points) {
        put(input, int64_t(p.x()));
        put(input, int64_t(p.y()));
    }
    const uint64_t rings_offset = input.size();
    input.insert(input.end(), rings.begin(), rings.end());
    const uint64_t vertices_offset = input.size();
    input.insert(input.end(), vertices.begin(), vertices.end());
    std::vector<uint8_t> inside;
    const bool ok = dispatch<uint8_t>(
        opPointsInExPolygons, points.size(), input, 16, uint32_t(ring_count), rings_offset, vertices_offset, 4,
        vertex_count + ring_count + 1, [](const std::vector<uint8_t> &o, size_t i) { return uint8_t(get<uint32_t>(o, i * 4) != 0); },
        std::function<uint8_t(size_t)>([&cpu](size_t i) { return uint8_t(cpu(i) ? 1 : 0); }), inside);
    if (ok)
        out = std::move(inside);
    return ok;
}

} // namespace Slic3r::Gpu
