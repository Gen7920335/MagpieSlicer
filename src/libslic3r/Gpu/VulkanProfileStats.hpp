#pragma once

#include "VulkanSlicer.hpp"
#include "../SlicingProfiler.hpp"

namespace Slic3r::Gpu {

// GUI and CLI export the same snapshot without initializing the GPU device.
inline SlicingProfileVulkanStats vulkan_profile_stats(const VulkanSlicerRuntimeStats &runtime)
{
    SlicingProfileVulkanStats stats;
    stats.selected_device = runtime.selected_device;
    stats.execution_profile = runtime.execution_profile;
    stats.validation_mode = runtime.validation_mode;
    stats.last_diagnostic = runtime.last_diagnostic;
    stats.dispatch_calls = runtime.dispatch_calls;
    stats.queue_submissions = runtime.queue_submissions;
    stats.submitted_work_items = runtime.submitted_intersections;
    stats.accepted_gpu_items = runtime.accepted_gpu_intersections;
    stats.cpu_validation_checks = runtime.cpu_validation_checks;
    stats.validation_failures = runtime.validation_failures;
    stats.skipped_workloads = runtime.skipped_small_workloads;
    stats.total_gpu_ms = runtime.total_gpu_ms;
    stats.total_host_ms = runtime.total_host_ms;
    return stats;
}

} // namespace Slic3r::Gpu
