#include "VulkanSlicer.hpp"
#ifdef MAGPIE_SLICING_PROFILER
#include "../SlicingProfiler.hpp"
#endif

#include "Utils.hpp"
#include "nlohmann/json.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>
#include <utility>

#include <boost/filesystem.hpp>

#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
#include <intrin.h>
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
#include <cpuid.h>
#endif

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#include <vulkan/vulkan.h>
#endif

namespace Slic3r::Gpu {

namespace {

constexpr uint32_t kDefaultWorkgroupSize = 128;
constexpr size_t   kNvidiaPascalStagingRequestCapacity = 16 * 1024;
constexpr size_t   kNvidiaRtxStagingRequestCapacity = 64 * 1024;
constexpr size_t   kGenericStagingRequestCapacity = 4 * 1024;
// Commonly sized host-visible buffers stay resident between slices. Oversized
// buffers are trimmed at the slice boundary so repeated interactive slicing
// avoids allocation churn without retaining an unbounded amount of VRAM.
constexpr size_t   kMaximumReusableStagingRequestCapacity = 256 * 1024;
constexpr size_t   kRetainedIntersectionRequestCapacity = 64 * 1024;
constexpr size_t   kRetainedTreeRequestCapacity = 32 * 1024;
constexpr size_t   kRetainedTreeEdgeCapacity = 128 * 1024;
constexpr uint64_t kDispatchFenceTimeoutNs = 10'000'000'000ULL;
constexpr auto     kInitializationRetryDelay = std::chrono::seconds(5);
constexpr int      kDispatchPolicyCacheSchema = 1;

// The GUI changes this flag through VulkanSlicerBackend. Keeping it here
// avoids coupling the slicing engine to GUI/AppConfig headers.
bool automated_verification_flag(const char* name)
{
    const char* value = std::getenv(name);
    return value != nullptr && std::strcmp(value, "1") == 0;
}

std::string trim_hardware_name(std::string value)
{
    const auto first = value.find_first_not_of(" \t\r\n\0", 0);
    if (first == std::string::npos)
        return {};
    const auto last = value.find_last_not_of(" \t\r\n\0");
    value = value.substr(first, last - first + 1);
    std::string compact;
    compact.reserve(value.size());
    bool previous_space = false;
    for (const unsigned char character : value) {
        const bool space = std::isspace(character) != 0;
        if (!space || !previous_space)
            compact.push_back(space ? ' ' : char(character));
        previous_space = space;
    }
    return compact;
}

std::string cpu_identifier()
{
    char brand[49] {};
#if defined(_MSC_VER) && (defined(_M_IX86) || defined(_M_X64))
    int registers[4] {};
    __cpuid(registers, 0x80000000);
    const unsigned int maximum_leaf = static_cast<unsigned int>(registers[0]);
    if (maximum_leaf >= 0x80000004) {
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            __cpuid(registers, int(0x80000002 + leaf));
            std::memcpy(brand + leaf * 16, registers, 16);
        }
    }
#elif defined(__GNUC__) && (defined(__i386__) || defined(__x86_64__))
    const unsigned int maximum_leaf = __get_cpuid_max(0x80000000, nullptr);
    if (maximum_leaf >= 0x80000004) {
        for (unsigned int leaf = 0; leaf < 3; ++leaf) {
            unsigned int eax = 0, ebx = 0, ecx = 0, edx = 0;
            __cpuid(0x80000002 + leaf, eax, ebx, ecx, edx);
            std::memcpy(brand + leaf * 16, &eax, 4);
            std::memcpy(brand + leaf * 16 + 4, &ebx, 4);
            std::memcpy(brand + leaf * 16 + 8, &ecx, 4);
            std::memcpy(brand + leaf * 16 + 12, &edx, 4);
        }
    }
#endif
    std::string identifier = trim_hardware_name(brand);
    if (identifier.empty()) {
        if (const char* environment_identifier = std::getenv("PROCESSOR_IDENTIFIER"))
            identifier = trim_hardware_name(environment_identifier);
    }
    return identifier.empty() ? "Unknown CPU" : identifier;
}

class AutomatedVerificationDispatchRecorder
{
public:
    void record(const char* operation, size_t request_count)
    {
        const char* path = std::getenv("MAGPIE_VULKAN_SLICER_DIAGNOSTICS_FILE");
        if (path == nullptr || path[0] == '\0')
            return;

        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_path != path) {
            m_output.close();
            m_path = path;
            m_output.open(m_path, std::ios::app);
        }
        if (m_output)
            m_output << operation << ',' << request_count << '\n';
    }

private:
    std::mutex    m_mutex;
    std::string   m_path;
    std::ofstream m_output;
};

AutomatedVerificationDispatchRecorder& automated_verification_dispatch_recorder()
{
    static AutomatedVerificationDispatchRecorder recorder;
    return recorder;
}

void record_automated_verification_dispatch(const char* operation, size_t request_count)
{
    automated_verification_dispatch_recorder().record(operation, request_count);
}

std::atomic_bool g_compute_enabled {
    automated_verification_flag("MAGPIE_VULKAN_SLICER_ENABLE")
};
std::atomic<VulkanSlicerComputeMode> g_compute_mode {
    automated_verification_flag("MAGPIE_VULKAN_SLICER_MAXIMUM") ? VulkanSlicerComputeMode::Maximum :
    (automated_verification_flag("MAGPIE_VULKAN_SLICER_GPU_PRIORITY") ? VulkanSlicerComputeMode::Priority :
                                                                       VulkanSlicerComputeMode::Balanced)
};
const bool g_force_dispatch_for_automated_verification =
    automated_verification_flag("MAGPIE_VULKAN_SLICER_FORCE_DISPATCH");

class RuntimeStatsRegistry {
public:
    void set_backend(std::string device, std::string profile, std::string diagnostic,
                     uint32_t configured_workgroup_size, uint32_t maximum_workgroup_size,
                     size_t reusable_staging_capacity)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.selected_device = std::move(device);
        m_stats.execution_profile = std::move(profile);
        m_stats.last_diagnostic = std::move(diagnostic);
        m_stats.configured_workgroup_size = configured_workgroup_size;
        m_stats.maximum_workgroup_size = maximum_workgroup_size;
        m_stats.reusable_staging_capacity = reusable_staging_capacity;
        m_stats.current_operation = m_stats.selected_device.empty() ?
            "Vulkan unavailable" : "Ready for infill/support scan conversion";
    }

    void set_preferred_intersection_batch(size_t request_count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.preferred_intersection_batch = request_count;
    }

    void begin_slice()
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.dispatch_calls               = 0;
        m_stats.queue_submissions            = 0;
        m_stats.submitted_intersections      = 0;
        m_stats.accepted_gpu_intersections   = 0;
        m_stats.cpu_validation_checks        = 0;
        m_stats.validation_failures          = 0;
        m_stats.skipped_small_workloads      = 0;
        m_stats.skipped_intersections        = 0;
        m_stats.smallest_submitted_intersection_batch = 0;
        m_stats.largest_submitted_intersection_batch  = 0;
        m_stats.largest_skipped_intersection_batch    = 0;
        m_stats.last_gpu_ms                  = 0.0;
        m_stats.last_host_ms                 = 0.0;
        m_stats.total_gpu_ms                 = 0.0;
        m_stats.total_host_ms                = 0.0;
        m_stats.current_operation = m_stats.selected_device.empty() ?
            "Vulkan not initialized for this slice" :
            "No GPU batch has been submitted for this slice";
        m_stats.last_diagnostic = "GPU work counters reset for a new slicing session.";
    }

    void record_dispatch(size_t request_count, const VulkanVerticalIntersectionBatch& batch)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.dispatch_calls;
        m_stats.queue_submissions += batch.queue_submissions;
        m_stats.submitted_intersections += request_count;
        if (m_stats.smallest_submitted_intersection_batch == 0)
            m_stats.smallest_submitted_intersection_batch = request_count;
        else
            m_stats.smallest_submitted_intersection_batch =
                std::min(m_stats.smallest_submitted_intersection_batch, request_count);
        m_stats.largest_submitted_intersection_batch =
            std::max(m_stats.largest_submitted_intersection_batch, request_count);
        m_stats.last_gpu_ms = batch.gpu_elapsed_ms;
        m_stats.last_host_ms = batch.host_elapsed_ms;
        if (batch.gpu_elapsed_ms >= 0.0)
            m_stats.total_gpu_ms += batch.gpu_elapsed_ms;
        m_stats.total_host_ms += batch.host_elapsed_ms;
        m_stats.current_operation = batch.dispatched ?
            "Exact infill/support edge intersections" : "CPU fallback after Vulkan dispatch failure";
        m_stats.last_diagnostic = batch.diagnostic;
    }

    void record_skipped(size_t request_count)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.skipped_small_workloads;
        m_stats.skipped_intersections += request_count;
        m_stats.largest_skipped_intersection_batch =
            std::max(m_stats.largest_skipped_intersection_batch, request_count);
        m_stats.current_operation = "CPU fallback for a small intersection batch";
        m_stats.last_diagnostic = "Vulkan skipped a small vertical-intersection workload.";
    }

    void record_usage(size_t accepted_gpu_results, size_t cpu_validation_checks,
                      bool validation_failed, const std::string& diagnostic)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stats.accepted_gpu_intersections += accepted_gpu_results;
        m_stats.cpu_validation_checks += cpu_validation_checks;
        if (validation_failed)
            ++m_stats.validation_failures;
        m_stats.last_diagnostic = diagnostic;
    }

    void record_tree_contour_broad_phase(size_t request_count, size_t edge_count,
                                         const std::string& diagnostic)
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_stats.dispatch_calls;
        ++m_stats.queue_submissions;
        m_stats.current_operation = "Tree support contour broad phase (CPU exact confirmation)";
        m_stats.last_diagnostic = diagnostic + " (" + std::to_string(request_count) +
            " branches, " + std::to_string(edge_count) + " contour edges).";
    }

    VulkanSlicerRuntimeStats snapshot() const
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        VulkanSlicerRuntimeStats snapshot = m_stats;
        snapshot.validation_mode = validation_mode_name();
        return snapshot;
    }

private:
    static std::string validation_mode_name();

    mutable std::mutex          m_mutex;
    VulkanSlicerRuntimeStats    m_stats;
};

VulkanIntersectionValidationMode configured_validation_mode()
{
    const char* value = std::getenv("MAGPIE_VULKAN_SLICER_VALIDATION");
    if (value == nullptr)
        value = std::getenv("ORCA_VULKAN_SLICER_VALIDATION");
    if (value != nullptr) {
        std::string normalized(value);
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                       [](unsigned char character) { return char(std::tolower(character)); });
        if (normalized == "strict")
            return VulkanIntersectionValidationMode::Strict;
    }
    return g_compute_mode.load(std::memory_order_acquire) == VulkanSlicerComputeMode::Maximum ?
        VulkanIntersectionValidationMode::Qualified : VulkanIntersectionValidationMode::Sampled;
}

std::string RuntimeStatsRegistry::validation_mode_name()
{
    switch (configured_validation_mode()) {
    case VulkanIntersectionValidationMode::Strict: return "strict CPU reference for every GPU result";
    case VulkanIntersectionValidationMode::Sampled: return "sampled CPU reference after GPU qualification";
    case VulkanIntersectionValidationMode::Qualified: return "startup-qualified GPU results without live CPU duplication";
    }
    return "unknown Vulkan validation mode";
}

RuntimeStatsRegistry& runtime_stats_registry()
{
    static RuntimeStatsRegistry registry;
    return registry;
}

bool is_gtx_1060(const std::string& name, uint32_t vendor_id)
{
    return vendor_id == 0x10de && name.find("GTX 1060") != std::string::npos;
}

bool is_nvidia_rtx(const std::string& name, uint32_t vendor_id)
{
    return vendor_id == 0x10de && name.find("RTX") != std::string::npos;
}

uint32_t choose_workgroup_size(uint32_t maximum_invocations, uint32_t maximum_size_x,
                               bool gtx_1060, bool nvidia_rtx)
{
    const uint32_t hard_limit = std::min(maximum_invocations, maximum_size_x);
    // A workgroup does not become faster merely by using the API maximum.
    // 128 threads is a good Pascal occupancy target; 256 gives the current
    // RTX device enough warps to hide integer-arithmetic latency without
    // consuming the 1024-thread maximum in one group.
    uint32_t preferred = nvidia_rtx ? 256 : (gtx_1060 ? 128 : kDefaultWorkgroupSize);
    while (preferred > hard_limit && preferred > 1)
        preferred /= 2;
    return std::max(1u, preferred);
}

} // namespace

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
namespace {

struct alignas(8) PackedVerticalIntersectionRequest {
    int64_t  ax;
    int64_t  ay;
    int64_t  bx;
    int64_t  by;
    int64_t  scan_x;
    uint64_t stable_id;
};
static_assert(sizeof(PackedVerticalIntersectionRequest) == 48);

struct alignas(8) PackedVerticalIntersectionResult {
    int64_t  numerator;
    int64_t  denominator;
    uint64_t stable_id;
    uint32_t valid;
    uint32_t reserved;
};
static_assert(sizeof(PackedVerticalIntersectionResult) == 32);

struct alignas(8) PackedTreeContourRequest {
    int64_t  ax;
    int64_t  ay;
    int64_t  bx;
    int64_t  by;
    uint64_t stable_id;
    uint32_t first_candidate;
    uint32_t candidate_count;
};
static_assert(sizeof(PackedTreeContourRequest) == 48);

struct alignas(8) PackedTreeContourEdge {
    int64_t ax;
    int64_t ay;
    int64_t bx;
    int64_t by;
};
static_assert(sizeof(PackedTreeContourEdge) == 32);

enum class DispatchModeOverride {
    None,
    Balanced,
    GpuPriority,
    GpuMaximum,
    CpuOnly
};

bool fits_gpu_coord(const WideCoord& value)
{
    return value >= std::numeric_limits<Coord>::min() &&
           value <= std::numeric_limits<Coord>::max();
}

std::string lowercase_hardware_name(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return char(std::tolower(character)); });
    return value;
}

bool is_obvious_low_end_gpu(const std::string& device_name, VkPhysicalDeviceType device_type)
{
    if (device_type != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
        return true;
    const std::string normalized = lowercase_hardware_name(device_name);
    static constexpr const char* low_end_devices[] = {
        "geforce gt 1010", "geforce gt 1030", "geforce gt 710", "geforce gt 720",
        "geforce gt 730", "geforce gt 740", "radeon rx 540", "radeon rx 550"
    };
    return std::any_of(std::begin(low_end_devices), std::end(low_end_devices),
                       [&normalized](const char* name) { return normalized.find(name) != std::string::npos; });
}

std::string classify_gpu_for_dispatch(const std::string& device_name, VkPhysicalDeviceType device_type)
{
    if (is_obvious_low_end_gpu(device_name, device_type))
        return "low-end CPU preferred";
    const std::string normalized = lowercase_hardware_name(device_name);
    static constexpr const char* accelerator_families[] = {
        "geforce rtx", "nvidia rtx", "radeon rx 6", "radeon rx 7", "intel arc"
    };
    const bool known_accelerator = std::any_of(
        std::begin(accelerator_families), std::end(accelerator_families),
        [&normalized](const char* name) { return normalized.find(name) != std::string::npos; });
    return known_accelerator ? "high-throughput candidate" : "calibration required";
}

uint32_t host_visible_coherent_memory_type(VkPhysicalDevice physical_device, uint32_t type_mask)
{
    VkPhysicalDeviceMemoryProperties memory_properties {};
    vkGetPhysicalDeviceMemoryProperties(physical_device, &memory_properties);
    for (uint32_t index = 0; index < memory_properties.memoryTypeCount; ++index) {
        const bool allowed = (type_mask & (uint32_t(1) << index)) != 0;
        const VkMemoryPropertyFlags flags = memory_properties.memoryTypes[index].propertyFlags;
        if (allowed && (flags & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                           (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
            return index;
    }
    return UINT32_MAX;
}

bool create_storage_buffer(VkPhysicalDevice physical_device, VkDevice device, VkDeviceSize size,
                           VkBuffer& buffer, VkDeviceMemory& memory)
{
    VkBufferCreateInfo buffer_info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    buffer_info.size = size;
    buffer_info.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
        return false;

    VkMemoryRequirements requirements {};
    vkGetBufferMemoryRequirements(device, buffer, &requirements);
    const uint32_t memory_type = host_visible_coherent_memory_type(physical_device, requirements.memoryTypeBits);
    if (memory_type == UINT32_MAX)
        return false;

    VkMemoryAllocateInfo allocation_info { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    allocation_info.allocationSize = requirements.size;
    allocation_info.memoryTypeIndex = memory_type;
    if (vkAllocateMemory(device, &allocation_info, nullptr, &memory) != VK_SUCCESS)
        return false;
    return vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS;
}

std::vector<uint32_t> load_intersection_shader(uint32_t workgroup_size)
{
    const std::string filename = "perimeter_infill_candidates_" +
        std::to_string(workgroup_size) + ".spv";
    std::vector<std::string> paths { Slic3r::resources_dir() + "/vulkan/" + filename };
#ifdef SLIC3R_VULKAN_BUILD_SHADER_DIR
    // The Windows/macOS build tree links resources/ to the source checkout.
    // The generated module is deliberately not written there, so use the
    // CMake binary directory only when the packaged resource is unavailable.
    paths.emplace_back(std::string(SLIC3R_VULKAN_BUILD_SHADER_DIR) + "/" + filename);
#endif
    for (const std::string& path : paths) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            continue;
        const std::streamsize byte_count = stream.tellg();
        if (byte_count <= 0 || (byte_count % std::streamsize(sizeof(uint32_t))) != 0)
            continue;
        std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
        stream.seekg(0);
        if (stream.read(reinterpret_cast<char*>(words.data()), byte_count))
            return words;
    }
    return {};
}

std::vector<uint32_t> load_tree_contour_shader()
{
    const std::string filename = "tree_support_contour_candidates.spv";
    std::vector<std::string> paths { Slic3r::resources_dir() + "/vulkan/" + filename };
#ifdef SLIC3R_VULKAN_BUILD_SHADER_DIR
    paths.emplace_back(std::string(SLIC3R_VULKAN_BUILD_SHADER_DIR) + "/" + filename);
#endif
    for (const std::string& path : paths) {
        std::ifstream stream(path, std::ios::binary | std::ios::ate);
        if (!stream)
            continue;
        const std::streamsize byte_count = stream.tellg();
        if (byte_count <= 0 || (byte_count % std::streamsize(sizeof(uint32_t))) != 0)
            continue;
        std::vector<uint32_t> words(size_t(byte_count) / sizeof(uint32_t));
        stream.seekg(0);
        if (stream.read(reinterpret_cast<char*>(words.data()), byte_count))
            return words;
    }
    return {};
}

class VulkanIntersectionContext {
public:
    ~VulkanIntersectionContext()
    {
        destroy_context_resources(!m_device_faulted);
    }

    VulkanVerticalIntersectionBatch dispatch(const std::vector<VulkanVerticalIntersectionRequest>& requests)
    {
        VulkanVerticalIntersectionBatch batch;
        if (requests.empty()) {
            batch.diagnostic = "No vertical intersections were submitted to Vulkan.";
            return batch;
        }

        if (!prepare_for_slicing()) {
            batch.diagnostic = m_diagnostic;
            return batch;
        }
        // Request collection may begin before the Vulkan context has been
        // initialized. Recheck the calibrated hardware policy here so the
        // first large batch cannot slip through the provisional threshold.
        if (!g_force_dispatch_for_automated_verification &&
            !should_dispatch_vertical_intersections(requests.size())) {
            runtime_stats_registry().record_skipped(requests.size());
            batch.diagnostic = "Vulkan retained the initialized batch on CPU after applying the hardware policy.";
            return batch;
        }

        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        return dispatch_locked(requests);
    }

    VulkanTreeContourBatch dispatch_tree_contours(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_edges)
    {
        VulkanTreeContourBatch batch;
        const VulkanSlicerComputeMode compute_mode = g_compute_mode.load(std::memory_order_acquire);
        const size_t minimum_pair_count = compute_mode == VulkanSlicerComputeMode::Maximum ? 1 :
            (compute_mode == VulkanSlicerComputeMode::Priority ? 128 : 16 * 1024);
        if (requests.empty() || contour_edges.empty()) {
            batch.diagnostic = "Tree contour broad phase has no branch or contour segments.";
            return batch;
        }
        size_t candidate_pair_count = 0;
        for (const VulkanTreeContourRequest& request : requests) {
            const size_t first = request.first_candidate;
            const size_t count = request.candidate_count == std::numeric_limits<uint32_t>::max() ?
                contour_edges.size() : size_t(request.candidate_count);
            if (first > contour_edges.size() || count > contour_edges.size() - first) {
                batch.diagnostic = "Tree contour broad phase received an invalid candidate range.";
                return batch;
            }
            if (candidate_pair_count > std::numeric_limits<size_t>::max() - count) {
                batch.diagnostic = "Tree contour broad phase candidate count overflowed.";
                return batch;
            }
            candidate_pair_count += count;
        }
        if (!g_force_dispatch_for_automated_verification && candidate_pair_count < minimum_pair_count) {
            batch.diagnostic = "Tree contour broad phase retained on CPU for a small workload.";
            return batch;
        }
        if (!prepare_for_slicing()) {
            batch.diagnostic = m_diagnostic;
            return batch;
        }
        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        return dispatch_tree_contours_locked(requests, contour_edges);
    }

    bool prepare_for_slicing()
    {
        std::lock_guard<std::mutex> initialize_lock(m_initialize_mutex);
        if (!m_ready) {
            const auto now = std::chrono::steady_clock::now();
            if (m_device_faulted) {
                m_diagnostic = "Vulkan compute was disabled after a device or dispatch failure; using CPU geometry.";
            } else if (!m_initialize_attempted || now >= m_next_initialize_attempt) {
                if (m_initialize_attempted)
                    destroy_context_resources(false);
                m_initialize_attempted = true;
                initialize();
                if (!m_ready)
                    m_next_initialize_attempt = now + kInitializationRetryDelay;
            }
        }
        refresh_intersection_dispatch_mode();
        if (!m_ready) {
            if (m_diagnostic.empty())
                m_diagnostic = "Vulkan infill compute did not finish initialization.";
            runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                                 m_workgroup_size, m_maximum_workgroup_size,
                                                 m_staging_request_capacity);
        }
        return m_ready;
    }

    bool should_dispatch_vertical_intersections(size_t request_count) const
    {
        return request_count >= (m_ready ? m_preferred_intersection_batch.load(std::memory_order_acquire)
                                         : kDefaultPreferredIntersectionBatch);
    }

    void release_unused_staging_memory(bool force)
    {
        if (!m_ready || m_device == VK_NULL_HANDLE)
            return;
        std::lock_guard<std::mutex> lock(m_dispatch_mutex);
        if (m_staging_request_capacity == 0 && m_tree_request_capacity == 0)
            return;
        const bool release_intersections = force ||
            m_staging_request_capacity > kRetainedIntersectionRequestCapacity;
        const bool release_tree = force ||
            m_tree_request_capacity > kRetainedTreeRequestCapacity ||
            m_tree_edge_capacity > kRetainedTreeEdgeCapacity;
        if (release_intersections)
            destroy_staging_buffers();
        if (release_tree)
            destroy_tree_staging_buffers();
        runtime_stats_registry().set_backend(
            m_selected_device, m_execution_profile,
            force ? "Released Vulkan staging buffers after an interrupted slice." :
            (release_intersections || release_tree ?
                "Trimmed oversized Vulkan staging buffers after slicing." :
                "Retained bounded Vulkan staging buffers for the next slice."),
            m_workgroup_size, m_maximum_workgroup_size, m_staging_request_capacity);
    }

private:
    void destroy_context_resources(bool wait_for_idle)
    {
        if (m_device != VK_NULL_HANDLE) {
            if (wait_for_idle)
                vkDeviceWaitIdle(m_device);
            destroy_tree_staging_buffers();
            destroy_staging_buffers();
            if (m_tree_descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_tree_descriptor_pool, nullptr);
            if (m_dispatch_fence != VK_NULL_HANDLE)
                vkDestroyFence(m_device, m_dispatch_fence, nullptr);
            if (m_timestamp_query_pool != VK_NULL_HANDLE)
                vkDestroyQueryPool(m_device, m_timestamp_query_pool, nullptr);
            if (m_descriptor_pool != VK_NULL_HANDLE)
                vkDestroyDescriptorPool(m_device, m_descriptor_pool, nullptr);
            if (m_command_pool != VK_NULL_HANDLE)
                vkDestroyCommandPool(m_device, m_command_pool, nullptr);
            if (m_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_pipeline, nullptr);
            if (m_tree_pipeline != VK_NULL_HANDLE)
                vkDestroyPipeline(m_device, m_tree_pipeline, nullptr);
            if (m_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_pipeline_layout, nullptr);
            if (m_tree_pipeline_layout != VK_NULL_HANDLE)
                vkDestroyPipelineLayout(m_device, m_tree_pipeline_layout, nullptr);
            if (m_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_descriptor_set_layout, nullptr);
            if (m_tree_descriptor_set_layout != VK_NULL_HANDLE)
                vkDestroyDescriptorSetLayout(m_device, m_tree_descriptor_set_layout, nullptr);
            vkDestroyDevice(m_device, nullptr);
        }
        if (m_instance != VK_NULL_HANDLE)
            vkDestroyInstance(m_instance, nullptr);

        m_instance = VK_NULL_HANDLE;
        m_physical_device = VK_NULL_HANDLE;
        m_device = VK_NULL_HANDLE;
        m_queue = VK_NULL_HANDLE;
        m_queue_family = UINT32_MAX;
        m_descriptor_set_layout = VK_NULL_HANDLE;
        m_pipeline_layout = VK_NULL_HANDLE;
        m_pipeline = VK_NULL_HANDLE;
        m_command_pool = VK_NULL_HANDLE;
        m_descriptor_pool = VK_NULL_HANDLE;
        m_descriptor_set = VK_NULL_HANDLE;
        m_command_buffer = VK_NULL_HANDLE;
        m_tree_descriptor_set_layout = VK_NULL_HANDLE;
        m_tree_pipeline_layout = VK_NULL_HANDLE;
        m_tree_pipeline = VK_NULL_HANDLE;
        m_tree_descriptor_pool = VK_NULL_HANDLE;
        m_tree_descriptor_set = VK_NULL_HANDLE;
        m_tree_command_buffer = VK_NULL_HANDLE;
        m_dispatch_fence = VK_NULL_HANDLE;
        m_timestamp_query_pool = VK_NULL_HANDLE;
        m_ready = false;
    }

    bool submit_and_wait(VkCommandBuffer command_buffer, const char* operation)
    {
        const VkSubmitInfo submit_info {
            VK_STRUCTURE_TYPE_SUBMIT_INFO, nullptr, 0, nullptr, nullptr, 1, &command_buffer, 0, nullptr };
        const VkResult reset_result = vkResetFences(m_device, 1, &m_dispatch_fence);
        const VkResult submit_result = reset_result == VK_SUCCESS ?
            vkQueueSubmit(m_queue, 1, &submit_info, m_dispatch_fence) : reset_result;
        const VkResult wait_result = submit_result == VK_SUCCESS ?
            vkWaitForFences(m_device, 1, &m_dispatch_fence, VK_TRUE, kDispatchFenceTimeoutNs) : submit_result;
        if (wait_result == VK_SUCCESS)
            return true;
        m_device_faulted = true;
        m_ready = false;
        m_diagnostic = std::string("Vulkan ") + operation +
            (wait_result == VK_TIMEOUT ? " timed out; CPU fallback is active for this process." :
                                         " failed; CPU fallback is active for this process.");
        return false;
    }

    void initialize()
    {
        VkApplicationInfo application_info { VK_STRUCTURE_TYPE_APPLICATION_INFO };
        application_info.pApplicationName = "MagpieSlicer";
        application_info.applicationVersion = 1;
        application_info.pEngineName = "MagpieSlicer";
        application_info.engineVersion = 1;
        application_info.apiVersion = VK_API_VERSION_1_2;

        VkInstanceCreateInfo instance_info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
        instance_info.pApplicationInfo = &application_info;
        if (vkCreateInstance(&instance_info, nullptr, &m_instance) != VK_SUCCESS) {
            m_diagnostic = "Vulkan instance creation failed for infill compute.";
            return;
        }

        uint32_t device_count = 0;
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
            m_diagnostic = "No Vulkan physical device is available for infill compute.";
            return;
        }
        std::vector<VkPhysicalDevice> devices(device_count);
        if (vkEnumeratePhysicalDevices(m_instance, &device_count, devices.data()) != VK_SUCCESS) {
            m_diagnostic = "Vulkan physical-device enumeration failed for infill compute.";
            return;
        }

        int64_t best_score = -1;
        VkPhysicalDeviceProperties selected_properties {};
        for (VkPhysicalDevice candidate : devices) {
            VkPhysicalDeviceFeatures features {};
            VkPhysicalDeviceProperties properties {};
            vkGetPhysicalDeviceFeatures(candidate, &features);
            vkGetPhysicalDeviceProperties(candidate, &properties);
            if (features.shaderInt64 != VK_TRUE)
                continue;

            uint32_t family_count = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, nullptr);
            std::vector<VkQueueFamilyProperties> families(family_count);
            vkGetPhysicalDeviceQueueFamilyProperties(candidate, &family_count, families.data());
            VkPhysicalDeviceMemoryProperties memory_properties {};
            vkGetPhysicalDeviceMemoryProperties(candidate, &memory_properties);
            uint64_t device_local_bytes = 0;
            for (uint32_t heap = 0; heap < memory_properties.memoryHeapCount; ++heap) {
                if ((memory_properties.memoryHeaps[heap].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) != 0)
                    device_local_bytes += memory_properties.memoryHeaps[heap].size;
            }
            for (uint32_t family = 0; family < family_count; ++family) {
                if ((families[family].queueFlags & VK_QUEUE_COMPUTE_BIT) == 0 || families[family].queueCount == 0)
                    continue;
                // Prefer the strongest eligible physical device instead of
                // assuming enumeration order. Discrete GPUs dominate, then
                // device-local memory and compute-dispatch limits break ties.
                const int64_t score =
                    (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? int64_t(1) << 60 : 0) +
                    int64_t(std::min<uint64_t>(device_local_bytes / (1024 * 1024), (int64_t(1) << 40) - 1)) * 1024 +
                    properties.limits.maxComputeWorkGroupInvocations;
                if (score > best_score) {
                    best_score = score;
                    m_physical_device = candidate;
                    m_queue_family = family;
                    selected_properties = properties;
                }
            }
        }
        if (m_physical_device == VK_NULL_HANDLE) {
            m_diagnostic = "No Vulkan compute queue with shaderInt64 is available for exact infill intersections.";
            return;
        }

        m_selected_device = selected_properties.deviceName;
        m_selected_vendor_id = selected_properties.vendorID;
        m_selected_device_id = selected_properties.deviceID;
        m_selected_driver_version = selected_properties.driverVersion;
        m_selected_device_type = selected_properties.deviceType;
        m_cpu_identifier = cpu_identifier();
        m_cpu_logical_threads = std::max(1u, std::thread::hardware_concurrency());
        m_hardware_policy_class = classify_gpu_for_dispatch(m_selected_device, m_selected_device_type);
        m_is_gtx_1060 = is_gtx_1060(m_selected_device, selected_properties.vendorID);
        m_is_nvidia_rtx = is_nvidia_rtx(m_selected_device, selected_properties.vendorID);
        m_maximum_workgroup_size = std::min(selected_properties.limits.maxComputeWorkGroupInvocations,
                                            selected_properties.limits.maxComputeWorkGroupSize[0]);
        m_workgroup_size = choose_workgroup_size(selected_properties.limits.maxComputeWorkGroupInvocations,
                                                  selected_properties.limits.maxComputeWorkGroupSize[0],
                                                  m_is_gtx_1060, m_is_nvidia_rtx);
        m_initial_staging_request_capacity = m_is_nvidia_rtx ? kNvidiaRtxStagingRequestCapacity :
            (m_is_gtx_1060 ? kNvidiaPascalStagingRequestCapacity : kGenericStagingRequestCapacity);
        m_timestamp_period_ns = double(selected_properties.limits.timestampPeriod);
        m_compute_timestamps_available =
            selected_properties.limits.timestampComputeAndGraphics == VK_TRUE && m_timestamp_period_ns > 0.0;
        const uint64_t maximum_request_size = std::min(
            uint64_t(selected_properties.limits.maxStorageBufferRange) / sizeof(PackedVerticalIntersectionRequest),
            uint64_t(selected_properties.limits.maxStorageBufferRange) / sizeof(PackedVerticalIntersectionResult));
        m_storage_buffer_request_limit = size_t(std::min<uint64_t>(maximum_request_size, std::numeric_limits<uint32_t>::max()));
        m_max_compute_workgroup_count_x = selected_properties.limits.maxComputeWorkGroupCount[0];
        update_submission_request_limit();
        if (m_max_requests_per_submission == 0) {
            m_diagnostic = "The selected Vulkan device has no usable storage-buffer range for infill compute.";
            return;
        }

        const float queue_priority = 1.f;
        VkDeviceQueueCreateInfo queue_info { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
        queue_info.queueFamilyIndex = m_queue_family;
        queue_info.queueCount = 1;
        queue_info.pQueuePriorities = &queue_priority;
        VkPhysicalDeviceFeatures enabled_features {};
        enabled_features.shaderInt64 = VK_TRUE;
        VkDeviceCreateInfo device_info { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
        device_info.queueCreateInfoCount = 1;
        device_info.pQueueCreateInfos = &queue_info;
        device_info.pEnabledFeatures = &enabled_features;
        if (vkCreateDevice(m_physical_device, &device_info, nullptr, &m_device) != VK_SUCCESS) {
            m_diagnostic = "Vulkan logical-device creation failed for infill compute.";
            return;
        }
        vkGetDeviceQueue(m_device, m_queue_family, 0, &m_queue);

        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };
        VkDescriptorSetLayoutCreateInfo layout_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = uint32_t(std::size(bindings));
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_descriptor_set_layout) != VK_SUCCESS) {
            m_diagnostic = "Vulkan descriptor-layout creation failed for infill compute.";
            return;
        }

        VkPipelineLayoutCreateInfo pipeline_layout_info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &m_descriptor_set_layout;
        if (vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_pipeline_layout) != VK_SUCCESS) {
            m_diagnostic = "Vulkan pipeline-layout creation failed for infill compute.";
            return;
        }

        VkCommandPoolCreateInfo command_pool_info { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        command_pool_info.queueFamilyIndex = m_queue_family;
        command_pool_info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        if (vkCreateCommandPool(m_device, &command_pool_info, nullptr, &m_command_pool) != VK_SUCCESS) {
            m_diagnostic = "Vulkan command-pool creation failed for infill compute.";
            return;
        }
        const VkDescriptorPoolSize pool_size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 };
        VkDescriptorPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_descriptor_pool) != VK_SUCCESS) {
            m_diagnostic = "Vulkan descriptor-pool creation failed for infill compute.";
            return;
        }

        VkDescriptorSetAllocateInfo descriptor_allocation_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        descriptor_allocation_info.descriptorPool = m_descriptor_pool;
        descriptor_allocation_info.descriptorSetCount = 1;
        descriptor_allocation_info.pSetLayouts = &m_descriptor_set_layout;
        if (vkAllocateDescriptorSets(m_device, &descriptor_allocation_info, &m_descriptor_set) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable infill descriptor set.";
            return;
        }

        VkCommandBufferAllocateInfo command_allocation_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_allocation_info.commandPool = m_command_pool;
        command_allocation_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_allocation_info.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(m_device, &command_allocation_info, &m_command_buffer) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable infill command buffer.";
            return;
        }

        // Both geometry kernels synchronously consume their host-visible
        // result buffer. A fence waits only for this submission, unlike
        // vkQueueWaitIdle(), which drains every operation in the queue and
        // adds avoidable driver latency to dense infill/wall batches.
        VkFenceCreateInfo dispatch_fence_info { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        if (vkCreateFence(m_device, &dispatch_fence_info, nullptr, &m_dispatch_fence) != VK_SUCCESS) {
            m_diagnostic = "Vulkan could not allocate the reusable compute dispatch fence.";
            return;
        }

        if (m_compute_timestamps_available) {
            VkQueryPoolCreateInfo query_pool_info { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO };
            query_pool_info.queryType = VK_QUERY_TYPE_TIMESTAMP;
            query_pool_info.queryCount = 2;
            if (vkCreateQueryPool(m_device, &query_pool_info, nullptr, &m_timestamp_query_pool) != VK_SUCCESS)
                m_compute_timestamps_available = false;
        }

        if (!autotune_workgroup_size())
            return;
        update_execution_profile();

        m_ready = true;
        if (!run_exact_qualification()) {
            m_ready = false;
            if (m_diagnostic.empty())
                m_diagnostic = "Vulkan exact vertical-intersection qualification failed.";
            runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                                 m_workgroup_size, m_maximum_workgroup_size,
                                                 m_staging_request_capacity);
            return;
        }
        calibrate_intersection_dispatch_policy();
        update_execution_profile();
        runtime_stats_registry().set_preferred_intersection_batch(
            m_preferred_intersection_batch.load(std::memory_order_acquire));
        m_diagnostic = "Vulkan exact vertical-intersection compute is qualified and ready on " + m_selected_device + ".";
        runtime_stats_registry().set_backend(m_selected_device, m_execution_profile, m_diagnostic,
                                             m_workgroup_size, m_maximum_workgroup_size,
                                             m_staging_request_capacity);
    }

    void update_submission_request_limit()
    {
        const uint64_t maximum_workgroup_requests = uint64_t(m_max_compute_workgroup_count_x) * m_workgroup_size;
        m_max_requests_per_submission = size_t(std::min<uint64_t>(m_storage_buffer_request_limit,
                                                                    maximum_workgroup_requests));
        m_max_requests_per_submission = std::min(m_max_requests_per_submission,
                                                 kMaximumReusableStagingRequestCapacity);
    }

    void update_execution_profile()
    {
        const size_t preferred_batch = m_preferred_intersection_batch.load(std::memory_order_acquire);
        std::ostringstream profile;
        profile << (m_is_nvidia_rtx ? "NVIDIA RTX" : (m_is_gtx_1060 ? "NVIDIA GTX 1060 / Pascal" : "Generic Vulkan"))
                << ": " << m_workgroup_size << " threads/group (autotuned; device maximum "
                << m_maximum_workgroup_size << "), " << m_initial_staging_request_capacity
                << " reusable requests, "
                << (m_compute_mode == VulkanSlicerComputeMode::Maximum ? "maximum GPU" :
                    (m_compute_mode == VulkanSlicerComputeMode::Priority ? "GPU-priority" : "balanced CPU/GPU"))
                << " batch >= " << preferred_batch
                << ", policy " << m_dispatch_policy_source
                << " [" << m_hardware_policy_class << "]";
        m_execution_profile = profile.str();
    }

    std::string dispatch_policy_hardware_key() const
    {
        std::ostringstream key;
        key << "schema=" << kDispatchPolicyCacheSchema
            << "|cpu=" << m_cpu_identifier
            << "|threads=" << m_cpu_logical_threads
            << "|gpu=" << m_selected_device
            << "|vendor=" << m_selected_vendor_id
            << "|device=" << m_selected_device_id
            << "|driver=" << m_selected_driver_version;
        return key.str();
    }

    boost::filesystem::path dispatch_policy_cache_path() const
    {
        const std::string root = Slic3r::data_dir();
        return root.empty() ? boost::filesystem::path() :
            boost::filesystem::path(root) / "cache" / "vulkan-dispatch-policy.json";
    }

    bool try_load_cached_dispatch_policy()
    {
        const boost::filesystem::path path = dispatch_policy_cache_path();
        try {
            if (path.empty() || !boost::filesystem::is_regular_file(path))
                return false;
            std::ifstream input(path.string());
            nlohmann::json document;
            input >> document;
            if (document.value("schema", 0) != kDispatchPolicyCacheSchema ||
                document.value("hardware_key", std::string()) != dispatch_policy_hardware_key())
                return false;
            const uint64_t cached_batch = document.at("balanced_intersection_batch").get<uint64_t>();
            if (cached_batch == 0 || cached_batch > uint64_t(m_max_requests_per_submission) + 1)
                return false;
            m_balanced_intersection_batch = size_t(cached_batch);
            m_gpu_throughput_is_extremely_slow = document.value("gpu_extremely_slow", false);
            m_dispatch_policy_source = "cached calibration";
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    void save_cached_dispatch_policy() const
    {
        const boost::filesystem::path path = dispatch_policy_cache_path();
        if (path.empty())
            return;
        try {
            boost::filesystem::create_directories(path.parent_path());
            boost::filesystem::path temporary = path;
            temporary += ".tmp";
            const nlohmann::json document {
                { "schema", kDispatchPolicyCacheSchema },
                { "hardware_key", dispatch_policy_hardware_key() },
                { "cpu", m_cpu_identifier },
                { "logical_threads", m_cpu_logical_threads },
                { "gpu", m_selected_device },
                { "gpu_vendor_id", m_selected_vendor_id },
                { "gpu_device_id", m_selected_device_id },
                { "driver_version", m_selected_driver_version },
                { "balanced_intersection_batch", m_balanced_intersection_batch },
                { "gpu_extremely_slow", m_gpu_throughput_is_extremely_slow }
            };
            {
                std::ofstream output(temporary.string(), std::ios::trunc);
                if (!output)
                    return;
                output << document.dump(2) << '\n';
                if (!output)
                    return;
            }
            if (boost::filesystem::exists(path))
                boost::filesystem::remove(path);
            boost::filesystem::rename(temporary, path);
        } catch (const std::exception&) {
            // Dispatch policy persistence is an optimization. Never make it a
            // prerequisite for deterministic CPU fallback or slicing startup.
        }
    }

    void apply_intersection_dispatch_mode()
    {
        switch (m_dispatch_mode_override) {
        case DispatchModeOverride::Balanced:
        case DispatchModeOverride::CpuOnly:
            m_compute_mode = VulkanSlicerComputeMode::Balanced;
            break;
        case DispatchModeOverride::GpuPriority:
            m_compute_mode = VulkanSlicerComputeMode::Priority;
            break;
        case DispatchModeOverride::GpuMaximum:
            m_compute_mode = VulkanSlicerComputeMode::Maximum;
            break;
        case DispatchModeOverride::None:
            m_compute_mode = g_compute_mode.load(std::memory_order_acquire);
            break;
        }
        size_t preferred_batch = m_balanced_intersection_batch;
        if (m_dispatch_mode_override == DispatchModeOverride::CpuOnly) {
            preferred_batch = m_max_requests_per_submission + 1;
        } else if (m_compute_mode == VulkanSlicerComputeMode::Maximum) {
            preferred_batch = 1;
        } else if (m_compute_mode == VulkanSlicerComputeMode::Priority) {
            // On is an explicit request to use qualified Vulkan hardware.
            // Tiny transfers still stay on CPU to avoid pathological dispatch
            // overhead; device and validation failures retain CPU fallback.
            preferred_batch = std::min(kGpuPriorityMinimumIntersectionBatch,
                                       m_max_requests_per_submission);
        } else if (m_gpu_throughput_is_extremely_slow) {
            // The variable GPU cost lost to the CPU benchmark, not merely the
            // one-time dispatch cost. Keeping this device on CPU prevents the
            // GPU-priority preference from making an entire slice slower.
            preferred_batch = m_max_requests_per_submission + 1;
        }
        m_preferred_intersection_batch.store(preferred_batch, std::memory_order_release);
    }

    void refresh_intersection_dispatch_mode()
    {
        if (!m_ready || m_dispatch_mode_override != DispatchModeOverride::None)
            return;
        const VulkanSlicerComputeMode requested_mode = g_compute_mode.load(std::memory_order_acquire);
        if (requested_mode == m_compute_mode)
            return;
        apply_intersection_dispatch_mode();
        update_execution_profile();
        runtime_stats_registry().set_preferred_intersection_batch(
            m_preferred_intersection_batch.load(std::memory_order_acquire));
    }

    void calibrate_intersection_dispatch_policy()
    {
        const char* policy = std::getenv("MAGPIE_VULKAN_SLICER_POLICY");
        if (policy == nullptr)
            policy = std::getenv("ORCA_VULKAN_SLICER_POLICY");
        const std::string normalized_policy = policy == nullptr ? std::string() :
            lowercase_hardware_name(trim_hardware_name(policy));
        if (normalized_policy == "cpu") {
            m_dispatch_mode_override = DispatchModeOverride::CpuOnly;
            m_gpu_throughput_is_extremely_slow = true;
            m_balanced_intersection_batch = m_max_requests_per_submission + 1;
            m_dispatch_policy_source = "environment CPU override";
            apply_intersection_dispatch_mode();
            return;
        }
        if (normalized_policy == "gpu") {
            m_dispatch_mode_override = DispatchModeOverride::GpuPriority;
            m_gpu_throughput_is_extremely_slow = false;
            m_balanced_intersection_batch = kGpuPriorityMinimumIntersectionBatch;
            m_dispatch_policy_source = "environment GPU override";
            apply_intersection_dispatch_mode();
            return;
        }
        if (normalized_policy == "max" || normalized_policy == "maximum") {
            m_dispatch_mode_override = DispatchModeOverride::GpuMaximum;
            m_gpu_throughput_is_extremely_slow = false;
            m_balanced_intersection_batch = 1;
            m_dispatch_policy_source = "environment maximum GPU override";
            apply_intersection_dispatch_mode();
            return;
        }
        if (normalized_policy == "auto")
            m_dispatch_mode_override = DispatchModeOverride::Balanced;

        if (is_obvious_low_end_gpu(m_selected_device, m_selected_device_type)) {
            m_gpu_throughput_is_extremely_slow = true;
            m_balanced_intersection_batch = m_max_requests_per_submission + 1;
            m_dispatch_policy_source = "hardware low-end classification";
            apply_intersection_dispatch_mode();
            return;
        }
        if (try_load_cached_dispatch_policy()) {
            apply_intersection_dispatch_mode();
            return;
        }

        const size_t small_count = std::min<size_t>(4096, m_max_requests_per_submission);
        const size_t large_count = std::min<size_t>(32768, m_max_requests_per_submission);
        if (small_count < 256 || large_count <= small_count) {
            m_balanced_intersection_batch = kDefaultPreferredIntersectionBatch;
            m_dispatch_policy_source = "device-limit fallback";
            apply_intersection_dispatch_mode();
            return;
        }

        std::vector<VulkanVerticalIntersectionRequest> requests;
        requests.reserve(large_count);
        for (size_t index = 0; index < large_count; ++index) {
            const int64_t left = -900000 + int64_t(index % 509) * 73;
            const int64_t right = left + 3001 + int64_t(index % 127);
            requests.push_back({ { { left, -700000 + int64_t(index) * 13 },
                                   { right, 900000 - int64_t(index) * 17 } },
                                 left + 1 + int64_t(index % (right - left - 1)), uint64_t(index) });
        }
        const std::vector<VulkanVerticalIntersectionRequest> small(requests.begin(), requests.begin() + small_count);
        const VulkanVerticalIntersectionBatch gpu_small = dispatch_locked(small);
        const VulkanVerticalIntersectionBatch gpu_large = dispatch_locked(requests);
        if (!gpu_small.dispatched || !gpu_large.dispatched) {
            m_balanced_intersection_batch = kDefaultPreferredIntersectionBatch;
            m_dispatch_policy_source = "calibration unavailable";
            apply_intersection_dispatch_mode();
            return;
        }

        constexpr int cpu_repetitions = 8;
        volatile int64_t checksum = 0;
        const auto cpu_start = std::chrono::steady_clock::now();
        for (int repetition = 0; repetition < cpu_repetitions; ++repetition) {
            for (const VulkanVerticalIntersectionRequest& request : requests) {
                const int64_t denominator = request.segment.b.x > request.segment.a.x ?
                    request.segment.b.x - request.segment.a.x : request.segment.a.x - request.segment.b.x;
                const int64_t t_numerator = request.segment.b.x > request.segment.a.x ?
                    request.scan_x - request.segment.a.x : request.segment.a.x - request.scan_x;
                checksum += t_numerator * (request.segment.b.y - request.segment.a.y) +
                    request.segment.a.y * denominator;
            }
        }
        const double cpu_per_request_ms = std::chrono::duration<double, std::milli>(
            std::chrono::steady_clock::now() - cpu_start).count() / (cpu_repetitions * large_count);
        (void)checksum;
        const double gpu_per_request_ms = std::max(0.0, gpu_large.host_elapsed_ms - gpu_small.host_elapsed_ms) /
            double(large_count - small_count);
        const double gpu_fixed_overhead_ms = std::max(0.0, gpu_small.host_elapsed_ms - gpu_per_request_ms * small_count);
        // Packing and CPU topology still remain after this GPU result arrives.
        // Only a conservative fraction of the arithmetic benchmark is treated
        // as recoverable CPU work when choosing the crossover point.
        const double recoverable_cpu_per_request_ms = cpu_per_request_ms * 0.35;
        constexpr double required_gpu_fraction = 0.90;
        const double profitable_cpu_budget_per_request_ms =
            recoverable_cpu_per_request_ms * required_gpu_fraction;
        if (gpu_per_request_ms >= profitable_cpu_budget_per_request_ms) {
            // GPU-priority mode is intentionally permissive. A synchronized
            // micro-benchmark may make the GPU look slower even when a real
            // slice has enough parallel work to benefit. Only reject a device
            // if the *large* calibrated submission is both materially long
            // and many times slower than the full CPU reference.
            constexpr double extreme_gpu_slowdown_factor = 32.0;
            constexpr double extreme_gpu_elapsed_ms = 50.0;
            const double cpu_large_reference_ms = cpu_per_request_ms * double(large_count);
            m_gpu_throughput_is_extremely_slow =
                gpu_large.host_elapsed_ms >= extreme_gpu_elapsed_ms &&
                gpu_large.host_elapsed_ms >= cpu_large_reference_ms * extreme_gpu_slowdown_factor;
            m_balanced_intersection_batch = m_max_requests_per_submission + 1;
            m_dispatch_policy_source = "fresh calibration";
            apply_intersection_dispatch_mode();
            save_cached_dispatch_policy();
            return;
        }
        const size_t crossover = size_t(std::ceil(gpu_fixed_overhead_ms /
            (profitable_cpu_budget_per_request_ms - gpu_per_request_ms)));
        m_gpu_throughput_is_extremely_slow = false;
        m_balanced_intersection_batch = std::clamp<size_t>(crossover, kDefaultPreferredIntersectionBatch,
                                                             std::min<size_t>(65536, m_max_requests_per_submission));
        m_dispatch_policy_source = "fresh calibration";
        apply_intersection_dispatch_mode();
        save_cached_dispatch_policy();
    }

    bool create_compute_pipeline(uint32_t workgroup_size, VkPipeline& pipeline)
    {
        const std::vector<uint32_t> shader_words = load_intersection_shader(workgroup_size);
        if (shader_words.empty())
            return false;
        VkShaderModuleCreateInfo shader_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader_info.codeSize = shader_words.size() * sizeof(uint32_t);
        shader_info.pCode = shader_words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &shader_info, nullptr, &shader) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage_info { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage = stage_info;
        pipeline_info.layout = m_pipeline_layout;
        const VkResult result = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        return result == VK_SUCCESS;
    }

    bool autotune_workgroup_size()
    {
        // Measure the actual hardware instead of assuming that an RTX name or
        // the API's 1024-thread limit is optimal. Integer intersection work is
        // latency-sensitive; several resident 64-256-thread groups usually
        // beat one maximum-sized group.
        std::vector<uint32_t> candidates { m_workgroup_size };
        for (const uint32_t candidate : { 64u, 128u, 256u, 512u, 1024u }) {
            if (candidate <= m_maximum_workgroup_size)
                candidates.emplace_back(candidate);
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

        std::vector<std::pair<uint32_t, VkPipeline>> pipelines;
        pipelines.reserve(candidates.size());
        for (const uint32_t candidate : candidates) {
            VkPipeline pipeline = VK_NULL_HANDLE;
            if (create_compute_pipeline(candidate, pipeline))
                pipelines.emplace_back(candidate, pipeline);
        }
        if (pipelines.empty()) {
            m_diagnostic = "Vulkan could not create any specialized infill compute pipeline.";
            return false;
        }

        const size_t probe_count = std::min(m_max_requests_per_submission,
            std::max<size_t>(256, std::min(m_initial_staging_request_capacity,
                                            m_max_requests_per_submission)));
        std::vector<VulkanVerticalIntersectionRequest> probe_requests;
        probe_requests.reserve(probe_count);
        for (size_t index = 0; index < probe_count; ++index) {
            const int64_t left = -300000 + int64_t(index % 211) * 37;
            const int64_t right = left + 1009 + int64_t(index % 71);
            const bool reverse = (index & 1) != 0;
            probe_requests.push_back({
                { { reverse ? right : left, -500000 + int64_t(index) * 7 },
                  { reverse ? left : right, 600000 - int64_t(index) * 11 } },
                left + 1 + int64_t(index % (right - left - 1)), uint64_t(index)
            });
        }

        VkPipeline selected_pipeline = VK_NULL_HANDLE;
        uint32_t selected_workgroup_size = m_workgroup_size;
        double best_elapsed_ms = std::numeric_limits<double>::infinity();
        for (const auto& candidate : pipelines) {
            m_pipeline = candidate.second;
            m_workgroup_size = candidate.first;
            update_submission_request_limit();
            // One warm-up run lets shader/power state settle before the three
            // timestamped trials that decide the live group size.
            if (!dispatch_locked(probe_requests).dispatched)
                continue;
            double candidate_elapsed_ms = std::numeric_limits<double>::infinity();
            bool succeeded = true;
            for (int trial = 0; trial < 3; ++trial) {
                const VulkanVerticalIntersectionBatch batch = dispatch_locked(probe_requests);
                if (!batch.dispatched) {
                    succeeded = false;
                    break;
                }
                const double elapsed_ms = batch.gpu_elapsed_ms >= 0.0 ?
                    batch.gpu_elapsed_ms : batch.host_elapsed_ms;
                candidate_elapsed_ms = std::min(candidate_elapsed_ms, elapsed_ms);
            }
            if (succeeded && candidate_elapsed_ms < best_elapsed_ms) {
                best_elapsed_ms = candidate_elapsed_ms;
                selected_workgroup_size = candidate.first;
                selected_pipeline = candidate.second;
            }
        }

        if (selected_pipeline == VK_NULL_HANDLE) {
            for (const auto& candidate : pipelines)
                vkDestroyPipeline(m_device, candidate.second, nullptr);
            m_pipeline = VK_NULL_HANDLE;
            m_diagnostic = "Vulkan workgroup autotuning could not complete a compute dispatch.";
            return false;
        }
        for (const auto& candidate : pipelines) {
            if (candidate.second != selected_pipeline)
                vkDestroyPipeline(m_device, candidate.second, nullptr);
        }
        m_pipeline = selected_pipeline;
        m_workgroup_size = selected_workgroup_size;
        update_submission_request_limit();
        return true;
    }

    bool ensure_tree_pipeline()
    {
        if (m_tree_pipeline != VK_NULL_HANDLE)
            return true;
        const VkDescriptorSetLayoutBinding bindings[] = {
            { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr },
            { 2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }
        };
        VkDescriptorSetLayoutCreateInfo layout_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        layout_info.bindingCount = uint32_t(std::size(bindings));
        layout_info.pBindings = bindings;
        if (vkCreateDescriptorSetLayout(m_device, &layout_info, nullptr, &m_tree_descriptor_set_layout) != VK_SUCCESS)
            return false;
        VkPipelineLayoutCreateInfo pipeline_layout_info { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        pipeline_layout_info.setLayoutCount = 1;
        pipeline_layout_info.pSetLayouts = &m_tree_descriptor_set_layout;
        if (vkCreatePipelineLayout(m_device, &pipeline_layout_info, nullptr, &m_tree_pipeline_layout) != VK_SUCCESS)
            return false;
        const std::vector<uint32_t> shader_words = load_tree_contour_shader();
        if (shader_words.empty())
            return false;
        VkShaderModuleCreateInfo shader_info { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
        shader_info.codeSize = shader_words.size() * sizeof(uint32_t);
        shader_info.pCode = shader_words.data();
        VkShaderModule shader = VK_NULL_HANDLE;
        if (vkCreateShaderModule(m_device, &shader_info, nullptr, &shader) != VK_SUCCESS)
            return false;
        VkPipelineShaderStageCreateInfo stage_info { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO };
        stage_info.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage_info.module = shader;
        stage_info.pName = "main";
        VkComputePipelineCreateInfo pipeline_info { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
        pipeline_info.stage = stage_info;
        pipeline_info.layout = m_tree_pipeline_layout;
        const VkResult pipeline_result = vkCreateComputePipelines(
            m_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &m_tree_pipeline);
        vkDestroyShaderModule(m_device, shader, nullptr);
        if (pipeline_result != VK_SUCCESS)
            return false;
        const VkDescriptorPoolSize pool_size { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3 };
        VkDescriptorPoolCreateInfo pool_info { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        pool_info.maxSets = 1;
        pool_info.poolSizeCount = 1;
        pool_info.pPoolSizes = &pool_size;
        if (vkCreateDescriptorPool(m_device, &pool_info, nullptr, &m_tree_descriptor_pool) != VK_SUCCESS)
            return false;
        VkDescriptorSetAllocateInfo descriptor_info { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        descriptor_info.descriptorPool = m_tree_descriptor_pool;
        descriptor_info.descriptorSetCount = 1;
        descriptor_info.pSetLayouts = &m_tree_descriptor_set_layout;
        if (vkAllocateDescriptorSets(m_device, &descriptor_info, &m_tree_descriptor_set) != VK_SUCCESS)
            return false;
        VkCommandBufferAllocateInfo command_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        command_info.commandPool = m_command_pool;
        command_info.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        command_info.commandBufferCount = 1;
        return vkAllocateCommandBuffers(m_device, &command_info, &m_tree_command_buffer) == VK_SUCCESS;
    }

    void destroy_tree_staging_buffers()
    {
        const VkDeviceMemory memories[] { m_tree_request_memory, m_tree_edge_memory, m_tree_result_memory };
        const VkBuffer buffers[] { m_tree_request_buffer, m_tree_edge_buffer, m_tree_result_buffer };
        void* const mappings[] { m_tree_request_mapping, m_tree_edge_mapping, m_tree_result_mapping };
        for (size_t index = 0; index < std::size(memories); ++index) {
            if (mappings[index] != nullptr && memories[index] != VK_NULL_HANDLE)
                vkUnmapMemory(m_device, memories[index]);
            if (buffers[index] != VK_NULL_HANDLE)
                vkDestroyBuffer(m_device, buffers[index], nullptr);
            if (memories[index] != VK_NULL_HANDLE)
                vkFreeMemory(m_device, memories[index], nullptr);
        }
        m_tree_request_buffer = m_tree_edge_buffer = m_tree_result_buffer = VK_NULL_HANDLE;
        m_tree_request_memory = m_tree_edge_memory = m_tree_result_memory = VK_NULL_HANDLE;
        m_tree_request_mapping = m_tree_edge_mapping = m_tree_result_mapping = nullptr;
        m_tree_request_capacity = m_tree_edge_capacity = 0;
    }

    bool ensure_tree_staging_buffers(size_t request_count, size_t edge_count)
    {
        const VulkanSlicerComputeMode compute_mode = g_compute_mode.load(std::memory_order_acquire);
        const size_t maximum_capacity = compute_mode == VulkanSlicerComputeMode::Maximum ? 4 * 1024 * 1024 :
            (compute_mode == VulkanSlicerComputeMode::Priority ? 1024 * 1024 : 128 * 1024);
        if (request_count > maximum_capacity || edge_count > maximum_capacity ||
            request_count > m_max_compute_workgroup_count_x)
            return false;
        if (m_tree_request_capacity >= request_count && m_tree_edge_capacity >= edge_count &&
            m_tree_request_mapping != nullptr && m_tree_edge_mapping != nullptr &&
            m_tree_result_mapping != nullptr)
            return true;
        size_t request_capacity = 1024;
        while (request_capacity < request_count)
            request_capacity *= 2;
        size_t edge_capacity = 1024;
        while (edge_capacity < edge_count)
            edge_capacity *= 2;
        destroy_tree_staging_buffers();
        const VkDeviceSize request_size = VkDeviceSize(request_capacity * sizeof(PackedTreeContourRequest));
        const VkDeviceSize edge_size = VkDeviceSize(edge_capacity * sizeof(PackedTreeContourEdge));
        const VkDeviceSize result_size = VkDeviceSize(request_capacity * sizeof(uint32_t));
        if (!create_storage_buffer(m_physical_device, m_device, request_size, m_tree_request_buffer, m_tree_request_memory) ||
            !create_storage_buffer(m_physical_device, m_device, edge_size, m_tree_edge_buffer, m_tree_edge_memory) ||
            !create_storage_buffer(m_physical_device, m_device, result_size, m_tree_result_buffer, m_tree_result_memory)) {
            destroy_tree_staging_buffers();
            return false;
        }
        if (vkMapMemory(m_device, m_tree_request_memory, 0, request_size, 0, &m_tree_request_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_tree_edge_memory, 0, edge_size, 0, &m_tree_edge_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_tree_result_memory, 0, result_size, 0, &m_tree_result_mapping) != VK_SUCCESS) {
            destroy_tree_staging_buffers();
            return false;
        }
        m_tree_request_capacity = request_capacity;
        m_tree_edge_capacity = edge_capacity;
        return true;
    }

    void destroy_staging_buffers()
    {
        if (m_input_mapping != nullptr && m_input_memory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, m_input_memory);
        if (m_output_mapping != nullptr && m_output_memory != VK_NULL_HANDLE)
            vkUnmapMemory(m_device, m_output_memory);
        m_input_mapping = nullptr;
        m_output_mapping = nullptr;
        if (m_input_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, m_input_buffer, nullptr);
        if (m_output_buffer != VK_NULL_HANDLE)
            vkDestroyBuffer(m_device, m_output_buffer, nullptr);
        if (m_input_memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, m_input_memory, nullptr);
        if (m_output_memory != VK_NULL_HANDLE)
            vkFreeMemory(m_device, m_output_memory, nullptr);
        m_input_buffer = VK_NULL_HANDLE;
        m_output_buffer = VK_NULL_HANDLE;
        m_input_memory = VK_NULL_HANDLE;
        m_output_memory = VK_NULL_HANDLE;
        m_staging_request_capacity = 0;
    }

    bool ensure_staging_buffers(size_t request_count)
    {
        if (request_count > m_max_requests_per_submission)
            return false;
        if (m_staging_request_capacity >= request_count &&
            m_input_mapping != nullptr && m_output_mapping != nullptr)
            return true;

        size_t capacity = m_initial_staging_request_capacity;
        while (capacity < request_count && capacity <= m_max_requests_per_submission / 2)
            capacity *= 2;
        capacity = std::max(capacity, request_count);
        capacity = std::min(capacity, m_max_requests_per_submission);

        destroy_staging_buffers();
        const VkDeviceSize input_size = VkDeviceSize(capacity * sizeof(PackedVerticalIntersectionRequest));
        const VkDeviceSize output_size = VkDeviceSize(capacity * sizeof(PackedVerticalIntersectionResult));
        if (!create_storage_buffer(m_physical_device, m_device, input_size, m_input_buffer, m_input_memory) ||
            !create_storage_buffer(m_physical_device, m_device, output_size, m_output_buffer, m_output_memory)) {
            destroy_staging_buffers();
            return false;
        }
        if (vkMapMemory(m_device, m_input_memory, 0, input_size, 0, &m_input_mapping) != VK_SUCCESS ||
            vkMapMemory(m_device, m_output_memory, 0, output_size, 0, &m_output_mapping) != VK_SUCCESS) {
            destroy_staging_buffers();
            return false;
        }
        m_staging_request_capacity = capacity;
        return true;
    }

    bool run_exact_qualification()
    {
        // Qualification covers signs, reversed edges, non-zero origins and
        // high-but-safe fixed-point values before live geometry is accepted.
        std::vector<VulkanVerticalIntersectionRequest> requests;
        requests.reserve(2048);
        for (uint64_t index = 0; index < 2048; ++index) {
            const int64_t left = -500000 + int64_t(index % 97) * 101;
            const int64_t right = left + 1003 + int64_t(index % 89);
            const bool reverse = (index & 1) != 0;
            const int64_t ay = -750000 + int64_t(index) * 37;
            const int64_t by = 950000 - int64_t(index) * 53;
            const int64_t scan_x = left + 1 + int64_t(index % (right - left - 1));
            requests.push_back({
                { { reverse ? right : left, ay }, { reverse ? left : right, by } },
                scan_x, index
            });
        }

        const VulkanVerticalIntersectionBatch batch = dispatch_locked(requests);
        if (!batch.dispatched || batch.intersections.size() != requests.size()) {
            m_diagnostic = "Vulkan exact-integer qualification dispatch failed: " + batch.diagnostic;
            return false;
        }
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanVerticalIntersectionRequest& request = requests[index];
            const int64_t denominator = request.segment.b.x > request.segment.a.x ?
                request.segment.b.x - request.segment.a.x : request.segment.a.x - request.segment.b.x;
            const int64_t t_numerator = request.segment.b.x > request.segment.a.x ?
                request.scan_x - request.segment.a.x : request.segment.a.x - request.scan_x;
            const int64_t numerator = t_numerator * (request.segment.b.y - request.segment.a.y) +
                request.segment.a.y * denominator;
            const VulkanVerticalIntersection& result = batch.intersections[index];
            if (!result.valid || result.stable_id != request.stable_id ||
                result.numerator != numerator || result.denominator != denominator) {
                m_diagnostic = "Vulkan exact-integer qualification mismatch at vector " + std::to_string(index) + ".";
                return false;
            }
        }
        return true;
    }

    VulkanTreeContourBatch dispatch_tree_contours_locked(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_edges)
    {
        VulkanTreeContourBatch batch;
        auto fail = [&](const std::string& diagnostic) {
            batch.dispatched = false;
            batch.may_intersect.clear();
            batch.diagnostic = diagnostic;
            return batch;
        };
        if (!ensure_tree_pipeline())
            return fail("Vulkan tree contour pipeline is unavailable.");
        if (!ensure_tree_staging_buffers(requests.size(), contour_edges.size()))
            return fail("Vulkan tree contour workload exceeds its bounded staging capacity.");

        auto* packed_requests = static_cast<PackedTreeContourRequest*>(m_tree_request_mapping);
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanTreeContourRequest& request = requests[index];
            const uint32_t candidate_count = request.candidate_count == std::numeric_limits<uint32_t>::max() ?
                uint32_t(contour_edges.size()) : request.candidate_count;
            packed_requests[index] = { request.segment.a.x, request.segment.a.y,
                                       request.segment.b.x, request.segment.b.y, request.stable_id,
                                       request.first_candidate, candidate_count };
        }
        auto* packed_edges = static_cast<PackedTreeContourEdge*>(m_tree_edge_mapping);
        for (size_t index = 0; index < contour_edges.size(); ++index) {
            const Segment& edge = contour_edges[index];
            packed_edges[index] = { edge.a.x, edge.a.y, edge.b.x, edge.b.y };
        }

        const VkDescriptorBufferInfo request_info {
            m_tree_request_buffer, 0, VkDeviceSize(requests.size() * sizeof(PackedTreeContourRequest)) };
        const VkDescriptorBufferInfo edge_info {
            m_tree_edge_buffer, 0, VkDeviceSize(contour_edges.size() * sizeof(PackedTreeContourEdge)) };
        const VkDescriptorBufferInfo result_info {
            m_tree_result_buffer, 0, VkDeviceSize(requests.size() * sizeof(uint32_t)) };
        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &request_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &edge_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_tree_descriptor_set, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &result_info, nullptr }
        };
        vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);
        if (vkResetCommandBuffer(m_tree_command_buffer, 0) != VK_SUCCESS)
            return fail("Vulkan could not reset the tree contour command buffer.");
        VkCommandBufferBeginInfo begin_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_tree_command_buffer, &begin_info) != VK_SUCCESS)
            return fail("Vulkan could not begin the tree contour command buffer.");
        vkCmdBindPipeline(m_tree_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_tree_pipeline);
        vkCmdBindDescriptorSets(m_tree_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE,
                                m_tree_pipeline_layout, 0, 1, &m_tree_descriptor_set, 0, nullptr);
        vkCmdDispatch(m_tree_command_buffer, uint32_t(requests.size()), 1, 1);
        if (vkEndCommandBuffer(m_tree_command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not end the tree contour command buffer.");
        if (!submit_and_wait(m_tree_command_buffer, "tree contour dispatch"))
            return fail(m_diagnostic);

        const auto* result = static_cast<const uint32_t*>(m_tree_result_mapping);
        batch.may_intersect.resize(requests.size());
        for (size_t index = 0; index < requests.size(); ++index)
            batch.may_intersect[index] = result[index] != 0 ? 1 : 0;
        batch.dispatched = true;
        batch.diagnostic = "Vulkan tree contour broad phase completed on " + m_selected_device + ".";
        runtime_stats_registry().record_tree_contour_broad_phase(
            requests.size(), contour_edges.size(), batch.diagnostic);
        return batch;
    }

    VulkanVerticalIntersectionBatch dispatch_locked(const std::vector<VulkanVerticalIntersectionRequest>& requests)
    {
        const auto host_start = std::chrono::steady_clock::now();
        VulkanVerticalIntersectionBatch batch;
        auto finish = [&] {
            batch.host_elapsed_ms = std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - host_start).count();
            return batch;
        };
        auto fail = [&](const std::string& message) {
            batch.dispatched = false;
            batch.intersections.clear();
            batch.diagnostic = message;
            return finish();
        };
        if (requests.size() > m_max_requests_per_submission) {
            std::ostringstream message;
            message << "Vulkan vertical-intersection workload of " << requests.size()
                    << " requests exceeds this device's reusable-buffer limit of "
                    << m_max_requests_per_submission << ".";
            return fail(message.str());
        }
        if (!ensure_staging_buffers(requests.size()))
            return fail("Vulkan could not prepare reusable host-visible infill-intersection buffers.");

        const bool validate_cpu_reference =
            configured_validation_mode() != VulkanIntersectionValidationMode::Qualified;
        std::vector<Coord> y_origins(requests.size());
        std::vector<Coord> expected_numerators(validate_cpu_reference ? requests.size() : 0);
        std::vector<Coord> expected_denominators(requests.size());
        auto* input = static_cast<PackedVerticalIntersectionRequest*>(m_input_mapping);
        for (size_t index = 0; index < requests.size(); ++index) {
            const VulkanVerticalIntersectionRequest& request = requests[index];
            const WideCoord ax = request.segment.a.x;
            const WideCoord ay = request.segment.a.y;
            const WideCoord bx = request.segment.b.x;
            const WideCoord by = request.segment.b.y;
            const WideCoord scan_x = request.scan_x;
            const bool forward = bx > ax;
            const WideCoord denominator = forward ? bx - ax : ax - bx;
            const WideCoord t_numerator = forward ? scan_x - ax : ax - scan_x;
            if (denominator <= 0 || t_numerator <= 0 || t_numerator >= denominator)
                return fail("Vulkan batch rejected a non-interior scanline request; the whole batch remains on CPU.");

            const WideCoord x_origin = scan_x;
            const WideCoord y_origin = (ay + by) / 2;
            const WideCoord local_ax = ax - x_origin;
            const WideCoord local_bx = bx - x_origin;
            const WideCoord local_ay = ay - y_origin;
            const WideCoord local_by = by - y_origin;
            const WideCoord local_numerator = t_numerator * (local_by - local_ay) + local_ay * denominator;
            const WideCoord global_numerator = local_numerator + y_origin * denominator;
            if (!fits_gpu_coord(local_ax) || !fits_gpu_coord(local_bx) ||
                !fits_gpu_coord(local_ay) || !fits_gpu_coord(local_by) ||
                !fits_gpu_coord(y_origin) || !fits_gpu_coord(local_numerator) ||
                !fits_gpu_coord(global_numerator) || !fits_gpu_coord(denominator))
                return fail("Vulkan batch exceeded the exact signed-64-bit coordinate contract; the whole batch remains on CPU.");

            y_origins[index] = static_cast<Coord>(y_origin);
            if (validate_cpu_reference)
                expected_numerators[index] = static_cast<Coord>(global_numerator);
            expected_denominators[index] = static_cast<Coord>(denominator);
            input[index] = { static_cast<Coord>(local_ax), static_cast<Coord>(local_ay),
                             static_cast<Coord>(local_bx), static_cast<Coord>(local_by), 0,
                             request.stable_id };
        }

        const VkDeviceSize input_size = VkDeviceSize(requests.size() * sizeof(PackedVerticalIntersectionRequest));
        const VkDeviceSize output_size = VkDeviceSize(requests.size() * sizeof(PackedVerticalIntersectionResult));
        const VkDescriptorBufferInfo input_info { m_input_buffer, 0, input_size };
        const VkDescriptorBufferInfo output_info { m_output_buffer, 0, output_size };
        const VkWriteDescriptorSet writes[] = {
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descriptor_set, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &input_info, nullptr },
            { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, m_descriptor_set, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &output_info, nullptr }
        };
        vkUpdateDescriptorSets(m_device, uint32_t(std::size(writes)), writes, 0, nullptr);

        if (vkResetCommandBuffer(m_command_buffer, 0) != VK_SUCCESS)
            return fail("Vulkan could not reset the reusable infill command buffer.");
        VkCommandBufferBeginInfo begin_info { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        if (vkBeginCommandBuffer(m_command_buffer, &begin_info) != VK_SUCCESS)
            return fail("Vulkan could not begin the reusable infill command buffer.");
        if (m_compute_timestamps_available)
            vkCmdResetQueryPool(m_command_buffer, m_timestamp_query_pool, 0, 2);
        vkCmdBindPipeline(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline);
        vkCmdBindDescriptorSets(m_command_buffer, VK_PIPELINE_BIND_POINT_COMPUTE, m_pipeline_layout, 0, 1, &m_descriptor_set, 0, nullptr);
        if (m_compute_timestamps_available)
            vkCmdWriteTimestamp(m_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, 0);
        vkCmdDispatch(m_command_buffer, uint32_t((requests.size() + m_workgroup_size - 1) / m_workgroup_size), 1, 1);
        if (m_compute_timestamps_available)
            vkCmdWriteTimestamp(m_command_buffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, m_timestamp_query_pool, 1);
        if (vkEndCommandBuffer(m_command_buffer) != VK_SUCCESS)
            return fail("Vulkan could not end the reusable infill command buffer.");
        if (!submit_and_wait(m_command_buffer, "infill-intersection dispatch"))
            return fail(m_diagnostic);

        if (m_compute_timestamps_available) {
            uint64_t timestamps[2] {};
            if (vkGetQueryPoolResults(m_device, m_timestamp_query_pool, 0, 2, sizeof(timestamps), timestamps,
                                      sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                batch.gpu_elapsed_ms = double(timestamps[1] - timestamps[0]) * m_timestamp_period_ns / 1'000'000.0;
        }

        const auto* output = static_cast<const PackedVerticalIntersectionResult*>(m_output_mapping);
        batch.intersections.reserve(requests.size());
        for (size_t index = 0; index < requests.size(); ++index) {
            const PackedVerticalIntersectionResult& result = output[index];
            if (result.valid == 0 || result.stable_id != requests[index].stable_id ||
                result.denominator != expected_denominators[index])
                return fail("Vulkan result contract mismatch; the entire intersection batch was discarded.");
            const WideCoord restored_numerator = WideCoord(result.numerator) +
                WideCoord(y_origins[index]) * result.denominator;
            if (!fits_gpu_coord(restored_numerator) ||
                (validate_cpu_reference && static_cast<Coord>(restored_numerator) != expected_numerators[index]))
                return fail("Vulkan exact-reference mismatch; the entire intersection batch was discarded.");
            batch.intersections.push_back({ static_cast<Coord>(restored_numerator), result.denominator,
                                            result.stable_id, true });
        }
        batch.dispatched = true;
        batch.queue_submissions = 1;
        batch.diagnostic = "Vulkan exact vertical-intersection dispatch completed on " + m_selected_device + ".";
        return finish();
    }

    std::mutex     m_initialize_mutex;
    std::mutex     m_dispatch_mutex;
    std::chrono::steady_clock::time_point m_next_initialize_attempt {};
    bool           m_initialize_attempted { false };
    bool           m_device_faulted { false };
    VkInstance     m_instance { VK_NULL_HANDLE };
    VkPhysicalDevice m_physical_device { VK_NULL_HANDLE };
    VkDevice       m_device { VK_NULL_HANDLE };
    VkQueue        m_queue { VK_NULL_HANDLE };
    uint32_t       m_queue_family { UINT32_MAX };
    VkDescriptorSetLayout m_descriptor_set_layout { VK_NULL_HANDLE };
    VkPipelineLayout m_pipeline_layout { VK_NULL_HANDLE };
    VkPipeline     m_pipeline { VK_NULL_HANDLE };
    VkCommandPool  m_command_pool { VK_NULL_HANDLE };
    VkDescriptorPool m_descriptor_pool { VK_NULL_HANDLE };
    VkDescriptorSet m_descriptor_set { VK_NULL_HANDLE };
    VkCommandBuffer m_command_buffer { VK_NULL_HANDLE };
    VkDescriptorSetLayout m_tree_descriptor_set_layout { VK_NULL_HANDLE };
    VkPipelineLayout m_tree_pipeline_layout { VK_NULL_HANDLE };
    VkPipeline m_tree_pipeline { VK_NULL_HANDLE };
    VkDescriptorPool m_tree_descriptor_pool { VK_NULL_HANDLE };
    VkDescriptorSet m_tree_descriptor_set { VK_NULL_HANDLE };
    VkCommandBuffer m_tree_command_buffer { VK_NULL_HANDLE };
    VkFence         m_dispatch_fence { VK_NULL_HANDLE };
    VkQueryPool     m_timestamp_query_pool { VK_NULL_HANDLE };
    VkBuffer        m_input_buffer { VK_NULL_HANDLE };
    VkBuffer        m_output_buffer { VK_NULL_HANDLE };
    VkDeviceMemory  m_input_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_output_memory { VK_NULL_HANDLE };
    void*           m_input_mapping { nullptr };
    void*           m_output_mapping { nullptr };
    VkBuffer        m_tree_request_buffer { VK_NULL_HANDLE };
    VkBuffer        m_tree_edge_buffer { VK_NULL_HANDLE };
    VkBuffer        m_tree_result_buffer { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_request_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_edge_memory { VK_NULL_HANDLE };
    VkDeviceMemory  m_tree_result_memory { VK_NULL_HANDLE };
    void*           m_tree_request_mapping { nullptr };
    void*           m_tree_edge_mapping { nullptr };
    void*           m_tree_result_mapping { nullptr };
    size_t          m_staging_request_capacity { 0 };
    size_t          m_tree_request_capacity { 0 };
    size_t          m_tree_edge_capacity { 0 };
    size_t          m_initial_staging_request_capacity { kGenericStagingRequestCapacity };
    std::atomic_size_t m_preferred_intersection_batch { kDefaultPreferredIntersectionBatch };
    size_t          m_balanced_intersection_batch { kDefaultPreferredIntersectionBatch };
    size_t          m_max_requests_per_submission { 0 };
    size_t          m_storage_buffer_request_limit { 0 };
    uint32_t        m_workgroup_size { kDefaultWorkgroupSize };
    uint32_t        m_maximum_workgroup_size { 0 };
    uint32_t        m_max_compute_workgroup_count_x { 0 };
    double          m_timestamp_period_ns { 0.0 };
    std::string     m_selected_device;
    std::string     m_execution_profile;
    std::string     m_cpu_identifier;
    std::string     m_hardware_policy_class { "unclassified" };
    std::string     m_dispatch_policy_source { "not calibrated" };
    uint32_t        m_cpu_logical_threads { 1 };
    uint32_t        m_selected_vendor_id { 0 };
    uint32_t        m_selected_device_id { 0 };
    uint32_t        m_selected_driver_version { 0 };
    VkPhysicalDeviceType m_selected_device_type { VK_PHYSICAL_DEVICE_TYPE_OTHER };
    bool            m_is_gtx_1060 { false };
    bool            m_is_nvidia_rtx { false };
    bool            m_compute_timestamps_available { false };
    bool            m_gpu_throughput_is_extremely_slow { false };
    VulkanSlicerComputeMode m_compute_mode { VulkanSlicerComputeMode::Balanced };
    DispatchModeOverride m_dispatch_mode_override { DispatchModeOverride::None };
    bool           m_ready { false };
    std::string    m_diagnostic;

    static constexpr size_t kDefaultPreferredIntersectionBatch = 4096;
    static constexpr size_t kGpuPriorityMinimumIntersectionBatch = 256;
};

VulkanIntersectionContext& vulkan_intersection_context()
{
    // The Vulkan loader may already be torn down when C++ function-static
    // destructors run during wx/driver shutdown. Keep this small context alive
    // until Windows reclaims it with the process, instead of calling Vulkan
    // after the loader has unloaded. Runtime staging buffers are explicitly
    // released after every slice, so this is not a growing allocation.
    static auto* context = new VulkanIntersectionContext;
    return *context;
}

} // namespace
#endif

bool VulkanSlicerBackend::compiled_with_vulkan()
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    return true;
#else
    return false;
#endif
}

VulkanSlicerCapabilities VulkanSlicerBackend::query_capabilities()
{
    VulkanSlicerCapabilities capabilities;

#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    capabilities.compiled_with_vulkan = true;

    VkApplicationInfo application_info { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    application_info.pApplicationName = "MagpieSlicer";
    application_info.applicationVersion = 1;
    application_info.pEngineName = "MagpieSlicer";
    application_info.engineVersion = 1;
    application_info.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo instance_info { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    instance_info.pApplicationInfo = &application_info;

    VkInstance instance = VK_NULL_HANDLE;
    const VkResult create_result = vkCreateInstance(&instance_info, nullptr, &instance);
    if (create_result != VK_SUCCESS) {
        capabilities.diagnostic = "Vulkan instance creation failed (" + std::to_string(create_result) + ")";
        return capabilities;
    }

    capabilities.loader_available = true;
    uint32_t device_count = 0;
    if (vkEnumeratePhysicalDevices(instance, &device_count, nullptr) != VK_SUCCESS || device_count == 0) {
        vkDestroyInstance(instance, nullptr);
        capabilities.diagnostic = "No Vulkan compute device is available";
        return capabilities;
    }

    std::vector<VkPhysicalDevice> physical_devices(device_count);
    if (vkEnumeratePhysicalDevices(instance, &device_count, physical_devices.data()) != VK_SUCCESS) {
        vkDestroyInstance(instance, nullptr);
        capabilities.diagnostic = "Vulkan device enumeration failed";
        return capabilities;
    }

    capabilities.devices.reserve(physical_devices.size());
    for (VkPhysicalDevice physical_device : physical_devices) {
        VkPhysicalDeviceSubgroupProperties subgroup_properties { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
        VkPhysicalDeviceProperties2 properties2 { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        properties2.pNext = &subgroup_properties;
        VkPhysicalDeviceFeatures features {};
        vkGetPhysicalDeviceProperties2(physical_device, &properties2);
        vkGetPhysicalDeviceFeatures(physical_device, &features);

        VulkanDeviceInfo info;
        info.name = properties2.properties.deviceName;
        info.vendor_id = properties2.properties.vendorID;
        info.device_id = properties2.properties.deviceID;
        info.discrete = properties2.properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
        info.shader_int64 = features.shaderInt64 == VK_TRUE;
        info.subgroup_operations = subgroup_properties.supportedStages != 0;
        info.max_workgroup_invocations = properties2.properties.limits.maxComputeWorkGroupInvocations;
        info.max_workgroup_size_x = properties2.properties.limits.maxComputeWorkGroupSize[0];
        info.max_workgroup_count_x = properties2.properties.limits.maxComputeWorkGroupCount[0];
        info.max_storage_buffer_range = properties2.properties.limits.maxStorageBufferRange;

        uint32_t queue_family_count = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
        std::vector<VkQueueFamilyProperties> queue_families(queue_family_count);
        if (queue_family_count != 0) {
            vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());
            info.compute_queue = std::any_of(queue_families.begin(), queue_families.end(), [](const VkQueueFamilyProperties& family) {
                return (family.queueFlags & VK_QUEUE_COMPUTE_BIT) != 0 && family.queueCount != 0;
            });
        }
        capabilities.devices.emplace_back(std::move(info));
    }
    vkDestroyInstance(instance, nullptr);

    std::stable_sort(capabilities.devices.begin(), capabilities.devices.end(), [](const VulkanDeviceInfo& lhs, const VulkanDeviceInfo& rhs) {
        if (lhs.discrete != rhs.discrete)
            return lhs.discrete > rhs.discrete;
        if (lhs.shader_int64 != rhs.shader_int64)
            return lhs.shader_int64 > rhs.shader_int64;
        if (lhs.max_workgroup_invocations != rhs.max_workgroup_invocations)
            return lhs.max_workgroup_invocations > rhs.max_workgroup_invocations;
        if (lhs.max_storage_buffer_range != rhs.max_storage_buffer_range)
            return lhs.max_storage_buffer_range > rhs.max_storage_buffer_range;
        if (lhs.vendor_id != rhs.vendor_id)
            return lhs.vendor_id < rhs.vendor_id;
        return lhs.device_id < rhs.device_id;
    });

    std::ostringstream diagnostic;
    diagnostic << "Detected " << capabilities.devices.size() << " Vulkan device(s); "
               << "only compute-queue and shaderInt64 devices are eligible for exact tiled geometry.";
    capabilities.diagnostic = diagnostic.str();
#else
    capabilities.diagnostic = "Vulkan slicer was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.";
#endif

    return capabilities;
}

VulkanVerticalIntersectionBatch VulkanSlicerBackend::dispatch_vertical_intersections(
    const std::vector<VulkanVerticalIntersectionRequest>& requests)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled()) {
        VulkanVerticalIntersectionBatch batch;
        batch.diagnostic = "Vulkan infill compute is disabled in Preferences.";
        return batch;
    }
#ifdef MAGPIE_SLICING_PROFILER
    ScopedSlicingProfileEvent profile_event("vulkan", "Exact vertical intersections",
                                            SlicingProfileBackend::GPU, requests.size());
#endif
    VulkanVerticalIntersectionBatch batch = vulkan_intersection_context().dispatch(requests);
#ifdef MAGPIE_SLICING_PROFILER
    profile_event.set_result(batch.dispatched ? SlicingProfileBackend::GPU : SlicingProfileBackend::CPUFallback,
                             batch.gpu_elapsed_ms, requests.size(), batch.diagnostic);
#endif
    if (!requests.empty())
        runtime_stats_registry().record_dispatch(requests.size(), batch);
    if (batch.dispatched)
        record_automated_verification_dispatch("vertical", requests.size());
    return batch;
#else
    VulkanVerticalIntersectionBatch batch;
    batch.diagnostic = "Vulkan infill compute was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.";
    return batch;
#endif
}

VulkanTreeContourBatch VulkanSlicerBackend::dispatch_tree_contour_candidates(
    const std::vector<VulkanTreeContourRequest>& requests,
    const std::vector<Segment>& contour_edges)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled()) {
        VulkanTreeContourBatch batch;
        batch.diagnostic = "Vulkan tree contour compute is disabled in Preferences.";
        return batch;
    }
#ifdef MAGPIE_SLICING_PROFILER
    ScopedSlicingProfileEvent profile_event("vulkan", "Tree contour broad phase",
                                            SlicingProfileBackend::GPU, requests.size());
#endif
    VulkanTreeContourBatch batch = vulkan_intersection_context().dispatch_tree_contours(requests, contour_edges);
#ifdef MAGPIE_SLICING_PROFILER
    profile_event.set_result(batch.dispatched ? SlicingProfileBackend::Hybrid : SlicingProfileBackend::CPUFallback,
                             -1.0, requests.size(), batch.diagnostic);
#endif
    if (batch.dispatched)
        record_automated_verification_dispatch("tree", requests.size());
    return batch;
#else
    VulkanTreeContourBatch batch;
    batch.diagnostic = "Vulkan tree contour compute was not compiled.";
    return batch;
#endif
}

VulkanAabbBatch VulkanSlicerBackend::dispatch_indexed_aabb_candidates(
    const std::vector<VulkanAabb>& queries,
    const std::vector<VulkanAabb>& targets,
    Coord cell_size,
    VulkanAabbOperation operation)
{
    VulkanAabbBatch batch;
    if (!compute_enabled()) {
        batch.diagnostic = "Vulkan spatial candidate compute is disabled in Preferences.";
        return batch;
    }
    if (queries.empty() || targets.empty()) {
        batch.may_overlap.assign(queries.size(), uint8_t(0));
        batch.resolved = true;
        batch.diagnostic = "Spatial candidate broad phase has no query or target boxes.";
        return batch;
    }
    const VulkanSlicerComputeMode compute_mode = VulkanSlicerBackend::compute_mode();
    const size_t minimum_pair_count = compute_mode == VulkanSlicerComputeMode::Maximum ? 1 :
        (compute_mode == VulkanSlicerComputeMode::Priority ? 128 : 16 * 1024);
    const size_t maximum_compact_pairs = compute_mode == VulkanSlicerComputeMode::Maximum ? 4 * 1024 * 1024 :
        (compute_mode == VulkanSlicerComputeMode::Priority ? 1024 * 1024 : 128 * 1024);
    bool force_dispatch = false;
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    force_dispatch = g_force_dispatch_for_automated_verification;
#endif
    if (queries.size() > std::numeric_limits<size_t>::max() / targets.size() ||
        (!force_dispatch && queries.size() * targets.size() < minimum_pair_count)) {
        batch.diagnostic = "Spatial candidate broad phase retained on CPU for a small workload.";
        return batch;
    }

    Point global_min = targets.front().min;
    Point global_max = targets.front().max;
    for (const VulkanAabb& target : targets) {
        global_min.x = std::min(global_min.x, target.min.x);
        global_min.y = std::min(global_min.y, target.min.y);
        global_max.x = std::max(global_max.x, target.max.x);
        global_max.y = std::max(global_max.y, target.max.y);
    }
    if (cell_size <= 0) {
        const Coord extent_x = std::max<Coord>(1, global_max.x - global_min.x);
        const Coord extent_y = std::max<Coord>(1, global_max.y - global_min.y);
        const Coord axis_cells = std::max<Coord>(1, Coord(std::ceil(std::sqrt(double(targets.size())))));
        cell_size = std::max<Coord>(1, std::max(extent_x, extent_y) / axis_cells);
    }

    using Cell = std::pair<int64_t, int64_t>;
    constexpr uint64_t maximum_cells_per_box = 4096;
    auto floor_div = [cell_size](int64_t value) {
        int64_t quotient = value / cell_size;
        const int64_t remainder = value % cell_size;
        if (remainder < 0)
            --quotient;
        return quotient;
    };
    auto cell_range = [&](const VulkanAabb& box) {
        return std::array<int64_t, 4> { floor_div(std::min(box.min.x, box.max.x)),
                                        floor_div(std::min(box.min.y, box.max.y)),
                                        floor_div(std::max(box.min.x, box.max.x)),
                                        floor_div(std::max(box.min.y, box.max.y)) };
    };
    auto range_cell_count = [](const std::array<int64_t, 4>& range) -> uint64_t {
        const uint64_t width = uint64_t(range[2] - range[0]) + 1;
        const uint64_t height = uint64_t(range[3] - range[1]) + 1;
        return width > std::numeric_limits<uint64_t>::max() / height ?
            std::numeric_limits<uint64_t>::max() : width * height;
    };

    std::map<Cell, std::vector<uint32_t>> grid;
    std::vector<uint32_t> global_targets;
    for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
        if (target_index > std::numeric_limits<uint32_t>::max()) {
            batch.diagnostic = "Spatial candidate target count exceeds the deterministic index limit.";
            return batch;
        }
        const auto range = cell_range(targets[target_index]);
        if (range_cell_count(range) > maximum_cells_per_box) {
            global_targets.push_back(uint32_t(target_index));
            continue;
        }
        for (int64_t y = range[1]; y <= range[3]; ++y)
            for (int64_t x = range[0]; x <= range[2]; ++x)
                grid[{ x, y }].push_back(uint32_t(target_index));
    }

    std::vector<VulkanTreeContourRequest> compact_queries;
    std::vector<Segment> compact_targets;
    std::vector<VulkanAabbBatch::OverlapPair> compact_pair_sources;
    const bool return_overlap_pairs = operation == VulkanAabbOperation::SeamTravel ||
                                      operation == VulkanAabbOperation::ClassicWall ||
                                      operation == VulkanAabbOperation::CuraSupport ||
                                      operation == VulkanAabbOperation::ArachneWall;
    compact_queries.reserve(queries.size());
    std::vector<uint32_t> candidates;
    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        const VulkanAabb& query = queries[query_index];
        const auto range = cell_range(query);
        candidates.clear();
        candidates.insert(candidates.end(), global_targets.begin(), global_targets.end());
        if (range_cell_count(range) > maximum_cells_per_box) {
            candidates.resize(targets.size());
            for (size_t index = 0; index < targets.size(); ++index)
                candidates[index] = uint32_t(index);
        } else {
            for (int64_t y = range[1]; y <= range[3]; ++y) {
                for (int64_t x = range[0]; x <= range[2]; ++x) {
                    const auto found = grid.find({ x, y });
                    if (found != grid.end())
                        candidates.insert(candidates.end(), found->second.begin(), found->second.end());
                }
            }
            std::sort(candidates.begin(), candidates.end());
            candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        }
        if (candidates.size() > maximum_compact_pairs - compact_targets.size()) {
            batch.diagnostic = "Spatial candidate batch exceeds the bounded Vulkan staging capacity.";
            return batch;
        }
        if (return_overlap_pairs) {
            for (const uint32_t target_index : candidates) {
                const uint32_t candidate_index = uint32_t(compact_targets.size());
                const VulkanAabb& target = targets[target_index];
                compact_targets.push_back({ target.min, target.max });
                compact_queries.push_back({ { query.min, query.max }, uint64_t(query_index),
                                            candidate_index, 1 });
                compact_pair_sources.push_back({ uint32_t(query_index), target_index });
            }
        } else {
            const uint32_t first_candidate = uint32_t(compact_targets.size());
            for (const uint32_t target_index : candidates) {
                const VulkanAabb& target = targets[target_index];
                compact_targets.push_back({ target.min, target.max });
            }
            compact_queries.push_back({ { query.min, query.max }, uint64_t(query_index),
                                        first_candidate, uint32_t(candidates.size()) });
        }
    }

    batch.candidate_pairs = compact_targets.size();
    if (compact_targets.empty()) {
        batch.may_overlap.assign(queries.size(), uint8_t(0));
        batch.resolved = true;
        batch.diagnostic = "Spatial index proved that no query can overlap a target.";
        return batch;
    }
    VulkanTreeContourBatch indexed;
    const char* operation_name = "spatial";
    switch (operation) {
    case VulkanAabbOperation::TreeSupport: operation_name = "tree-spatial"; break;
    case VulkanAabbOperation::DistanceField: operation_name = "distance-spatial"; break;
    case VulkanAabbOperation::Gyroid: operation_name = "gyroid-spatial"; break;
    case VulkanAabbOperation::SeamTravel: operation_name = "seam-travel-spatial"; break;
    case VulkanAabbOperation::ClassicWall: operation_name = "classic-wall-spatial"; break;
    case VulkanAabbOperation::CuraSupport: operation_name = "cura-support-spatial"; break;
    case VulkanAabbOperation::ArachneWall: operation_name = "arachne-wall-spatial"; break;
    case VulkanAabbOperation::Spatial: break;
    }
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
#ifdef MAGPIE_SLICING_PROFILER
    ScopedSlicingProfileEvent profile_event("vulkan", operation_name,
                                            SlicingProfileBackend::GPU, compact_targets.size());
#endif
    indexed = vulkan_intersection_context().dispatch_tree_contours(compact_queries, compact_targets);
#ifdef MAGPIE_SLICING_PROFILER
    profile_event.set_result(indexed.dispatched ? SlicingProfileBackend::Hybrid : SlicingProfileBackend::CPUFallback,
                             -1.0, compact_targets.size(), indexed.diagnostic);
#endif
    if (indexed.dispatched) {
        record_automated_verification_dispatch(operation_name, compact_targets.size());
    }
#else
    indexed.diagnostic = "Vulkan spatial candidate compute was not compiled.";
#endif
    if (indexed.dispatched && indexed.may_intersect.size() == compact_queries.size()) {
        auto boxes_overlap = [](const Segment& query, const Segment& target) {
            const int64_t query_min_x = std::min(query.a.x, query.b.x);
            const int64_t query_max_x = std::max(query.a.x, query.b.x);
            const int64_t query_min_y = std::min(query.a.y, query.b.y);
            const int64_t query_max_y = std::max(query.a.y, query.b.y);
            const int64_t target_min_x = std::min(target.a.x, target.b.x);
            const int64_t target_max_x = std::max(target.a.x, target.b.x);
            const int64_t target_min_y = std::min(target.a.y, target.b.y);
            const int64_t target_max_y = std::max(target.a.y, target.b.y);
            return query_min_x <= target_max_x && target_min_x <= query_max_x &&
                   query_min_y <= target_max_y && target_min_y <= query_max_y;
        };
        for (size_t compact_query_index = 0; compact_query_index < compact_queries.size(); ++compact_query_index) {
            if (indexed.may_intersect[compact_query_index] != 0 ||
                !should_validate_vertical_intersection(compact_query_index, compact_queries.size()))
                continue;
            const VulkanTreeContourRequest& query = compact_queries[compact_query_index];
            bool cpu_may_overlap = false;
            const size_t candidate_end = size_t(query.first_candidate) + size_t(query.candidate_count);
            for (size_t candidate_index = query.first_candidate;
                 candidate_index < candidate_end; ++candidate_index) {
                if (boxes_overlap(query.segment, compact_targets[candidate_index])) {
                    cpu_may_overlap = true;
                    break;
                }
            }
            if (cpu_may_overlap) {
                indexed.dispatched = false;
                indexed.may_intersect.clear();
                indexed.diagnostic = "Vulkan spatial validation failed; the entire candidate batch was discarded.";
                break;
            }
        }
    }
    batch.dispatched = indexed.dispatched;
    batch.resolved = indexed.dispatched;
    batch.diagnostic = indexed.diagnostic;
    if (indexed.dispatched) {
        if (return_overlap_pairs) {
            batch.may_overlap.assign(queries.size(), uint8_t(0));
            for (size_t index = 0; index < indexed.may_intersect.size(); ++index) {
                if (indexed.may_intersect[index] == 0)
                    continue;
                const VulkanAabbBatch::OverlapPair pair = compact_pair_sources[index];
                batch.may_overlap[pair.query] = uint8_t(1);
                batch.overlap_pairs.push_back(pair);
            }
        } else {
            batch.may_overlap = std::move(indexed.may_intersect);
        }
    }
    return batch;
}

bool VulkanSlicerBackend::should_dispatch_vertical_intersections(size_t request_count)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled())
        return false;
    if (g_force_dispatch_for_automated_verification)
        return request_count != 0;
    return vulkan_intersection_context().should_dispatch_vertical_intersections(request_count);
#else
    return false;
#endif
}

bool VulkanSlicerBackend::prepare_for_slicing()
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!compute_enabled()) {
        runtime_stats_registry().set_backend({}, "Disabled",
            "Vulkan compute is disabled in Preferences; using the exact CPU geometry path.",
            0, 0, 0);
        return false;
    }
    return vulkan_intersection_context().prepare_for_slicing();
#else
    runtime_stats_registry().set_backend({}, {},
        "Vulkan infill compute was not compiled; configure with -DSLIC3R_ENABLE_VULKAN_SLICER=ON.",
        0, 0, 0);
    return false;
#endif
}

void VulkanSlicerBackend::set_compute_enabled(bool enabled)
{
    g_compute_enabled.store(enabled, std::memory_order_release);
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    if (!enabled) {
        // Do not tear the context down here: the preference can change while a
        // worker is completing a slice. Post-slice cleanup owns staging
        // reclamation and a later enabled slice can reuse its tuned pipeline.
        runtime_stats_registry().set_backend({}, "Disabled",
            "Vulkan compute is disabled in Preferences; using the exact CPU geometry path.",
            0, 0, 0);
    }
#endif
}

bool VulkanSlicerBackend::compute_enabled()
{
    return g_compute_enabled.load(std::memory_order_acquire);
}

void VulkanSlicerBackend::set_compute_mode(VulkanSlicerComputeMode mode)
{
    g_compute_mode.store(mode, std::memory_order_release);
}

VulkanSlicerComputeMode VulkanSlicerBackend::compute_mode()
{
    return g_compute_mode.load(std::memory_order_acquire);
}

void VulkanSlicerBackend::begin_slicing_session()
{
    runtime_stats_registry().begin_slice();
}

void VulkanSlicerBackend::release_unused_staging_memory(bool force)
{
#ifdef SLIC3R_ENABLE_VULKAN_SLICER
    vulkan_intersection_context().release_unused_staging_memory(force);
#endif
}

VulkanIntersectionValidationMode VulkanSlicerBackend::vertical_intersection_validation_mode()
{
    return configured_validation_mode();
}

bool VulkanSlicerBackend::should_validate_vertical_intersection(size_t index, size_t count)
{
    const VulkanIntersectionValidationMode mode = configured_validation_mode();
    if (mode == VulkanIntersectionValidationMode::Strict)
        return true;
    if (mode == VulkanIntersectionValidationMode::Qualified)
        return false;
    // The in-process qualification has already checked broad signed-integer
    // vectors. Keep a cheap guard at both boundaries and throughout long
    // real-model batches without redoing every GPU multiply/add on the CPU.
    return index == 0 || index + 1 == count || (index % 512) == 0;
}

void VulkanSlicerBackend::note_skipped_vertical_intersection_workload(size_t request_count)
{
    if (request_count != 0)
        runtime_stats_registry().record_skipped(request_count);
}

void VulkanSlicerBackend::note_vertical_intersection_usage(size_t accepted_gpu_results,
                                                            size_t cpu_validation_checks,
                                                            bool validation_failed,
                                                            const std::string& diagnostic)
{
    runtime_stats_registry().record_usage(accepted_gpu_results, cpu_validation_checks,
                                          validation_failed, diagnostic);
}

VulkanSlicerRuntimeStats VulkanSlicerBackend::query_runtime_stats()
{
    return runtime_stats_registry().snapshot();
}

std::string VulkanSlicerBackend::runtime_diagnostic_report()
{
    const VulkanSlicerRuntimeStats stats = query_runtime_stats();
    std::ostringstream report;
    report << "Vulkan slicer runtime diagnostics\n"
           << "device: " << (stats.selected_device.empty() ? "not initialized" : stats.selected_device) << '\n'
           << "profile: " << (stats.execution_profile.empty() ? "not initialized" : stats.execution_profile) << '\n'
           << "validation: " << stats.validation_mode << '\n'
           << "workgroup: " << stats.configured_workgroup_size << " configured / "
           << stats.maximum_workgroup_size << " device maximum\n"
           << "reusable staging: " << stats.reusable_staging_capacity << " requests\n"
           << "preferred intersection batch: " << stats.preferred_intersection_batch << " requests\n"
           << "dispatch calls: " << stats.dispatch_calls << ", queue submissions: " << stats.queue_submissions << '\n'
           << "submitted intersections: " << stats.submitted_intersections
           << ", GPU accepted: " << stats.accepted_gpu_intersections << '\n'
           << "submitted batch range: " << stats.smallest_submitted_intersection_batch
           << ".." << stats.largest_submitted_intersection_batch << " requests\n"
           << "CPU validation checks: " << stats.cpu_validation_checks
           << ", failures: " << stats.validation_failures << '\n'
           << "small workloads skipped: " << stats.skipped_small_workloads
           << " (" << stats.skipped_intersections << " intersections, average "
           << (stats.skipped_small_workloads == 0 ? 0 :
               stats.skipped_intersections / stats.skipped_small_workloads)
           << ", largest " << stats.largest_skipped_intersection_batch << ")\n"
           << "last GPU time: " << stats.last_gpu_ms << " ms, last host time: " << stats.last_host_ms << " ms\n"
           << "last status: " << stats.last_diagnostic;
    return report.str();
}

} // namespace Slic3r::Gpu
