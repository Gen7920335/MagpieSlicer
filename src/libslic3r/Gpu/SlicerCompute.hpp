#pragma once
#include "CudaSlicer.hpp"

namespace Slic3r::Gpu {
// Explicit CUDA selection takes priority; Vulkan policy is unchanged otherwise.
struct SlicerCompute {
    static bool compute_enabled() { return CudaSlicerBackend::enabled() || VulkanSlicerBackend::compute_enabled(); }
    static VulkanAabbBatch dispatch_indexed_aabb_candidates(
        const std::vector<VulkanAabb>& queries, const std::vector<VulkanAabb>& targets,
        Coord cell_size = 0, VulkanAabbOperation operation = VulkanAabbOperation::Spatial) {
        return CudaSlicerBackend::enabled() ?
            CudaSlicerBackend::dispatch_indexed_aabb_candidates(queries, targets, cell_size, operation) :
            VulkanSlicerBackend::dispatch_indexed_aabb_candidates(queries, targets, cell_size, operation);
    }
};
}
