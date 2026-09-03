#include "Gpu/CudaSlicer.hpp"
#include <chrono>
#include <atomic>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <thread>
#ifdef MAGPIE_CUDA_PRODUCT_SELFTEST
#include "Polyline.hpp"
#include "AppConfig.hpp"
#include "Utils.hpp"
#include "Thread.hpp"
#include <filesystem>
namespace Slic3r { Polylines reconnect_polylines(const Polylines&, double); }
#endif

using namespace Slic3r::Gpu;
void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
int main() {
    try {
#ifdef MAGPIE_CUDA_PRODUCT_SELFTEST
        // Isolated config path: never touch the user's application profile.
        Slic3r::save_main_thread_id();
        const auto config_test_path = std::filesystem::temp_directory_path() /
            ("magpie-cuda-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directory(config_test_path);
        Slic3r::set_data_dir(config_test_path.u8string());
        Slic3r::AppConfig config;
        require(config.get("cuda_slicer_mode") == "off", "CUDA default must be off");
        require(config.get("slicing_timing_detail") == "detailed", "Default recording detail changed");
        config.set_slicing_acceleration_mode("cuda_slicer_mode", "max");
        require(config.get("vulkan_slicer_mode") == "off", "CUDA selection left Vulkan enabled");
        config.set_slicing_acceleration_mode("vulkan_slicer_mode", "max");
        require(config.get("cuda_slicer_mode") == "off", "Vulkan selection left CUDA enabled");
        config.set_slicing_acceleration_mode("cuda_slicer_mode", "max");
        config.set("slicing_timing_detail", "stages");
        config.set_bool("slicing_timing_auto_save", true);
        config.save();
        Slic3r::AppConfig loaded;
        require(loaded.load().empty(), "Saved settings failed to load");
        require(loaded.get("cuda_slicer_mode") == "max" && loaded.get("vulkan_slicer_mode") == "off", "Backend settings did not survive restart");
        require(loaded.get("slicing_timing_detail") == "stages" && loaded.get_bool("slicing_timing_auto_save"), "Timing settings did not survive restart");
        loaded.set_slicing_acceleration_mode("cuda_slicer_mode", "unknown");
        require(loaded.get("cuda_slicer_mode") == "off", "Unknown CUDA setting enabled acceleration");
        loaded.set("slicing_timing_detail", "unknown");
        loaded.set_defaults();
        require(loaded.get("slicing_timing_detail") == "detailed", "Invalid timing setting not normalized");
        std::cout << "PASS AppConfig defaults, exclusive backend selection and save/load: " << config_test_path.u8string() << std::endl;
#endif
        _putenv_s("MAGPIE_CUDA_SLICER_ENABLE", "0");
        CudaSlicerBackend::begin_slicing_session();
        std::vector<VulkanVerticalIntersectionRequest> requests {{{{0, -9}, {10, 12}}, 5, 99}};
        require(!CudaSlicerBackend::dispatch_vertical_intersections(requests).dispatched, "Disabled path dispatched");
        require(CudaSlicerBackend::runtime_stats().queue_submissions == 0, "Disabled path submitted");
        _putenv_s("MAGPIE_CUDA_SLICER_ENABLE", "1");
        _putenv_s("MAGPIE_CUDA_SLICER_FORCE_DISPATCH", "1");
        CudaSlicerBackend::begin_slicing_session(false);
        require(!CudaSlicerBackend::enabled() && !CudaSlicerBackend::should_dispatch(65537), "GUI Off overridden by environment");
        require(!CudaSlicerBackend::dispatch_vertical_intersections(requests).dispatched, "GUI Off submitted CUDA work");
        _putenv_s("MAGPIE_CUDA_SLICER_ENABLE", "0");
        CudaSlicerBackend::begin_slicing_session(true);
        require(CudaSlicerBackend::enabled() && CudaSlicerBackend::should_dispatch(4096), "GUI On ignored");
        require(!CudaSlicerBackend::should_dispatch(1), "GUI On inherited CLI force override");
        _putenv_s("MAGPIE_CUDA_SLICER_ENABLE", "1");
        CudaSlicerBackend::begin_slicing_session();
        require(CudaSlicerBackend::should_dispatch(1), "CLI forced dispatch no longer works");
        std::cout << "PASS GUI On/Off precedence and CLI compatibility" << std::endl;
        auto invalid = requests;
        invalid.push_back({{{0, 0}, {0, 8}}, 0, 12});
        require(!CudaSlicerBackend::dispatch_vertical_intersections(invalid).dispatched, "Degenerate batch accepted");
        require(CudaSlicerBackend::runtime_stats().queue_submissions == 0, "Invalid preflight submitted");
        invalid = {{{{std::numeric_limits<int64_t>::min(), 0}, {std::numeric_limits<int64_t>::max(), 3}}, 0, 1}};
        require(!CudaSlicerBackend::dispatch_vertical_intersections(invalid).dispatched, "Overflow accepted");
        invalid = {{{{0, -4000000000000000000LL}, {3, 4000000000000000000LL}}, 1, 1}};
        require(!CudaSlicerBackend::dispatch_vertical_intersections(invalid).dispatched, "Overflowing intermediate accepted");
        invalid = requests; invalid[0].scan_x = 0;
        require(!CudaSlicerBackend::dispatch_vertical_intersections(invalid).dispatched, "Endpoint accepted");
        std::mt19937_64 random(4070);
        requests.clear();
        for (uint64_t i = 0; i < 65537; ++i) {
            int64_t ax = int64_t(random() % 200000001) - 100000000;
            int64_t ay = int64_t(random() % 200000001) - 100000000;
            int64_t bx = ax + 2 + random() % 10000000;
            int64_t by = int64_t(random() % 200000001) - 100000000;
            int64_t x = ax + 1 + random() % (bx - ax - 1);
            if (i % 2) { std::swap(ax, bx); std::swap(ay, by); }
            requests.push_back({{{ax, ay}, {bx, by}}, x, i * 17 + 3});
        }
        // Barycentric reference is independent of the kernel's expression.
        for (int repeat = 0; repeat < 3; ++repeat) {
            const auto batch = CudaSlicerBackend::dispatch_vertical_intersections(requests);
            std::cout << "repeat=" << repeat << " accepted=" << batch.dispatched
                << " host_ms=" << batch.host_elapsed_ms << " kernel_ms=" << batch.gpu_elapsed_ms
                << " diagnostic=" << batch.diagnostic << std::endl;
            require(batch.dispatched && batch.intersections.size() == requests.size(), "CUDA runtime did not return full batch");
            for (size_t i = 0; i < requests.size(); ++i) {
                const auto& q = requests[i]; const auto& got = batch.intersections[i];
                const auto& a = q.segment.a.x < q.segment.b.x ? q.segment.a : q.segment.b;
                const auto& b = q.segment.a.x < q.segment.b.x ? q.segment.b : q.segment.a;
                using Wide = boost::multiprecision::cpp_int;
                const Wide numerator = Wide(a.y) * (Wide(b.x) - q.scan_x) + Wide(b.y) * (Wide(q.scan_x) - a.x);
                require(got.valid && got.stable_id == q.stable_id && got.numerator == numerator && got.denominator == b.x - a.x, "Independent reference mismatch");
            }
        }
        auto stats = CudaSlicerBackend::runtime_stats();
        require(stats.queue_submissions == 3 && stats.accepted_gpu_items == 3 * requests.size() && stats.validation_failures == 0, "Runtime accounting mismatch");
        std::cout << "PASS independent checks=" << stats.accepted_gpu_items << " device=" << stats.selected_device
            << " init_ms=" << stats.initialization_ms << " allocation_ms=" << stats.allocation_ms
            << " pack_reference_ms=" << stats.packing_ms << " upload_ms=" << stats.upload_ms
            << " download_ms=" << stats.download_ms << " validation_ms=" << stats.validation_ms << std::endl;
        std::atomic<int> thread_failures {0};
        auto worker = [&]() {
            auto batch = CudaSlicerBackend::dispatch_vertical_intersections(requests);
            if (!batch.dispatched || batch.intersections.size() != requests.size()) ++thread_failures;
        };
        std::thread first(worker), second(worker);
        first.join(); second.join();
        require(thread_failures == 0, "Context reuse across worker threads failed");
        stats = CudaSlicerBackend::runtime_stats();
        require(stats.queue_submissions == 5 && stats.accepted_gpu_items == 5 * requests.size(), "Concurrent accounting mismatch");
        CudaSlicerBackend::begin_slicing_session();
        require(CudaSlicerBackend::runtime_stats().queue_submissions == 0, "Session counters not reset");
        require(CudaSlicerBackend::dispatch_vertical_intersections(requests).dispatched, "Retained context after session reset failed");
        std::cout << "PASS worker-thread reuse and next-session reset" << std::endl;
        for (int iteration = 0; iteration < 1000; ++iteration) {
            std::vector<VulkanVerticalIntersectionRequest> changing;
            for (int j = 0; j < 17; ++j)
                changing.push_back({{{-500, iteration * 13 + j}, {700, iteration * -17 - j}}, 20, uint64_t(iteration * 17 + j)});
            auto batch = CudaSlicerBackend::dispatch_vertical_intersections(changing);
            if (!batch.dispatched) std::cerr << "changing batch=" << iteration << " " << batch.diagnostic << std::endl;
            require(batch.dispatched, "Changing input transfer/kernel ordering failed");
        }
        std::cout << "PASS 1000 changing-input batches" << std::endl;
_putenv_s("MAGPIE_CUDA_SLICER_FORCE_DISPATCH", "1");
        auto check_boxes = [&](const std::vector<VulkanAabb>& qs, const std::vector<VulkanAabb>& ts, int64_t cell) {
            const auto batch = CudaSlicerBackend::dispatch_indexed_aabb_candidates(qs, ts, cell);
            if (!batch.resolved) std::cerr << batch.diagnostic << std::endl;
            require(batch.resolved && batch.may_overlap.size() == qs.size(), "AABB batch unresolved");
            std::vector<VulkanAabbBatch::OverlapPair> expected;
            std::vector<uint8_t> flags(qs.size(), 0);
            for (uint32_t q = 0; q < qs.size(); ++q) for (uint32_t t = 0; t < ts.size(); ++t) {
                const auto& a = qs[q]; const auto& b = ts[t];
                // Independent exhaustive oracle also catches CPU grid omissions.
                const bool hit = std::max(std::min(a.min.x,a.max.x),std::min(b.min.x,b.max.x)) <=
                                 std::min(std::max(a.min.x,a.max.x),std::max(b.min.x,b.max.x)) &&
                                 std::max(std::min(a.min.y,a.max.y),std::min(b.min.y,b.max.y)) <=
                                 std::min(std::max(a.min.y,a.max.y),std::max(b.min.y,b.max.y));
                if (hit) { expected.push_back({q,t}); flags[q] = 1; }
            }
            require(expected == batch.overlap_pairs && flags == batch.may_overlap, "AABB candidate omission/order mismatch");
        };
        for (int pass = 0; pass < 20; ++pass) {
            std::vector<VulkanAabb> qs, ts;
            for (int i = 0; i < 120; ++i) {
                auto coord = [&]() { return int64_t(random() % 2001) - 1000; };
                VulkanAabb box{{coord(),coord()},{coord(),coord()}};
                (i % 2 ? qs : ts).push_back(box);
            }
            check_boxes(qs, ts, pass % 2 ? 113 : 0);
            require(CudaSlicerBackend::dispatch_vertical_intersections(requests).dispatched, "Mixed kernel buffer reuse failed");
        }
        check_boxes({{{INT64_MIN,INT64_MIN},{INT64_MAX,INT64_MAX}},
                     {{-1,-1},{0,0}}, {{INT64_MAX,0},{INT64_MAX,0}}},
                    {{{INT64_MAX,0},{INT64_MAX,0}}, {{0,0},{1,1}},
                     {{INT64_MIN,INT64_MIN},{INT64_MIN,INT64_MIN}}}, 1);
        check_boxes({}, {{{0,0},{1,1}}}, 1);
        check_boxes({{{0,0},{1,1}}}, {}, 1);
        // >262144 pairs exercises chunking while changing buffer strides.
        check_boxes(std::vector<VulkanAabb>(520, {{-1,-1},{1,1}}),
                    std::vector<VulkanAabb>(520, {{1,1},{2,2}}), 1);
        auto capped = CudaSlicerBackend::dispatch_indexed_aabb_candidates(
            std::vector<VulkanAabb>(1100, {{0,0},{0,0}}), std::vector<VulkanAabb>(1100, {{0,0},{0,0}}), 1);
        require(!capped.resolved && capped.overlap_pairs.empty(), "Capacity failure leaked partial AABB results");
        _putenv_s("MAGPIE_CUDA_DISABLE_OPERATIONS", "spatial");
        require(!CudaSlicerBackend::dispatch_indexed_aabb_candidates({{{0,0},{1,1}}}, {{{0,0},{1,1}}}).resolved,
                "Per-operation exclusion ignored");
        _putenv_s("MAGPIE_CUDA_DISABLE_OPERATIONS", "");
        require(CudaSlicerBackend::runtime_stats().validation_failures == 0, "New kernel validation failure");
        std::cout << "PASS AABB exhaustive oracle, full int64, chunking, caps, operation selection and mixed kernels" << std::endl;
std::vector<Point> distance_points;
        std::vector<double> distance_reference;
        // Analytic distance to a horizontal segment, including endpoint and degenerate cases.
        for (int i = -80; i <= 80; ++i) {
            distance_points.push_back({i, i%13});
            const double dx = i < -30 ? i+30 : (i > 30 ? i-30 : 0);
            distance_reference.push_back(dx*dx+double(i%13)*double(i%13));
        }
        auto distances = CudaSlicerBackend::dispatch_point_segment_distances(distance_points, {{{-30,0},{30,0}}}, distance_reference);
        require(distances.resolved && distances.distances_squared == distance_reference, "Analytic distance kernel mismatch");
        std::vector<double> degenerate_reference;
        for (const auto& p : distance_points) degenerate_reference.push_back(double(p.x)*p.x+double(p.y)*p.y);
        require(CudaSlicerBackend::dispatch_point_segment_distances(distance_points, {{{0,0},{0,0}}}, degenerate_reference).resolved, "Degenerate distance mismatch");
        auto wrong_reference = distance_reference; wrong_reference[8] += 1;
        auto rejected_distance = CudaSlicerBackend::dispatch_point_segment_distances(distance_points, {{{-30,0},{30,0}}}, wrong_reference);
        require(!rejected_distance.resolved && rejected_distance.distances_squared.empty(), "Mismatched distance result leaked");
        CudaSlicerBackend::begin_slicing_session(); // Expected rejection above is isolated.
        std::vector<float> zs {-1, -0.0f, 0.0f, 1, 2, 4, 9};
        std::vector<CudaMeshZRequest> facets {{-3,-2,-1},{0,0,0},{2,1,2},{10,11,12},{-9,-8,-7},{0,4,9}};
        auto ranges = CudaSlicerBackend::dispatch_mesh_layer_ranges(facets,zs);
        require(ranges.resolved && ranges.ranges.size() == 6, "Mesh ranges unresolved");
        const uint32_t firsts[] {0,1,3,7,0,1}, lasts[] {1,3,5,7,0,7};
        for (size_t i = 0; i < facets.size(); ++i) {
            require(ranges.ranges[i].first == firsts[i] && ranges.ranges[i].last == lasts[i], "Mesh inclusive boundary/tie mismatch");
        }
        require(!CudaSlicerBackend::dispatch_mesh_layer_ranges(facets,{2,1}).resolved, "Unsorted layer input accepted");
        facets[1].z0 = std::numeric_limits<float>::quiet_NaN();
        require(!CudaSlicerBackend::dispatch_mesh_layer_ranges(facets,zs).resolved, "Nonfinite mesh input accepted");
        require(CudaSlicerBackend::runtime_stats().validation_failures == 0, "Unexpected distance/mesh failure");
        std::cout << "PASS distance analytic endpoints, zero length, whole-batch mismatch rejection; mesh ties/empty ranges/nonfinite" << std::endl;
std::vector<Point> containment_points;
        std::vector<uint8_t> containment_reference;
        for (int y = -12; y <= 12; ++y) for (int x = -12; x <= 24; ++x) {
            containment_points.push_back({x,y});
            const bool outer = x >= -10 && x <= 10 && y >= -10 && y <= 10;
            const bool hole = x > -3 && x < 3 && y > -3 && y < 3;
            const bool second = x >= 8 && x <= 20 && y >= 0 && y <= 5;
            containment_reference.push_back(uint8_t((outer && !hole) || second));
        }
        std::vector<CudaPolygonContours> polygons {
            {{{{-10,-10},{10,-10},{10,10},{-10,10}},{{-3,-3},{-3,3},{3,3},{3,-3}}}},
            {{{{8,0},{20,0},{20,5},{8,5}}}}
        };
        auto contains = CudaSlicerBackend::dispatch_points_in_polygons(containment_points,polygons,containment_reference);
        require(contains.resolved && contains.inside == containment_reference, "Containment hole/border/union mismatch");
        for (auto& p : polygons) for (auto& ring : p.rings) std::reverse(ring.begin(),ring.end());
        require(CudaSlicerBackend::dispatch_points_in_polygons(containment_points,polygons,containment_reference).resolved, "Containment reversed winding mismatch");
        polygons[0].rings[0][0].x = INT64_MAX;
        require(!CudaSlicerBackend::dispatch_points_in_polygons(containment_points,polygons,containment_reference).resolved, "Containment overflow admitted");
        auto pairs = CudaSlicerBackend::dispatch_aabb_pairs({{{0,0},{1,1}},{{-1,-1},{0,0}}}, {{{1,1},{2,2}},{{1,1},{2,2}}});
        require(pairs.resolved && pairs.may_overlap == std::vector<uint8_t>({1,0}), "Paired AABB mismatch");
        require(CudaSlicerBackend::runtime_stats().validation_failures == 0, "Unexpected containment failure");
        std::cout << "PASS point-in-polygon hole borders, overlapping union, reversed rings, overflow; paired AABB" << std::endl;
        CudaSlicerBackend::begin_slicing_session(true, true, true);
        require(CudaSlicerBackend::enabled() && CudaSlicerBackend::skips_cpu_validation() &&
                CudaSlicerBackend::should_dispatch(1), "Max GPU mode was not activated");
        auto max_intersection = CudaSlicerBackend::dispatch_vertical_intersections(
            {{{{0, -9}, {10, 12}}, 5, 777}});
        require(max_intersection.dispatched && max_intersection.intersections.size() == 1,
                "Max GPU small intersection did not dispatch");
        auto max_distance = CudaSlicerBackend::dispatch_point_segment_distances(
            {{0, 7}}, {{{-30,0},{30,0}}}, {});
        require(max_distance.resolved && max_distance.distances_squared == std::vector<double>({49.0}),
                "Max GPU distance required a CPU reference");
        CudaPolygonContours max_polygon;
        max_polygon.rings.push_back({{-10,-10},{10,-10},{10,10},{-10,10}});
        auto max_containment = CudaSlicerBackend::dispatch_points_in_polygons(
            {{0,0}}, {max_polygon}, {});
        require(max_containment.resolved && max_containment.inside == std::vector<uint8_t>({1}),
                "Max GPU containment required a CPU reference");
        const auto max_stats = CudaSlicerBackend::runtime_stats();
        std::cout << "Max GPU stats: cpu_checks=" << max_stats.cpu_validation_checks
                  << " validation_ms=" << max_stats.validation_ms
                  << " mode=" << max_stats.validation_mode << std::endl;
        require(max_stats.cpu_validation_checks == 0 && max_stats.validation_ms == 0 &&
                max_stats.validation_mode.find("CPU result duplication disabled") != std::string::npos,
                "Max GPU still performed CPU result validation");
        std::cout << "PASS Max GPU forced dispatch and zero CPU result-validation checks" << std::endl;
#ifdef MAGPIE_CUDA_PRODUCT_SELFTEST
        Slic3r::Polylines chained {
            Slic3r::Polyline(Slic3r::Points{{0,0},{10,0}}),
            Slic3r::Polyline(Slic3r::Points{{10,0},{20,0}}),
            Slic3r::Polyline(Slic3r::Points{{20,0},{30,0}})
        };
        const auto joined = Slic3r::reconnect_polylines(chained,1.0);
        require(joined.size() == 1 && joined.front().first_point() == Slic3r::Point(0,0) &&
                joined.front().last_point() == Slic3r::Point(30,0), "AABB omitted a join after base polyline extension");
        std::cout << "PASS actual path reconnection after expanding the original AABB" << std::endl;
#endif
        return 0;
    } catch (const std::exception& error) { std::cerr << "FAIL: " << error.what() << std::endl; return 1; }
}
