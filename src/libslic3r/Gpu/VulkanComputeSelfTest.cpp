#include "Gpu/VulkanSlicer.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <random>
#include <tuple>
#include <vector>

int main(int argc, char** argv)
{
    Slic3r::Gpu::VulkanSlicerBackend::set_compute_enabled(true);
    if (argc != 2) {
        std::cerr << "usage: magpie-vulkan-compute-selftest <resources-directory>\n";
        return 64;
    }

    Slic3r::set_resources_dir(argv[1]);
    const Slic3r::Gpu::VulkanSlicerCapabilities capabilities =
        Slic3r::Gpu::VulkanSlicerBackend::query_capabilities();
    std::cout << "Vulkan capability scan: " << capabilities.diagnostic << '\n';
    for (const Slic3r::Gpu::VulkanDeviceInfo& device : capabilities.devices) {
        std::cout << "  device: " << device.name
                  << " (vendor=0x" << std::hex << device.vendor_id << std::dec << ")\n"
                  << "    shaderInt64=" << (device.shader_int64 ? "yes" : "no")
                  << ", compute queue=" << (device.compute_queue ? "yes" : "no") << '\n'
                  << "    workgroup: " << device.max_workgroup_invocations
                  << " invocations, x=" << device.max_workgroup_size_x
                  << ", dispatch-x=" << device.max_workgroup_count_x << '\n'
                  << "    max storage buffer=" << device.max_storage_buffer_range << " bytes\n";
    }
    if (!Slic3r::Gpu::VulkanSlicerBackend::prepare_for_slicing()) {
        std::cerr << "Vulkan initialization unavailable: "
                  << Slic3r::Gpu::VulkanSlicerBackend::query_runtime_stats().last_diagnostic << '\n';
        return 2;
    }
    std::vector<Slic3r::Gpu::VulkanVerticalIntersectionRequest> requests;
    requests.reserve(1024);
    for (uint64_t index = 0; index < 1024; ++index) {
        const int64_t ay = int64_t(index) - 512;
        const int64_t by = 700 - int64_t(index);
        requests.push_back({ { { 0, ay }, { 1000, by } }, 375, index });
    }

    const Slic3r::Gpu::VulkanVerticalIntersectionBatch batch =
        Slic3r::Gpu::VulkanSlicerBackend::dispatch_vertical_intersections(requests);
    if (!batch.dispatched || batch.intersections.size() != requests.size()) {
        std::cerr << "Vulkan dispatch unavailable: " << batch.diagnostic << '\n';
        return 2;
    }

    for (size_t index = 0; index < requests.size(); ++index) {
        const auto& request = requests[index];
        const auto& result = batch.intersections[index];
        const int64_t denominator = request.segment.b.x - request.segment.a.x;
        const int64_t numerator = (request.scan_x - request.segment.a.x) *
            (request.segment.b.y - request.segment.a.y) + request.segment.a.y * denominator;
        if (!result.valid || result.stable_id != request.stable_id ||
            result.denominator != denominator || result.numerator != numerator) {
            std::cerr << "Vulkan result mismatch at request " << index << '\n';
            return 3;
        }
    }

    std::vector<Slic3r::Gpu::VulkanTreeContourRequest> tree_requests;
    std::vector<Slic3r::Gpu::Segment> tree_contours;
    for (uint64_t index = 0; index < 64; ++index)
        tree_requests.push_back({ { { 0, int64_t(index) }, { 1000, int64_t(index) } }, index });
    for (int64_t index = 0; index < 511; ++index)
        tree_contours.push_back({ { 10'000, index }, { 10'100, index } });
    tree_contours.push_back({ { 500, 0 }, { 500, 50 } });
    const Slic3r::Gpu::VulkanTreeContourBatch tree_batch =
        Slic3r::Gpu::VulkanSlicerBackend::dispatch_tree_contour_candidates(tree_requests, tree_contours);
    const bool tree_results_match = tree_batch.may_intersect.size() == tree_requests.size() &&
        std::all_of(tree_batch.may_intersect.begin(), tree_batch.may_intersect.end(),
                    [index = size_t(0)](uint8_t value) mutable { return (index++ <= 50) == (value != 0); });
    if (!tree_batch.dispatched || !tree_results_match) {
        std::cerr << "Vulkan tree contour broad-phase self-test failed: " << tree_batch.diagnostic << '\n';
        return 4;
    }

    std::mt19937_64 random(0x4d4147504945ULL);
    std::uniform_int_distribution<int64_t> coordinate(-100'000, 100'000);
    std::uniform_int_distribution<int64_t> extent(100, 8'000);
    std::vector<Slic3r::Gpu::VulkanAabb> targets;
    std::vector<Slic3r::Gpu::VulkanAabb> queries;
    for (size_t index = 0; index < 384; ++index) {
        const int64_t x = coordinate(random);
        const int64_t y = coordinate(random);
        const int64_t w = extent(random);
        const int64_t h = extent(random);
        targets.push_back({ { x, y }, { x + w, y + h } });
    }
    for (size_t index = 0; index < 192; ++index) {
        const int64_t x = coordinate(random);
        const int64_t y = coordinate(random);
        const int64_t w = extent(random);
        const int64_t h = extent(random);
        queries.push_back({ { x, y }, { x + w, y + h } });
    }
    std::vector<uint8_t> expected(queries.size(), uint8_t(0));
    std::vector<Slic3r::Gpu::VulkanAabbBatch::OverlapPair> expected_pairs;
    for (size_t query_index = 0; query_index < queries.size(); ++query_index) {
        const auto& query = queries[query_index];
        for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
            const auto& target = targets[target_index];
            if (query.min.x <= target.max.x && target.min.x <= query.max.x &&
                query.min.y <= target.max.y && target.min.y <= query.max.y) {
                expected[query_index] = 1;
                expected_pairs.push_back({ uint32_t(query_index), uint32_t(target_index) });
            }
        }
    }
    const std::array<Slic3r::Gpu::VulkanAabbOperation, 8> operations {
        Slic3r::Gpu::VulkanAabbOperation::Spatial,
        Slic3r::Gpu::VulkanAabbOperation::TreeSupport,
        Slic3r::Gpu::VulkanAabbOperation::DistanceField,
        Slic3r::Gpu::VulkanAabbOperation::Gyroid,
        Slic3r::Gpu::VulkanAabbOperation::SeamTravel,
        Slic3r::Gpu::VulkanAabbOperation::ClassicWall,
        Slic3r::Gpu::VulkanAabbOperation::CuraSupport,
        Slic3r::Gpu::VulkanAabbOperation::ArachneWall
    };
    for (const auto operation : operations) {
        const auto spatial_batch = Slic3r::Gpu::VulkanSlicerBackend::dispatch_indexed_aabb_candidates(
            queries, targets, 4'000, operation);
        if (!spatial_batch.resolved || !spatial_batch.dispatched ||
            spatial_batch.may_overlap != expected) {
            std::cerr << "Vulkan indexed AABB self-test failed for operation "
                      << int(operation) << ": " << spatial_batch.diagnostic << '\n';
            return 5;
        }
        if (int(operation) >= int(Slic3r::Gpu::VulkanAabbOperation::SeamTravel)) {
            if (spatial_batch.overlap_pairs != expected_pairs) {
                std::cerr << "Vulkan indexed AABB pair set is incomplete or contains an extra pair for operation "
                          << int(operation) << ".\n";
                return 7;
            }
            if (!std::is_sorted(spatial_batch.overlap_pairs.begin(), spatial_batch.overlap_pairs.end(),
                    [](const auto& lhs, const auto& rhs) {
                        return std::tie(lhs.query, lhs.target) < std::tie(rhs.query, rhs.target);
                    })) {
                std::cerr << "Vulkan indexed AABB pair order is not deterministic.\n";
                return 8;
            }
            for (const auto& pair : spatial_batch.overlap_pairs) {
                const auto& query = queries[pair.query];
                const auto& target = targets[pair.target];
                if (!(query.min.x <= target.max.x && target.min.x <= query.max.x &&
                      query.min.y <= target.max.y && target.min.y <= query.max.y)) {
                    std::cerr << "Vulkan indexed AABB returned a false overlap pair.\n";
                    return 9;
                }
            }
        }
    }
    const std::vector<Slic3r::Gpu::VulkanAabb> remote_target {
        { { 1'000'000, 1'000'000 }, { 1'001'000, 1'001'000 } }
    };
    const auto index_only_batch = Slic3r::Gpu::VulkanSlicerBackend::dispatch_indexed_aabb_candidates(
        queries, remote_target, 4'000, Slic3r::Gpu::VulkanAabbOperation::DistanceField);
    if (!index_only_batch.resolved || index_only_batch.dispatched ||
        !std::all_of(index_only_batch.may_overlap.begin(), index_only_batch.may_overlap.end(),
                     [](uint8_t value) { return value == 0; })) {
        std::cerr << "CPU spatial-index proof self-test failed: " << index_only_batch.diagnostic << '\n';
        return 6;
    }

    std::cout << "Vulkan exact vertical-intersection self-test passed for "
              << batch.intersections.size() << " requests; tree contour broad phase passed for "
              << tree_batch.may_intersect.size() << " branches; indexed AABB operations passed for "
              << queries.size() << " randomized queries.\n"
              << Slic3r::Gpu::VulkanSlicerBackend::runtime_diagnostic_report() << '\n';
    return 0;
}
