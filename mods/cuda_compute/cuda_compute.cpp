// MAGPIE: the CUDA compute mod (slot "accel.batch", mods/sdk/include/magpie_accel.h).
//
// Runs four per-item calculations of the slicer on an NVIDIA GPU so CUDA on and off can be compared:
// facet layer ranges, rectilinear infill crossings, Lightning boundary distances and tree support contact
// points inside the overhang. Each kernel reproduces its CPU function in the core bit for bit
// (CudaKernels.ptx), so the G-code is the same with CUDA on, off or unavailable; in "on" mode the core
// checks that and throws away any batch that differs.
//
// The kernels are PTX text that the NVIDIA driver compiles when they are first used. Neither the build
// nor the user needs the CUDA toolkit; without an NVIDIA driver every batch is declined and the core
// computes it on the CPU.
//
// Parameter "mode": off never loads the driver; on = the core checks every result against the CPU;
// max = results used unchecked (the core allows that for an official build of this mod only).
//
// Environment overrides, for CLI runs: MAGPIE_CUDA_SLICER_ENABLE=0/1/on/max sets the mode,
// MAGPIE_CUDA_SLICER_FORCE_DISPATCH=1 sends batches of any size to the GPU (tests), and
// MAGPIE_CUDA_REPORT=1 prints the per-operation counts to stderr at the end of each slice.

#include <magpie_mod.hpp>
#include <magpie_accel.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <type_traits>
#include <vector>

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

// PTX text of CudaKernels.ptx, generated at build time (mods/sdk/EmbedText.cmake).
#include "CudaKernelsPtx.hpp"

#define CUDA_MOD_ID "cuda_compute"
#define CUDA_MOD_VERSION "1.0.0"

MAGPIE_MOD_DEFINE(CUDA_MOD_ID, CUDA_MOD_VERSION)

namespace {

void mod_log(int32_t level, const std::string &text) { magpie::log(level, CUDA_MOD_ID, text); }

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
    std::atomic<uint64_t> too_large{ 0 }, busy{ 0 }, failed{ 0 };
    void reset() { gpu_batches = gpu_items = gpu_us = too_large = busy = failed = 0; }
};

std::atomic<int32_t> g_mode{ MP_ACCEL_OFF };
std::atomic<bool>    g_force_dispatch{ false };
OpStats              g_stats[opCount];

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
                mod_log(MP_LOG_INFO, "CUDA compute: " + m_device_name + " ready");
            else
                mod_log(MP_LOG_WARNING, "CUDA compute unavailable: " + m_reason);
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

// Smallest batch, in work items, worth a GPU round trip. A guess (line 1 used the same), not measured.
constexpr uint64_t minimum_batch_items = 4096;

mp_status declined() { return { MP_ERR_FAILED, "declined" }; }

// Runs a packed batch through the GPU. False means the core computes the batch on the CPU: too large,
// the GPU busy with another batch, no GPU or a driver error.
bool dispatch(Op op, size_t count, const std::vector<uint8_t> &input, size_t input_stride, uint32_t aux, uint64_t off1,
              uint64_t off2, size_t output_stride, uint64_t steps_per_item, std::vector<uint8_t> &output)
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
    const auto   started          = std::chrono::steady_clock::now();
    const size_t items_per_launch = size_t(std::max<uint64_t>(1, std::min(max_items_per_launch,
                                                                          max_steps_per_launch / std::max<uint64_t>(1, steps_per_item))));
    std::string error;
    if (!rt.run(op, input, input_stride, count, aux, off1, off2, output_stride, items_per_launch, output, error)) {
        // PERMANENT DIAGNOSTIC: names the driver call that failed; the core then computes the batch on the CPU.
        mod_log(MP_LOG_WARNING, std::string("CUDA ") + op_names[op] + ": " + error + "; CPU used");
        ++stats.failed;
        return false;
    }
    lock.unlock();
    stats.gpu_us += uint64_t(std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - started).count());
    ++stats.gpu_batches;
    stats.gpu_items += count;
    return true;
}

const char *mode_name(int32_t mode) { return mode == MP_ACCEL_TRUSTED ? "max" : mode == MP_ACCEL_VERIFY ? "on" : "off"; }

std::string report()
{
    std::ostringstream out;
    out << "CUDA compute (" << mode_name(g_mode.load()) << "):\n";
    for (int op = 0; op < opCount; ++op) {
        const OpStats &s = g_stats[op];
        out << "  " << op_names[op] << ": GPU " << s.gpu_batches << " batches, " << s.gpu_items << " items, " << (s.gpu_us / 1000.)
            << " ms; CPU: busy " << s.busy << ", too large " << s.too_large << ", failed " << s.failed << "\n";
    }
    return out.str();
}

// ---------------------------------------------------------------------------------------------------
// accel.batch v1

mp_status begin(void *, mp_ctx *ctx, int32_t *mode)
{
    return magpie::guarded([&] {
        std::string value = magpie::param_str(ctx, "mode").value_or("off");
        if (const char *env = std::getenv("MAGPIE_CUDA_SLICER_ENABLE"); env != nullptr && *env != 0)
            value = std::string(env) == "1" ? "on" : std::string(env);
        const char *force = std::getenv("MAGPIE_CUDA_SLICER_FORCE_DISPATCH");
        g_force_dispatch.store(force != nullptr && std::string(force) == "1");
        for (OpStats &stats : g_stats)
            stats.reset();
        *mode = value == "on" ? MP_ACCEL_VERIFY : value == "max" ? MP_ACCEL_TRUSTED : MP_ACCEL_OFF;
        g_mode.store(*mode);
        if (*mode != MP_ACCEL_OFF)
            mod_log(MP_LOG_INFO, std::string("CUDA compute: ") + mode_name(*mode));
    });
}

void end(void *)
{
    if (g_mode.load() == MP_ACCEL_OFF)
        return;
    const std::string text = report();
    g_mode.store(MP_ACCEL_OFF);
    mod_log(MP_LOG_INFO, text);
    if (const char *env = std::getenv("MAGPIE_CUDA_REPORT"); env != nullptr && std::string(env) == "1")
        std::fprintf(stderr, "%s", text.c_str());
}

int32_t accepts(void *, uint32_t, uint64_t items)
{
    return items > 0 && (items >= minimum_batch_items || g_force_dispatch.load(std::memory_order_relaxed)) ? 1 : 0;
}

int32_t available(void *, char *reason, uint32_t cap)
{
    Runtime                    &rt = runtime();
    std::lock_guard<std::mutex> lock(rt.mutex);
    std::string                 why;
    const bool                  ok = rt.ready(&why);
    if (!ok && reason != nullptr && cap > 0) {
        const size_t n = std::min<size_t>(why.size(), cap - 1);
        std::memcpy(reason, why.data(), n);
        reason[n] = '\0';
    }
    return ok ? 1 : 0;
}

mp_status facet_layer_ranges(void *, const float *facet_z, uint64_t n, const float *zs, uint64_t n_zs, uint32_t *out)
{
    if (n == 0 || n_zs == 0 || n_zs > std::numeric_limits<uint32_t>::max())
        return declined();
    std::vector<uint8_t> input(size_t(n) * 12 + size_t(n_zs) * 4);
    std::memcpy(input.data(), facet_z, size_t(n) * 12);
    const uint64_t zs_offset = uint64_t(n) * 12;
    std::memcpy(input.data() + zs_offset, zs, size_t(n_zs) * 4);
    // Two binary searches over the layers per facet.
    const uint64_t       steps = 2 * uint64_t(std::ceil(std::log2(double(n_zs) + 1.))) + 2;
    std::vector<uint8_t> output;
    if (!dispatch(opMeshLayerRanges, size_t(n), input, 12, uint32_t(n_zs), zs_offset, 0, 12, steps, output))
        return declined();
    std::memcpy(out, output.data(), size_t(n) * 12);
    return { MP_OK, nullptr };
}

mp_status vertical_crossings(void *, const int64_t *p1p2x, uint64_t n, int64_t *out_p, uint32_t *out_q)
{
    if (n == 0)
        return declined();
    std::vector<uint8_t> input(size_t(n) * 40);
    std::memcpy(input.data(), p1p2x, size_t(n) * 40);
    std::vector<uint8_t> output;
    if (!dispatch(opVerticalIntersections, size_t(n), input, 40, 0, 0, 0, 16, 1, output))
        return declined();
    for (size_t i = 0; i < size_t(n); ++i) {
        out_p[i] = get<int64_t>(output, i * 16);
        out_q[i] = uint32_t(get<uint64_t>(output, i * 16 + 8));
    }
    return { MP_OK, nullptr };
}

mp_status min_dist2(void *, const mp_point *points, uint64_t n, const mp_point *edges, uint64_t m, double *out)
{
    if (n == 0 || m > std::numeric_limits<uint32_t>::max())
        return declined();
    std::vector<uint8_t> input;
    input.reserve(size_t(n) * 16 + size_t(m) * 32);
    for (uint64_t i = 0; i < n; ++i) {
        put(input, points[i].x);
        put(input, points[i].y);
    }
    const uint64_t edges_offset = input.size();
    for (uint64_t i = 0; i < 2 * m; ++i) {
        put(input, edges[i].x);
        put(input, edges[i].y);
    }
    std::vector<uint8_t> output;
    if (!dispatch(opMinDistances, size_t(n), input, 16, uint32_t(m), edges_offset, 0, 8, m + 1, output))
        return declined();
    std::memcpy(out, output.data(), size_t(n) * 8);
    return { MP_OK, nullptr };
}

mp_status points_in_expolys(void *, const mp_point *points, uint64_t n, const mp_expolys *polys, uint8_t *out)
{
    if (n == 0 || polys == nullptr)
        return declined();
    // Ring table: {first vertex, vertex count, is contour, unused}; a contour starts each ExPolygon.
    std::vector<uint8_t> rings, vertices;
    uint64_t             vertex_count = 0, ring_count = 0;
    for (uint32_t e = 0; e < polys->n; ++e)
        for (uint32_t r = 0; r < polys->items[e].n; ++r) {
            const mp_ring &ring = polys->items[e].rings[r];
            put(rings, uint32_t(vertex_count));
            put(rings, uint32_t(ring.n));
            put(rings, uint32_t(r == 0 ? 1 : 0));
            put(rings, uint32_t(0));
            for (uint32_t k = 0; k < ring.n; ++k) {
                put(vertices, ring.pts[k].x);
                put(vertices, ring.pts[k].y);
            }
            vertex_count += ring.n;
            ++ring_count;
        }
    if (vertex_count > std::numeric_limits<uint32_t>::max() || ring_count > std::numeric_limits<uint32_t>::max())
        return declined();
    std::vector<uint8_t> input;
    input.reserve(size_t(n) * 16 + rings.size() + vertices.size());
    for (uint64_t i = 0; i < n; ++i) {
        put(input, points[i].x);
        put(input, points[i].y);
    }
    const uint64_t rings_offset = input.size();
    input.insert(input.end(), rings.begin(), rings.end());
    const uint64_t vertices_offset = input.size();
    input.insert(input.end(), vertices.begin(), vertices.end());
    std::vector<uint8_t> output;
    if (!dispatch(opPointsInExPolygons, size_t(n), input, 16, uint32_t(ring_count), rings_offset, vertices_offset, 4,
                  vertex_count + ring_count + 1, output))
        return declined();
    for (size_t i = 0; i < size_t(n); ++i)
        out[i] = uint8_t(get<uint32_t>(output, i * 4) != 0);
    return { MP_OK, nullptr };
}

const mp_accel_batch_v1 table = { sizeof(mp_accel_batch_v1), begin, end, accepts, available,
                                  facet_layer_ranges, vertical_crossings, min_dist2, points_in_expolys };

// Coordinates go to the GPU as int64 like the core's, and the kernels do the CPU's int64 arithmetic,
// wrapping included, so there is nothing to range-check.
static_assert(sizeof(mp_point) == 2 * sizeof(int64_t), "the kernels read coordinates as int64");

} // namespace

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_ACCEL_SLOT, MP_ACCEL_SLOT_VERSION, "cuda", &table, sizeof(table), nullptr);
}

void magpie::mod_shutdown() {}
