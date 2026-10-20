// CUDA test mod (src/libslic3r/Gpu/CudaCompute.hpp): Off touches nothing, and every kernel gives
// exactly what its CPU function gives. The GPU cases need an NVIDIA driver and are skipped without one.

#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <libslic3r/ExPolygon.hpp>
#include <libslic3r/Gpu/CudaCompute.hpp>
#include <libslic3r/Line.hpp>
#include <libslic3r/Point.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Gpu;

namespace {

bool gpu_or_skip()
{
    std::string reason;
    if (cuda_available(&reason))
        return true;
    WARN("No usable CUDA GPU, GPU cases skipped: " << reason);
    return false;
}

// Batches below the dispatch floor stay on the CPU; the GPU cases use at least this many items.
constexpr size_t batch = 2 * cuda_minimum_batch_items;

// The test's own statement of each rule, not the slicer's code.
CudaLayerRange expected_range(const std::array<float, 3> &z, const std::vector<float> &zs)
{
    const float lo = std::min({ z[0], z[1], z[2] }), hi = std::max({ z[0], z[1], z[2] });
    uint32_t first = 0;
    while (first < zs.size() && zs[first] < lo)
        ++first;
    uint32_t last = first;
    while (last < zs.size() && !(hi < zs[last]))
        ++last;
    return { first, last, uint32_t(z[1] == lo ? 1 : (z[2] == lo ? 2 : 0)) };
}

CudaRational expected_crossing(const CudaVerticalCrossing &c)
{
    // t / q along p1-p2, q kept in 32 bits as SegmentIntersection::pos_q is.
    const bool     forward = c.p2.x() > c.p1.x();
    const int64_t  t       = forward ? c.x - c.p1.x() : c.p1.x() - c.x;
    const uint32_t q       = uint32_t(forward ? c.p2.x() - c.p1.x() : c.p1.x() - c.p2.x());
    return { t * int64_t(c.p2.y() - c.p1.y()) + int64_t(c.p1.y()) * int64_t(q), q };
}

Polygon ring(coord_t cx, coord_t cy, coord_t r, int n, bool hole)
{
    Polygon p;
    for (int k = 0; k < n; ++k) {
        const double a = 2. * 3.14159265358979323846 * k / n;
        // A coarse grid, so random points land on vertices and edges often.
        p.points.emplace_back(coord_t(cx + r * std::cos(a)) / 100 * 100, coord_t(cy + r * std::sin(a)) / 100 * 100);
    }
    if (hole)
        p.reverse();
    return p;
}

} // namespace

TEST_CASE("CUDA mode names parse to their modes", "[CudaCompute]")
{
    CHECK(cuda_mode_from_string("off") == CudaMode::Off);
    CHECK(cuda_mode_from_string("") == CudaMode::Off);
    CHECK(cuda_mode_from_string("on") == CudaMode::On);
    CHECK(cuda_mode_from_string("max") == CudaMode::MaxGpu);
    CHECK(cuda_mode_from_string("ON") == CudaMode::Off);
}

TEST_CASE("CUDA off sends nothing to the GPU", "[CudaCompute]")
{
    CudaSession session("off");
    REQUIRE(cuda_mode() == CudaMode::Off);
    CHECK_FALSE(cuda_should_dispatch(batch));
    std::vector<std::array<float, 3>> facet_z(batch, { 0.f, 1.f, 2.f });
    std::vector<CudaLayerRange>       ranges;
    int                               cpu_calls = 0;
    CHECK_FALSE(cuda_mesh_layer_ranges(facet_z, { 0.5f, 1.5f }, [&](size_t) { ++cpu_calls; return CudaLayerRange{}; }, ranges));
    CHECK(ranges.empty());
    CHECK(cpu_calls == 0);
}

TEST_CASE("CUDA session ends in off", "[CudaCompute]")
{
    {
        CudaSession session("max");
        CHECK(cuda_mode() == CudaMode::MaxGpu);
    }
    CHECK(cuda_mode() == CudaMode::Off);
}

TEST_CASE("CUDA kernels give exactly what their CPU functions give", "[CudaCompute]")
{
    if (!gpu_or_skip())
        return;
    // Max: the GPU results come back unchecked, and the test compares them itself. The comparisons are
    // exact on purpose, floats included: the kernels must reproduce the CPU bit for bit.
    CudaSession     session("max");
    std::mt19937_64 rng(20261020);
    auto            never = [](size_t) -> bool { FAIL("Max mode must not call the CPU reference"); return false; };

    SECTION("facet layer ranges")
    {
        std::vector<float> zs;
        std::uniform_real_distribution<float> height(0.f, 50.f);
        for (int i = 0; i < 400; ++i)
            zs.push_back(height(rng));
        std::sort(zs.begin(), zs.end());
        zs.insert(zs.end(), 3, zs.back()); // equal slice heights at the top
        zs.insert(zs.begin(), 0.f);
        std::vector<std::array<float, 3>> facet_z;
        std::uniform_real_distribution<float> any(-5.f, 55.f), step(-3.f, 3.f);
        std::uniform_int_distribution<size_t> pick(0, zs.size() - 1);
        for (size_t i = 0; i < batch; ++i) {
            // Vertices exactly on slice planes, flat facets, -0 against +0 and facets outside the range.
            const float base = i % 3 == 0 ? zs[pick(rng)] : any(rng);
            std::array<float, 3> z;
            for (float &v : z)
                v = (rng() % 3 == 0) ? base : base + step(rng);
            if (i % 97 == 0)
                z = { -0.f, 0.f, 0.f };
            facet_z.push_back(z);
        }
        std::vector<CudaLayerRange> ranges;
        REQUIRE(cuda_mesh_layer_ranges(facet_z, zs, [&](size_t) { never(0); return CudaLayerRange{}; }, ranges));
        REQUIRE(ranges.size() == facet_z.size());
        size_t mismatches = 0;
        for (size_t i = 0; i < facet_z.size(); ++i)
            mismatches += !(ranges[i] == expected_range(facet_z[i], zs));
        CHECK(mismatches == 0);
    }

    SECTION("rectilinear crossings")
    {
        std::vector<CudaVerticalCrossing> crossings;
        std::uniform_int_distribution<int64_t> coord(-1'000'000'000, 1'000'000'000), span(1, 100'000'000);
        for (size_t i = 0; i < batch; ++i) {
            const coord_t x  = coord(rng);
            coord_t       x1 = x - span(rng), x2 = x + span(rng);
            if (i % 2)
                std::swap(x1, x2);
            crossings.push_back({ Point(x1, coord(rng)), Point(x2, coord(rng)), x });
        }
        // A span over 2^32 scaled units (4.3 m) wraps pos_q like the CPU does.
        crossings[0] = { Point(-3'000'000'000LL, 5), Point(3'000'000'000LL, -7), 1 };
        std::vector<CudaRational> values;
        REQUIRE(cuda_vertical_intersections(crossings, [&](size_t) { never(0); return CudaRational{}; }, values));
        REQUIRE(values.size() == crossings.size());
        size_t mismatches = 0;
        for (size_t i = 0; i < crossings.size(); ++i)
            mismatches += !(values[i] == expected_crossing(crossings[i]));
        CHECK(mismatches == 0);
    }

    SECTION("boundary distances, also over several launches")
    {
        // 20000 edges x 8192 points is over one launch's 2^26 steps, so the batch runs in several launches.
        const size_t edge_count = GENERATE(as<size_t>{}, 0, 1, 300, 20000);
        Lines        edges;
        std::uniform_int_distribution<coord_t> coord(-100'000'000, 100'000'000), d(-10'000'000, 10'000'000);
        for (size_t i = 0; i < edge_count; ++i) {
            const Point a(coord(rng), coord(rng));
            edges.emplace_back(a, i % 20 == 0 ? a : Point(a.x() + d(rng), a.y() + d(rng))); // some zero length
        }
        Points points;
        for (size_t i = 0; i < batch; ++i)
            points.emplace_back(coord(rng), coord(rng));
        for (size_t i = 0; i < std::min<size_t>(edges.size(), 64); ++i)
            points[i] = edges[i].b; // on an edge end
        std::vector<double> d2;
        REQUIRE(cuda_min_distances_squared(points, edges, [&](size_t) { never(0); return 0.; }, d2));
        REQUIRE(d2.size() == points.size());
        size_t mismatches = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            double expected = std::numeric_limits<double>::max();
            for (const Line &e : edges)
                expected = std::min(expected, Line::distance_to_squared(points[i], e.a, e.b));
            mismatches += d2[i] != expected;
        }
        CHECK(mismatches == 0);
    }

    SECTION("points in ExPolygons, borders included")
    {
        ExPolygons expolygons;
        std::uniform_int_distribution<coord_t> center(-200'000, 200'000), radius(50'000, 150'000), small(5'000, 30'000);
        for (int k = 0; k < 8; ++k) {
            const coord_t cx = center(rng), cy = center(rng);
            ExPolygon     expoly;
            expoly.contour = ring(cx, cy, radius(rng), 3 + int(rng() % 10), false);
            for (int h = 0; h < int(rng() % 3); ++h)
                expoly.holes.push_back(ring(cx + small(rng) - 15'000, cy + small(rng) - 15'000, small(rng), 3 + int(rng() % 6), true));
            expolygons.push_back(expoly);
        }
        ExPolygon degenerate;
        degenerate.contour.points = { Point(0, 0), Point(100, 0) };
        expolygons.push_back(degenerate);
        Points points;
        std::uniform_int_distribution<coord_t> coord(-400'000, 400'000);
        for (size_t i = 0; i < batch; ++i)
            points.emplace_back(coord(rng) / 100 * 100, coord(rng) / 100 * 100);
        for (const ExPolygon &e : expolygons) {
            points.insert(points.end(), e.contour.points.begin(), e.contour.points.end());
            for (const Polygon &h : e.holes)
                points.insert(points.end(), h.points.begin(), h.points.end());
        }
        std::vector<uint8_t> inside;
        REQUIRE(cuda_points_in_expolygons(points, expolygons, never, inside));
        REQUIRE(inside.size() == points.size());
        size_t mismatches = 0, in = 0;
        for (size_t i = 0; i < points.size(); ++i) {
            const bool expected = std::any_of(expolygons.begin(), expolygons.end(),
                                              [&](const ExPolygon &e) { return e.contains(points[i]); });
            in += expected;
            mismatches += bool(inside[i]) != expected;
        }
        CHECK(mismatches == 0);
        // The sample hits both sides, or the comparison proves little.
        CHECK(in > points.size() / 20);
        CHECK(in < points.size() - points.size() / 20);
    }
}

TEST_CASE("CUDA on discards a batch that differs from the CPU", "[CudaCompute]")
{
    if (!gpu_or_skip())
        return;
    CudaSession                       session("on");
    const std::vector<float>          zs = { 0.f, 1.f, 2.f, 3.f };
    std::vector<std::array<float, 3>> facet_z(batch, { 0.5f, 1.5f, 2.5f });
    std::vector<CudaLayerRange>       ranges;
    // The right reference: accepted.
    CHECK(cuda_mesh_layer_ranges(facet_z, zs, [&](size_t i) { return expected_range(facet_z[i], zs); }, ranges));
    CHECK(ranges.size() == batch);
    // One wrong item in the reference: the whole batch is thrown away and the caller uses the CPU.
    auto wrong = [&](size_t i) {
        CudaLayerRange r = expected_range(facet_z[i], zs);
        if (i == batch - 1)
            ++r.last;
        return r;
    };
    CHECK_FALSE(cuda_mesh_layer_ranges(facet_z, zs, wrong, ranges));
    CHECK(ranges.empty());
    CHECK(cuda_report().find("rejected 1") != std::string::npos);
}
