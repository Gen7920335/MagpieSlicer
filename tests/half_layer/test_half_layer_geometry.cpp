#include <catch2/catch_all.hpp>

#include "libslic3r/PerimeterGenerator.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/HalfLayerPlan.hpp"
#ifdef MAGPIE_HALF_LAYER_CPU_GATE
#include "libslic3r/Gpu/CudaSlicer.hpp"
#include "libslic3r/Gpu/VulkanSlicer.hpp"
#endif

#include <algorithm>
#include <map>
#include <functional>

using namespace Slic3r;

#ifdef MAGPIE_HALF_LAYER_CPU_GATE
TEST_CASE("Half-layer CPU experiment contains neither GPU backend", "[HalfLayer][CPUOnly]")
{
    CHECK_FALSE(Gpu::CudaSlicerBackend::compiled_with_cuda());
    CHECK_FALSE(Gpu::VulkanSlicerBackend::compiled_with_vulkan());
}
#endif

namespace {
constexpr double wall_width_tolerance_mm = 0.00001; // Width rounding in scaled geometry.
constexpr double wall_flow_tolerance_mm3_per_mm = 0.00001; // Float Flow cross-section rounding.
constexpr double boundary_tolerance_mm = 0.00001; // Position rounding in offsets and simplification.
struct WallSample {
    int inset;
    ExtrusionToolHint tool;
    ExtrusionRole role;
    float height;
    float width;
    double volume_per_mm;
    double min_x_mm;
};

void collect_walls(const ExtrusionEntity &entity, std::vector<WallSample> &samples)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const auto *child : collection->entities)
            collect_walls(*child, samples);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const auto &path : loop->paths)
            samples.push_back({loop->inset_idx, loop->tool_hint, path.role(), path.height, path.width, path.mm3_per_mm,
                unscale<double>(get_extents(path.polyline.to_polyline()).min.x())});
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        samples.push_back({path->inset_idx, path->tool_hint, path->role(), path->height, path->width, path->mm3_per_mm,
            unscale<double>(get_extents(path->polyline.to_polyline()).min.x())});
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const auto &path : multi->paths)
            samples.push_back({multi->inset_idx, multi->tool_hint, path.role(), path.height, path.width, path.mm3_per_mm,
                unscale<double>(get_extents(path.polyline.to_polyline()).min.x())});
    } else {
        FAIL("Unexpected extrusion type in rectangular wall fixture");
    }
}

std::vector<WallSample> generate_walls(bool arachne, bool half_height, int wall_count, double height,
                                     double outside_width, double inside_width,
                                     int detail_count = 0, int layer_id = 10, bool interlocking = false,
                                     double *fill_min_x_mm = nullptr, double model_width_mm = 20.,
                                     const std::function<void(PrintRegionConfig &)> &configure = {},
                                     bool partial_support = false,
                                     const std::function<void(PrintConfig &)> &configure_print = {},
                                     bool thick_bridges = true,
                                     std::vector<WallSample> *gap_samples = nullptr)
{
    PrintConfig print_config;
    PrintObjectConfig object_config;
    PrintRegionConfig region_config;
    region_config.wall_loops.value = wall_count;
    region_config.only_one_wall_top.value = false;
    region_config.only_one_wall_first_layer.value = false;
    region_config.detect_overhang_wall.value = false;
    region_config.precise_outer_wall.value = false;
    region_config.use_smaller_nozzles_in_crisp_corners.value = false;
    if (detail_count > 0) {
        print_config.nozzle_diameter.values = {0.4, 0.2};
        print_config.filament_diameter.values = {1.75, 1.75};
        print_config.filament_map.values = {1, 2};
        print_config.filament_colour.values = {"#FF0000", "#FF0000"};
        print_config.filament_type.values = {"PLA", "PLA"};
        print_config.filament_soluble.values = {false, false};
        print_config.toolhead_outer_wall_line_width.values = {FloatOrPercent(outside_width, false), FloatOrPercent(0.22, false)};
        region_config.outer_wall_filament_id.value = 1;
        region_config.inner_wall_filament_id.value = 1;
        region_config.use_smaller_nozzles_in_crisp_corners.value = true;
        region_config.crisp_corner_detail_toolhead.value = 0;
        region_config.crisp_corner_small_nozzle_wall_count.value = detail_count;
        region_config.crisp_corner_interlace_small_nozzle_walls.value = interlocking;
        region_config.crisp_corner_nozzle_wall_overlap.value = 15.;
    }
    if (configure)
        configure(region_config);
    if (configure_print)
        configure_print(print_config);
    SurfaceCollection surfaces;
    const coord_t side_scaled = scaled<coord_t>(20.); // Fixture side length: 20 mm.
    const coord_t width_scaled = scaled<coord_t>(model_width_mm);
    surfaces.append(ExPolygons{ExPolygon(Polygon{Point(0, 0), Point(width_scaled, 0),
        Point(width_scaled, side_scaled), Point(0, side_scaled)})}, stInternal);
    LayerRegionPtrs compatible;
    ExtrusionEntityCollection loops, gaps;
    SurfaceCollection fill;
    ExPolygons fill_no_overlap;
    Flow inside(float(inside_width), float(height), 0.4f);
    Flow outside(float(outside_width), float(height), 0.4f);
    PerimeterGenerator generator(&surfaces, &compatible, height, 1. + height * 0.5, inside,
        &region_config, &object_config, &print_config, false, 0., &loops, &gaps, &fill, &fill_no_overlap);
    generator.layer_id = layer_id;
    generator.ext_perimeter_flow = outside;
    generator.overhang_flow = thick_bridges ? Flow::bridging_flow(0.4f, 0.4f) :
        inside.with_flow_ratio(region_config.bridge_flow.value);
    generator.solid_infill_flow = inside;
    generator.half_layer_outer_walls = half_height;
    const ExPolygons lower{ExPolygon(Polygon{Point(0, 0), Point(width_scaled / 2, 0),
        Point(width_scaled / 2, side_scaled), Point(0, side_scaled)})};
    if (partial_support)
        generator.lower_slices = &lower;
    if (arachne)
        generator.process_arachne();
    else
        generator.process_classic();
    std::vector<WallSample> samples;
    collect_walls(loops, samples);
    if (gap_samples != nullptr)
        collect_walls(gaps, *gap_samples);
    if (fill_min_x_mm != nullptr)
        *fill_min_x_mm = fill_no_overlap.empty() ? std::numeric_limits<double>::infinity() :
            unscale<double>(get_extents(fill_no_overlap).min.x());
    return samples;
}
}

TEST_CASE("Classic shell-only gap fill uses the shell physical height", "[HalfLayer][Geometry][ShellGap]")
{
    const bool enabled = GENERATE(false, true);
    const double height_mm = GENERATE(0.1, 0.2, 0.3);
    const double width_mm = GENERATE(1.2, 2., 3.);
    CAPTURE(enabled, height_mm, width_mm);
    std::vector<WallSample> gaps;
    const auto walls = generate_walls(false, enabled, 4, height_mm, 0.42, 0.48,
        0, 10, false, nullptr, width_mm, {}, false, {}, true, &gaps);
    REQUIRE_FALSE(walls.empty());
    REQUIRE_FALSE(gaps.empty());
    const bool half_gap = enabled && width_mm < 3.; // Analytic fixture: no room for an inner bead below 3 mm.
    if (half_gap)
        for (const auto &wall : walls)
            CHECK(wall.inset < 2);
    if (width_mm == 3.)
        CHECK(std::any_of(walls.begin(), walls.end(), [](const WallSample &wall) { return wall.inset >= 2; }));
    for (const auto &gap : gaps) {
        CHECK(gap.role == erGapFill);
        CHECK(gap.height == Catch::Approx(half_gap ? 0.5 * height_mm : height_mm));
        CHECK(gap.volume_per_mm == Catch::Approx(Flow(gap.width, gap.height, 0.4f).mm3_per_mm()));
    }
}

TEST_CASE("Half-height overhang walls fit their physical height band", "[HalfLayer][Geometry][Overhang]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const double height = GENERATE(0.1, 0.2, 0.3);
    const bool thick_bridges = GENERATE(false, true);
    CAPTURE(arachne, enabled, height, thick_bridges);
    const auto samples = generate_walls(arachne, enabled, 4, height, 0.42, 0.48,
        0, 10, false, nullptr, 20., [](PrintRegionConfig &config) {
            config.detect_overhang_wall.value = true;
            config.bridge_flow.value = 1.;
        }, true, {}, thick_bridges);
    size_t supported = 0, shell_bridges = 0, core_bridges = 0;
    for (const auto &sample : samples) {
        REQUIRE(sample.inset >= 0);
        if (sample.role != erOverhangPerimeter) {
            ++supported;
            continue;
        }
        if (enabled && sample.inset < 2) {
            ++shell_bridges;
            CHECK(sample.height == Catch::Approx(0.5 * height));
        } else {
            ++core_bridges;
            CHECK(sample.height == Catch::Approx(thick_bridges ? 0.4 : height));
            const Flow bead(sample.width, sample.height, 0.4f);
            CHECK(sample.volume_per_mm == Catch::Approx(thick_bridges ? PI * 0.4 * 0.4 / 4. : bead.mm3_per_mm()));
        }
    }
    CHECK(supported > 0);
    CHECK(core_bridges > 0);
    CHECK((shell_bridges > 0) == enabled);
}

TEST_CASE("Half-height bridge Flow respects ratio and mapped physical nozzle width", "[HalfLayer][Geometry][Overhang][MultiNozzle]")
{
    const bool arachne = GENERATE(false, true);
    const int details = GENERATE(0, 1, 2, 3);
    const double ratio = GENERATE(0.8, 1., 1.2);
    const double height = GENERATE(0.1, 0.2, 0.3);
    const int layer_id = GENERATE(10, 11);
    CAPTURE(arachne, details, ratio, height, layer_id);
    const auto samples = generate_walls(arachne, true, 4, height, 0.42, 0.48,
        details, layer_id, true, nullptr, 20., [&](PrintRegionConfig &config) {
            config.detect_overhang_wall.value = true;
            config.bridge_flow.value = ratio;
            config.bridge_line_width = ConfigOptionFloatOrPercent(0.65, false);
            // Material 1 -> hotend 2 (large), material 2 -> hotend 1 (detail).
            // Physical width must not be indexed by the material number.
        }, true, [&](PrintConfig &config) {
            config.nozzle_diameter.values = {0.2, 0.4};
            config.filament_diameter.values = {1.75, 1.75};
            config.filament_map.values = {2, 1};
            config.filament_colour.values = {"#FF0000", "#FF0000"};
            config.filament_type.values = {"PLA", "PLA"};
            config.filament_soluble.values = {false, false};
            config.toolhead_outer_wall_line_width.values = {FloatOrPercent(0.22, false), FloatOrPercent(0.42, false)};
            config.toolhead_bridge_line_width.values = {FloatOrPercent(110., true), FloatOrPercent(125., true)};
        });
    size_t bridges = 0;
    for (const auto &sample : samples) {
        if (sample.role != erOverhangPerimeter || sample.inset >= 2)
            continue;
        ++bridges;
        const bool detail = sample.tool == ExtrusionToolHint::DetailWall;
        const int effective_details = details > 1 && layer_id % 2 == 1 ? details - 1 : details;
        CHECK(detail == (sample.inset < effective_details));
        const double nozzle_mm = detail ? 0.2 : 0.4;
        const double configured_width_mm = nozzle_mm * (detail ? 1.1 : 1.25);
        const Flow nominal(float(configured_width_mm), float(0.5 * height), float(nozzle_mm));
        CHECK(sample.height == Catch::Approx(0.5 * height));
        CHECK(sample.volume_per_mm == Catch::Approx(nominal.mm3_per_mm() * ratio).margin(wall_flow_tolerance_mm3_per_mm));
        const Flow bead(sample.width, sample.height, float(nozzle_mm));
        CHECK(sample.volume_per_mm == Catch::Approx(bead.mm3_per_mm()).margin(wall_flow_tolerance_mm3_per_mm));
    }
    CHECK(bridges > 0);
}

TEST_CASE("Half-height shell keeps XY wall count and full-height core", "[HalfLayer][Geometry]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const int count = GENERATE(1, 2, 4, 7);
    const double height = GENERATE(0.1, 0.2, 0.3);
    const double outside_width = GENERATE(0.42, 0.5);
    const double inside_width = 0.48;
    CAPTURE(arachne, enabled, count, height, outside_width);
    const auto samples = generate_walls(arachne, enabled, count, height, outside_width, inside_width);
    REQUIRE_FALSE(samples.empty());
    std::map<int, size_t> indices;
    for (const WallSample &sample : samples) {
        REQUIRE(sample.inset >= 0);
        REQUIRE(sample.inset < count);
        ++indices[sample.inset];
        const bool outer = sample.inset < (enabled ? 2 : 1);
        const float expected_height = float(enabled && outer ? height * 0.5 : height);
        CHECK(sample.height == Catch::Approx(expected_height));
        CHECK(sample.role == (outer ? erExternalPerimeter : erPerimeter));
        CHECK(sample.tool == ExtrusionToolHint::Auto);
        CHECK(std::isfinite(sample.volume_per_mm));
        CHECK(sample.volume_per_mm > 0.);
        if (enabled) {
            const Flow expected(float(outer ? outside_width : inside_width), expected_height, 0.4f);
            // Width tolerance in mm; one scaled coordinate is 0.000001 mm.
            CHECK(sample.width == Catch::Approx(expected.width()).margin(wall_width_tolerance_mm));
            CHECK(sample.volume_per_mm == Catch::Approx(expected.mm3_per_mm()).margin(wall_flow_tolerance_mm3_per_mm));
        }
    }
    CHECK(indices.size() == size_t(count));
}

TEST_CASE("Half-height outer wall keeps the model XY boundary", "[HalfLayer][Geometry][Boundary]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const double height = GENERATE(0.1, 0.2, 0.3);
    CAPTURE(arachne, enabled, height);
    const auto samples = generate_walls(arachne, enabled, 4, height, 0.42, 0.48);
    double minimum_footprint_x_mm = std::numeric_limits<double>::infinity();
    for (const WallSample &sample : samples)
        if (sample.inset == 0)
            minimum_footprint_x_mm = std::min(minimum_footprint_x_mm, sample.min_x_mm - sample.width * 0.5);
    // Extruded footprint boundary tolerance in mm, including integer clipping rounding.
    CHECK(minimum_footprint_x_mm == Catch::Approx(0.).margin(boundary_tolerance_mm));
}

TEST_CASE("Half-height roles do not reassign detail nozzles or interlock parity", "[HalfLayer][Geometry][MultiNozzle]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const bool interlocking = GENERATE(false, true);
    const int requested_detail = GENERATE(1, 2, 4);
    const int layer_id = GENERATE(10, 11);
    CAPTURE(arachne, enabled, interlocking, requested_detail, layer_id);
    const int base_walls = 3;
    const int expected_total = requested_detail + base_walls;
    const int expected_detail = interlocking && layer_id % 2 == 1 && requested_detail > 1 ?
        requested_detail - 1 : requested_detail;
    const auto samples = generate_walls(arachne, enabled, base_walls, 0.1, 0.44, 0.48,
                                       requested_detail, layer_id, interlocking);
    std::map<int, size_t> indices;
    for (const auto &sample : samples) {
        REQUIRE(sample.inset >= 0);
        REQUIRE(sample.inset < expected_total);
        ++indices[sample.inset];
        const bool detail = sample.inset < expected_detail;
        const bool outer = sample.inset < (enabled ? 2 : 1);
        CHECK(sample.tool == (detail ? ExtrusionToolHint::DetailWall : ExtrusionToolHint::LargeWall));
        CHECK(sample.role == (outer ? erExternalPerimeter : erPerimeter));
        CHECK(sample.height == Catch::Approx(enabled && outer ? 0.05 : 0.1));
        if (enabled) {
            const double expected_width = detail ? 0.22 : (outer ? 0.44 : 0.48);
            // Width tolerance in mm, including integer clipping rounding.
            CHECK(sample.width == Catch::Approx(expected_width).margin(wall_width_tolerance_mm));
        }
    }
    CHECK(indices.size() == size_t(expected_total));
}

TEST_CASE("Half-height infill boundary uses the actual innermost bead spacing", "[HalfLayer][Geometry][Boundary]")
{
    const bool arachne = GENERATE(false, true);
    const int count = GENERATE(1, 2, 4);
    const double height = GENERATE(0.1, 0.2, 0.3);
    CAPTURE(arachne, count, height);
    double fill_min_x_mm = 0.;
    generate_walls(arachne, true, count, height, 0.42, 0.48, 0, 10, false, &fill_min_x_mm);
    const Flow outside(0.42f, float(height * 0.5), 0.4f);
    const Flow inside(0.48f, float(height), 0.4f);
    double expected_min_x_mm = 0.5 * (outside.width() + outside.spacing());
    for (int inset = 1; inset < count; ++inset)
        expected_min_x_mm += inset < 2 ? outside.spacing() : inside.spacing();
    // Infill boundary tolerance in mm after integer offsets and simplification.
    CHECK(fill_min_x_mm == Catch::Approx(expected_min_x_mm).margin(boundary_tolerance_mm));
}

TEST_CASE("Half-height multi-nozzle interlocking keeps a stable core boundary", "[HalfLayer][Geometry][Boundary][MultiNozzle]")
{
    const bool arachne = GENERATE(false, true);
    const int detail_count = GENERATE(2, 3, 4);
    CAPTURE(arachne, detail_count);
    double even_fill_mm = 0.;
    double odd_fill_mm = 0.;
    generate_walls(arachne, true, 3, 0.1, 0.44, 0.48, detail_count, 10, true, &even_fill_mm);
    generate_walls(arachne, true, 3, 0.1, 0.44, 0.48, detail_count, 11, true, &odd_fill_mm);
    // Stable core boundary tolerance in mm; excludes only integer clipping residue.
    CHECK(odd_fill_mm == Catch::Approx(even_fill_mm).margin(boundary_tolerance_mm));
}

TEST_CASE("Half-height shells preserve printable narrow walls", "[HalfLayer][Geometry][Narrow]")
{
    // The existing Classic generator does not print the 0.4 mm rectangle;
    // include it only for Arachne, whose variable-width path is printable.
    const auto [arachne, model_width_mm] = GENERATE(table<bool, double>({
        {false, 0.8}, {false, 1.2}, {false, 2.4},
        {true, 0.4}, {true, 0.8}, {true, 1.2}, {true, 2.4}}));
    CAPTURE(arachne, model_width_mm);
    const auto baseline = generate_walls(arachne, false, 4, 0.1, 0.42, 0.48,
                                         0, 10, false, nullptr, model_width_mm);
    const auto samples = generate_walls(arachne, true, 4, 0.1, 0.42, 0.48,
                                        0, 10, false, nullptr, model_width_mm);
    REQUIRE_FALSE(baseline.empty());
    REQUIRE_FALSE(samples.empty());
    for (const auto &sample : samples) {
        REQUIRE(sample.inset >= 0);
        REQUIRE(sample.inset < 4);
        CHECK(sample.height == Catch::Approx(sample.inset < 2 ? 0.05 : 0.1));
        CHECK(std::isfinite(sample.volume_per_mm));
        CHECK(sample.volume_per_mm > 0.);
    }
}

TEST_CASE("Half-height Classic thin-wall paths use the half band", "[HalfLayer][Geometry][ThinWall]")
{
    const bool enabled = GENERATE(false, true);
    const double height_mm = GENERATE(0.1, 0.2, 0.3);
    const double model_width_mm = GENERATE(0.2, 0.4, 0.6);
    CAPTURE(enabled, height_mm, model_width_mm);
    const auto samples = generate_walls(false, enabled, 4, height_mm, 0.42, 0.48,
        0, 10, false, nullptr, model_width_mm,
        [](PrintRegionConfig &config) { config.detect_thin_wall.value = true; });
    REQUIRE_FALSE(samples.empty());
    for (const auto &sample : samples) {
        CHECK(sample.role == erExternalPerimeter);
        CHECK(sample.height == Catch::Approx(enabled ? height_mm * 0.5 : height_mm));
        if (enabled)
            CHECK(sample.inset == 0);
        const Flow expected(sample.width, sample.height, 0.4f);
        CHECK(sample.volume_per_mm == Catch::Approx(expected.mm3_per_mm()).margin(wall_flow_tolerance_mm3_per_mm));
    }
}

TEST_CASE("Half-height shells retain first and top one-wall exceptions", "[HalfLayer][Geometry][OneWall]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const bool first_layer = GENERATE(false, true);
    CAPTURE(arachne, enabled, first_layer);
    const auto samples = generate_walls(arachne, enabled, 4, 0.2, 0.42, 0.48,
        0, first_layer ? 0 : 10, false, nullptr, 20.,
        [first_layer](PrintRegionConfig &config) {
            config.only_one_wall_first_layer.value = first_layer;
            config.only_one_wall_top.value = !first_layer;
        });
    REQUIRE_FALSE(samples.empty());
    for (const auto &sample : samples) {
        CHECK(sample.inset == 0);
        CHECK(sample.height == Catch::Approx(enabled ? 0.1 : 0.2));
    }
}

TEST_CASE("Half-height shells retain precise outer-wall separation", "[HalfLayer][Geometry][Precise]")
{
    const bool arachne = GENERATE(false, true);
    const bool enabled = GENERATE(false, true);
    const bool precise = GENERATE(false, true);
    const double height_mm = GENERATE(0.1, 0.2, 0.3);
    CAPTURE(arachne, enabled, precise, height_mm);
    const auto samples = generate_walls(arachne, enabled, 4, height_mm, 0.42, 0.48,
        0, 10, false, nullptr, 20., [precise](PrintRegionConfig &config) {
            config.precise_outer_wall.value = precise;
            config.wall_sequence.value = WallSequence::InnerOuter;
        });
    std::map<int, WallSample> by_inset;
    for (const auto &sample : samples)
        by_inset.emplace(sample.inset, sample);
    REQUIRE(by_inset.count(0) == 1);
    REQUIRE(by_inset.count(1) == 1);
    const double actual_distance_mm = by_inset.at(1).min_x_mm - by_inset.at(0).min_x_mm;
    const Flow first(0.42f, float(enabled ? height_mm * 0.5 : height_mm), 0.4f);
    const Flow second(enabled ? 0.42f : 0.48f, float(enabled ? height_mm * 0.5 : height_mm), 0.4f);
    double expected_distance_mm = (first.spacing() + second.spacing()) * 0.5;
    if (precise) {
        // Existing generators intentionally differ: Classic uses full width
        // separation; Arachne shifts only the outermost bead by half its
        // width-minus-spacing correction. Keep both established conventions.
        expected_distance_mm = arachne ? expected_distance_mm + 0.5 * (first.width() - first.spacing()) :
            (first.width() + second.width()) * 0.5;
    }
    CHECK(actual_distance_mm == Catch::Approx(expected_distance_mm).margin(boundary_tolerance_mm));
}
