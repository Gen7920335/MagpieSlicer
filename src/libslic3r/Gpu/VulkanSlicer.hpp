#pragma once

#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include "GpuExactGeometry.hpp"

namespace Slic3r::Gpu {

struct VulkanDeviceInfo {
    std::string name;
    uint32_t    vendor_id { 0 };
    uint32_t    device_id { 0 };
    bool        discrete { false };
    bool        compute_queue { false };
    bool        shader_int64 { false };
    bool        subgroup_operations { false };
    uint32_t    max_workgroup_invocations { 0 };
    uint32_t    max_workgroup_size_x { 0 };
    uint32_t    max_workgroup_count_x { 0 };
    uint64_t    max_storage_buffer_range { 0 };
};

struct VulkanSlicerCapabilities {
    bool                     compiled_with_vulkan { false };
    bool                     loader_available { false };
    std::string              diagnostic;
    std::vector<VulkanDeviceInfo> devices;
};

// One request represents the exact rational intersection of a polygon edge
// and a vertical infill scanline.  The caller only submits strict interior
// intersections; contour-vertex rules stay on the CPU because they depend on
// neighbouring edges and exact winding rules.
struct VulkanVerticalIntersectionRequest {
    Segment  segment;
    Coord    scan_x { 0 };
    uint64_t stable_id { 0 };
};

struct VulkanVerticalIntersection {
    Coord    numerator { 0 };
    Coord    denominator { 1 };
    uint64_t stable_id { 0 };
    bool     valid { false };
};

struct VulkanVerticalIntersectionBatch {
    bool                                    dispatched { false };
    std::string                             diagnostic;
    // GPU timestamp-query time, excluding host-side request packing and the
    // synchronization wait. A negative value means the selected driver does
    // not expose compute timestamps.
    double                                  gpu_elapsed_ms { -1.0 };
    double                                  host_elapsed_ms { 0.0 };
    uint32_t                                queue_submissions { 0 };
    std::vector<VulkanVerticalIntersection> intersections;
};

// Tree-support branch movement asks whether many candidate branch segments
// can cross a layer contour. Vulkan performs only a conservative bounding-box
// broad phase: a false result proves that no contour segment can intersect;
// a true result stays on the established CPU Clipper path for the exact test.
struct VulkanTreeContourRequest {
    Segment  segment;
    uint64_t stable_id { 0 };
    uint32_t first_candidate { 0 };
    uint32_t candidate_count { std::numeric_limits<uint32_t>::max() };
};

struct VulkanTreeContourBatch {
    bool                 dispatched { false };
    std::string          diagnostic;
    std::vector<uint8_t> may_intersect;
};

struct VulkanAabb {
    Point min;
    Point max;
};

struct VulkanAabbBatch {
    struct OverlapPair {
        uint32_t query { 0 };
        uint32_t target { 0 };

        bool operator==(const OverlapPair& rhs) const { return query == rhs.query && target == rhs.target; }
    };

    bool                 resolved { false };
    bool                 dispatched { false };
    std::string          diagnostic;
    std::vector<uint8_t> may_overlap;
    // Populated for operation-specific 6542 callers. Pairs are stable-sorted
    // by query and target; callers still perform their established exact
    // topology test before changing a toolpath.
    std::vector<OverlapPair> overlap_pairs;
    size_t               candidate_pairs { 0 };
};

enum class VulkanAabbOperation {
    Spatial,
    TreeSupport,
    DistanceField,
    Gyroid,
    SeamTravel,
    ClassicWall,
    CuraSupport,
    ArachneWall
};

// The accelerated scan-conversion path must remain observable while it is
// experimental. These counters are process-local and deliberately do not
// affect the generated G-code.
struct VulkanSlicerRuntimeStats {
    uint64_t    dispatch_calls { 0 };
    uint64_t    queue_submissions { 0 };
    uint64_t    submitted_intersections { 0 };
    uint64_t    accepted_gpu_intersections { 0 };
    uint64_t    cpu_validation_checks { 0 };
    uint64_t    validation_failures { 0 };
    uint64_t    skipped_small_workloads { 0 };
    uint64_t    skipped_intersections { 0 };
    size_t      smallest_submitted_intersection_batch { 0 };
    size_t      largest_submitted_intersection_batch { 0 };
    size_t      largest_skipped_intersection_batch { 0 };
    double      total_gpu_ms { 0.0 };
    double      total_host_ms { 0.0 };
    double      last_gpu_ms { -1.0 };
    double      last_host_ms { 0.0 };
    uint32_t    configured_workgroup_size { 0 };
    uint32_t    maximum_workgroup_size { 0 };
    size_t      reusable_staging_capacity { 0 };
    size_t      preferred_intersection_batch { 4096 };
    std::string selected_device;
    std::string execution_profile;
    // A concise human-readable label for the last compute phase. The progress
    // notification uses this rather than exposing a driver diagnostic there.
    std::string current_operation;
    std::string validation_mode;
    std::string last_diagnostic;
};

enum class VulkanIntersectionValidationMode {
    // Compare every GPU result to the CPU reference. This is the audit mode
    // for a new driver or a correctness investigation.
    Strict,
    // The default performance mode after the in-process qualification test:
    // check the first, last, and periodic results of every batch.
    Sampled,
    // Maximum mode relies on exact startup qualification and result contracts
    // without repeating intersection arithmetic on the CPU.
    Qualified
};

enum class VulkanSlicerComputeMode {
    Balanced,
    Priority,
    Maximum
};

// This capability layer is intentionally independent of the slicing pipeline.
// It lets the UI select a GPU only when exact-integer prerequisites are met;
// the CPU pipeline remains the authoritative fallback for every stage.
class VulkanSlicerBackend {
public:
    static bool compiled_with_vulkan();
    static VulkanSlicerCapabilities query_capabilities();

    // User-facing preference. Disabling this prevents both initialization and
    // dispatch of the experimental compute path; the established CPU geometry
    // pipeline remains fully available.
    static void set_compute_enabled(bool enabled);
    static bool compute_enabled();

    // Auto, On and Max GPU map to one policy value so transitions cannot leave
    // independent acceleration flags in an inconsistent state.
    static void set_compute_mode(VulkanSlicerComputeMode mode);
    static VulkanSlicerComputeMode compute_mode();

    // Clears per-slice counters while retaining the qualified device and
    // pipeline. The progress UI can then report this slice's actual GPU work
    // instead of carrying a stale job label into a later CPU-only stage.
    static void begin_slicing_session();

    // Creates, autotunes and exact-qualifies the compute context before the
    // first infill workload. Call this from the slicing worker so the progress
    // UI can distinguish "ready but idle" from an initialization failure.
    static bool prepare_for_slicing();

    // Retains bounded host-visible buffers for the next interactive slice and
    // releases only oversized allocations. Interrupted slices force a complete
    // staging cleanup while preserving the qualified device and pipelines.
    static void release_unused_staging_memory(bool force = false);

    // Executes the high-cardinality, fixed-point portion of rectilinear and
    // support-infill scan conversion. Results are returned in input order;
    // startup qualification and the selected live validation policy protect
    // every intersection before it is committed to a toolpath.
    static VulkanVerticalIntersectionBatch dispatch_vertical_intersections(
        const std::vector<VulkanVerticalIntersectionRequest>& requests);

    // Conservative tree-support broad phase. Inputs and results preserve
    // request order. Empty output means that the CPU must retain all queries.
    static VulkanTreeContourBatch dispatch_tree_contour_candidates(
        const std::vector<VulkanTreeContourRequest>& requests,
        const std::vector<Segment>& contour_edges);

    // Deterministic spatial broad phase shared by support, distance-field and
    // non-rectilinear fill callers. A CPU-built uniform grid compacts each
    // query to its local target range before Vulkan evaluates the remaining
    // integer AABB pairs. A false result is exact; true remains on the caller's
    // established CPU geometry path.
    static VulkanAabbBatch dispatch_indexed_aabb_candidates(
        const std::vector<VulkanAabb>& queries,
        const std::vector<VulkanAabb>& targets,
        Coord cell_size = 0,
        VulkanAabbOperation operation = VulkanAabbOperation::Spatial);

    // Calibrated at Vulkan initialization from this CPU's fixed-point
    // throughput and the selected GPU's real submission latency.
    static bool should_dispatch_vertical_intersections(size_t request_count);

    // GPU output is exact signed-integer arithmetic. Sampled validation is the
    // production default; automated verification opts into a full CPU
    // reference through MAGPIE_VULKAN_SLICER_VALIDATION=strict.
    static VulkanIntersectionValidationMode vertical_intersection_validation_mode();
    static bool should_validate_vertical_intersection(size_t index, size_t count);

    static void note_skipped_vertical_intersection_workload(size_t request_count);
    static void note_vertical_intersection_usage(size_t accepted_gpu_results,
                                                size_t cpu_validation_checks,
                                                bool validation_failed,
                                                const std::string& diagnostic);
    static VulkanSlicerRuntimeStats query_runtime_stats();
    static std::string runtime_diagnostic_report();

    static bool supports_deterministic_geometry(const VulkanDeviceInfo& device)
    {
        return device.compute_queue && device.shader_int64;
    }
};

} // namespace Slic3r::Gpu
