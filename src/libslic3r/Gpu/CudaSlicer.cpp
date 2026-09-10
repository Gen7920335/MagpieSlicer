#include "CudaSlicer.hpp"
#include "CudaIntersectionAbi.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <mutex>
#include <map>
#include <numeric>
#include <cmath>
#include <stdexcept>

#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cuda.h>
#include "CudaIntersectionBinary.hpp"
#endif

namespace Slic3r::Gpu {
namespace {
using Clock = std::chrono::steady_clock;
double elapsed_ms(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
// Capacity is in requests. Per-record strides are bounded separately below.
constexpr size_t maximum_requests = 256 * 1024;
constexpr size_t maximum_input_stride_bytes = 128;
constexpr size_t maximum_output_stride_bytes = 32;
// Experimental dispatch floor, not a measured profitability threshold.
constexpr size_t minimum_requests = 4096;
constexpr const char* busy_fallback_diagnostic = "CUDA runtime busy; CPU fallback without waiting";
constexpr const char* deferred_preflight_diagnostic = "CUDA preflight accounted while runtime busy";
std::atomic<bool> session_enabled {false};
std::atomic<bool> session_force_dispatch {false};
std::atomic<bool> session_skip_cpu_validation {false};
bool environment_flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && std::string(value) == "1";
}
struct Runtime {
    std::mutex mutex;
    SlicingProfileGpuStats stats;
    std::atomic<uint64_t> busy_fallbacks {0};
    std::atomic<uint64_t> busy_decision_us {0};
    std::atomic<uint64_t> deferred_preflight_calls {0};
    std::atomic<uint64_t> deferred_preflight_us {0};
    std::atomic<uint64_t> deferred_fallback_us {0};
    void reset_stats() {
        stats = {};
        stats.execution_profile = session_skip_cpu_validation.load() ?
            "CUDA Max GPU" : "validated CUDA compute";
        stats.validation_mode = session_skip_cpu_validation.load() ?
            "max: CPU result duplication disabled" :
            "strict: every result compared with CPU reference";
        stats.initialization_ms = stats.allocation_ms = stats.packing_ms = 0;
        stats.upload_ms = stats.download_ms = stats.validation_ms = stats.fallback_ms = 0;
        stats.queue_wait_ms = stats.kernel_wall_ms = stats.preflight_ms = 0;
        busy_fallbacks.store(0, std::memory_order_relaxed);
        busy_decision_us.store(0, std::memory_order_relaxed);
        deferred_preflight_calls.store(0, std::memory_order_relaxed);
        deferred_preflight_us.store(0, std::memory_order_relaxed);
        deferred_fallback_us.store(0, std::memory_order_relaxed);
    }
    Runtime() { reset_stats(); }

#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    HMODULE driver = nullptr;
    CUdevice device = 0;
    CUcontext context = nullptr;
    CUmodule module = nullptr;
    CUfunction kernel = nullptr;
    CUstream stream = nullptr;
    CUevent started = nullptr, finished = nullptr;
    CUdeviceptr input = 0, output = 0;
    void* host_input = nullptr;
    void* host_output = nullptr;
    size_t input_capacity_bytes = 0, output_capacity_bytes = 0;
    bool unavailable = false;
    std::string device_name;
#define CUDA_FUNCTIONS(X) \
    X(cuInit, "cuInit") \
    X(cuDeviceGet, "cuDeviceGet") \
    X(cuDeviceGetName, "cuDeviceGetName") \
    X(cuDevicePrimaryCtxRetain, "cuDevicePrimaryCtxRetain") \
    X(cuDevicePrimaryCtxRelease, "cuDevicePrimaryCtxRelease_v2") \
    X(cuCtxPushCurrent, "cuCtxPushCurrent_v2") \
    X(cuCtxPopCurrent, "cuCtxPopCurrent_v2") \
    X(cuModuleLoadData, "cuModuleLoadData") \
    X(cuModuleUnload, "cuModuleUnload") \
    X(cuModuleGetFunction, "cuModuleGetFunction") \
    X(cuStreamCreate, "cuStreamCreate") \
    X(cuStreamDestroy, "cuStreamDestroy_v2") \
    X(cuStreamSynchronize, "cuStreamSynchronize") \
    X(cuEventCreate, "cuEventCreate") \
    X(cuEventDestroy, "cuEventDestroy_v2") \
    X(cuEventRecord, "cuEventRecord") \
    X(cuEventSynchronize, "cuEventSynchronize") \
    X(cuEventElapsedTime, "cuEventElapsedTime") \
    X(cuMemAlloc, "cuMemAlloc_v2") \
    X(cuMemFree, "cuMemFree_v2") \
    X(cuMemAllocHost, "cuMemAllocHost_v2") \
    X(cuMemFreeHost, "cuMemFreeHost") \
    X(cuMemcpyHtoDAsync, "cuMemcpyHtoDAsync_v2") \
    X(cuMemcpyDtoHAsync, "cuMemcpyDtoHAsync_v2") \
    X(cuLaunchKernel, "cuLaunchKernel")
#define DECLARE_CUDA(name, symbol) decltype(&name) p_##name = nullptr;
    CUDA_FUNCTIONS(DECLARE_CUDA)
#undef DECLARE_CUDA
    static void check(CUresult code, const char* operation) {
        if (code != CUDA_SUCCESS)
            throw std::runtime_error(std::string(operation) + " failed (CUDA " + std::to_string(int(code)) + ")");
    }
    struct CurrentContext {
        Runtime& runtime;
        explicit CurrentContext(Runtime& r) : runtime(r) {
            check(r.p_cuCtxPushCurrent(r.context), "cuCtxPushCurrent");
        }
        ~CurrentContext() { CUcontext previous; runtime.p_cuCtxPopCurrent(&previous); }
    };
    void initialize() {
        if (unavailable) throw std::runtime_error("CUDA unavailable for this process; CPU fallback");
        if (kernel != nullptr) return;
        unavailable = true;
        // Load only the OS driver, never a DLL from the working directory.
        driver = LoadLibraryExW(L"nvcuda.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!driver) throw std::runtime_error("NVIDIA CUDA driver not available");
#define LOAD_CUDA(name, symbol) \
        p_##name = reinterpret_cast<decltype(p_##name)>(GetProcAddress(driver, symbol)); \
        if (!p_##name) throw std::runtime_error("Missing CUDA driver entry: " symbol);
        CUDA_FUNCTIONS(LOAD_CUDA)
#undef LOAD_CUDA
        check(p_cuInit(0), "cuInit");
        check(p_cuDeviceGet(&device, 0), "cuDeviceGet");
        char name[256] = {};
        check(p_cuDeviceGetName(name, sizeof(name), device), "cuDeviceGetName");
        device_name = name;
        check(p_cuDevicePrimaryCtxRetain(&context, device), "cuDevicePrimaryCtxRetain");
        CurrentContext current(*this);
        check(p_cuModuleLoadData(&module, magpie_cuda_binary), "cuModuleLoadData");
        check(p_cuStreamCreate(&stream, CU_STREAM_NON_BLOCKING), "cuStreamCreate");
        check(p_cuEventCreate(&started, CU_EVENT_DEFAULT), "cuEventCreate");
        check(p_cuEventCreate(&finished, CU_EVENT_DEFAULT), "cuEventCreate");
        check(p_cuModuleGetFunction(&kernel, module, "magpie_vertical_intersections"), "cuModuleGetFunction");
        unavailable = false;
    }
    void reserve(size_t input_bytes, size_t output_bytes) {
        if (input_bytes <= input_capacity_bytes && output_bytes <= output_capacity_bytes) return;
        const size_t next_input = std::min(maximum_requests * maximum_input_stride_bytes,
            std::max(input_bytes, input_capacity_bytes * 2));
        const size_t next_output = std::min(maximum_requests * maximum_output_stride_bytes,
            std::max(output_bytes, output_capacity_bytes * 2));
        CUdeviceptr new_input = 0, new_output = 0;
        void* new_host_input = nullptr;
        void* new_host_output = nullptr;
        try {
            check(p_cuMemAlloc(&new_input, next_input), "cuMemAlloc input");
            check(p_cuMemAlloc(&new_output, next_output), "cuMemAlloc output");
            check(p_cuMemAllocHost(&new_host_input, next_input), "cuMemAllocHost input");
            check(p_cuMemAllocHost(&new_host_output, next_output), "cuMemAllocHost output");
        } catch (...) {
            if (new_input) p_cuMemFree(new_input);
            if (new_output) p_cuMemFree(new_output);
            if (new_host_input) p_cuMemFreeHost(new_host_input);
            if (new_host_output) p_cuMemFreeHost(new_host_output);
            throw;
        }
        if (input) p_cuMemFree(input);
        if (output) p_cuMemFree(output);
        if (host_input) p_cuMemFreeHost(host_input);
        if (host_output) p_cuMemFreeHost(host_output);
        input = new_input; output = new_output;
        input_capacity_bytes = next_input; output_capacity_bytes = next_output;
        host_input = new_host_input; host_output = new_host_output;
    }
    // Caller holds mutex through validation: returned pinned output belongs to
    // this runtime and cannot be retained across the next execute call.
    double execute(const char* kernel_name, const void* records, size_t count,
                   size_t input_stride, size_t output_stride, size_t packed_input_bytes = 0) {
        if (count == 0 || count > maximum_requests || input_stride == 0 ||
            input_stride > maximum_input_stride_bytes || output_stride == 0 ||
            output_stride > maximum_output_stride_bytes)
            throw std::runtime_error("CUDA dispatch exceeds bounded ABI capacity");
        const size_t input_bytes = packed_input_bytes ? packed_input_bytes : count * input_stride;
        const size_t output_bytes = count * output_stride;
        if (input_bytes < count * input_stride || input_bytes > maximum_requests * maximum_input_stride_bytes)
            throw std::runtime_error("CUDA packed input capacity exceeded");
        auto phase = Clock::now();
        try { initialize(); } catch (...) { stats.initialization_ms += elapsed_ms(phase); throw; }
        stats.initialization_ms += elapsed_ms(phase);
        stats.selected_device = device_name;
        CurrentContext current(*this);
        try {
            CUfunction function = nullptr;
            check(p_cuModuleGetFunction(&function, module, kernel_name), "cuModuleGetFunction");
            phase = Clock::now(); reserve(input_bytes, output_bytes); stats.allocation_ms += elapsed_ms(phase);
            phase = Clock::now();
            // Pinned staging and one stream enforce HtoD -> kernel -> DtoH.
            std::memcpy(host_input, records, input_bytes);
            check(p_cuMemcpyHtoDAsync(input, host_input, input_bytes, stream), "cuMemcpyHtoDAsync");
            check(p_cuStreamSynchronize(stream), "CUDA upload completion");
            stats.upload_ms += elapsed_ms(phase);
            stats.uploaded_bytes += input_bytes;
            phase = Clock::now();
            unsigned int record_count = static_cast<unsigned int>(count);
            void* arguments[] = {&input, &output, &record_count};
            check(p_cuEventRecord(started, stream), "cuEventRecord start");
            check(p_cuLaunchKernel(function, (record_count + 255) / 256, 1, 1, 256, 1, 1,
                                  0, stream, arguments, nullptr), "cuLaunchKernel");
            ++stats.queue_submissions; stats.submitted_work_items += count;
            check(p_cuEventRecord(finished, stream), "cuEventRecord end");
            check(p_cuEventSynchronize(finished), "cuEventSynchronize");
            float gpu_ms = 0;
            check(p_cuEventElapsedTime(&gpu_ms, started, finished), "cuEventElapsedTime");
            stats.total_gpu_ms += gpu_ms;
            stats.kernel_wall_ms += elapsed_ms(phase);
            phase = Clock::now();
            check(p_cuMemcpyDtoHAsync(host_output, output, output_bytes, stream), "cuMemcpyDtoHAsync");
            check(p_cuStreamSynchronize(stream), "CUDA download completion");
            stats.download_ms += elapsed_ms(phase);
            stats.downloaded_bytes += output_bytes;
            return gpu_ms;
        } catch (...) {
            if (stream) p_cuStreamSynchronize(stream);
            unavailable = true;
            throw;
        }
    }
    ~Runtime() {
        if (context && p_cuCtxPushCurrent && p_cuCtxPushCurrent(context) == CUDA_SUCCESS) {
            if (stream) p_cuStreamSynchronize(stream);
            if (input) p_cuMemFree(input);
            if (output) p_cuMemFree(output);
            if (host_input) p_cuMemFreeHost(host_input);
            if (host_output) p_cuMemFreeHost(host_output);
            if (started) p_cuEventDestroy(started);
            if (finished) p_cuEventDestroy(finished);
            if (stream) p_cuStreamDestroy(stream);
            if (module) p_cuModuleUnload(module);
            CUcontext previous;
            p_cuCtxPopCurrent(&previous);
        }
        if (context && p_cuDevicePrimaryCtxRelease) p_cuDevicePrimaryCtxRelease(device);
        if (driver) FreeLibrary(driver);
    }
#undef CUDA_FUNCTIONS
#endif
};
Runtime& runtime() { static Runtime instance; return instance; }
void publish(const SlicingProfileGpuStats& stats) {
#ifdef MAGPIE_SLICING_TIMING
    SlicingProfiler::instance().set_gpu_stats(SlicingProfileGpuApi::CUDA, stats);
#endif
}
}

void CudaSlicerBackend::begin_slicing_session() {
    begin_slicing_session(environment_flag("MAGPIE_CUDA_SLICER_ENABLE"),
                          environment_flag("MAGPIE_CUDA_SLICER_FORCE_DISPATCH"),
                          environment_flag("MAGPIE_CUDA_SLICER_SKIP_CPU_VALIDATION"));
}
void CudaSlicerBackend::begin_slicing_session(bool enable, bool force_dispatch, bool skip_cpu_validation) {
    session_enabled.store(enable);
    session_force_dispatch.store(force_dispatch);
    session_skip_cpu_validation.store(enable && skip_cpu_validation);
    auto& r = runtime();
    std::lock_guard<std::mutex> lock(r.mutex);
    r.reset_stats();
}
bool CudaSlicerBackend::compiled_with_cuda() {
#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    return true;
#else
    return false;
#endif
}
bool CudaSlicerBackend::enabled() {
#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    return session_enabled.load();
#else
    return false;
#endif
}
bool CudaSlicerBackend::skips_cpu_validation() {
    return enabled() && session_skip_cpu_validation.load();
}
bool CudaSlicerBackend::should_dispatch(size_t count) {
    return enabled() && count > 0 && count <= maximum_requests &&
        (count >= minimum_requests || session_force_dispatch.load());
}
SlicingProfileGpuStats CudaSlicerBackend::runtime_stats() {
    auto& r = runtime();
    std::lock_guard<std::mutex> lock(r.mutex);
    SlicingProfileGpuStats stats = r.stats;
    const uint64_t busy = r.busy_fallbacks.load(std::memory_order_relaxed);
    const uint64_t deferred_preflights = r.deferred_preflight_calls.load(std::memory_order_relaxed);
    const double busy_ms = double(r.busy_decision_us.load(std::memory_order_relaxed)) / 1000.0;
    const double deferred_preflight_ms = double(r.deferred_preflight_us.load(std::memory_order_relaxed)) / 1000.0;
    stats.dispatch_calls += busy + deferred_preflights;
    stats.skipped_workloads += busy + deferred_preflights;
    stats.total_host_ms += busy_ms + deferred_preflight_ms;
    stats.preflight_ms += deferred_preflight_ms;
    if (busy > 0) stats.diagnostic_counts[busy_fallback_diagnostic] += busy;
    if (deferred_preflights > 0) stats.diagnostic_counts[deferred_preflight_diagnostic] += deferred_preflights;
    stats.fallback_ms += double(r.deferred_fallback_us.load(std::memory_order_relaxed)) / 1000.0;
    return stats;
}
void CudaSlicerBackend::note_cpu_fallback(double wall_ms) {
    auto& r = runtime();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        r.deferred_fallback_us.fetch_add(uint64_t(std::max(0.0, wall_ms) * 1000.0), std::memory_order_relaxed);
        return;
    }
    r.stats.fallback_ms += wall_ms;
    publish(r.stats);
}

bool CudaSlicerBackend::operation_enabled(const char* operation) {
    if (!enabled()) return false;
    const char* excluded = std::getenv("MAGPIE_CUDA_DISABLE_OPERATIONS");
    return !excluded || ("," + std::string(excluded) + ",").find("," + std::string(operation) + ",") == std::string::npos;
}

VulkanAabbBatch CudaSlicerBackend::dispatch_indexed_aabb_candidates(
    const std::vector<VulkanAabb>& queries, const std::vector<VulkanAabb>& targets,
    Coord cell_size, VulkanAabbOperation operation)
{
    static const char* names[] = {"spatial", "tree_support", "distance_field", "gyroid",
                                  "seam_travel", "classic_wall", "cura_support", "arachne_wall"};
    const auto op = static_cast<size_t>(operation);
    const char* name = op < sizeof(names) / sizeof(*names) ? names[op] : "spatial";
    VulkanAabbBatch batch;
    if (!operation_enabled(name)) { batch.diagnostic = "CUDA operation disabled"; return batch; }
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent timing("cuda", std::string("aabb_") + name, SlicingProfileBackend::CPU, queries.size());
#endif
    const auto wall_start = Clock::now();
    auto& r = runtime();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        r.busy_fallbacks.fetch_add(1, std::memory_order_relaxed);
        r.busy_decision_us.fetch_add(uint64_t(elapsed_ms(wall_start) * 1000.0), std::memory_order_relaxed);
        batch.diagnostic = busy_fallback_diagnostic;
#ifdef MAGPIE_SLICING_TIMING
        timing.set_result(SlicingProfileBackend::CPUFallback, -1.0, queries.size(), batch.diagnostic);
#endif
        return batch;
    }
    r.stats.queue_wait_ms += elapsed_ms(wall_start);
    ++r.stats.dispatch_calls;
    double gpu_ms = 0;
    const uint64_t submissions_before = r.stats.queue_submissions;
    auto finish = [&]() {
        r.stats.total_host_ms += elapsed_ms(wall_start);
        r.stats.last_diagnostic = batch.diagnostic;
        if (!batch.diagnostic.empty()) ++r.stats.diagnostic_counts[batch.diagnostic];
        batch.dispatched = r.stats.queue_submissions != submissions_before;
        if (!batch.resolved) { batch.may_overlap.clear(); batch.overlap_pairs.clear(); ++r.stats.skipped_workloads; }
#ifdef MAGPIE_SLICING_TIMING
        timing.set_result(batch.dispatched ? SlicingProfileBackend::CUDA : SlicingProfileBackend::CPU,
                          batch.dispatched ? gpu_ms : -1, batch.candidate_pairs, batch.diagnostic);
#endif
        publish(r.stats);
        return batch;
    };
#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    // Bounding the grid AND its references avoids an unbounded quadratic scan.
    constexpr size_t pair_limit = 1024 * 1024, reference_limit = 1024 * 1024;
    if (queries.size() > maximum_requests || targets.size() > maximum_requests) {
        batch.diagnostic = "CUDA AABB input capacity exceeded"; return finish();
    }
    const auto packing_start = Clock::now();
    bool packing_recorded = false;
    auto packing_done = [&]() {
        if (!packing_recorded) r.stats.packing_ms += elapsed_ms(packing_start);
        packing_recorded = true;
    };
    try {
        auto normalized = [](const VulkanAabb& box) {
            return VulkanAabb{{std::min(box.min.x, box.max.x), std::min(box.min.y, box.max.y)},
                              {std::max(box.min.x, box.max.x), std::max(box.min.y, box.max.y)}};
        };
        std::vector<VulkanAabb> qs, ts;
        qs.reserve(queries.size()); ts.reserve(targets.size());
        for (const auto& box : queries) qs.push_back(normalized(box));
        for (const auto& box : targets) ts.push_back(normalized(box));
        if (cell_size <= 0) {
            uint64_t span = 1;
            if (!ts.empty()) {
                auto bounds = ts.front();
                for (const auto& b : ts) {
                    bounds.min.x = std::min(bounds.min.x, b.min.x); bounds.min.y = std::min(bounds.min.y, b.min.y);
                    bounds.max.x = std::max(bounds.max.x, b.max.x); bounds.max.y = std::max(bounds.max.y, b.max.y);
                }
                // Unsigned subtraction represents the full signed-coordinate span.
                span = std::max(uint64_t(bounds.max.x) - uint64_t(bounds.min.x),
                                uint64_t(bounds.max.y) - uint64_t(bounds.min.y));
            }
            const uint64_t divisions = std::max<uint64_t>(1, uint64_t(std::sqrt(double(ts.size()))));
            const uint64_t width = span / divisions + (span % divisions != 0);
            cell_size = int64_t(std::max<uint64_t>(1, std::min<uint64_t>(width, INT64_MAX)));
        }
        auto floor_cell = [cell_size](int64_t value) {
            const int64_t q = value / cell_size;
            return q - (value % cell_size < 0 ? 1 : 0);
        };
        auto grid_bounds = [&](const VulkanAabb& b) {
            return VulkanAabb{{floor_cell(b.min.x), floor_cell(b.min.y)}, {floor_cell(b.max.x), floor_cell(b.max.y)}};
        };
        auto small_grid = [](const VulkanAabb& b) {
            const uint64_t dx = uint64_t(b.max.x) - uint64_t(b.min.x);
            const uint64_t dy = uint64_t(b.max.y) - uint64_t(b.min.y);
            return dx < 4096 && dy < 4096 && (dx + 1) * (dy + 1) <= 4096;
        };
        auto visit_cells = [](const VulkanAabb& b, auto&& visitor) {
            for (int64_t x = b.min.x;;) {
                for (int64_t y = b.min.y;;) {
                    visitor(x, y);
                    if (y == b.max.y) break;
                    ++y;
                }
                if (x == b.max.x) break;
                ++x;
            }
        };
        std::map<std::pair<int64_t, int64_t>, std::vector<uint32_t>> grid;
        std::vector<uint32_t> global;
        size_t references = 0, visited_references = 0;
        for (uint32_t i = 0; i < ts.size(); ++i) {
            const auto b = grid_bounds(ts[i]);
            if (!small_grid(b)) { global.push_back(i); continue; }
            visit_cells(b, [&](int64_t x, int64_t y) {
                if (++references > reference_limit) throw std::runtime_error("CUDA AABB grid reference cap; CPU fallback");
                grid[{x, y}].push_back(i);
            });
        }
        std::vector<MagpieCudaAabbInput> records;
        std::vector<uint32_t> candidates;
        for (uint32_t q = 0; q < qs.size(); ++q) {
            candidates = global;
            const auto b = grid_bounds(qs[q]);
            if (!small_grid(b)) {
                candidates.resize(ts.size());
                std::iota(candidates.begin(), candidates.end(), uint32_t(0));
            } else {
                visit_cells(b, [&](int64_t x, int64_t y) {
                    const auto found = grid.find({x, y});
                    if (found == grid.end()) return;
                    visited_references += found->second.size();
                    if (visited_references > reference_limit * 4)
                        throw std::runtime_error("CUDA AABB candidate scan cap; CPU fallback");
                    candidates.insert(candidates.end(), found->second.begin(), found->second.end());
                });
                std::sort(candidates.begin(), candidates.end());
                candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
            }
            if (candidates.size() > pair_limit - records.size())
                throw std::runtime_error("CUDA AABB candidate pair cap; CPU fallback");
            for (uint32_t t : candidates) {
                const auto& a = qs[q]; const auto& b = ts[t];
                records.push_back({a.min.x, a.min.y, a.max.x, a.max.y, b.min.x, b.min.y, b.max.x, b.max.y,
                                   (uint64_t(q) << 32) | t});
            }
        }
        batch.candidate_pairs = records.size();
        packing_done();
        if (!records.empty() && !should_dispatch(std::min(records.size(), maximum_requests))) {
            batch.diagnostic = "CUDA AABB batch below experimental dispatch floor"; return finish();
        }
        batch.may_overlap.assign(qs.size(), 0);
        for (size_t offset = 0; offset < records.size(); offset += maximum_requests) {
            const size_t count = std::min(maximum_requests, records.size() - offset);
            gpu_ms += r.execute("magpie_aabb_overlap", records.data() + offset, count,
                                sizeof(MagpieCudaAabbInput), sizeof(MagpieCudaPredicateOutput));
            const auto validation_start = Clock::now();
            const auto* out = static_cast<const MagpieCudaPredicateOutput*>(r.host_output);
            bool valid = true;
            for (size_t i = 0; i < count; ++i) {
                const auto& v = records[offset + i];
                valid &= out[i].stable_id == v.stable_id && out[i].value <= 1;
                if (!session_skip_cpu_validation.load()) {
                    const bool expected = !(v.qmaxx < v.tminx || v.tmaxx < v.qminx ||
                                            v.qmaxy < v.tminy || v.tmaxy < v.qminy);
                    ++r.stats.cpu_validation_checks;
                    valid &= out[i].value == uint64_t(expected);
                }
                if (out[i].value == 1) {
                    const auto q = uint32_t(v.stable_id >> 32), t = uint32_t(v.stable_id);
                    batch.may_overlap[q] = 1;
                    batch.overlap_pairs.push_back({q, t});
                }
            }
            r.stats.validation_ms += elapsed_ms(validation_start);
            if (!valid) {
                ++r.stats.validation_failures;
                batch.diagnostic = "CUDA AABB result rejected; discard whole operation"; return finish();
            }
        }
        // Stable query/target order follows grid compaction, never GPU scheduling.
        batch.resolved = true;
        r.stats.accepted_gpu_items += records.size();
        batch.diagnostic = records.empty() ? "CPU grid proves no AABB candidates" :
            (session_skip_cpu_validation.load() ? "CUDA AABB candidates accepted without CPU duplication" :
                                                  "CUDA exact AABB candidates accepted");
    } catch (const std::exception& error) {
        packing_done();
        batch.diagnostic = error.what();
    }
#endif
    return finish();
}

namespace {
void record_preflight(const char* operation, size_t count, Clock::time_point started, const std::string& reason)
{
    if (!CudaSlicerBackend::enabled()) return;
    const double preflight_ms = elapsed_ms(started);
    auto& r = runtime();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
#ifdef MAGPIE_SLICING_TIMING
    // The enclosing request event carries duration; this zero-work decision
    // annotates why there is no device dispatch without inventing GPU time.
    ScopedSlicingProfileEvent decision("cuda-decision", operation, SlicingProfileBackend::CPUFallback, count);
    decision.set_result(SlicingProfileBackend::CPUFallback, -1.0, count, reason);
#endif
    if (!lock.owns_lock()) {
        r.deferred_preflight_calls.fetch_add(1, std::memory_order_relaxed);
        r.deferred_preflight_us.fetch_add(uint64_t(preflight_ms * 1000.0), std::memory_order_relaxed);
        return;
    }
    ++r.stats.dispatch_calls;
    ++r.stats.skipped_workloads;
    r.stats.preflight_ms += preflight_ms;
    r.stats.total_host_ms += preflight_ms;
    r.stats.last_diagnostic = reason;
    ++r.stats.diagnostic_counts[reason];
    publish(r.stats);
}

template<class Output, class Equal>
bool validated_packed_dispatch(const char* kernel, const char* operation, const std::vector<uint8_t>& packed,
                              size_t input_stride, const std::vector<Output>& expected, Equal equal,
                              double packing_ms, std::vector<Output>& result, std::string& diagnostic)
{
    const auto wall_start = Clock::now();
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent timing("cuda", operation, SlicingProfileBackend::CPU, expected.size());
#endif
    auto& r = runtime();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        r.busy_fallbacks.fetch_add(1, std::memory_order_relaxed);
        r.busy_decision_us.fetch_add(uint64_t(elapsed_ms(wall_start) * 1000.0), std::memory_order_relaxed);
        result.clear();
        diagnostic = busy_fallback_diagnostic;
#ifdef MAGPIE_SLICING_TIMING
        timing.set_result(SlicingProfileBackend::CPUFallback, -1.0, expected.size(), diagnostic);
#endif
        return false;
    }
    r.stats.queue_wait_ms += elapsed_ms(wall_start);
    ++r.stats.dispatch_calls;
    r.stats.packing_ms += packing_ms;
    bool resolved = false;
#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    const auto before = r.stats.queue_submissions;
    double gpu_ms = 0;
    try {
        gpu_ms = r.execute(kernel, packed.data(), expected.size(), input_stride, sizeof(Output), packed.size());
        const auto* output = static_cast<const Output*>(r.host_output);
        const auto validation_start = Clock::now();
        bool valid = true;
        if (!session_skip_cpu_validation.load()) {
            for (size_t i = 0; i < expected.size(); ++i) {
                ++r.stats.cpu_validation_checks;
                valid &= equal(output[i], expected[i]);
            }
            r.stats.validation_ms += elapsed_ms(validation_start);
        }
        if (valid) {
            result.assign(output, output + expected.size());
            r.stats.accepted_gpu_items += expected.size();
            resolved = true;
            diagnostic = session_skip_cpu_validation.load() ?
                "CUDA batch accepted without CPU duplication" : "CUDA full batch validated";
        } else {
            ++r.stats.validation_failures;
            diagnostic = "CUDA validation failed; discard whole batch";
        }
    } catch (const std::exception& error) { diagnostic = error.what(); }
#ifdef MAGPIE_SLICING_TIMING
    timing.set_result(r.stats.queue_submissions != before ? SlicingProfileBackend::CUDA : SlicingProfileBackend::CPU,
                      r.stats.queue_submissions != before ? gpu_ms : -1, expected.size(), diagnostic);
#endif
#endif
    if (!resolved) { result.clear(); ++r.stats.skipped_workloads; }
    r.stats.total_host_ms += elapsed_ms(wall_start) + packing_ms;
    r.stats.last_diagnostic = diagnostic;
    if (!diagnostic.empty()) ++r.stats.diagnostic_counts[diagnostic];
    publish(r.stats);
    return resolved;
}
}

CudaContainmentBatch CudaSlicerBackend::dispatch_points_in_polygons(
    const std::vector<Point>& points, const std::vector<CudaPolygonContours>& polygons,
    const std::vector<uint8_t>& cpu_reference)
{
    CudaContainmentBatch batch;
    const auto request_start = Clock::now();
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent request_timing("cuda-request", "point_in_polygon", SlicingProfileBackend::System, points.size());
#endif
    auto finish_preflight = [&]() {
        if (batch.diagnostic.empty()) batch.diagnostic = "CUDA empty geometry: resolved on CPU";
        record_preflight("point_in_polygon", points.size(), request_start, batch.diagnostic);
        return batch;
    };
    if (!operation_enabled("point_in_polygon") || !should_dispatch(points.size()) ||
        (!skips_cpu_validation() && cpu_reference.size() != points.size())) {
        batch.diagnostic = "CUDA containment disabled, small or inconsistent batch"; return finish_preflight();
    }
    const auto packing_start = Clock::now();
    constexpr int64_t limit = int64_t(1) << 29;
    auto fits = [limit](const Point& p) { return p.x >= -limit && p.x <= limit && p.y >= -limit && p.y <= limit; };
    for (const auto& p : points) if (!fits(p)) { batch.diagnostic = "CUDA containment coordinate preflight"; return finish_preflight(); }
    std::vector<MagpieCudaPolygonEdge> edges;
    for (size_t poly = 0; poly < polygons.size(); ++poly) {
        if (poly > UINT32_MAX) { batch.diagnostic = "CUDA polygon id overflow"; return finish_preflight(); }
        const auto& rings = polygons[poly].rings;
        if (rings.empty()) { batch.diagnostic = "CUDA polygon has no outer ring"; return finish_preflight(); }
        for (size_t ring_idx = 0; ring_idx < rings.size(); ++ring_idx) {
            const auto& ring = rings[ring_idx];
            if (ring.size() < 3 || ring.size() > maximum_requests - edges.size()) {
                batch.diagnostic = "CUDA polygon degenerate or exceeds edge capacity"; return finish_preflight();
            }
            for (size_t i = 0; i < ring.size(); ++i) {
                const auto& a = ring[i]; const auto& b = ring[(i+1)%ring.size()];
                if (!fits(a) || !fits(b)) { batch.diagnostic = "CUDA polygon coordinate preflight"; return finish_preflight(); }
                edges.push_back({a.x,a.y,b.x,b.y,uint32_t(poly),uint32_t((i+1 == ring.size() ? 1 : 0) | (ring_idx == 0 ? 2 : 0)),0});
            }
        }
    }
    if (edges.empty()) { batch.resolved = true; batch.inside.assign(points.size(),0); return finish_preflight(); }
    if (points.size() > (64 * 1024 * 1024) / edges.size()) {
        batch.diagnostic = "CUDA containment work capacity"; return finish_preflight();
    }
    std::vector<uint8_t> packed(points.size()*sizeof(MagpieCudaDistancePoint)+edges.size()*sizeof(MagpieCudaPolygonEdge));
    std::vector<MagpieCudaPredicateOutput> expected;
    expected.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        if (!skips_cpu_validation() && cpu_reference[i] > 1) { batch.diagnostic = "CUDA containment reference invalid"; return finish_preflight(); }
        const MagpieCudaDistancePoint v {points[i].x,points[i].y,uint32_t(edges.size()),0,uint64_t(i)};
        std::memcpy(packed.data()+i*sizeof(v),&v,sizeof(v));
        expected.push_back({uint64_t(i),skips_cpu_validation() ? 0u : cpu_reference[i]});
    }
    std::memcpy(packed.data()+points.size()*sizeof(MagpieCudaDistancePoint),edges.data(),edges.size()*sizeof(MagpieCudaPolygonEdge));
    std::vector<MagpieCudaPredicateOutput> out;
    batch.resolved = validated_packed_dispatch("magpie_points_in_polygons", "point_in_polygon", packed,
        sizeof(MagpieCudaDistancePoint), expected, [](const auto& a, const auto& b) {
            return a.stable_id == b.stable_id && a.value == b.value;
        }, elapsed_ms(packing_start), out, batch.diagnostic);
    if (batch.resolved) {
        for (size_t i = 0; i < out.size(); ++i) {
            if (out[i].stable_id != i || out[i].value > 1) {
                batch.resolved = false; batch.inside.clear();
                batch.diagnostic = "CUDA containment result failed structural checks";
                break;
            }
            batch.inside.push_back(uint8_t(out[i].value));
        }
    }
    return batch;
}

VulkanAabbBatch CudaSlicerBackend::dispatch_aabb_pairs(
    const std::vector<VulkanAabb>& queries, const std::vector<VulkanAabb>& targets)
{
    VulkanAabbBatch batch;
    const auto request_start = Clock::now();
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent request_timing("cuda-request", "aabb_cura_support", SlicingProfileBackend::System, queries.size());
#endif
    auto finish_preflight = [&]() {
        if (batch.diagnostic.empty()) batch.diagnostic = "CUDA empty geometry: resolved on CPU";
        record_preflight("aabb_cura_support", queries.size(), request_start, batch.diagnostic);
        return batch;
    };
    if (!operation_enabled("cura_support") || !should_dispatch(queries.size()) || queries.size() != targets.size()) {
        batch.diagnostic = "CUDA paired AABB disabled, small or inconsistent batch"; return finish_preflight();
    }
    const auto packing_start = Clock::now();
    std::vector<uint8_t> packed(queries.size()*sizeof(MagpieCudaAabbInput));
    std::vector<MagpieCudaPredicateOutput> expected;
    for (size_t i = 0; i < queries.size(); ++i) {
        const auto& q = queries[i]; const auto& t = targets[i];
        const MagpieCudaAabbInput v {std::min(q.min.x,q.max.x),std::min(q.min.y,q.max.y),std::max(q.min.x,q.max.x),std::max(q.min.y,q.max.y),
            std::min(t.min.x,t.max.x),std::min(t.min.y,t.max.y),std::max(t.min.x,t.max.x),std::max(t.min.y,t.max.y),uint64_t(i)};
        std::memcpy(packed.data()+i*sizeof(v),&v,sizeof(v));
        expected.push_back({uint64_t(i),uint64_t(!(v.qmaxx < v.tminx || v.tmaxx < v.qminx || v.qmaxy < v.tminy || v.tmaxy < v.qminy))});
    }
    std::vector<MagpieCudaPredicateOutput> out;
    batch.candidate_pairs = queries.size();
    batch.resolved = validated_packed_dispatch("magpie_aabb_overlap", "aabb_cura_support", packed,
        sizeof(MagpieCudaAabbInput), expected, [](const auto& a, const auto& b) {
            return a.stable_id == b.stable_id && a.value == b.value;
        }, elapsed_ms(packing_start), out, batch.diagnostic);
    batch.dispatched = batch.resolved;
    if (batch.resolved) for (const auto& v : out) batch.may_overlap.push_back(uint8_t(v.value));
    return batch;
}

CudaDistanceBatch CudaSlicerBackend::dispatch_point_segment_distances(
    const std::vector<Point>& points, const std::vector<Segment>& edges,
    const std::vector<double>& cpu_reference)
{
    CudaDistanceBatch batch;
    const auto request_start = Clock::now();
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent request_timing("cuda-request", "point_segment_distance", SlicingProfileBackend::System, points.size());
#endif
    auto finish_preflight = [&]() {
        if (batch.diagnostic.empty()) batch.diagnostic = "CUDA empty geometry: resolved on CPU";
        record_preflight("point_segment_distance", points.size(), request_start, batch.diagnostic);
        return batch;
    };
    if (!operation_enabled("point_segment_distance") || !should_dispatch(points.size())) {
        batch.diagnostic = "CUDA distances disabled or below dispatch floor"; return finish_preflight();
    }
    if (edges.empty() || edges.size() > maximum_requests ||
        (!skips_cpu_validation() && points.size() != cpu_reference.size()) ||
        points.size() > (64 * 1024 * 1024) / edges.size()) {
        batch.diagnostic = "CUDA distance workload outside bounded capacity"; return finish_preflight();
    }
    const auto packing_start = Clock::now();
    // These are fixed-point integers: no subtraction overflow or lossy integer
    // conversion is admitted. All double results still require the caller's
    // established CPU distance algorithm as an exact reference.
    constexpr int64_t limit = int64_t(1) << 50;
    auto fits = [limit](const Point& p) { return p.x >= -limit && p.x <= limit && p.y >= -limit && p.y <= limit; };
    for (const auto& p : points) if (!fits(p)) { batch.diagnostic = "CUDA distance coordinate preflight"; return finish_preflight(); }
    for (const auto& e : edges) if (!fits(e.a) || !fits(e.b)) { batch.diagnostic = "CUDA distance coordinate preflight"; return finish_preflight(); }
    for (double d : cpu_reference) if (!std::isfinite(d) || d < 0) {
        batch.diagnostic = "CUDA distance reference invalid"; return finish_preflight();
    }
    std::vector<uint8_t> packed(points.size()*sizeof(MagpieCudaDistancePoint) + edges.size()*sizeof(MagpieCudaDistanceEdge));
    std::vector<MagpieCudaDistanceOutput> expected;
    expected.reserve(points.size());
    for (size_t i = 0; i < points.size(); ++i) {
        const MagpieCudaDistancePoint p {points[i].x, points[i].y, uint32_t(edges.size()), 0, uint64_t(i)};
        std::memcpy(packed.data()+i*sizeof(p), &p, sizeof(p));
        expected.push_back({skips_cpu_validation() ? 0.0 : cpu_reference[i],uint64_t(i)});
    }
    for (size_t i = 0; i < edges.size(); ++i) {
        const auto& e = edges[i]; const MagpieCudaDistanceEdge v {e.a.x,e.a.y,e.b.x,e.b.y};
        std::memcpy(packed.data()+points.size()*sizeof(MagpieCudaDistancePoint)+i*sizeof(v), &v, sizeof(v));
    }
    std::vector<MagpieCudaDistanceOutput> output;
    batch.resolved = validated_packed_dispatch("magpie_point_segment_distances", "point_segment_distance", packed,
        sizeof(MagpieCudaDistancePoint), expected, [](const auto& a, const auto& b) {
            return a.stable_id == b.stable_id && a.distance_squared == b.distance_squared;
        }, elapsed_ms(packing_start), output, batch.diagnostic);
    if (batch.resolved) {
        batch.distances_squared.reserve(output.size());
        for (size_t i = 0; i < output.size(); ++i) {
            const auto& v = output[i];
            if (v.stable_id != i || !std::isfinite(v.distance_squared) || v.distance_squared < 0) {
                batch.resolved = false; batch.distances_squared.clear();
                batch.diagnostic = "CUDA distance result failed structural checks";
                break;
            }
            batch.distances_squared.push_back(v.distance_squared);
        }
    }
    return batch;
}

CudaMeshZBatch CudaSlicerBackend::dispatch_mesh_layer_ranges(
    const std::vector<CudaMeshZRequest>& facets, const std::vector<float>& zs)
{
    CudaMeshZBatch batch;
    const auto request_start = Clock::now();
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent request_timing("cuda-request", "mesh_layer_ranges", SlicingProfileBackend::System, facets.size());
#endif
    auto finish_preflight = [&]() {
        if (batch.diagnostic.empty()) batch.diagnostic = "CUDA empty geometry: resolved on CPU";
        record_preflight("mesh_layer_ranges", facets.size(), request_start, batch.diagnostic);
        return batch;
    };
    if (!operation_enabled("mesh_layer_ranges") || !should_dispatch(facets.size())) {
        batch.diagnostic = "CUDA mesh ranges disabled or below dispatch floor"; return finish_preflight();
    }
    if (zs.empty() || zs.size() > maximum_requests || !std::is_sorted(zs.begin(),zs.end())) {
        batch.diagnostic = "CUDA layer range input invalid"; return finish_preflight();
    }
    for (float z : zs) if (!std::isfinite(z)) { batch.diagnostic = "CUDA layer nonfinite"; return finish_preflight(); }
    const auto packing_start = Clock::now();
    std::vector<uint8_t> packed(facets.size()*sizeof(MagpieCudaMeshZInput)+zs.size()*sizeof(float));
    std::vector<MagpieCudaMeshZOutput> expected;
    expected.reserve(facets.size());
    for (size_t i = 0; i < facets.size(); ++i) {
        const auto& f = facets[i];
        if (!std::isfinite(f.z0) || !std::isfinite(f.z1) || !std::isfinite(f.z2)) {
            batch.diagnostic = "CUDA mesh nonfinite"; return finish_preflight();
        }
        const float lo = std::fmin(f.z0,std::fmin(f.z1,f.z2)), hi = std::fmax(f.z0,std::fmax(f.z1,f.z2));
        if (skips_cpu_validation()) {
            expected.push_back({0, 0, 0, uint32_t(i)});
        } else {
            const auto first = std::lower_bound(zs.begin(),zs.end(),lo);
            const auto last = std::upper_bound(first,zs.end(),hi);
            expected.push_back({uint32_t(first-zs.begin()),uint32_t(last-zs.begin()), f.z1 == lo ? 1u : (f.z2 == lo ? 2u : 0u), uint32_t(i)});
        }
        const MagpieCudaMeshZInput v {f.z0,f.z1,f.z2,uint32_t(zs.size())};
        std::memcpy(packed.data()+i*sizeof(v),&v,sizeof(v));
    }
    std::memcpy(packed.data()+facets.size()*sizeof(MagpieCudaMeshZInput),zs.data(),zs.size()*sizeof(float));
    std::vector<MagpieCudaMeshZOutput> output;
    batch.resolved = validated_packed_dispatch("magpie_mesh_layer_ranges", "mesh_layer_ranges", packed,
        sizeof(MagpieCudaMeshZInput), expected, [](const auto& a, const auto& b) {
            return a.first == b.first && a.last == b.last && a.lowest_vertex == b.lowest_vertex && a.stable_id == b.stable_id;
        }, elapsed_ms(packing_start), output, batch.diagnostic);
    if (batch.resolved) {
        for (size_t i = 0; i < output.size(); ++i) {
            const auto& v = output[i];
            if (v.stable_id != i || v.first > v.last || v.last > zs.size() || v.lowest_vertex > 2) {
                batch.resolved = false; batch.ranges.clear();
                batch.diagnostic = "CUDA mesh range result failed structural checks";
                break;
            }
            batch.ranges.push_back({v.first,v.last,v.lowest_vertex});
        }
    }
    return batch;
}

VulkanVerticalIntersectionBatch CudaSlicerBackend::dispatch_vertical_intersections(
    const std::vector<VulkanVerticalIntersectionRequest>& requests)
{
    VulkanVerticalIntersectionBatch batch;
    if (!operation_enabled("intersections")) { batch.diagnostic = "CUDA intersections disabled"; return batch; }
#ifdef MAGPIE_SLICING_TIMING
    ScopedSlicingProfileEvent timing("cuda", "vertical_intersections", SlicingProfileBackend::CPU, requests.size());
#endif
    const auto wall_start = Clock::now();
    auto& r = runtime();
    std::unique_lock<std::mutex> lock(r.mutex, std::try_to_lock);
    if (!lock.owns_lock()) {
        r.busy_fallbacks.fetch_add(1, std::memory_order_relaxed);
        r.busy_decision_us.fetch_add(uint64_t(elapsed_ms(wall_start) * 1000.0), std::memory_order_relaxed);
        batch.diagnostic = busy_fallback_diagnostic;
#ifdef MAGPIE_SLICING_TIMING
        timing.set_result(SlicingProfileBackend::CPUFallback, -1.0, requests.size(), batch.diagnostic);
#endif
        return batch;
    }
    r.stats.queue_wait_ms += elapsed_ms(wall_start);
    ++r.stats.dispatch_calls;
    auto finish = [&]() {
        batch.host_elapsed_ms = elapsed_ms(wall_start);
#ifdef MAGPIE_SLICING_TIMING
        timing.set_result(batch.queue_submissions ? SlicingProfileBackend::CUDA : SlicingProfileBackend::CPU,
                          batch.queue_submissions ? batch.gpu_elapsed_ms : -1, requests.size(), batch.diagnostic);
#endif
        r.stats.total_host_ms += batch.host_elapsed_ms;
        r.stats.last_diagnostic = batch.diagnostic;
        if (!batch.diagnostic.empty()) ++r.stats.diagnostic_counts[batch.diagnostic];
        publish(r.stats);
        return batch;
    };
    if (!should_dispatch(requests.size())) {
        ++r.stats.skipped_workloads;
        batch.diagnostic = requests.empty() || requests.size() > maximum_requests ?
            "CUDA batch count outside bounded capacity" :
            "CUDA intersections below experimental dispatch floor";
        return finish();
    }
#if defined(SLIC3R_ENABLE_CUDA_SLICER) && defined(_WIN32)
    // cpp_int avoids overflow even for adversarial full-range int64 coordinates.
    using Wide = boost::multiprecision::cpp_int;
    const Wide low = std::numeric_limits<int64_t>::min(), high = std::numeric_limits<int64_t>::max();
    auto fits = [&](const Wide& v) { return v >= low && v <= high; };
    const auto packing_start = Clock::now();
    std::vector<MagpieCudaIntersectionInput> input;
    std::vector<MagpieCudaIntersectionOutput> expected;
    input.reserve(requests.size()); expected.reserve(requests.size());
    for (const auto& q : requests) {
        const Wide ax = q.segment.a.x, ay = q.segment.a.y, bx = q.segment.b.x, by = q.segment.b.y;
        const Wide dx = bx - ax, dy = by - ay;
        const Wide denominator = dx > 0 ? dx : -dx;
        const Wide t = dx > 0 ? Wide(q.scan_x) - ax : ax - Wide(q.scan_x);
        const Wide first = t * dy, second = ay * denominator, numerator = first + second;
        if (denominator <= 0 || t <= 0 || t >= denominator || !fits(denominator) ||
            !fits(t) || !fits(dy) || !fits(first) || !fits(second) || !fits(numerator)) {
            r.stats.packing_ms += elapsed_ms(packing_start);
            ++r.stats.skipped_workloads;
            batch.diagnostic = "CUDA preflight rejected non-interior or overflowing arithmetic; discard whole batch";
            return finish();
        }
        input.push_back({q.segment.a.x, q.segment.a.y, q.segment.b.x, q.segment.b.y, q.scan_x, q.stable_id});
        if (!session_skip_cpu_validation.load())
            expected.push_back({static_cast<int64_t>(numerator), static_cast<int64_t>(denominator), q.stable_id, 1});
    }
    r.stats.packing_ms += elapsed_ms(packing_start);
    try {
        const uint64_t submitted_before = r.stats.queue_submissions;
        batch.gpu_elapsed_ms = r.execute("magpie_vertical_intersections", input.data(), requests.size(),
            sizeof(MagpieCudaIntersectionInput), sizeof(MagpieCudaIntersectionOutput));
        batch.queue_submissions = uint32_t(r.stats.queue_submissions - submitted_before);
        const auto* output = static_cast<const MagpieCudaIntersectionOutput*>(r.host_output);
        auto phase = Clock::now();
            bool valid = true;
            size_t first_invalid = requests.size();
            for (size_t i = 0; i < requests.size(); ++i) {
                bool item_valid = output[i].valid == 1 && output[i].stable_id == requests[i].stable_id &&
                    output[i].denominator > 0;
                if (!session_skip_cpu_validation.load()) {
                    ++r.stats.cpu_validation_checks;
                    item_valid &= output[i].numerator == expected[i].numerator &&
                                  output[i].denominator == expected[i].denominator;
                }
                if (!item_valid && first_invalid == requests.size()) first_invalid = i;
                valid &= item_valid;
            }
            if (!session_skip_cpu_validation.load())
                r.stats.validation_ms += elapsed_ms(phase);
            if (!valid) {
                ++r.stats.validation_failures;
                batch.diagnostic = "CUDA result checks failed; discard whole batch; index=" +
                    std::to_string(first_invalid);
            } else {
                batch.intersections.reserve(requests.size());
                for (size_t i = 0; i < requests.size(); ++i) {
                    const auto& value = output[i];
                    batch.intersections.push_back({value.numerator, value.denominator, value.stable_id, true});
                }
                batch.dispatched = true;
                r.stats.accepted_gpu_items += requests.size();
                batch.diagnostic = session_skip_cpu_validation.load() ?
                    "CUDA batch accepted without CPU duplication" : "CUDA exact batch accepted";
            }

    } catch (const std::exception& error) {
        r.unavailable = true;
        batch.intersections.clear(); batch.dispatched = false;
        batch.diagnostic = error.what();
    }
#endif
    return finish();
}
}
