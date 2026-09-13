#include <catch2/catch_all.hpp>
#include <set>
#include <sstream>
#include "libslic3r/HalfLayerSources.hpp"
#include "libslic3r/HalfLayerWalls.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include <functional>
#include <iostream>
#include "fff_print/test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
constexpr double source_position_tolerance_mm = 0.000005; // Integer mesh intersection rounding.
constexpr double source_area_tolerance_mm2 = 0.001; // Integer clipping residue over a 20 mm square.

double source_area_mm2(const ExPolygons &polygons)
{
    double area_scaled = 0.;
    for (const auto &polygon : polygons)
        area_scaled += polygon.area();
    return area_scaled * SCALING_FACTOR * SCALING_FACTOR;
}

void check_parent_cache(const PrintObject &object, const LayerPtrs &parents,
                        const std::vector<ExPolygons> &slices, const std::vector<VolumeSlices> &volume_cache)
{
    REQUIRE(object.layer_count() == parents.size());
    for (size_t i = 0; i < parents.size(); ++i) {
        CHECK(object.layers()[i] == parents[i]);
        CHECK(object.layers()[i]->lslices == slices[i]);
    }
    REQUIRE(object.firstLayerObjSlice().size() == volume_cache.size());
    for (size_t i = 0; i < volume_cache.size(); ++i) {
        CHECK(object.firstLayerObjSlice()[i].volume_id == volume_cache[i].volume_id);
        CHECK(object.firstLayerObjSlice()[i].slices == volume_cache[i].slices);
    }
}

DynamicPrintConfig source_config(double height, unsigned filaments = 1)
{
    auto config = filaments == 1 ? DynamicPrintConfig::full_print_config() : multifilament_config(filaments);
    config.set_deserialize_strict({{"layer_height", height}, {"initial_layer_print_height", height},
        {"elefant_foot_compensation", 0.}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}});
    return config;
}
}

TEST_CASE("Real half-height source walls retain parent height and identity", "[HalfLayer][Sources][Walls]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double height_mm = GENERATE(0.1, 0.2, 0.3);
    const double shear_per_z = GENERATE(-0.25, 0., 0.25);
    CAPTURE(engine, height_mm, shear_per_z);
    auto config = source_config(height_mm);
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    config.set_deserialize_strict({{"wall_loops", 4}, {"detect_overhang_wall", false},
        {"precise_outer_wall", false}, {"outer_wall_line_width", 0.42}, {"inner_wall_line_width", 0.48}});
    TriangleMesh shape = make_cube(20., 20., 5.);
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = shear_per_z;
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    const size_t parent_index = 5;
    const Layer &parent = *object.layers().at(parent_index);
    const LayerRegionPtrs compatible;
    std::array<double, 2> shell_min_x;
    for (unsigned phase = 0; phase < 2; ++phase) {
        const Layer &source = *sources.phases[phase].at(parent_index);
        const LayerRegion &region = *source.regions().front();
        REQUIRE(region.perimeters.empty());
        CHECK_THROWS_AS(make_half_layer_wall_geometry(region, region.slices, compatible, 0.), std::invalid_argument);
        CHECK_THROWS_AS(make_half_layer_wall_geometry(region, region.slices, compatible,
            std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
        const auto geometry = make_half_layer_wall_geometry(region, region.slices, compatible, parent.height);
        CHECK(region.perimeters.empty());
        CHECK(region.thin_fills.empty());
        CHECK(source.id() == parent.id());
        CHECK(source.height == Catch::Approx(parent.height * 0.5));
        std::set<int> indices;
        double minimum_x_mm = std::numeric_limits<double>::infinity();
        std::function<void(const ExtrusionEntity &)> inspect;
        inspect = [&](const ExtrusionEntity &entity) {
            if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                for (const auto *child : collection->entities)
                    inspect(*child);
                return;
            }
            const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity);
            REQUIRE(loop != nullptr);
            indices.insert(loop->inset_idx);
            for (const auto &path : loop->paths) {
                const bool outer = loop->inset_idx < 2;
                CHECK(path.height == Catch::Approx(outer ? 0.5 * parent.height : parent.height));
                CHECK(path.role() == (outer ? erExternalPerimeter : erPerimeter));
                if (loop->inset_idx == 0)
                    minimum_x_mm = std::min(minimum_x_mm,
                        unscale<double>(get_extents(path.polyline.to_polyline()).min.x()));
            }
        };
        inspect(geometry.perimeters);
        CHECK(indices == std::set<int>{0, 1, 2, 3});
        const auto shell = select_half_layer_wall_group(geometry.perimeters, HalfLayerWallGroup::Shell);
        const auto core = select_half_layer_wall_group(geometry.perimeters, HalfLayerWallGroup::Core);
        indices.clear();
        inspect(*shell);
        CHECK(indices == std::set<int>{0, 1});
        indices.clear();
        inspect(*core);
        CHECK(indices == std::set<int>{2, 3});
        CHECK(shell->total_volume() + core->total_volume() == Catch::Approx(geometry.perimeters.total_volume()));
        REQUIRE(std::isfinite(minimum_x_mm));
        shell_min_x[phase] = minimum_x_mm;
        const double section_left_mm = unscale<double>(get_extents(source.lslices).min.x());
        CHECK(minimum_x_mm - 0.21 == Catch::Approx(section_left_mm).margin(source_position_tolerance_mm));
    }
    CHECK(shell_min_x[1] - shell_min_x[0] == Catch::Approx(shear_per_z * 0.5 * parent.height).margin(source_position_tolerance_mm));
    CHECK(parent.regions().front()->perimeters.empty());
}

TEST_CASE("Half-layer group selection preserves mixed loops and nested routing", "[HalfLayer][Sources][WallGroups]")
{
    ExtrusionEntityCollection source;
    source.no_sort = true;
    auto nested = std::make_unique<ExtrusionEntityCollection>();
    nested->set_reverse();
    nested->inset_idx = 7;
    nested->tool_hint = ExtrusionToolHint::LargeWall;
    for (int depth = 0; depth < 4; ++depth) {
        ExtrusionPath supported(erExternalPerimeter, 0.04, 0.42, 0.1);
        supported.polyline = Polyline3(Polyline{Point(0, 0), Point(scaled<coord_t>(10.), 0)});
        ExtrusionPath bridge(erOverhangPerimeter, 0.035, 0.37, 0.1);
        bridge.polyline = Polyline3(Polyline{Point(scaled<coord_t>(10.), 0), Point(0, 0)});
        ExtrusionLoop loop(ExtrusionPaths{supported, bridge});
        loop.inset_idx = depth;
        loop.tool_hint = depth == 0 ? ExtrusionToolHint::DetailWall : ExtrusionToolHint::LargeWall;
        nested->append(loop);
    }
    source.entities.push_back(nested.release());
    for (const auto group : {HalfLayerWallGroup::Shell, HalfLayerWallGroup::Core}) {
        const auto selected = select_half_layer_wall_group(source, group);
        CHECK(selected->no_sort);
        REQUIRE(selected->entities.size() == 1);
        const auto *selected_nested = dynamic_cast<const ExtrusionEntityCollection *>(selected->entities.front());
        REQUIRE(selected_nested != nullptr);
        CHECK_FALSE(selected_nested->can_reverse());
        CHECK(selected_nested->inset_idx == 7);
        CHECK(selected_nested->tool_hint == ExtrusionToolHint::LargeWall);
        REQUIRE(selected_nested->entities.size() == 2);
        for (size_t i = 0; i < 2; ++i) {
            const int depth = int(i) + (group == HalfLayerWallGroup::Shell ? 0 : 2);
            const auto *loop = dynamic_cast<const ExtrusionLoop *>(selected_nested->entities[i]);
            REQUIRE(loop != nullptr);
            CHECK(loop->inset_idx == depth);
            CHECK(loop->tool_hint == (depth == 0 ? ExtrusionToolHint::DetailWall : ExtrusionToolHint::LargeWall));
            REQUIRE(loop->paths.size() == 2);
            CHECK(loop->paths[0].role() == erExternalPerimeter);
            CHECK(loop->paths[1].role() == erOverhangPerimeter);
            CHECK(loop->paths.back().last_point() == loop->paths.front().first_point());
        }
    }
    const auto volume_before = source.total_volume();
    ExtrusionPath unknown(erOverhangPerimeter, 0.04, 0.42, 0.1);
    source.append(unknown);
    CHECK_THROWS_AS(select_half_layer_wall_group(source, HalfLayerWallGroup::Shell), std::invalid_argument);
    CHECK_THROWS_AS(select_half_layer_wall_group(source, HalfLayerWallGroup::Core), std::invalid_argument);
    CHECK(source.total_volume() == Catch::Approx(volume_before));
}

// Diagnostic of the deliberately unassembled source outputs, NOT a passing
// acceptance test for printable shell/core contact. This fixture prevents a
// future scheduler from treating independently generated paths as a joint plan.
TEST_CASE("Independent half-layer walls expose sloped shell-core contact gaps", "[HalfLayer][Sources][ContactDiagnostic]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double height_mm = GENERATE(0.1, 0.2, 0.3);
    const double shear_per_z = GENERATE(0., 0.25, 1.);
    CAPTURE(engine, height_mm, shear_per_z);
    auto config = source_config(height_mm);
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    config.set_deserialize_strict({{"wall_loops", 4}, {"detect_overhang_wall", false},
        {"precise_outer_wall", false}, {"outer_wall_line_width", 0.42}, {"inner_wall_line_width", 0.48}});
    TriangleMesh shape = make_cube(20., 20., 5.);
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = shear_per_z;
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    const auto &parent = *object.layers().at(5);
    const auto &parent_region = *parent.regions().front();
    const auto &lower_region = *sources.phases[0].at(5)->regions().front();
    const LayerRegionPtrs compatible;
    const auto parent_geometry = make_half_layer_wall_geometry(parent_region, parent_region.slices, compatible, parent.height);
    const auto lower_geometry = make_half_layer_wall_geometry(lower_region, lower_region.slices, compatible, parent.height);
    const auto core = select_half_layer_wall_group(parent_geometry.perimeters, HalfLayerWallGroup::Core);
    const auto shell = select_half_layer_wall_group(lower_geometry.perimeters, HalfLayerWallGroup::Shell);
    Polygons coverage = core->polygons_covered_by_width();
    shell->polygons_covered_by_width(coverage, 0.f);
    // Independent 0.30 x 16 mm probe across the expected left wall/core join,
    // far from fixture corners. No planner coverage thresholds define it.
    const auto bounds = get_extents(parent.lslices);
    const double left_mm = unscale<double>(bounds.min.x());
    const double bottom_mm = unscale<double>(bounds.min.y());
    const auto point = [](double x_mm, double y_mm) { return Point(scaled<coord_t>(x_mm), scaled<coord_t>(y_mm)); };
    const ExPolygons probe{ExPolygon(Polygon{
        point(left_mm + 0.70, bottom_mm + 2.), point(left_mm + 1.00, bottom_mm + 2.),
        point(left_mm + 1.00, bottom_mm + 18.), point(left_mm + 0.70, bottom_mm + 18.)}),
        ExPolygon(Polygon{point(left_mm + 19.00, bottom_mm + 2.), point(left_mm + 19.30, bottom_mm + 2.),
        point(left_mm + 19.30, bottom_mm + 18.), point(left_mm + 19.00, bottom_mm + 18.)})};
    const double uncovered_mm2 = source_area_mm2(diff_ex(probe, union_ex(coverage)));
    // Analytic rectangular-bead overlap is 3H/4 * (1 - pi/4); source shift
    // is shear*H/4. Compare the emitted geometry to that independent model.
    const double expected_gap_mm = std::max(0., parent.height * (0.25 * shear_per_z - 0.75 * (1. - PI / 4.)));
    CHECK(uncovered_mm2 == Catch::Approx(16. * expected_gap_mm).margin(source_area_tolerance_mm2));
    CHECK((uncovered_mm2 > source_area_tolerance_mm2) == (shear_per_z == 1.));
    std::cout << "half_layer_contact_diagnostic engine=" << int(engine) << " parent_h_mm=" << height_mm
              << " shear=" << shear_per_z << " uncovered_mm2=" << uncovered_mm2 << '\n';

    // Exercise the production candidate owner; this affine fixture alone does
    // not qualify changing topology, gap-fill ownership or final scheduling.
    const auto &upper_region = *sources.phases[1].at(5)->regions().front();
    const auto candidate = make_half_layer_region_wall_candidate(parent_region, lower_region, upper_region);
    const auto core_coverage = candidate.core->polygons_covered_by_width();
    for (unsigned phase = 0; phase < 2; ++phase) {
        const auto &region = *sources.phases[phase].at(5)->regions().front();
        Polygons coupled_coverage = core_coverage;
        candidate.shells[phase]->polygons_covered_by_width(coupled_coverage, 0.f);
        const double remaining_gap_mm2 = source_area_mm2(diff_ex(probe, union_ex(coupled_coverage)));
        const double core_outside_section_mm2 = source_area_mm2(diff_ex(union_ex(core_coverage), to_expolygons(region.slices.surfaces)));
        CHECK(remaining_gap_mm2 <= source_area_tolerance_mm2);
        CHECK(core_outside_section_mm2 <= source_area_tolerance_mm2);
        std::cout << "half_layer_coupled_candidate phase=" << phase << " gap_mm2=" << remaining_gap_mm2
                  << " core_outside_section_mm2=" << core_outside_section_mm2 << '\n';
    }
}

TEST_CASE("Detached half-layer sources sample the actual mesh and retain parent identity", "[HalfLayer][Sources]")
{
    const double height = GENERATE(0.1, 0.2, 0.3);
    const double xy_compensation = GENERATE(-0.2, 0., 0.2);
    CAPTURE(height, xy_compensation);
    auto config = source_config(height);
    config.set_deserialize_strict({{"xy_contour_compensation", xy_compensation}});
    TriangleMesh shape = cube(20.);
    Transform3d shear = Transform3d::Identity();
    constexpr double x_shear_per_z = 0.25; // Dimensionless XY shift / mesh Z.
    shear.matrix()(0, 2) = x_shear_per_z;
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const LayerPtrs parents = object.layers();
    std::vector<ExPolygons> parent_slices;
    for (const Layer *layer : parents)
        parent_slices.push_back(layer->lslices);
    const auto volume_cache = object.firstLayerObjSlice();
    const auto sources = object.make_half_layer_source_layers();
    check_parent_cache(object, parents, parent_slices, volume_cache);
    REQUIRE(parents.size() > 10);
    for (unsigned phase = 0; phase < 2; ++phase) {
        // A quarter-plane can be above this fixture's 20 mm mesh even though
        // the parent's midpoint is still inside. Such top sections are empty.
        size_t expected_source_count = 0;
        for (const Layer *parent : parents) {
            const double sample_z_mm = parent->slice_z + (phase == 0 ? -0.25 : 0.25) * parent->height;
            if (sample_z_mm < 20.) // Analytic fixture top, in mm.
                ++expected_source_count;
        }
        REQUIRE(sources.phases[phase].size() == expected_source_count);
        for (size_t i = 0; i < expected_source_count; ++i) {
            const Layer &source = *sources.phases[phase][i];
            const Layer &parent = *parents[i];
            CHECK(source.id() == parent.id());
            CHECK(source.height == Catch::Approx(parent.height * 0.5));
            CHECK(source.print_z == Catch::Approx(parent.bottom_z() + (phase + 1) * parent.height * 0.5));
            const double phase_shift_mm = (phase == 0 ? -0.25 : 0.25) * parent.height;
            CHECK(source.slice_z == Catch::Approx(parent.slice_z + phase_shift_mm));
            const double actual_x_shift_mm = unscale<double>(get_extents(source.lslices).min.x() - get_extents(parent.lslices).min.x());
            CHECK(actual_x_shift_mm == Catch::Approx(phase_shift_mm * x_shear_per_z).margin(source_position_tolerance_mm));
        }
    }
    LayerPtrs physical_layers;
    for (size_t i = 0; i < parents.size(); ++i)
        for (unsigned phase = 0; phase < 2; ++phase)
            if (i < sources.phases[phase].size())
                physical_layers.push_back(sources.phases[phase][i].get());
    for (size_t i = 0; i < physical_layers.size(); ++i) {
        CHECK(physical_layers[i]->lower_layer == (i == 0 ? nullptr : physical_layers[i - 1]));
        CHECK(physical_layers[i]->upper_layer == (i + 1 == physical_layers.size() ? nullptr : physical_layers[i + 1]));
    }
}

TEST_CASE("A detached upper source grid may be entirely outside a thin mesh", "[HalfLayer][Sources][Empty]")
{
    const double mesh_height_mm = GENERATE(0.13, 0.18);
    const bool material_interlocking = GENERATE(false, true);
    CAPTURE(mesh_height_mm, material_interlocking);
    Print print;
    Model model;
    auto config = source_config(0.2, material_interlocking ? 2 : 1);
    if (material_interlocking) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({{"single_extruder_multi_material", false}, {"interlocking_beam", true}});
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
        config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    }
    init_print({TriangleMesh(make_cube(20., 20., mesh_height_mm))}, print, model, config);
    if (material_interlocking) {
        auto *modifier = model.objects.front()->add_volume(make_cube(10., 20., mesh_height_mm), ModelVolumeType::PARAMETER_MODIFIER);
        modifier->config.set_key_value("extruder", new ConfigOptionInt(2));
        modifier->config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
        print.apply(model, config);
    }
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const LayerPtrs parents = object.layers();
    REQUIRE(parents.size() == 1);
    if (material_interlocking) {
        REQUIRE(object.num_printing_regions() == 2);
        CHECK(object.printing_region(0).extruder(frExternalPerimeter) != object.printing_region(1).extruder(frExternalPerimeter));
    }
    const std::vector<ExPolygons> parent_slices {parents.front()->lslices};
    const auto volume_cache = object.firstLayerObjSlice();
    const auto sources = object.make_half_layer_source_layers();
    check_parent_cache(object, parents, parent_slices, volume_cache);
    REQUIRE(sources.phases[0].size() == 1);
    CHECK(source_area_mm2(sources.phases[0].front()->lslices) == Catch::Approx(400.).margin(source_area_tolerance_mm2));
    CHECK(sources.phases[1].size() == (mesh_height_mm < 0.15 ? 0 : 1)); // Upper sample plane, in mm.
}

TEST_CASE("Detached source failure restores model layers and first-layer cache", "[HalfLayer][Sources][Exception]")
{
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, source_config(0.2));
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const LayerPtrs parents = object.layers();
    REQUIRE(parents.size() > 1);
    std::vector<ExPolygons> parent_slices;
    for (const Layer *layer : parents)
        parent_slices.push_back(layer->lslices);
    const auto volume_cache = object.firstLayerObjSlice();
    // Fail after allocating the first detached layer, inside the scoped grid.
    const double saved_height_mm = parents[1]->height;
    parents[1]->height = 0.;
    CHECK_THROWS_AS(object.make_half_layer_source_layers(), std::invalid_argument);
    parents[1]->height = saved_height_mm;
    check_parent_cache(object, parents, parent_slices, volume_cache);
    CHECK_NOTHROW(object.make_half_layer_source_layers());
}

TEST_CASE("Detached sources retain negative volumes and parameter modifier regions", "[HalfLayer][Sources][Modifiers]")
{
    const bool negative_volume = GENERATE(false, true);
    CAPTURE(negative_volume);
    const auto config = source_config(0.2);
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    auto *model_object = model.objects.front();
    if (negative_volume) {
        auto cutout = make_cube(4., 4., 30.);
        cutout.translate(Vec3f(8.f, 8.f, -5.f));
        model_object->add_volume(std::move(cutout), ModelVolumeType::NEGATIVE_VOLUME);
    }
    auto *modifier = model_object->add_volume(make_cube(10., 20., 20.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set_key_value("wall_loops", new ConfigOptionInt(6));
    print.apply(model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const auto sources = object.make_half_layer_source_layers();
    const double expected_total_area_mm2 = negative_volume ? 384. : 400.;
    for (unsigned phase = 0; phase < 2; ++phase) {
        REQUIRE(sources.phases[phase].size() > 10);
        const Layer &layer = *sources.phases[phase][10];
        CHECK(source_area_mm2(layer.lslices) == Catch::Approx(expected_total_area_mm2).margin(source_area_tolerance_mm2));
        unsigned nonempty_regions = 0;
        for (const LayerRegion *region : layer.regions()) {
            if (region->slices.empty())
                continue;
            ++nonempty_regions;
            CHECK(source_area_mm2(to_expolygons(region->slices.surfaces)) ==
                  Catch::Approx(expected_total_area_mm2 * 0.5).margin(source_area_tolerance_mm2));
        }
        CHECK(nonempty_regions == 2);
    }
}

TEST_CASE("Detached sources retain polyhole conversion through the model height", "[HalfLayer][Sources][Polyhole]")
{
    const bool polyholes = GENERATE(false, true);
    const bool twisted = GENERATE(false, true);
    CAPTURE(polyholes, twisted);
    auto config = source_config(0.2);
    config.set_deserialize_strict({{"hole_to_polyhole", polyholes}, {"hole_to_polyhole_twisted", twisted}});
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    auto cutout = make_cylinder(3., 30.);
    cutout.translate(Vec3f(10.f, 10.f, -5.f));
    model.objects.front()->add_volume(std::move(cutout), ModelVolumeType::NEGATIVE_VOLUME);
    print.apply(model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const auto sources = object.make_half_layer_source_layers();
    for (unsigned phase = 0; phase < 2; ++phase) {
        for (const size_t index : {size_t(0), size_t(1), size_t(10), size_t(50), size_t(98)}) {
            CAPTURE(phase, index);
            const auto &parent = object.layers()[index]->lslices;
            const auto &source = sources.phases[phase][index]->lslices;
            REQUIRE(parent.size() == 1);
            REQUIRE(source.size() == 1);
            REQUIRE(parent.front().holes.size() == 1);
            REQUIRE(source.front().holes.size() == 1);
            const auto parent_vertices = parent.front().holes.front().size();
            const auto source_vertices = source.front().holes.front().size();
            CAPTURE(parent_vertices, source_vertices);
            // A straight analytic bore must retain the same polygonization at
            // either actual Z sample. Rotation is irrelevant for this check.
            CHECK(source_vertices == parent_vertices);
            CHECK(source_area_mm2(source) == Catch::Approx(source_area_mm2(parent)).margin(source_area_tolerance_mm2));
            if (polyholes && source_vertices == parent_vertices) {
                const Point source_center = source.front().holes.front().centroid();
                const Point parent_center = parent.front().holes.front().centroid();
                CAPTURE(source_center, parent_center);
                double max_vertex_error_mm = 0.;
                for (size_t vertex = 0; vertex < source_vertices; ++vertex) {
                    const Point source_vector = source.front().holes.front().points[vertex] - source_center;
                    const Point parent_vector = parent.front().holes.front().points[vertex] - parent_center;
                    max_vertex_error_mm = std::max(max_vertex_error_mm,
                        unscale<double>(source_vector.distance_to(parent_vector)));
                }
                // Compare twist around each polygon's own center: different
                // mesh planes need not have bit-identical clipped centroids.
                CHECK(max_vertex_error_mm <= source_position_tolerance_mm);
            }
        }
    }
}

TEST_CASE("Detached sources retain valid Z contouring sample offsets", "[HalfLayer][Sources][ZContouring]")
{
    const double sample_offset_mm = GENERATE(0.02, 0.05, 0.08);
    CAPTURE(sample_offset_mm);
    auto config = source_config(0.2);
    config.set_deserialize_strict({{"zaa_enabled", true}, {"zaa_min_z", sample_offset_mm}});
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const auto sources = object.make_half_layer_source_layers();
    const double object_offset_mm = object.slicing_parameters().object_print_z_min;
    for (unsigned phase = 0; phase < 2; ++phase) {
        for (size_t i = 0; i < sources.phases[phase].size(); ++i) {
            const Layer &source = *sources.phases[phase][i];
            const double expected_offset_mm = i == 0 ? 0.05 : sample_offset_mm; // First band uses its midpoint.
            CHECK(source.slice_z == Catch::Approx(source.bottom_z() - object_offset_mm + expected_offset_mm));
            CHECK(source_area_mm2(source.lslices) == Catch::Approx(400.).margin(source_area_tolerance_mm2));
        }
    }
}

TEST_CASE("An incompatible Z contouring offset does not corrupt original layers", "[HalfLayer][Sources][ZContouring][Exception]")
{
    auto config = source_config(0.2);
    config.set_deserialize_strict({{"zaa_enabled", true}, {"zaa_min_z", 0.15}}); // In parent band, outside 0.1 mm half band.
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const LayerPtrs parents = object.layers();
    std::vector<ExPolygons> parent_slices;
    for (const Layer *layer : parents)
        parent_slices.push_back(layer->lslices);
    const auto volume_cache = object.firstLayerObjSlice();
    CHECK_THROWS_WITH(object.make_half_layer_source_layers(), "Bad min Z value");
    check_parent_cache(object, parents, parent_slices, volume_cache);
}

TEST_CASE("Detached sources use physical raft offset and each variable parent height", "[HalfLayer][Sources][VariableHeight]")
{
    const int raft_layers = GENERATE(0, 3);
    CAPTURE(raft_layers);
    auto config = source_config(0.2);
    config.set_deserialize_strict({{"raft_layers", raft_layers}});
    Print print;
    Model model;
    init_print({cube(20.)}, print, model, config);
    model.objects.front()->layer_height_profile.set({0., 0.2, 6., 0.12, 12., 0.28, 20., 0.16});
    print.apply(model, config);
    PrintObject &object = *print.get_object(size_t(0));
    object.slice();
    const auto sources = object.make_half_layer_source_layers();
    std::set<int> heights_um;
    const double object_offset_mm = object.slicing_parameters().object_print_z_min;
    CHECK((object_offset_mm > 0.) == (raft_layers > 0));
    for (unsigned phase = 0; phase < 2; ++phase) {
        for (size_t i = 0; i < sources.phases[phase].size(); ++i) {
            const Layer &parent = *object.layers()[i];
            const Layer &source = *sources.phases[phase][i];
            heights_um.insert(int(std::lround(parent.height * 1000.)));
            CHECK(source.id() == parent.id());
            CHECK(source.height == Catch::Approx(parent.height * 0.5));
            CHECK(source.print_z == Catch::Approx(parent.bottom_z() + (phase + 1) * parent.height * 0.5));
            CHECK(source.slice_z == Catch::Approx(source.print_z - source.height * 0.5 - object_offset_mm));
            CHECK(source_area_mm2(source.lslices) == Catch::Approx(400.).margin(source_area_tolerance_mm2));
        }
    }
    CHECK(heights_um.size() > 1);
}

TEST_CASE("Building and discarding detached sources preserves ordinary G-code commands", "[HalfLayer][Sources][OffRegression]")
{
    const std::string wall_generator = GENERATE(std::string("classic"), std::string("arachne"));
    CAPTURE(wall_generator);
    auto config = source_config(0.2);
    config.set_deserialize_strict({{"wall_generator", wall_generator}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    const auto commands = [](const std::string &output) {
        // Ignore only comments/blank lines (including export timestamps).
        // Every machine command, argument and their ordering remains exact.
        std::istringstream stream(output);
        std::string result, line;
        while (std::getline(stream, line)) {
            const auto comment = line.find(';');
            if (comment != std::string::npos)
                line.resize(comment);
            const auto end = line.find_last_not_of(" \t\r");
            if (end != std::string::npos)
                result += line.substr(0, end + 1) + '\n';
        }
        return result;
    };
    std::array<std::string, 2> output_commands;
    for (unsigned use_sources = 0; use_sources < 2; ++use_sources) {
        Print print;
        Model model;
        init_print({cube(5.)}, print, model, config);
        if (use_sources) {
            auto &object = *print.get_object(size_t(0));
            const auto sources = object.make_half_layer_source_layers();
            REQUIRE_FALSE(sources.phases[0].empty());
            REQUIRE_FALSE(sources.phases[1].empty());
            const LayerRegionPtrs compatible;
            for (unsigned phase = 0; phase < 2; ++phase)
                for (size_t i = 0; i < sources.phases[phase].size(); ++i)
                    for (const auto *region : sources.phases[phase][i]->regions()) {
                        const auto geometry = make_half_layer_wall_geometry(*region, region->slices, compatible, object.layers()[i]->height);
                        const auto shell = select_half_layer_wall_group(geometry.perimeters, HalfLayerWallGroup::Shell);
                        REQUIRE_FALSE(shell->empty());
                        REQUIRE(region->perimeters.empty());
                    }
            for (size_t i = 0; i < std::min(sources.phases[0].size(), sources.phases[1].size()); ++i) {
                const auto candidate = make_half_layer_region_wall_candidate(*object.layers()[i]->regions().front(),
                    *sources.phases[0][i]->regions().front(), *sources.phases[1][i]->regions().front());
                CHECK(candidate.bands[0].model_layer_id == object.layers()[i]->id());
                CHECK(object.layers()[i]->regions().front()->perimeters.empty());
            }
        }
        output_commands[use_sources] = commands(gcode(print));
        REQUIRE_FALSE(output_commands[use_sources].empty());
    }
    const bool machine_commands_identical = output_commands[0] == output_commands[1];
    CHECK(machine_commands_identical);
}

TEST_CASE("Detached sources preserve painted material ownership including empty upper grids", "[HalfLayer][Sources][Painting]")
{
    const double mesh_height_mm = GENERATE(0.13, 0.18, 2.);
    const bool painted = GENERATE(false, true);
    const double outer_width_mm = GENERATE(0., 0.22); // Zero selects automatic Flow resolution.
    CAPTURE(mesh_height_mm, painted, outer_width_mm);
    auto config = source_config(0.2, 2);
    config.set_num_extruders(2);
    config.set_deserialize_strict({{"single_extruder_multi_material", false}, {"outer_wall_line_width", outer_width_mm}});
    set_toolhead_nozzle_diameter(config, 0, 0.4);
    set_toolhead_nozzle_diameter(config, 1, 0.2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    Print print;
    Model model;
    init_print({make_cube(20., 20., mesh_height_mm)}, print, model, config);
    if (painted) {
        auto *volume = model.objects.front()->volumes.front();
        TriangleSelector selector(volume->mesh());
        for (size_t i = 0; i < volume->mesh().its.indices.size(); ++i)
            selector.set_facet(int(i), EnforcerBlockerType::Extruder2);
        REQUIRE(volume->mmu_segmentation_facets.set(selector));
        print.apply(model, config);
    }
    PrintObject &object = *print.get_object(size_t(0));
    REQUIRE(print.config().filament_diameter.size() == 2);
    object.slice();
    for (const LayerRegion *region : object.layers().front()->regions())
        if (source_area_mm2(to_expolygons(region->slices.surfaces)) > source_area_tolerance_mm2)
            REQUIRE(region->region().extruder(frExternalPerimeter) == (painted ? 2 : 1));
    const auto sources = object.make_half_layer_source_layers();
    for (unsigned phase = 0; phase < 2; ++phase) {
        CHECK(sources.phases[phase].empty() == (phase == 1 && mesh_height_mm < 0.15)); // Upper sample plane, in mm.
        for (const auto &source : sources.phases[phase]) {
            double material_area_mm2 = 0.;
            for (const LayerRegion *region : source->regions()) {
                const double region_area_mm2 = source_area_mm2(to_expolygons(region->slices.surfaces));
                if (region_area_mm2 > source_area_tolerance_mm2) {
                    CHECK(region->region().extruder(frExternalPerimeter) == (painted ? 2 : 1)); // One-based material IDs.
                    CHECK(region->flow(frExternalPerimeter).nozzle_diameter() == Catch::Approx(painted ? 0.2 : 0.4));
                    CHECK(region->flow(frExternalPerimeter).height() == Catch::Approx(0.1));
                }
                material_area_mm2 += region_area_mm2;
            }
            CHECK(material_area_mm2 == Catch::Approx(400.).margin(source_area_tolerance_mm2));
        }
    }
}
