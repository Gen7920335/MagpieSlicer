#pragma once

#include "VulkanSlicer.hpp"
#include "../SlicingProfiler.hpp"

namespace Slic3r::Gpu {
struct CudaDistanceBatch {
    bool resolved {false};
    std::vector<double> distances_squared;
    std::string diagnostic;
};
struct CudaMeshZRequest { float z0, z1, z2; };
struct CudaMeshZRange { uint32_t first, last, lowest_vertex; };
struct CudaMeshZBatch {
    bool resolved {false};
    std::vector<CudaMeshZRange> ranges;
    std::string diagnostic;
};
// rings[0] is the outer contour; subsequent rings are holes. Polygon union,
// outer border included and hole border included match ExPolygon::contains.
struct CudaPolygonContours { std::vector<std::vector<Point>> rings; };
struct CudaContainmentBatch {
    bool resolved {false};
    std::vector<uint8_t> inside;
    std::string diagnostic;
};
// Reuse the existing exact-intersection request/result protocol. This backend
// does not initialize, dispatch, or rely on Vulkan.
class CudaSlicerBackend {
public:
    // CLI/self-tests use the environment; GUI passes an explicit saved choice.
    static void begin_slicing_session();
    static void begin_slicing_session(bool enable, bool force_dispatch = false, bool skip_cpu_validation = false);
    static bool compiled_with_cuda();
    static bool enabled();
    static bool skips_cpu_validation();
    static bool should_dispatch(size_t request_count);
    static bool operation_enabled(const char* operation);
    static CudaContainmentBatch dispatch_points_in_polygons(
        const std::vector<Point>& points, const std::vector<CudaPolygonContours>& polygons,
        const std::vector<uint8_t>& cpu_reference);
    static VulkanAabbBatch dispatch_aabb_pairs(
        const std::vector<VulkanAabb>& queries, const std::vector<VulkanAabb>& targets);
    static CudaDistanceBatch dispatch_point_segment_distances(
        const std::vector<Point>& points, const std::vector<Segment>& edges,
        const std::vector<double>& cpu_reference);
    static CudaMeshZBatch dispatch_mesh_layer_ranges(
        const std::vector<CudaMeshZRequest>& facets, const std::vector<float>& zs);
    static VulkanAabbBatch dispatch_indexed_aabb_candidates(
        const std::vector<VulkanAabb>& queries, const std::vector<VulkanAabb>& targets,
        Coord cell_size = 0, VulkanAabbOperation operation = VulkanAabbOperation::Spatial);
    static VulkanVerticalIntersectionBatch dispatch_vertical_intersections(
        const std::vector<VulkanVerticalIntersectionRequest>& requests);
    static SlicingProfileGpuStats runtime_stats();
    static void note_cpu_fallback(double wall_ms);
};
}
