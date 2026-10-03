#include <catch2/catch_all.hpp>
#include "libslic3r/HalfLayerSources.hpp"
#include "libslic3r/GCode.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/HalfLayerSupportSources.hpp"
#include "libslic3r/HalfLayerWalls.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/GCode/SeamPlacer.hpp"
#include "libslic3r/GCode/WipeTower2.hpp"
#include "libslic3r/Preset.hpp"
#include "fff_print/test_helpers.hpp"
#include <boost/filesystem/operations.hpp>
#include <algorithm>
#include <array>
#include <iostream>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <fstream>
#include "libslic3r/GCode/HalfLayerExecution.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportParameters.hpp"
#include <cstdlib>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {
constexpr double assembly_area_tolerance_mm2 = 0.001; // Fixed-point polygon residue, mm2.
double area_mm2(const ExPolygons &polygons)
{
    double result = 0.;
    for (const auto &polygon : polygons)
        result += polygon.area() * SCALING_FACTOR * SCALING_FACTOR;
    return result;
}

TEST_CASE("Coupled candidate rejects mismatched bands and preserves surface requests", "[HalfLayer][Assembly][Identity]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false}});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    auto sources = object.make_half_layer_source_layers();
    auto &parent = *object.layers().at(5)->regions().front();
    auto &lower = *sources.phases[0].at(5)->regions().front();
    auto &upper = *sources.phases[1].at(5)->regions().front();
    CHECK_THROWS_AS(make_half_layer_region_wall_candidate(parent, upper, lower), std::invalid_argument);
    CHECK_THROWS_AS(make_half_layer_region_wall_candidate(*object.layers().at(6)->regions().front(), lower, upper), std::invalid_argument);
    const double source_h_mm = lower.layer()->height;
    lower.layer()->height = parent.layer()->height;
    CHECK_THROWS_AS(make_half_layer_region_wall_candidate(parent, lower, upper), std::invalid_argument);
    lower.layer()->height = source_h_mm;
    upper.slices.surfaces.front().extra_perimeters = 1;
    CHECK_THROWS_AS(make_half_layer_region_wall_candidate(parent, lower, upper), std::invalid_argument);
    for (auto *region : {&parent, &lower, &upper})
        for (auto &surface : region->slices.surfaces)
            surface.extra_perimeters = 1;
    const auto candidate = make_half_layer_region_wall_candidate(parent, lower, upper);
    for (const auto &surface : candidate.core_slices.surfaces)
        CHECK(surface.extra_perimeters == 1);
    std::set<int> depths;
    const auto flattened = candidate.core->flatten();
    for (const auto *entity : flattened.entities)
        depths.insert(entity->inset_idx);
    CHECK(depths == std::set<int>{2, 3, 4});
    CHECK(parent.perimeters.empty());
    CHECK(lower.perimeters.empty());
    CHECK(upper.perimeters.empty());
}
}

TEST_CASE("Coupled core candidate stays within both hollow source sections", "[HalfLayer][Assembly][Candidate]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double parent_h_mm = GENERATE(0.1, 0.2, 0.3);
    const double ring_thickness_mm = GENERATE(0.6, 1.2, 2., 5.);
    const bool detail_nozzle = GENERATE(false, true);
    CAPTURE(engine, parent_h_mm, ring_thickness_mm, detail_nozzle);
    auto config = detail_nozzle ? multifilament_config(2) : DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", parent_h_mm}, {"initial_layer_print_height", parent_h_mm},
        {"elefant_foot_compensation", 0.}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"wall_loops", 4}, {"detect_overhang_wall", false}, {"detect_thin_wall", true},
        {"precise_outer_wall", false}, {"outer_wall_line_width", 0.42}, {"inner_wall_line_width", 0.48},
        {"use_smaller_nozzles_in_crisp_corners", detail_nozzle}, {"crisp_corner_small_nozzle_wall_count", 2},
        {"crisp_corner_interlace_small_nozzle_walls", true}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    if (detail_nozzle) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({{"single_extruder_multi_material", false}});
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    }
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = 1.; // 45-degree XY/Z shear, dimensionless.
    TriangleMesh shape = make_cube(20., 20., 5.);
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto cutout = make_cube(20. - 2. * ring_thickness_mm, 20. - 2. * ring_thickness_mm, 7.);
    cutout.translate(Vec3f(float(ring_thickness_mm), float(ring_thickness_mm), -1.f));
    cutout.transform(shear);
    model.objects.front()->add_volume(std::move(cutout), ModelVolumeType::NEGATIVE_VOLUME);
    print.apply(model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    for (const size_t parent_index : {size_t(5), size_t(6)}) {
        CAPTURE(parent_index);
        const auto &parent = *object.layers().at(parent_index);
        const auto &parent_region = *parent.regions().front();
        const auto &lower = *sources.phases[0].at(parent_index)->regions().front();
        const auto &upper = *sources.phases[1].at(parent_index)->regions().front();
        const auto candidate = make_half_layer_region_wall_candidate(parent_region, lower, upper);
        for (unsigned source = 0; source < 3; ++source) {
            const auto &demands = source < 2 ? candidate.phase_gap_demands[source] : candidate.core_gap_demands;
            const auto &original = source < 2 ? candidate.unassigned_phase_gap_fill[source] : candidate.unassigned_core_gap_fill;
            ExtrusionEntityCollection regenerated;
            for (const auto &demand : demands) {
                auto output = make_perimeter_gap_fill(demand);
                regenerated.append(std::move(output.entities));
            }
            CHECK(regenerated.total_volume() == Catch::Approx(original.total_volume()));
            const auto rebuilt_area = union_ex(regenerated.polygons_covered_by_width());
            const auto original_area = union_ex(original.polygons_covered_by_width());
            CHECK(area_mm2(diff_ex(rebuilt_area, original_area)) <= assembly_area_tolerance_mm2);
            CHECK(area_mm2(diff_ex(original_area, rebuilt_area)) <= assembly_area_tolerance_mm2);
        }
        const auto &core = candidate.core;
        const auto coverage = union_ex(core->polygons_covered_by_width());
        CHECK(candidate.print_object_region_id == parent_region.region().print_object_region_id());
        CHECK(candidate.bands[0].model_layer_id == parent.id());
        CHECK(candidate.bands[1].model_layer_id == parent.id());
        CHECK(candidate.bands[0].height_mm() == Catch::Approx(0.5 * parent.height));
        CHECK(parent_region.perimeters.empty());
        CHECK(lower.perimeters.empty());
        CHECK(upper.perimeters.empty());
        double worst_outside_mm2 = 0.;
        for (const auto *region : {&lower, &upper}) {
            const double outside_mm2 = area_mm2(diff_ex(coverage, to_expolygons(region->slices.surfaces)));
            worst_outside_mm2 = std::max(worst_outside_mm2, outside_mm2);
            CHECK(outside_mm2 <= assembly_area_tolerance_mm2);
        }
        const double duplicate_gap_area_mm2 = area_mm2(intersection_ex(
            union_ex(candidate.unassigned_phase_gap_fill[0].polygons_covered_by_width()),
            union_ex(candidate.unassigned_core_gap_fill.polygons_covered_by_width())));
        // A named diagnostic fixture, not production shape-specific logic:
        // retaining both unassigned copies would deposit material twice in a
        // visibly overlapping gap. This candidate is not yet an emission plan.
        if (engine == PerimeterGeneratorType::Classic && parent_h_mm == 0.2 && ring_thickness_mm == 1.2 && !detail_nozzle) {
            constexpr double visible_duplicate_gap_area_mm2 = 1.; // Independent 1 mm2 overlap floor.
            CHECK(duplicate_gap_area_mm2 > visible_duplicate_gap_area_mm2);
            REQUIRE(candidate.gap_fill.has_value());
            CHECK(candidate.gap_fill->core.empty());
        }
        if (candidate.gap_fill) {
            CHECK((candidate.gap_ownership == HalfLayerGapOwnership::NoGaps ||
                candidate.gap_ownership == HalfLayerGapOwnership::ShellOnly ||
                candidate.gap_ownership == HalfLayerGapOwnership::JointDemand));
            for (unsigned phase = 0; phase < 2; ++phase) {
                const auto assigned = union_ex(candidate.gap_fill->phases[phase].polygons_covered_by_width());
                const auto source_gap = union_ex(candidate.unassigned_phase_gap_fill[phase].polygons_covered_by_width());
                if (candidate.gap_ownership != HalfLayerGapOwnership::JointDemand) {
                    CHECK(candidate.gap_fill->phases[phase].total_volume() ==
                        Catch::Approx(candidate.unassigned_phase_gap_fill[phase].total_volume()));
                    CHECK(area_mm2(diff_ex(assigned, source_gap)) <= assembly_area_tolerance_mm2);
                    CHECK(area_mm2(diff_ex(source_gap, assigned)) <= assembly_area_tolerance_mm2);
                }
                CHECK(area_mm2(intersection_ex(assigned,
                    union_ex(candidate.gap_fill->core.polygons_covered_by_width()))) <= assembly_area_tolerance_mm2);
            }
        } else {
            // Unresolved physical core/fill is not an empty successful plan.
            CHECK(candidate.gap_ownership == HalfLayerGapOwnership::NeedsJointCore);
            CHECK((!candidate.core->empty() || !candidate.core_fill_surfaces.empty() || !candidate.core_fill_no_overlap.empty()));
        }
        std::cout << "coupled_hollow engine=" << int(engine) << " h_mm=" << parent_h_mm
                  << " ring_mm=" << ring_thickness_mm << " detail=" << detail_nozzle << " parent=" << parent_index
                  << " core_volume_mm3=" << core->total_volume() << " outside_mm2=" << worst_outside_mm2
                  << " phase0_gap_volume_mm3=" << candidate.unassigned_phase_gap_fill[0].total_volume()
                  << " core_gap_volume_mm3=" << candidate.unassigned_core_gap_fill.total_volume()
                  << " unassigned_duplicate_gap_mm2=" << duplicate_gap_area_mm2 << '\n';
    }
}

TEST_CASE("Joint gaps preserve a narrow neck attached to full-height core islands", "[HalfLayer][Assembly][JointGaps]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const double shear_ratio = GENERATE(0., 1.);
    const bool detail = GENERATE(false, true);
    CAPTURE(engine, h_mm, shear_ratio, detail);
    auto config = detail ? multifilament_config(2) : DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", 4}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"elefant_foot_compensation", 0.}, {"detect_overhang_wall", false}, {"detect_thin_wall", true},
        {"precise_outer_wall", false}, {"outer_wall_line_width", 0.42}, {"inner_wall_line_width", 0.48},
        {"use_smaller_nozzles_in_crisp_corners", detail}, {"crisp_corner_small_nozzle_wall_count", 2},
        {"crisp_corner_interlace_small_nozzle_walls", true}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    if (detail) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({{"single_extruder_multi_material", false}});
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    }
    auto shape = make_cube(8., 8., 5.);
    auto neck = make_cube(18., 1.2, 5.);
    neck.translate(Vec3f(7.f, 3.4f, 0.f));
    shape.merge(neck);
    auto right = make_cube(8., 8., 5.);
    right.translate(Vec3f(24.f, 0.f, 0.f));
    shape.merge(right);
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = shear_ratio;
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    for (const size_t parent_index : {size_t(5), size_t(6)}) {
        const auto &parent = *object.layers().at(parent_index)->regions().front();
        const std::array<const LayerRegion *, 2> phases{
            sources.phases[0].at(parent_index)->regions().front(), sources.phases[1].at(parent_index)->regions().front()};
        const auto candidate = make_half_layer_region_wall_candidate(parent, *phases[0], *phases[1]);
        CAPTURE(parent_index, candidate.gap_ownership);
        REQUIRE_FALSE(candidate.core->empty());
        REQUIRE(candidate.gap_fill.has_value());
        if (engine == PerimeterGeneratorType::Classic && !detail)
            CHECK(candidate.gap_ownership == HalfLayerGapOwnership::JointDemand);
        for (unsigned phase = 0; phase < 2; ++phase) {
            Polygons material = candidate.core->polygons_covered_by_width();
            polygons_append(material, candidate.shells[phase]->polygons_covered_by_width());
            polygons_append(material, candidate.gap_fill->core.polygons_covered_by_width());
            polygons_append(material, candidate.gap_fill->phases[phase].polygons_covered_by_width());
            const auto box = get_extents(to_expolygons(phases[phase]->slices.surfaces));
            // Independent analytic neck demand: central 12 x 1.1 mm rectangle,
            // inset 0.05 mm from the mesh sides, away from the end transitions.
            const ExPolygons probe{ExPolygon(Polygon{Points{
                box.min + Point::new_scale(10., 3.45), box.min + Point::new_scale(22., 3.45),
                box.min + Point::new_scale(22., 4.55), box.min + Point::new_scale(10., 4.55)}})};
            constexpr double minimum_coverage_fraction = 0.99; // Dimensionless fraction of independent 13.2 mm2 demand.
            const double coverage_fraction = area_mm2(intersection_ex(probe, material)) / 13.2;
            CAPTURE(phase, coverage_fraction);
            CHECK(coverage_fraction >= minimum_coverage_fraction);
            const auto phase_gap_area = union_ex(candidate.gap_fill->phases[phase].polygons_covered_by_width());
            CHECK(area_mm2(diff_ex(phase_gap_area, to_expolygons(phases[phase]->slices.surfaces))) <= assembly_area_tolerance_mm2);
        }
        std::cout << "joint_neck engine=" << int(engine) << " h_mm=" << h_mm << " shear=" << shear_ratio
                  << " detail=" << detail << " parent=" << parent_index
                  << " core_gap_mm3=" << candidate.gap_fill->core.total_volume()
                  << " lower_gap_mm3=" << candidate.gap_fill->phases[0].total_volume()
                  << " upper_gap_mm3=" << candidate.gap_fill->phases[1].total_volume() << '\n';
    }
}

TEST_CASE("Shell-only half-layer gap ownership preserves physical coverage without virtual core deposition", "[HalfLayer][Assembly][GapOwnership]")
{
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const double shear_ratio = GENERATE(0., 1.);
    const int wall_count = GENERATE(2, 4);
    CAPTURE(h_mm, shear_ratio, wall_count);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", wall_count}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"elefant_foot_compensation", 0.}, {"detect_overhang_wall", false}, {"detect_thin_wall", true},
        {"precise_outer_wall", false}, {"outer_wall_line_width", 0.42}, {"inner_wall_line_width", 0.48}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(PerimeterGeneratorType::Classic));
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = shear_ratio;
    auto shape = make_cube(20., 20., 5.);
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    // Analytic 1.2 mm ring, with 0.1 x 16 mm probes through the centers of
    // its two vertical sides. Probe sizes do not depend on planner widths.
    auto cutout = make_cube(17.6, 17.6, 7.);
    cutout.translate(Vec3f(1.2f, 1.2f, -1.f));
    cutout.transform(shear);
    model.objects.front()->add_volume(std::move(cutout), ModelVolumeType::NEGATIVE_VOLUME);
    print.apply(model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    const auto &parent = *object.layers().at(5)->regions().front();
    const std::array<const LayerRegion *, 2> phases{
        sources.phases[0].at(5)->regions().front(), sources.phases[1].at(5)->regions().front()};
    const auto candidate = make_half_layer_region_wall_candidate(parent, *phases[0], *phases[1]);
    REQUIRE(candidate.core->empty());
    REQUIRE(candidate.core_fill_surfaces.empty());
    REQUIRE(candidate.gap_fill.has_value());
    CHECK(candidate.gap_ownership == HalfLayerGapOwnership::ShellOnly);
    REQUIRE(candidate.gap_fill->core.empty());
    double assigned_volume_mm3 = 0.;
    for (unsigned phase = 0; phase < 2; ++phase) {
        const auto &gaps = candidate.gap_fill->phases[phase];
        REQUIRE_FALSE(gaps.empty());
        assigned_volume_mm3 += gaps.total_volume();
        const auto gap_coverage = union_ex(gaps.polygons_covered_by_width());
        const auto box = get_extents(to_expolygons(phases[phase]->slices.surfaces));
        for (const double x_mm : {0.6, 19.4}) {
            const coord_t x = box.min.x() + scale_(x_mm);
            const coord_t y = box.min.y();
            const ExPolygons probe{ExPolygon(Polygon{Points{
                Point(x - scale_(0.05), y + scale_(2.)), Point(x + scale_(0.05), y + scale_(2.)),
                Point(x + scale_(0.05), y + scale_(18.)), Point(x - scale_(0.05), y + scale_(18.))}})};
            constexpr double minimum_probe_coverage_fraction = 0.99; // Dimensionless fraction of an independent 1.6 mm2 probe.
            const double coverage_fraction = area_mm2(intersection_ex(probe, gap_coverage)) / 1.6;
            CAPTURE(phase, x_mm, coverage_fraction);
            CHECK(coverage_fraction >= minimum_probe_coverage_fraction);
        }
        const auto flattened = gaps.flatten();
        const auto inspect_path = [h_mm](const ExtrusionPath &path) {
            CHECK(path.role() == erGapFill);
            CHECK(path.height == Catch::Approx(0.5 * h_mm));
            CHECK(path.mm3_per_mm == Catch::Approx(Flow(path.width, path.height, 0.f).mm3_per_mm()));
        };
        for (const auto *entity : flattened.entities) {
            if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity)) {
                inspect_path(*path);
            } else if (const auto *paths = dynamic_cast<const ExtrusionMultiPath *>(entity)) {
                for (const auto &path : paths->paths)
                    inspect_path(path);
            } else {
                const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity);
                REQUIRE(loop != nullptr);
                for (const auto &path : loop->paths)
                    inspect_path(path);
            }
        }
    }
    const double concatenated_volume_mm3 = candidate.unassigned_phase_gap_fill[0].total_volume() +
        candidate.unassigned_phase_gap_fill[1].total_volume() + candidate.unassigned_core_gap_fill.total_volume();
    CHECK(assigned_volume_mm3 == Catch::Approx(candidate.unassigned_phase_gap_fill[0].total_volume() +
        candidate.unassigned_phase_gap_fill[1].total_volume()));
    CHECK(concatenated_volume_mm3 > assigned_volume_mm3);
    std::cout << "shell_gap_ownership h_mm=" << h_mm << " shear=" << shear_ratio << " walls=" << wall_count
              << " assigned_volume_mm3=" << assigned_volume_mm3 << " concatenated_volume_mm3=" << concatenated_volume_mm3 << '\n';
}

TEST_CASE("Coupled region candidates retain material ownership and mixed-nozzle wall counts", "[HalfLayer][Assembly][Materials]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const int requested_detail = GENERATE(1, 2, 3);
    CAPTURE(engine, h_mm, requested_detail);
    auto config = multifilament_config(2);
    config.set_num_extruders(2);
    set_toolhead_nozzle_diameter(config, 0, 0.4);
    set_toolhead_nozzle_diameter(config, 1, 0.2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    config.set_key_value("filament_map_mode", new ConfigOptionEnum<FilamentMapMode>(fmmManual));
    config.set_key_value("filament_colour", new ConfigOptionStrings{"#FF0000", "#FF0000"});
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"single_extruder_multi_material", false}, {"elefant_foot_compensation", 0.},
        {"wall_loops", 4}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"detect_overhang_wall", false}, {"precise_outer_wall", false},
        {"use_smaller_nozzles_in_crisp_corners", true}, {"crisp_corner_small_nozzle_wall_count", requested_detail},
        {"crisp_corner_interlace_small_nozzle_walls", true}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = 1.;
    TriangleMesh shape = make_cube(20., 20., 5.);
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto modifier_mesh = make_cube(10., 20., 7.);
    modifier_mesh.translate(Vec3f(0.f, 0.f, -1.f));
    modifier_mesh.transform(shear);
    auto *modifier = model.objects.front()->add_volume(std::move(modifier_mesh), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set_key_value("extruder", new ConfigOptionInt(2));
    modifier->config.set_key_value("outer_wall_filament_id", new ConfigOptionInt(2));
    modifier->config.set_key_value("inner_wall_filament_id", new ConfigOptionInt(2));
    print.apply(model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    for (const size_t parent_index : {size_t(5), size_t(6)}) {
        const auto &parent = *object.layers().at(parent_index);
        REQUIRE(parent.regions().size() == 2);
        std::vector<ExPolygons> material_core_coverage;
        for (size_t region_index = 0; region_index < 2; ++region_index) {
            const auto &region = *parent.regions()[region_index];
            const auto &lower = *sources.phases[0].at(parent_index)->regions()[region_index];
            const auto &upper = *sources.phases[1].at(parent_index)->regions()[region_index];
            const auto candidate = make_half_layer_region_wall_candidate(region, lower, upper);
            const int filament = int(region.region().extruder(frExternalPerimeter));
            CAPTURE(parent_index, region_index, filament);
            CHECK(candidate.print_object_region_id == region.region().print_object_region_id());
            const auto coverage = union_ex(candidate.core->polygons_covered_by_width());
            material_core_coverage.push_back(coverage);
            for (const auto *phase : {&lower, &upper}) {
                CHECK(area_mm2(diff_ex(coverage, to_expolygons(phase->slices.surfaces))) <= assembly_area_tolerance_mm2);
                CHECK(area_mm2(diff_ex(candidate.core_fill_no_overlap, to_expolygons(phase->slices.surfaces))) <= assembly_area_tolerance_mm2);
            }
            std::function<void(const ExtrusionEntity &, bool, std::set<int> &)> inspect;
            inspect = [&](const ExtrusionEntity &entity, bool shell, std::set<int> &depths) {
                if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                    for (const auto *child : collection->entities)
                        inspect(*child, shell, depths);
                    return;
                }
                const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity);
                REQUIRE(loop != nullptr);
                depths.insert(loop->inset_idx);
                const int effective_detail = requested_detail > 1 && parent_index % 2 == 1 ? requested_detail - 1 : requested_detail;
                const bool uses_detail = filament == 1 && loop->inset_idx < effective_detail;
                CHECK((loop->tool_hint == ExtrusionToolHint::DetailWall) == uses_detail);
                for (const auto &path : loop->paths) {
                    CHECK(path.height == Catch::Approx(shell ? 0.5 * parent.height : parent.height));
                    CHECK(path.role() == (shell ? erExternalPerimeter : erPerimeter));
                }
            };
            std::set<int> core_depths;
            inspect(*candidate.core, false, core_depths);
            for (unsigned phase = 0; phase < 2; ++phase) {
                auto all_depths = core_depths;
                inspect(*candidate.shells[phase], true, all_depths);
                const int expected_total = filament == 1 ? requested_detail + 4 : 4;
                CHECK(all_depths.size() == size_t(expected_total));
            }
        }
        CHECK(area_mm2(intersection_ex(material_core_coverage[0], material_core_coverage[1])) <= assembly_area_tolerance_mm2);
    }
}

TEST_CASE("Half-layer assembly merges compatible walls and restores modifier fill ownership", "[HalfLayer][Assembly][RegionGrouping]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const int extra_walls = GENERATE(0, 1);
    CAPTURE(engine, h_mm, extra_walls);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", 4}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"elefant_foot_compensation", 0.}, {"detect_overhang_wall", false}, {"precise_outer_wall", false}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    Print print;
    Model model;
    init_print({make_cube(20., 20., 5.)}, print, model, config);
    auto modifier_mesh = make_cube(10., 20., 7.);
    modifier_mesh.translate(Vec3f(0.f, 0.f, -1.f));
    auto *modifier = model.objects.front()->add_volume(std::move(modifier_mesh), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->config.set_key_value("sparse_infill_density", new ConfigOptionPercent(60.));
    print.apply(model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    auto &parent = *object.layers().at(5);
    auto &lower = *sources.phases[0].at(5);
    auto &upper = *sources.phases[1].at(5);
    REQUIRE(parent.region_count() == 2);
    REQUIRE(Layer::is_perimeter_compatible(print, parent.get_region(0)->region(), parent.get_region(1)->region()));
    for (auto *layer : {&parent, &lower, &upper})
        for (auto *region : layer->regions())
            for (auto &surface : region->slices.surfaces)
                surface.extra_perimeters = extra_walls;
    const auto candidate = make_half_layer_layer_candidate(parent, lower, upper);
    REQUIRE(candidate.groups.size() == 1);
    REQUIRE(candidate.fills.size() == 2);
    const auto &walls = candidate.groups.front();
    const int owner = parent.get_region(0)->region().config().sparse_infill_density >
        parent.get_region(1)->region().config().sparse_infill_density ? 0 : 1;
    CHECK(walls.print_object_region_id == parent.get_region(owner)->region().print_object_region_id());
    const auto box = get_extents(parent.lslices);
    const ExPolygons boundary_probe{ExPolygon(Polygon{Points{
        box.min + Point::new_scale(9.2, 4.), box.min + Point::new_scale(10.8, 4.),
        box.min + Point::new_scale(10.8, 16.), box.min + Point::new_scale(9.2, 16.)}})};
    Polygons all_walls = walls.core->polygons_covered_by_width();
    polygons_append(all_walls, walls.shells[0]->polygons_covered_by_width());
    CHECK(area_mm2(intersection_ex(boundary_probe, all_walls)) <= assembly_area_tolerance_mm2);
    const ExPolygons fill_probe{ExPolygon(Polygon{Points{
        box.min + Point::new_scale(4., 4.), box.min + Point::new_scale(16., 4.),
        box.min + Point::new_scale(16., 16.), box.min + Point::new_scale(4., 16.)}})};
    ExPolygons all_fill;
    for (size_t region = 0; region < 2; ++region) {
        const auto fill = to_expolygons(candidate.fills[region].surfaces.surfaces);
        CHECK(area_mm2(diff_ex(fill, to_expolygons(parent.get_region(int(region))->slices.surfaces))) <= assembly_area_tolerance_mm2);
        append(all_fill, fill);
        CHECK(parent.get_region(int(region))->perimeters.empty());
    }
    CHECK(area_mm2(intersection_ex(to_expolygons(candidate.fills[0].surfaces.surfaces),
        to_expolygons(candidate.fills[1].surfaces.surfaces))) <= assembly_area_tolerance_mm2);
    constexpr double minimum_fill_coverage_fraction = 0.99; // Independent 144 mm2 central square, dimensionless fraction.
    CHECK(area_mm2(intersection_ex(fill_probe, all_fill)) / 144. >= minimum_fill_coverage_fraction);
    std::set<int> depths;
    const auto flattened = walls.core->flatten();
    for (const auto *entity : flattened.entities)
        depths.insert(entity->inset_idx);
    CHECK(depths.size() == size_t(2 + extra_walls));
}

TEST_CASE("Half-layer zero-wall candidates preserve original parent fill boundaries", "[HalfLayer][Assembly][ZeroWalls]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const bool unavailable_detail = GENERATE(false, true);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", 0}, {"use_smaller_nozzles_in_crisp_corners", unavailable_detail},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = 1.;
    auto shape = make_cube(20., 20., 5.);
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    auto &parent = *object.layers().at(5)->regions().front();
    const auto candidate = make_half_layer_region_wall_candidate(parent,
        *sources.phases[0].at(5)->regions().front(), *sources.phases[1].at(5)->regions().front());
    CAPTURE(engine, h_mm, unavailable_detail);
    CHECK(candidate.gap_ownership == HalfLayerGapOwnership::Unsplit);
    REQUIRE(candidate.gap_fill.has_value());
    CHECK(candidate.shells[0]->empty());
    CHECK(candidate.shells[1]->empty());
    CHECK(parent.perimeters.empty());
    SurfaceCollection original_fill;
    ExPolygons original_no_overlap;
    parent.make_perimeters(parent.slices, {&parent}, &original_fill, &original_no_overlap);
    CHECK(area_mm2(diff_ex(candidate.core_fill_surfaces.surfaces, original_fill.surfaces)) <= assembly_area_tolerance_mm2);
    CHECK(area_mm2(diff_ex(original_fill.surfaces, candidate.core_fill_surfaces.surfaces)) <= assembly_area_tolerance_mm2);
    CHECK(area_mm2(diff_ex(candidate.core_fill_no_overlap, original_no_overlap)) <= assembly_area_tolerance_mm2);
    CHECK(area_mm2(diff_ex(original_no_overlap, candidate.core_fill_no_overlap)) <= assembly_area_tolerance_mm2);
}

TEST_CASE("Half-layer assembly retains separate surface requests and empty physical sections", "[HalfLayer][Assembly][SurfaceGroups]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    auto shape = make_cube(8., 8., 5.);
    auto right = make_cube(8., 8., 5.);
    right.translate(Vec3f(12.f, 0.f, 0.f));
    shape.merge(right);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    const auto sources = object.make_half_layer_source_layers();
    auto &parent = *object.layers().at(5);
    auto &lower = *sources.phases[0].at(5);
    auto &upper = *sources.phases[1].at(5);
    // Island classification boundary, scaled coordinate (10 mm from model min).
    const coord_t island_split_x = get_extents(parent.lslices).min.x() + scale_(10.);
    for (auto *layer : {&parent, &lower, &upper})
        for (auto &surface : layer->get_region(0)->slices.surfaces)
            surface.extra_perimeters = get_extents(surface.expolygon).min.x() < island_split_x ? 1 : 0;
    const auto candidate = make_half_layer_layer_candidate(parent, lower, upper);
    REQUIRE(candidate.groups.size() == 2);
    std::set<int> requests;
    for (const auto &group : candidate.groups) {
        REQUIRE_FALSE(group.core_slices.empty());
        const int extra = group.core_slices.surfaces.front().extra_perimeters;
        requests.insert(extra);
        std::set<int> depths;
        const auto flattened = group.core->flatten();
        for (const auto *entity : flattened.entities)
            depths.insert(entity->inset_idx);
        CHECK(depths.size() == size_t(2 + extra));
    }
    CHECK(requests == std::set<int>{0, 1});
    upper.get_region(0)->slices.clear();
    const auto upper_empty = make_half_layer_layer_candidate(parent, lower, upper);
    REQUIRE(upper_empty.groups.size() == 2);
    for (const auto &group : upper_empty.groups) {
        REQUIRE(group.gap_fill.has_value());
        CHECK_FALSE(group.shells[0]->empty());
        CHECK(group.shells[1]->empty());
        CHECK(group.gap_fill->phases[1].empty());
    }
    lower.get_region(0)->slices.clear();
    const auto both_empty = make_half_layer_layer_candidate(parent, lower, upper);
    REQUIRE(both_empty.groups.size() == 2);
    for (const auto &group : both_empty.groups) {
        CHECK(group.gap_ownership == HalfLayerGapOwnership::Unsplit);
        CHECK(group.shells[0]->empty());
        CHECK(group.shells[1]->empty());
        CHECK_FALSE(group.core->empty());
    }
}

TEST_CASE("Half-height wall setting owns live source geometry and invalidates on repeated toggles", "[HalfLayer][LiveWalls]")
{
    const auto engine = GENERATE(PerimeterGeneratorType::Classic, PerimeterGeneratorType::Arachne);
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const int walls = GENERATE(0, 1, 2, 4);
    auto config = DynamicPrintConfig::full_print_config();
    REQUIRE_FALSE(config.opt_bool("outer_wall_half_layer_height"));
    REQUIRE_FALSE(config.opt_bool("support_half_layer_height"));
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", walls}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false}});
    config.set_key_value("wall_generator", new ConfigOptionEnum<PerimeterGeneratorType>(engine));
    Print print;
    Model model;
    init_print({make_cube(20., 20., 5.)}, print, model, config);
    for (const bool enabled : {false, true, false, true, false}) {
        config.set_key_value("outer_wall_half_layer_height", new ConfigOptionBool(enabled));
        print.apply(model, config);
        auto &object = *print.get_object(size_t(0));
        print.process();
        CAPTURE(engine, h_mm, walls, enabled);
        REQUIRE(object.is_step_done(posPerimeters));
        const auto *sources = object.half_layer_sources();
        CHECK((sources != nullptr) == enabled);
        const auto &parent = *object.layers().at(5);
        const auto &core = parent.get_region(0)->perimeters;
        std::set<int> core_depths;
        const auto flattened = core.flatten();
        for (const auto *entity : flattened.entities)
            core_depths.insert(entity->inset_idx);
        CHECK(core_depths.size() == size_t(enabled ? std::max(0, walls - 2) : walls));
        if (sources != nullptr) {
            for (unsigned phase = 0; phase < 2; ++phase) {
                REQUIRE(sources->phases[phase].size() == object.layer_count());
                const auto &layer = *sources->phases[phase][5];
                CHECK(layer.id() == parent.id());
                CHECK(layer.height == Catch::Approx(0.5 * h_mm));
                CHECK(layer.print_z == Catch::Approx(parent.bottom_z() + (phase + 1) * 0.5 * h_mm));
                std::set<int> shell_depths;
                const auto shells = layer.get_region(0)->perimeters.flatten();
                for (const auto *entity : shells.entities) {
                    shell_depths.insert(entity->inset_idx);
                    CHECK(entity->role() == erExternalPerimeter);
                }
                CHECK(shell_depths.size() == size_t(std::min(2, walls)));
            }
        }
    }
}

TEST_CASE("Shell-only half layers participate in first-layer and empty-layer validation",
    "[HalfLayer][EmptyLayerSafety]")
{
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const int walls = GENERATE(1, 2, 4);
    CAPTURE(engine, walls);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"wall_generator", engine}, {"wall_loops", walls},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"outer_wall_half_layer_height", true}, {"top_shell_layers", 0}, {"bottom_shell_layers", 0},
        {"sparse_infill_density", "0%"}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    struct ExportStagePrint : Print {
        using Print::invalidate_step;
        using Print::set_started;
        using Print::set_done;
    } print;
    Model model;
    init_print({make_cube(10., 10., 3.)}, print, model, config);
    REQUIRE_NOTHROW(print.process());
    auto &object = *print.get_object(size_t(0));
    const auto *sources = object.half_layer_sources();
    REQUIRE(sources != nullptr);
    REQUIRE(sources->phases[0][0]->has_extrusions());
    REQUIRE(sources->phases[1][0]->has_extrusions());
    REQUIRE_FALSE(gcode(print).empty());
    for (const auto &warning : print.step_state_with_warnings(psGCodeExport).warnings)
        CHECK(warning.message_id != PrintStateBase::SlicingEmptyGcodeLayers);

    auto clear_physical = [](Layer &layer) {
        for (auto *region : layer.regions()) {
            region->perimeters.clear();
            region->fills.clear();
        }
    };
    // A real internal gap must still warn; accepting shell-only layers must not
    // turn off the safety check. Remove 1.0 mm of every physical source.
    for (size_t index = 3; index < 8; ++index) {
        clear_physical(*object.layers()[index]);
        for (unsigned phase = 0; phase < 2; ++phase)
            clear_physical(*sources->phases[phase][index]);
    }
    print.invalidate_step(psGCodeExport);
    REQUIRE(print.set_started(psGCodeExport));
    GCode::collect_layers_to_print(object);
    const auto warnings = print.step_state_with_warnings(psGCodeExport).warnings;
    CHECK(std::any_of(warnings.begin(), warnings.end(), [](const auto &warning) {
        return warning.message_id == PrintStateBase::SlicingEmptyGcodeLayers;
    }));
    print.set_done(psGCodeExport);
    // Delete every physical source at the first parent: this really is empty.
    clear_physical(*object.layers().front());
    for (unsigned phase = 0; phase < 2; ++phase)
        clear_physical(*sources->phases[phase].front());
    REQUIRE_THROWS_AS(GCode::collect_layers_to_print(object), Slic3r::SlicingError);
}

TEST_CASE("Half-layer settings survive process-preset and 3MF project round trips",
    "[HalfLayer][Persistence]")
{
    const std::array<std::string, 2> keys{
        "outer_wall_half_layer_height", "support_half_layer_height"};
    const std::vector<std::string> &print_options = Preset::print_options();
    for (const std::string &key : keys)
        REQUIRE(std::find(print_options.begin(), print_options.end(), key) != print_options.end());

    DynamicPrintConfig source_config = DynamicPrintConfig::full_print_config();
    source_config.set_deserialize_strict({
        {keys[0], true},
        {keys[1], true},
    });

    const boost::filesystem::path preset_path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("magpie-half-layer-preset-%%%%-%%%%.json");
    source_config.save_to_json(preset_path.string(), "Half-layer round trip", "User", "2.5.0");
    DynamicPrintConfig preset_config;
    std::map<std::string, std::string> preset_metadata;
    std::string preset_reason;
    const ConfigSubstitutions preset_substitutions = preset_config.load_from_json(
        preset_path.string(), ForwardCompatibilitySubstitutionRule::Disable,
        preset_metadata, preset_reason);
    boost::filesystem::remove(preset_path);
    REQUIRE(preset_reason.empty());
    REQUIRE(preset_substitutions.empty());
    for (const std::string &key : keys)
        CHECK(preset_config.opt_bool(key));

    Model source_model;
    ModelObject *source_object = source_model.add_object();
    source_object->add_volume(cube(10.));
    source_object->add_instance();
    std::set<std::pair<int, int>> object_instances{{0, 0}};
    PlateData source_plate(0, object_instances, false);
    const boost::filesystem::path project_path = boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("magpie-half-layer-project-%%%%-%%%%.3mf");
    const std::string project_path_string = project_path.string();
    StoreParams store;
    store.path = project_path_string.c_str();
    store.model = &source_model;
    store.plate_data_list = {&source_plate};
    store.config = &source_config;
    store.strategy = SaveStrategy::Silence | SaveStrategy::Zip64 | SaveStrategy::SplitModel;
    REQUIRE(store_bbs_3mf(store));

    Model restored_model;
    DynamicPrintConfig restored_config;
    ConfigSubstitutionContext project_substitutions{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs restored_plates;
    std::vector<Preset *> restored_presets;
    bool is_bambu = false;
    bool is_orca = false;
    Semver version;
    const bool loaded = load_bbs_3mf(project_path_string.c_str(), &restored_config,
        &project_substitutions, &restored_model, &restored_plates, &restored_presets,
        &is_bambu, &is_orca, &version, nullptr,
        LoadStrategy::LoadModel | LoadStrategy::LoadConfig | LoadStrategy::AddDefaultInstances);
    release_PlateData_list(restored_plates);
    for (Preset *preset : restored_presets)
        delete preset;
    boost::filesystem::remove(project_path);
    REQUIRE(loaded);
    for (const std::string &key : keys)
        CHECK(restored_config.opt_bool(key));
}

TEST_CASE("Disabling live half-height walls restores every ordinary machine command", "[HalfLayer][LiveWalls][OffRecovery]")
{
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const double h_mm = GENERATE(0.1, 0.3);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_loops", 4}, {"wall_generator", engine}, {"skirt_loops", 0}, {"brim_type", "no_brim"},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false}});
    const auto commands = [](const std::string &output) {
        std::istringstream input(output);
        std::string result, line;
        while (std::getline(input, line)) {
            const auto comment = line.find(';');
            if (comment != std::string::npos)
                line.resize(comment);
            const auto end = line.find_last_not_of(" \t\r");
            if (end != std::string::npos)
                result += line.substr(0, end + 1) + '\n';
        }
        return result;
    };
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    const std::string before = commands(gcode(print));
    REQUIRE_FALSE(before.empty());
    for (int repeat = 0; repeat < 2; ++repeat) {
        config.set_key_value("outer_wall_half_layer_height", new ConfigOptionBool(true));
        print.apply(model, config);
        print.process();
        REQUIRE(print.get_object(size_t(0))->half_layer_sources() != nullptr);
        config.set_key_value("outer_wall_half_layer_height", new ConfigOptionBool(false));
        print.apply(model, config);
        const auto after = commands(gcode(print));
        CAPTURE(engine, h_mm, repeat);
        CHECK(after == before);
        CHECK(print.get_object(size_t(0))->half_layer_sources() == nullptr);
    }
}

TEST_CASE("Live half-height walls meet generated solid infill at narrow neck transitions", "[HalfLayer][LiveWalls][SolidContact]")
{
    const bool enabled = GENERATE(false, true);
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    const bool detail = GENERATE(false, true);
    const double shear_ratio = GENERATE(0., 1.);
    auto config = detail ? multifilament_config(2) : DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"wall_generator", engine}, {"wall_loops", 4}, {"outer_wall_half_layer_height", enabled},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false},
        {"elefant_foot_compensation", 0.}, {"sparse_infill_density", 100.}, {"sparse_infill_pattern", "rectilinear"},
        {"use_smaller_nozzles_in_crisp_corners", detail}, {"crisp_corner_small_nozzle_wall_count", 2},
        {"crisp_corner_interlace_small_nozzle_walls", true}});
    if (detail) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({{"single_extruder_multi_material", false}});
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    }
    auto shape = make_cube(8., 8., 5.);
    auto neck = make_cube(18., 1.2, 5.);
    neck.translate(Vec3f(7.f, 3.4f, 0.f));
    shape.merge(neck);
    auto right = make_cube(8., 8., 5.);
    right.translate(Vec3f(24.f, 0.f, 0.f));
    shape.merge(right);
    Transform3d shear = Transform3d::Identity();
    shear.matrix()(0, 2) = shear_ratio;
    shape.transform(shear);
    Print print;
    Model model;
    init_print({shape}, print, model, config);
    auto &object = *print.get_object(size_t(0));
    print.process();
    REQUIRE(object.is_step_done(posInfill));
    const auto *sources = object.half_layer_sources();
    REQUIRE((sources != nullptr) == enabled);
    for (const size_t index : {size_t(5), size_t(6)}) {
        const auto &core = *object.layers()[index]->get_region(0);
        REQUIRE_FALSE(core.fills.empty());
        for (unsigned phase = 0; phase < 2; ++phase) {
            Polygons material = core.perimeters.polygons_covered_by_width();
            polygons_append(material, core.fills.polygons_covered_by_width());
            if (sources) {
                const auto &shell = *sources->phases[phase][index]->get_region(0);
                polygons_append(material, shell.perimeters.polygons_covered_by_width());
                polygons_append(material, shell.fills.polygons_covered_by_width());
            }
            const Point origin = get_extents(to_expolygons(core.slices.surfaces)).min +
                Point::new_scale(shear_ratio * (phase == 0 ? -0.25 : 0.25) * h_mm, 0.);
            const auto rectangle = [&origin](double x0, double y0, double x1, double y1) {
                return ExPolygon(Polygon{Points{origin + Point::new_scale(x0, y0), origin + Point::new_scale(x1, y0),
                    origin + Point::new_scale(x1, y1), origin + Point::new_scale(x0, y1)}});
            };
            // Independent mesh-derived demands, including the neck-to-infill joins.
            const ExPolygons probes{rectangle(0.05, 0.05, 7.95, 7.95), rectangle(24.05, 0.05, 31.95, 7.95),
                rectangle(7.95, 3.45, 24.05, 4.55)};
            const double coverage = area_mm2(intersection_ex(probes, material)) / (2. * 7.9 * 7.9 + 16.1 * 1.1);
            // The OFF counterfactual reaches only 98.4043% over these complete
            // boxes (half-layer-solid-contact-counterfactual.xml). Retain this
            // broad diagnostic, but judge the new join on independent contact
            // strips instead of attributing existing interior voids to it.
            constexpr double minimum_bulk_coverage_fraction = 0.98; // Dimensionless whole-mesh probe fraction.
            CAPTURE(enabled, engine, h_mm, detail, shear_ratio, index, phase, coverage);
            std::cout << "solid_contact enabled=" << enabled << " engine=" << engine << " h=" << h_mm
                      << " detail=" << detail << " shear=" << shear_ratio << " index=" << index
                      << " phase=" << phase << " coverage=" << coverage << '\n';
            for (size_t probe_index = 0; probe_index < probes.size(); ++probe_index)
                std::cout << " probe=" << probe_index << " missing_mm2="
                          << area_mm2(diff_ex(ExPolygons{probes[probe_index]}, material)) << '\n';
            CHECK(coverage >= minimum_bulk_coverage_fraction);
            const ExPolygons contacts{
                rectangle(0.8, 2.4, 2.4, 5.6), rectangle(5.6, 2.4, 7.2, 5.6),
                rectangle(2.4, 0.8, 5.6, 2.4), rectangle(2.4, 5.6, 5.6, 7.2),
                rectangle(24.8, 2.4, 26.4, 5.6), rectangle(29.6, 2.4, 31.2, 5.6),
                rectangle(26.4, 0.8, 29.6, 2.4), rectangle(26.4, 5.6, 29.6, 7.2),
                rectangle(7.2, 3.45, 10., 4.55), rectangle(22., 3.45, 24.8, 4.55)};
            constexpr double contact_demand_mm2 = 8. * 1.6 * 3.2 + 2. * 2.8 * 1.1; // Disjoint analytic rectangles, mm2.
            const double contact_coverage = area_mm2(intersection_ex(contacts, material)) / contact_demand_mm2;
            // OFF contact strips reach 98.0591%, so 99% was not a valid
            // baseline gate (half-layer-live-contact-strips.xml). Also require
            // the same coverage in ONE connected deposited component: an
            // annular wall/infill separation cannot pass by summing both sides.
            constexpr double minimum_contact_coverage_fraction = 0.98; // Dimensionless independent join demand fraction.
            CAPTURE(contact_coverage);
            CHECK(contact_coverage >= minimum_contact_coverage_fraction);
            const auto components = union_ex(material);
            double connected_contact_coverage = 0.;
            for (const auto &component : components)
                connected_contact_coverage = std::max(connected_contact_coverage,
                    area_mm2(intersection_ex(contacts, ExPolygons{component})) / contact_demand_mm2);
            CAPTURE(connected_contact_coverage);
            CHECK(connected_contact_coverage >= minimum_contact_coverage_fraction);
            std::cout << " contact_coverage=" << contact_coverage << " connected_contact_coverage="
                      << connected_contact_coverage << " components=" << components.size() << '\n';
        }
    }
}

TEST_CASE("Tool and seam consumers retain auxiliary physical walls under their logical parent", "[HalfLayer][Consumers]")
{
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const double h_mm = GENERATE(0.2, 0.3);
    const int detail_count = GENERATE(1, 2);
    auto config = multifilament_config(2);
    config.set_num_extruders(2);
    set_toolhead_nozzle_diameter(config, 0, 0.4);
    set_toolhead_nozzle_diameter(config, 1, 0.2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    config.set_deserialize_strict({{"single_extruder_multi_material", false}, {"wall_generator", engine},
        {"layer_height", h_mm}, {"initial_layer_print_height", h_mm}, {"wall_loops", 2},
        {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"detect_overhang_wall", false}, {"use_smaller_nozzles_in_crisp_corners", true},
        // This fixture intentionally needs both different-colored materials;
        // Auto must not substitute another color just to obtain a small nozzle.
        {"crisp_corner_detail_toolhead", 2}, {"crisp_corner_small_nozzle_wall_count", detail_count}});
    config.set_key_value("seam_position", new ConfigOptionEnum<SeamPosition>(spRear));
    Print print;
    Model model;
    init_print({make_cube(20., 20., 3.)}, print, model, config);
    print.process();
    const auto &object = *print.get_object(size_t(0));
    const auto *sources = object.half_layer_sources();
    REQUIRE(sources != nullptr);
    ToolOrdering ordering(print, 0);
    const auto &tools = ordering.tools_for_layer(object.layers()[5]->print_z);
    CAPTURE(engine, h_mm, detail_count);
    CHECK(tools.has_extruder(0));
    CHECK(tools.has_extruder(1));
    CHECK(tools.has_object);
    SeamPlacer seams;
    seams.init(print, [] {});
    const auto &layer_seams = seams.m_seam_per_object.at(&object).layers;
    REQUIRE(layer_seams.size() == object.layer_count());
    for (unsigned phase = 0; phase < 2; ++phase) {
        const double physical_slice_z_mm = sources->phases[phase][5]->slice_z;
        constexpr double decoded_z_tolerance_mm = 0.00001; // Float seam-coordinate tolerance, mm.
        const auto count = std::count_if(layer_seams[5].points.begin(), layer_seams[5].points.end(),
            [physical_slice_z_mm, decoded_z_tolerance_mm](const auto &point) {
                return std::abs(point.position.z() - physical_slice_z_mm) <= decoded_z_tolerance_mm;
            });
        CAPTURE(phase, physical_slice_z_mm, count);
        CHECK(count >= 4); // At least the four independently known square corners.
    }
}

TEST_CASE("Sliced half-height paths produce a complete material-resolved execution plan", "[HalfLayer][ExecutionPaths]")
{
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const double h_mm = GENERATE(0.1, 0.3);
    const int detail_count = GENERATE(0, 1, 2);
    const double outer_speed_mm_s = GENERATE(10., 100.);
    auto config = detail_count == 0 ? DynamicPrintConfig::full_print_config() : multifilament_config(2);
    if (detail_count != 0) {
        config.set_num_extruders(2);
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
        config.set_deserialize_strict({{"single_extruder_multi_material", false},
            {"use_smaller_nozzles_in_crisp_corners", true}, {"crisp_corner_detail_toolhead", 2},
            {"crisp_corner_small_nozzle_wall_count", detail_count}});
    }
    config.set_deserialize_strict({{"wall_generator", engine}, {"layer_height", h_mm},
        {"initial_layer_print_height", h_mm}, {"wall_loops", detail_count == 0 ? 4 : 2},
        {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"outer_wall_speed", outer_speed_mm_s}, {"bottom_shell_layers", 10}, {"fill_density", 100.},
        {"detect_overhang_wall", false}, {"enable_overhang_speed", false}, {"slow_down_layers", 0},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    Print print;
    Model model;
    init_print({make_cube(20., 20., 3.)}, print, model, config);
    print.process();
    const auto &object = *print.get_object(size_t(0));
    const auto &parent = *object.layers()[5];
    ToolOrdering ordering(print, 0);
    const auto &tools = ordering.tools_for_layer(parent.print_z);
    const auto &owned_tools = print.tool_ordering().tools_for_layer(parent.print_z);
    const HalfLayerExecutionFrame *owned_frame = print.tool_ordering().execution_frame(owned_tools);
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    REQUIRE(owned_frame != nullptr);
    REQUIRE(owned_frame->valid());
    std::vector<HalfLayerExecutionTask> lower, core, upper;
    append_half_layer_model_tasks(object, 5, tools, 0, lower, core, upper);
    CAPTURE(engine, h_mm, detail_count, outer_speed_mm_s);
    REQUIRE_FALSE(lower.empty());
    REQUIRE_FALSE(core.empty());
    REQUIRE_FALSE(upper.empty());
    const auto plan = schedule_half_layer_execution(lower, core, upper);
    CHECK(plan.tasks.size() == lower.size() + core.size() + upper.size());
    CHECK(plan.upper_begin >= lower.size());
    CHECK(plan.upper_end <= plan.tasks.size());
    std::set<const ExtrusionEntity *> expected, observed;
    auto collect = [&](auto &&self, const ExtrusionEntity &entity) -> void {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
            for (const auto *child : collection->entities)
                self(self, *child);
        } else if (entity.length() > 0.) {
            expected.insert(&entity);
        }
    };
    const auto *sources = object.half_layer_sources();
    for (const Layer *physical : std::array<const Layer *, 3>{&parent, sources->phases[0][5].get(), sources->phases[1][5].get()})
        for (const auto *region : physical->regions()) {
            collect(collect, region->perimeters);
            collect(collect, region->fills);
        }
    std::vector<const ExtrusionEntity *> actual_core;
    for (const auto &task : plan.tasks) {
        CHECK(observed.insert(task.entity).second);
        CHECK(task.logical_layer == &parent);
        CHECK(task.object == &object);
        CHECK(task.extrusion_seconds > 0.);
        CHECK(task.first_mm.z() == task.physical_layer->print_z);
        CHECK(task.clearance_mm >= 1.5 * h_mm);
        if (detail_count != 0 && task.perimeter) {
            if (task.entity->tool_hint == ExtrusionToolHint::DetailWall)
                CHECK(task.tool.filament == 1);
            if (task.entity->tool_hint == ExtrusionToolHint::LargeWall)
                CHECK(task.tool.filament == 0);
            CHECK(task.tool.physical_tool == task.tool.filament);
        }
        if (task.physical_layer == &parent)
            actual_core.push_back(task.entity);
    }
    CHECK(observed == expected);
    REQUIRE(owned_frame->plan.tasks.size() == expected.size());
    std::set<const ExtrusionEntity *> owned_observed;
    for (size_t i = 0; i < owned_frame->plan.tasks.size(); ++i) {
        const HalfLayerExecutionTask &task = owned_frame->plan.tasks[i];
        CHECK(task.id == i);
        CHECK(owned_observed.insert(task.entity).second);
    }
    CHECK(owned_observed == expected);
    const std::vector<unsigned int> execution_filaments = print.tool_ordering().execution_filaments(owned_tools);
    CHECK(execution_filaments.size() == owned_frame->plan.visits.size());
    CHECK(owned_tools.wipe_tower_partitions >= execution_filaments.size() - 1);
    if (detail_count != 0) {
        bool returns_to_earlier_filament = false;
        for (size_t i = 0; i + 2 < execution_filaments.size(); ++i)
            for (size_t j = i + 1; j + 1 < execution_filaments.size(); ++j)
                for (size_t k = j + 1; k < execution_filaments.size(); ++k)
                    returns_to_earlier_filament |= execution_filaments[i] == execution_filaments[k] &&
                        execution_filaments[i] != execution_filaments[j];
        CHECK(returns_to_earlier_filament);
    }
    REQUIRE(actual_core.size() == core.size());
    for (size_t i = 0; i < core.size(); ++i)
        CHECK(actual_core[i] == core[i].entity);
    std::vector<const ExtrusionEntity *> owned_core;
    for (const HalfLayerExecutionTask &task : owned_frame->plan.tasks)
        if (task.physical_layer == &parent)
            owned_core.push_back(task.entity);
    REQUIRE(owned_core.size() == core.size());
    for (size_t i = 0; i < core.size(); ++i)
        CHECK(owned_core[i] == core[i].entity);
    if (plan.upper_begin > 0 && plan.upper_end < plan.tasks.size())
        CHECK(plan.tasks[plan.upper_begin - 1].overrides_key != plan.tasks[plan.upper_end].overrides_key);
    // Uniform-height solid cube has enough independent fill paths to separate
    // the two passes. Check the selected outcome, not an input cost bound.
    CHECK(plan.upper_begin > plan.lower_end);
    CHECK(plan.upper_end < plan.tasks.size());
}

TEST_CASE("Support half bands retain classified volume contact separation and logical interface", "[HalfLayer][SupportBands]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"support_half_layer_height", true}, {"enable_support", false}, {"support_interface_top_layers", 2}});
    Print print;
    Model model;
    init_print({cube(2.)}, print, model, config);
    print.process();
    auto &object = *print.get_object(size_t(0));
    REQUIRE(object.support_layers().empty());
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr raft, bottom, top, base, interfaces, base_interfaces;
    const Polygons footprint{Polygon{Points{Point::new_scale(0., 0.), Point::new_scale(5., 0.),
        Point::new_scale(5., 5.), Point::new_scale(0., 5.)}}};
    for (size_t i = 0; i < 3; ++i) {
        auto &layer = storage.allocate_unguarded(SupporLayerType::Base);
        layer.height = 0.2;
        layer.print_z = 0.2 * double(i + 1);
        layer.bottom_z = 0.2 * double(i);
        layer.polygons = footprint;
        base.push_back(&layer);
    }
    auto &contact = storage.allocate_unguarded(SupporLayerType::BottomContact);
    contact.print_z = 0.85;
    contact.height = 0.2;
    contact.bottom_z = 0.6; // Physical deposition starts at 0.65: a 0.05 mm separation.
    contact.bridging = true;
    contact.polygons = footprint;
    bottom.push_back(&contact);
    auto sorted = generate_support_layers(object, raft, bottom, top, base, interfaces, base_interfaces, &storage);
    REQUIRE(base.size() == 6);
    REQUIRE(bottom.size() == 3);
    CHECK(bottom.front()->bottom_print_z() == Catch::Approx(0.65));
    CHECK(bottom.front()->bottom_z == Catch::Approx(0.6));
    CHECK(bottom.back()->print_z == Catch::Approx(0.85));
    CHECK(bottom.front()->bridging);
    CHECK_FALSE(bottom[1]->bridging);
    CHECK_FALSE(bottom[2]->bridging);
    double volume_mm3 = 0.;
    for (const auto *layer : sorted) {
        CHECK(layer->height <= 0.100001); // Maximum physical H/2 with 1 micrometre tolerance, mm.
        CHECK(layer->polygons == footprint);
        volume_mm3 += area(layer->polygons) * SCALING_FACTOR * SCALING_FACTOR * layer->height;
    }
    CHECK(volume_mm3 == Catch::Approx(20.)); // Independent 25 mm2 * (0.6 + 0.2) mm.
    CHECK(base[0]->half_layer_source_id == base[1]->half_layer_source_id);
    CHECK(bottom[0]->half_layer_source_id == bottom[2]->half_layer_source_id);
    SupportParameters parameters(object);
    generate_support_toolpaths(object.support_layers(), object.config(), parameters, object.slicing_parameters(),
        raft, bottom, top, base, interfaces, base_interfaces);
    size_t paths = 0;
    auto inspect = [&](auto &&self, const ExtrusionEntity &entity) -> void {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
            for (const auto *child : collection->entities)
                self(self, *child);
        else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            ++paths;
            CHECK(path->height <= 0.100001);
            CHECK(path->mm3_per_mm > 0.);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
            for (const auto &path : multi->paths)
                self(self, path);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
            for (const auto &path : loop->paths)
                self(self, path);
    };
    for (const auto *layer : object.support_layers())
        inspect(inspect, layer->support_fills);
    CHECK(paths > 0);
}

TEST_CASE("Native support generators produce half-height paths without changing model layers", "[HalfLayer][SupportHalfLive]")
{
    const int generator = GENERATE(0, 1, 2, 3, 4, 5);
    const SupportType type = generator == 0 ? stNormalAuto : generator == 1 ? stNormalCuraAuto :
        generator == 4 ? stResinAuto : generator == 5 ? stMixedAuto : stTreeAuto;
    const double h_mm = GENERATE(0.1, 0.2, 0.3);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"support_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
        {"support_interface_top_layers", 2}, {"support_interface_bottom_layers", 2},
        {"support_on_build_plate_only", true}, {"support_threshold_overlap", 0.}});
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
    if (generator == 2 || generator == 3)
        config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(generator == 2 ? smsTreeStrong : smsTreeOrganic));
    TriangleMesh mesh = make_cube(5., 10., 6.);
    TriangleMesh ceiling = make_cube(20., 10., 1.2);
    ceiling.translate(0.f, 0.f, 6.f);
    mesh.merge(ceiling);
    Print print;
    Model model;
    init_print({mesh}, print, model, config);
    print.process();
    const auto &object = *print.get_object(size_t(0));
    REQUIRE_FALSE(object.support_layers().empty());
    CAPTURE(generator, int(type), h_mm);
    const auto support_grid = half_height_support_z_grid(object);
    REQUIRE_FALSE(support_grid.empty());
    CHECK(support_grid.front() <= 0.5 * h_mm + 0.000001); // Bed-side band height, mm.
    for (const auto *layer : object.layers())
        CHECK(layer->height == Catch::Approx(h_mm));
    size_t paths = 0;
    auto inspect = [&](auto &&self, const ExtrusionEntity &entity) -> void {
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
            for (const auto *child : collection->entities)
                self(self, *child);
        else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
            ++paths;
            CAPTURE(path->height, int(path->role()));
            CHECK(path->height <= 0.5 * h_mm + 0.000001); // Actual bead-height tolerance, mm.
            CHECK(path->mm3_per_mm > 0.);
        } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity))
            for (const auto &path : multi->paths)
                self(self, path);
        else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
            for (const auto &path : loop->paths)
                self(self, path);
    };
    for (const auto *layer : object.support_layers()) {
        CAPTURE(layer->print_z, object.layers().front()->bottom_z());
        CHECK(layer->height <= 0.5 * h_mm + 0.000001);
        inspect(inspect, layer->support_fills);
    }
    CHECK(paths > 0);
}

TEST_CASE("Both wipe tower planners consume repeated half-layer visits", "[HalfLayer][ExecutionTower][TowerClearanceRegression]")
{
    const bool type1 = GENERATE(false, true);
    const bool separate_nozzles = GENERATE(false, true);
    const bool dense_tower = GENERATE(false, true);
    const double configured_hop_mm = GENERATE(0., 0.6);
    auto config = multifilament_config(2);
    if (separate_nozzles) {
        config.set_num_extruders(2);
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    } else {
        config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    }
    config.set_deserialize_strict({{"single_extruder_multi_material", !separate_nozzles},
        {"enable_prime_tower", true}, {"purge_in_prime_tower", true},
        {"wipe_tower_no_sparse_layers", dense_tower},
        {"machine_load_filament_time", 1.0}, {"machine_unload_filament_time", 2.0},
        {"machine_tool_change_time", 7.0},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"wall_loops", 4},
        {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
        {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false},
        {"z_hop", configured_hop_mm},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{0., 100., 20., 0.});
    config.set_key_value("flush_multiplier", new ConfigOptionFloats{1., 1.});
    config.set_key_value("grab_length", new ConfigOptionFloats{0., 0.});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{10., 5.});
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    for (int filament : {1, 2}) {
        overrides.push_back({{"extruder", filament}});
        for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                "sparse_infill_filament_id", "internal_solid_filament_id",
                "top_surface_filament_id", "bottom_surface_filament_id"})
            overrides.back().push_back({key, filament});
    }
    Print print;
    print.is_BBL_printer() = type1;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(6.), cube(5.)}, print, model, config, &overrides);
    CAPTURE(type1, separate_nozzles, dense_tower);
    REQUIRE_NOTHROW(print.process());
    const ToolOrdering &ordering = print.tool_ordering();
    REQUIRE(ordering.half_layer_execution_plan() != nullptr);
    const HalfLayerExecutionTask *task_a = nullptr, *task_b = nullptr;
    for (const HalfLayerExecutionFrame &frame : ordering.half_layer_execution_plan()->frames)
        for (const HalfLayerExecutionTask &task : frame.plan.tasks) {
            if (task.tool.filament == 0 && task_a == nullptr) task_a = &task;
            if (task.tool.filament == 1 && task_b == nullptr) task_b = &task;
        }
    REQUIRE(task_a != nullptr);
    REQUIRE(task_b != nullptr);
    REQUIRE(task_a->tool_entry_seconds_by_filament.size() >= 2);
    REQUIRE(task_b->tool_entry_seconds_by_filament.size() >= 2);
    if (separate_nozzles) {
        CHECK(task_a->tool_entry_seconds_by_filament[1] == Catch::Approx(7.));
        CHECK(task_b->tool_entry_seconds_by_filament[0] == Catch::Approx(7.));
    } else {
        CHECK(task_a->tool_entry_seconds_by_filament[1] == Catch::Approx(5.));
        CHECK(task_b->tool_entry_seconds_by_filament[0] == Catch::Approx(23.));
    }
    std::vector<std::pair<int, int>> expected;
    unsigned int current = type1 ? ordering.first_extruder() : ordering.all_extruders().back();
    for (const LayerTools &tools : ordering.layer_tools()) {
        if (!tools.has_wipe_tower)
            continue;
        for (unsigned int filament : ordering.execution_filaments(tools)) {
            if (filament != current) {
                expected.emplace_back(int(current), int(filament));
                current = filament;
            }
        }
    }
    REQUIRE_FALSE(expected.empty());
    std::vector<std::pair<int, int>> generated;
    for (const auto &layer_changes : print.wipe_tower_data().tool_changes)
        for (const WipeTower::ToolChangeResult &change : layer_changes)
            if (change.initial_tool >= 0 && change.new_tool >= 0 && change.initial_tool != change.new_tool)
                generated.emplace_back(change.initial_tool, change.new_tool);
    CHECK(generated == expected);
    CHECK(print.wipe_tower_data().number_of_toolchanges == int(expected.size()));
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
    GCodeReader reader;
    reader.apply_config(config);
    double tower_entry_clear_z = 0.;
    double pending_tower_z = 0.;
    bool awaiting_tower_restore = false;
    size_t tower_entries = 0;
    size_t tower_extrusions_at_plan_z = 0;
    reader.parse_buffer(exported, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string comment(line.comment());
        if (line.dist_XY(state) > 0. && comment.find("Travel to a Wipe Tower") != std::string::npos) {
            tower_entry_clear_z = line.new_Z(state);
            awaiting_tower_restore = true;
        }
        if (awaiting_tower_restore && comment.find("restore layer Z") != std::string::npos) {
            pending_tower_z = line.new_Z(state);
            ++tower_entries;
            CHECK(tower_entry_clear_z - pending_tower_z >= 0.3 - 0.00051);
            awaiting_tower_restore = false;
        } else if (pending_tower_z > 0. && line.extruding(state) && line.dist_XY(state) > 0.) {
            CHECK(line.new_Z(state) == Catch::Approx(pending_tower_z).margin(0.00051));
            ++tower_extrusions_at_plan_z;
            pending_tower_z = 0.;
        }
    });
    CHECK(tower_entries > 0);
    CHECK(tower_extrusions_at_plan_z >= tower_entries);

    // Independent motion oracle: a tower-to-model transit must clear the last
    // deposited plane, not the lower nominal plane of the next half shell.
    std::string role, previous_role;
    double previous_deposition_z_mm = 0.;
    std::vector<double> transit_z_mm;
    size_t returns = 0, connections = 0;
    bool connection = false;
    bool motion_failed = false;
    GCodeReader motion;
    motion.apply_config(config);
    motion.parse_buffer(exported, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        if (raw.rfind(";TYPE:", 0) == 0) role = raw.substr(6);
        else if (raw.rfind("; FEATURE: ", 0) == 0) role = raw.substr(11);
        if (raw.find("; half-layer tower connection end") != std::string::npos) {
            REQUIRE(connection);
            // End marker follows restoration to the actual tower plane.
            const double required_z_mm = line.new_Z(state) + std::max(configured_hop_mm, 0.3);
            constexpr double encoded_z_tolerance_mm = 0.00051;
            for (const double z_mm : transit_z_mm) {
                CAPTURE(type1, separate_nozzles, dense_tower, configured_hop_mm, z_mm, required_z_mm);
                motion_failed |= std::abs(z_mm - required_z_mm) > encoded_z_tolerance_mm;
                CHECK(z_mm == Catch::Approx(required_z_mm).margin(encoded_z_tolerance_mm));
            }
            ++connections;
            connection = false;
            transit_z_mm.clear();
        } else if (raw.find("; half-layer tower connection") != std::string::npos) {
            connection = true;
            transit_z_mm.clear();
        }
        if (line.dist_XY(state) <= 0.) return;
        if (!line.extruding(state)) {
            // Auto Z-hop may tessellate its rising helix into XYZ segments.
            // Judge the horizontal transit, not an intermediate ascent point.
            constexpr double horizontal_z_tolerance_mm = 0.00001;
            if (std::abs(line.dist_Z(state)) <= horizontal_z_tolerance_mm)
                transit_z_mm.push_back(line.new_Z(state));
            return;
        }
        const bool tower_return = previous_role == "Prime tower" && role != "Prime tower";
        if (tower_return) {
            REQUIRE_FALSE(transit_z_mm.empty());
            // Fixture parent height is 0.2 mm, independently of tower metadata.
            const double required_z_mm = std::max(previous_deposition_z_mm, double(line.new_Z(state))) +
                std::max(configured_hop_mm, 0.3);
            constexpr double encoded_z_tolerance_mm = 0.00051;
            for (const double z_mm : transit_z_mm) {
                CAPTURE(type1, separate_nozzles, dense_tower, configured_hop_mm, raw, z_mm, required_z_mm,
                    previous_deposition_z_mm, connection, tower_return);
                motion_failed |= z_mm < required_z_mm - encoded_z_tolerance_mm;
                CHECK(z_mm >= required_z_mm - encoded_z_tolerance_mm);
            }
            if (tower_return) ++returns;
        }
        connection = false;
        transit_z_mm.clear();
        previous_deposition_z_mm = line.new_Z(state);
        previous_role = role;
    });
    CHECK(returns > 0);
    CHECK(connections > 0);
    if (motion_failed || returns == 0 || connections == 0)
        std::ofstream("tower-clearance-" + std::to_string(type1) + "-" + std::to_string(separate_nozzles) +
            "-" + std::to_string(dense_tower) + "-" + std::to_string(configured_hop_mm) + ".gcode") << exported;
}

TEST_CASE("Dedicated wipe-tower material is an explicit execution visit", "[HalfLayer][TowerAuxiliary]")
{
    const bool type1 = GENERATE(false, true);
    auto config = multifilament_config(3);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1, 1});
    config.set_deserialize_strict({{"single_extruder_multi_material", true},
        {"enable_prime_tower", true}, {"purge_in_prime_tower", true}, {"wipe_tower_filament", 3},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"wall_loops", 4},
        {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
        {"only_one_wall_first_layer", false}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    for (int filament : {1, 2}) {
        overrides.push_back({{"extruder", filament}});
        for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                "sparse_infill_filament_id", "internal_solid_filament_id",
                "top_surface_filament_id", "bottom_surface_filament_id"})
            overrides.back().push_back({key, filament});
    }
    Print print;
    print.is_BBL_printer() = type1;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(6.), cube(5.)}, print, model, config, &overrides);
    CAPTURE(type1);
    REQUIRE_NOTHROW(print.process());
    const ToolOrdering &ordering = print.wipe_tower_data().tool_ordering;
    REQUIRE(ordering.half_layer_execution_plan() != nullptr);
    bool found_tower_only = false;
    for (const HalfLayerExecutionFrame &frame : ordering.half_layer_execution_plan()->frames) {
        REQUIRE(frame.valid());
        for (const HalfLayerExecutionVisit &visit : frame.plan.visits)
            if (visit.tower_only) {
                found_tower_only = true;
                CHECK(visit.tool.filament == 2);
                CHECK(visit.begin == frame.plan.tasks.size());
                CHECK(visit.end == frame.plan.tasks.size());
            }
    }
    CHECK(found_tower_only);
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
}

TEST_CASE("Half-height support retains the original logical event grid", "[HalfLayer][SupportEvents]")
{
    const int generator = GENERATE(0, 1, 2, 3, 4, 5);
    const int raft_layers = GENERATE(0, 3);
    const SupportType type = generator == 0 ? stNormalAuto : generator == 1 ? stNormalCuraAuto :
        generator == 4 ? stResinAuto : generator == 5 ? stMixedAuto : stTreeAuto;
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"support_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
        {"support_interface_top_layers", 2}, {"support_interface_bottom_layers", 2},
        {"support_on_build_plate_only", true}, {"support_threshold_overlap", 0.}, {"raft_layers", raft_layers}});
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
    if (generator == 2 || generator == 3)
        config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(generator == 2 ? smsTreeStrong : smsTreeOrganic));
    TriangleMesh mesh = make_cube(5., 10., 6.);
    TriangleMesh ceiling = make_cube(20., 10., 1.2);
    ceiling.translate(0.f, 0.f, 6.f);
    mesh.merge(ceiling);
    struct ExportStagePrint : Print {
        using Print::set_started;
        using Print::set_done;
    } print;
    Model model;
    init_print({mesh}, print, model, config);
    print.process();
    const auto &object = *print.get_object(size_t(0));
    const auto *sources = object.half_layer_support_sources();
    REQUIRE(sources != nullptr);
    REQUIRE_FALSE(sources->events.empty());
    CAPTURE(generator, raft_layers);
    CHECK(sources->events.size() < object.support_layers().size());
    std::vector<SupportLayer::LogicalBand> original_grid;
    std::set<const SupportLayer *> seen;
    double previous_z_mm = 0.;
    for (const auto &event : sources->events) {
        original_grid.push_back({event->print_z, event->height, event->interface_id()});
        CHECK(event->support_fills.empty()); // No copied toolpaths in event descriptors.
        bool has_paths = false;
        for (const auto *physical : event->physical_layers) {
            CHECK(seen.insert(physical).second);
            CHECK(physical->print_z > previous_z_mm - 0.000001); // Event boundary tolerance, mm.
            CHECK(physical->print_z <= event->print_z + 0.000001);
            has_paths |= physical->has_extrusions();
        }
        CHECK(event->has_extrusions() == has_paths);
        previous_z_mm = event->print_z;
    }
    CHECK(seen.size() == object.support_layers().size());
    // Match export's stage lifecycle: event collection may publish gap warnings
    // to the active Print step even though it never emits machine commands.
    REQUIRE(print.set_started(psGCodeExport));
    const auto on_events = GCode::collect_layers_to_print(object);
    print.set_done(psGCodeExport);
    std::vector<double> on_event_z;
    for (const auto &event : on_events)
        on_event_z.push_back(event.print_z());
    ToolOrdering on_tools(print, 0);
    ToolOrdering on_sequential_tools(object, 0);
    CHECK(on_tools.layer_tools().size() == on_events.size());
    CHECK(on_sequential_tools.layer_tools().size() == on_events.size());
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    for (const auto &event : sources->events) {
        if (event->has_extrusions()) {
            std::set<const ExtrusionEntity *> expected_support;
            auto collect_support = [&](auto &&self, const ExtrusionEntity &entity) -> void {
                if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
                    for (const ExtrusionEntity *child : collection->entities)
                        if (child != nullptr)
                            self(self, *child);
                } else if (entity.length() > 0.) {
                    expected_support.insert(&entity);
                }
            };
            for (const SupportLayer *physical : event->physical_layers)
                collect_support(collect_support, physical->support_fills);
            std::set<const ExtrusionEntity *> observed_support;
            for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames) {
                REQUIRE(frame.valid());
                for (const HalfLayerExecutionTask &task : frame.plan.tasks) {
                    if (!task.support || task.logical_layer != event.get())
                        continue;
                    CHECK(expected_support.count(task.entity) == 1);
                    CHECK(observed_support.insert(task.entity).second);
                    const auto *physical = static_cast<const SupportLayer *>(task.physical_layer);
                    CHECK(std::find(event->physical_layers.begin(), event->physical_layers.end(), physical) !=
                        event->physical_layers.end());
                    const auto deadline = std::lower_bound(print.tool_ordering().layer_tools().begin(),
                        print.tool_ordering().layer_tools().end(), LayerTools(physical->print_z - EPSILON));
                    REQUIRE(deadline != print.tool_ordering().layer_tools().end());
                    CHECK(frame.print_z == Catch::Approx(deadline->print_z).margin(0.000001));
                    CHECK(frame.print_z + 0.000001 >= physical->print_z);
                }
            }
            CHECK(observed_support == expected_support);
        }
    }
    config.set_deserialize_strict({{"support_half_layer_height", false}});
    print.apply(model, config);
    CHECK(print.get_object(size_t(0))->half_layer_support_sources() == nullptr);
    print.process();
    const auto &off = *print.get_object(size_t(0));
    CHECK(print.half_layer_execution_plan() == nullptr);
    REQUIRE(print.set_started(psGCodeExport));
    const auto off_events = GCode::collect_layers_to_print(off);
    print.set_done(psGCodeExport);
    REQUIRE(off_events.size() == on_event_z.size());
    for (size_t i = 0; i < on_event_z.size(); ++i)
        CHECK(off_events[i].print_z() == Catch::Approx(on_event_z[i]).margin(0.000001));
    REQUIRE(off.support_layers().size() == original_grid.size());
    for (size_t i = 0; i < original_grid.size(); ++i) {
        CHECK(off.support_layers()[i]->print_z == Catch::Approx(original_grid[i].print_z_mm).margin(0.000001));
        CHECK(off.support_layers()[i]->height == Catch::Approx(original_grid[i].height_mm).margin(0.000001));
        CHECK(off.support_layers()[i]->interface_id() == original_grid[i].interface_id);
    }
    config.set_deserialize_strict({{"support_half_layer_height", true}});
    print.apply(model, config);
    print.process();
    const auto *restored = print.get_object(size_t(0))->half_layer_support_sources();
    REQUIRE(restored != nullptr);
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    CHECK(restored->events.size() == original_grid.size());
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
}

TEST_CASE("Half-layer execution frame exports real G-code", "[HalfLayer][GCodeDispatch]")
{
    const int mode = GENERATE(0, 1, 2);
    const double layer_height = GENERATE(0.12, 0.2, 0.28);
    const int wall_loops = GENERATE(2, 4);
    const std::string wall_generator = GENERATE(std::string("classic"), std::string("arachne"));
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", layer_height}, {"initial_layer_print_height", layer_height},
        {"wall_generator", wall_generator}, {"wall_loops", wall_loops}, {"skirt_loops", 0}, {"brim_type", "no_brim"},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"outer_wall_half_layer_height", mode != 1}, {"support_half_layer_height", mode != 0},
        {"enable_support", mode != 0}, {"support_threshold_angle", 60}});
    Print print;
    Model model;
    if (mode == 0)
        init_print({cube(5.)}, print, model, config);
    else
        init_print({TestMesh::overhang}, print, model, config);
    const std::string output = gcode(print);
    CAPTURE(mode, layer_height, wall_loops, wall_generator);
    REQUIRE_FALSE(output.empty());
    CHECK(output.find("Half-height G-code scheduling is not yet available") == std::string::npos);
    CHECK(output.find(GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change)) != std::string::npos);
    CHECK(print.half_layer_execution_plan() != nullptr);

    const std::string layer_tag = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change);
    size_t layer_tags = 0;
    for (size_t pos = 0; (pos = output.find(layer_tag, pos)) != std::string::npos; pos += layer_tag.size())
        ++layer_tags;
    CHECK(layer_tags == GCode::collect_layers_to_print(print).size());

    std::set<double> expected_task_z;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames)
        for (const HalfLayerExecutionTask &task : frame.plan.tasks)
            expected_task_z.insert(task.physical_layer->print_z);
    std::set<double> emitted_task_z;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string_view comment = line.comment();
        const bool planned_role = comment.find("perimeter") != std::string_view::npos ||
            comment.find("infill") != std::string_view::npos || comment.find("support material") != std::string_view::npos ||
            comment.find("support transition") != std::string_view::npos || comment.find("ironing") != std::string_view::npos;
        if (!planned_role || !line.extruding(state) || line.dist_XY(state) <= 0.)
            return;
        const double z = line.new_Z(state);
        const auto match = std::find_if(expected_task_z.begin(), expected_task_z.end(),
            [z](double expected) { return std::abs(expected - z) <= 0.00051; });
        CHECK(match != expected_task_z.end());
        if (match != expected_task_z.end())
            emitted_task_z.insert(*match);
    });
    CHECK(emitted_task_z == expected_task_z);
}

TEST_CASE("Half-layer timing and clearance changes invalidate the borrowed execution plan",
    "[HalfLayer][ExecutionInvalidation]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"outer_wall_half_layer_height", true},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("z_hop", new ConfigOptionFloats{0.1});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    print.process();
    REQUIRE(print.is_step_done(psWipeTower));
    REQUIRE(print.half_layer_execution_plan() != nullptr);

    config.set_key_value("z_hop", new ConfigOptionFloats{0.8});
    print.apply(model, config);
    CHECK_FALSE(print.is_step_done(psWipeTower));
    CHECK(print.half_layer_execution_plan() == nullptr);
    CHECK(print.tool_ordering().half_layer_execution_plan() == nullptr);
    REQUIRE_FALSE(print.tool_ordering().layer_tools().empty());
    CHECK_THROWS_WITH(print.tool_ordering().execution_frame(print.tool_ordering().front()),
        Catch::Matchers::ContainsSubstring("invalidated or not ready"));

    print.process();
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    bool saw_updated_clearance = false;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames)
        for (const HalfLayerExecutionTask &task : frame.plan.tasks)
            saw_updated_clearance |= task.clearance_mm >= 0.8 - 0.000001;
    CHECK(saw_updated_clearance);
}

TEST_CASE("Half-layer settings reject spiral vase instead of silently disabling it",
    "[HalfLayer][SpiralCompatibility]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 1}, {"spiral_mode", true}, {"outer_wall_half_layer_height", true}});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    const StringObjectException error = print.validate();
    CHECK_FALSE(error.string.empty());
    CHECK(error.opt_key == "spiral_mode");
    CHECK(error.string.find("Half-height") != std::string::npos);
}

TEST_CASE("Half-layer settings preserve visit-local wipe-into claims and stored options",
    "[HalfLayer][WipeIntoCompatibility]")
{
    const bool type1 = GENERATE(false, true);
    const bool infill_only = GENERATE(false, true);
    auto config = multifilament_config(2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"outer_wall_half_layer_height", true},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"detect_overhang_wall", false}, {"enable_prime_tower", true},
        {"purge_in_prime_tower", true}, {"single_extruder_multi_material", true},
        {"flush_into_objects", false}, {"flush_into_infill", false},
        {"is_infill_first", true}, {"sparse_infill_density", "20%"},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("before_layer_change_gcode", new ConfigOptionString("G92 E0"));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{0., 100., 20., 0.});
    config.set_key_value("flush_multiplier", new ConfigOptionFloats{1.});
    config.set_key_value("grab_length", new ConfigOptionFloats{0.});
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides{
        {{"extruder", 1}}, {{"extruder", 2}}, {{"extruder", 2}}
    };
    for (auto &region : overrides)
        for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                "sparse_infill_filament_id", "internal_solid_filament_id",
                "top_surface_filament_id", "bottom_surface_filament_id"})
            region.push_back({key, region.front().opt_value});
    Print print;
    print.is_BBL_printer() = type1;
    Model model;
    init_print(std::vector<TriangleMesh>{make_cube(20., 20., 6.), make_cube(18., 18., 5.),
        make_cube(16., 16., 4.)}, print, model, config, &overrides);
    model.objects[1]->config.set("flush_into_objects", !infill_only);
    model.objects[1]->config.set("flush_into_infill", infill_only);
    print.apply(model, config);
    const StringObjectException error = print.validate();
    CAPTURE(type1, infill_only, error.string, error.opt_key);
    REQUIRE(error.string.empty());
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    double required_mm3 = 0.;
    double claimed_mm3 = 0.;
    double tower_mm3 = 0.;
    size_t eligible_target_tasks = 0;
    std::vector<double> expected_tower_purge_mm3;
    std::vector<std::pair<unsigned, unsigned>> expected_tower_pairs;
    std::set<std::tuple<const HalfLayerExecutionFrame *, size_t>> claimed_tasks;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames) {
        for (const HalfLayerExecutionVisit &visit : frame.plan.visits) {
            for (size_t task_id = visit.begin; task_id < visit.end; ++task_id) {
                const HalfLayerExecutionTask &task = frame.plan.tasks[task_id];
                if (task.object == print.get_object(size_t(1)) && task.wipe_into_eligible)
                    ++eligible_target_tasks;
            }
        }
        for (const HalfLayerPurgeTransaction &transaction : frame.plan.purge_transactions) {
            required_mm3 += transaction.required_volume_mm3;
            claimed_mm3 += transaction.claimed_volume_mm3;
            tower_mm3 += transaction.tower_volume_mm3;
            const double minimum = type1 ? 0. :
                config.option<ConfigOptionFloats>("filament_minimal_purge_on_wipe_tower")
                    ->get_at(transaction.destination_filament);
            expected_tower_purge_mm3.push_back(type1 ? transaction.tower_volume_mm3 :
                std::max(0., transaction.required_volume_mm3 - minimum -
                    transaction.claimed_volume_mm3) + minimum);
            expected_tower_pairs.emplace_back(transaction.source_filament,
                                               transaction.destination_filament);
            REQUIRE(transaction.visit_index < frame.plan.visits.size());
            const HalfLayerExecutionVisit &visit = frame.plan.visits[transaction.visit_index];
            CHECK(visit.purge_transaction < frame.plan.purge_transactions.size());
            for (size_t task_id : transaction.claimed_task_ids) {
                REQUIRE(task_id >= visit.begin);
                REQUIRE(task_id < visit.end);
                CHECK(frame.plan.tasks[task_id].wipe_into_eligible);
                CHECK(claimed_tasks.emplace(&frame, task_id).second);
            }
        }
    }
    CHECK(required_mm3 > 0.);
    CHECK(eligible_target_tasks > 0);
    if (infill_only) {
        CHECK(claimed_mm3 == Catch::Approx(0.));
        CHECK(tower_mm3 == Catch::Approx(required_mm3));
    } else {
        CHECK(claimed_mm3 > 0.);
        CHECK(tower_mm3 < required_mm3);
    }
    CHECK(std::set<unsigned int>(print.tool_ordering().all_extruders().begin(),
        print.tool_ordering().all_extruders().end()) == std::set<unsigned int>{0, 1});
    CHECK(print.get_object(size_t(1))->config().flush_into_infill.value == infill_only);
    CHECK(print.get_object(size_t(1))->config().flush_into_objects.value == !infill_only);
    std::vector<double> generated_tower_purge_mm3;
    std::vector<std::pair<unsigned, unsigned>> generated_tower_pairs;
    for (const auto &layer_changes : print.wipe_tower_data().tool_changes)
        for (const WipeTower::ToolChangeResult &change : layer_changes)
            if (change.initial_tool >= 0 && change.new_tool >= 0 &&
                change.initial_tool != change.new_tool)
            {
                generated_tower_purge_mm3.push_back(change.purge_volume);
                generated_tower_pairs.emplace_back(unsigned(change.initial_tool), unsigned(change.new_tool));
            }
    if (type1) {
        REQUIRE(generated_tower_purge_mm3.size() == expected_tower_purge_mm3.size());
        for (size_t i = 0; i < generated_tower_purge_mm3.size(); ++i) {
            CAPTURE(i, expected_tower_pairs[i].first, expected_tower_pairs[i].second,
                generated_tower_pairs[i].first, generated_tower_pairs[i].second);
            CHECK(generated_tower_purge_mm3[i] == Catch::Approx(expected_tower_purge_mm3[i]).margin(0.01));
        }
    }
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
}

TEST_CASE("Half-height support uses same-nozzle cross-material wipe reservations",
    "[HalfLayer][WipeIntoSupport]")
{
    const bool type1 = GENERATE(false, true);
    const double destination_flow = GENERATE(0.5, 1.25);
    auto config = multifilament_config(2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"support_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
        {"support_interface_top_layers", 2}, {"support_filament", 0},
        {"support_interface_filament", 0}, {"enable_prime_tower", true},
        {"purge_in_prime_tower", true}, {"single_extruder_multi_material", true},
        {"flush_into_objects", false}, {"flush_into_infill", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("before_layer_change_gcode", new ConfigOptionString("G92 E0"));
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{0., 100., 20., 0.});
    config.set_key_value("flush_multiplier", new ConfigOptionFloats{1.});
    config.set_key_value("grab_length", new ConfigOptionFloats{0.});
    config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{20., 0.05});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{1., destination_flow});
    config.set_key_value("z_hop", new ConfigOptionFloats{0.1, 0.8});
    TriangleMesh supported = mesh(TestMesh::overhang);
    TriangleMesh second = cube(5.);
    second.translate(30.f, 0.f, 0.f);
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    for (int filament : {1, 2}) {
        overrides.push_back({{"extruder", filament}});
        for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                "sparse_infill_filament_id", "internal_solid_filament_id",
                "top_surface_filament_id", "bottom_surface_filament_id"})
            overrides.back().push_back({key, filament});
    }
    Print print;
    print.is_BBL_printer() = type1;
    Model model;
    init_print({supported, second}, print, model, config, &overrides);
    model.objects[0]->config.set("flush_into_support", true);
    print.apply(model, config);
    CAPTURE(type1);
    REQUIRE(print.validate().string.empty());
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    size_t eligible_support_tasks = 0;
    size_t reassigned_support_tasks = 0;
    size_t claimed_reassigned_support_tasks = 0;
    bool retarget_probe_checked = false;
    double required_mm3 = 0.;
    double claimed_mm3 = 0.;
    double tower_mm3 = 0.;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames) {
        REQUIRE(frame.valid());
        for (const HalfLayerExecutionTask &task : frame.plan.tasks) {
            if (task.support && task.object == print.get_object(size_t(0)) && task.wipe_into_eligible)
                ++eligible_support_tasks;
            if (task.support && task.object == print.get_object(size_t(0)) &&
                task.wipe_reservation_id != size_t(-1)) {
                ++reassigned_support_tasks;
                CHECK(task.normal_tool.physical_tool == task.tool.physical_tool);
                CHECK(task.normal_tool.filament != task.tool.filament);
                CHECK(task.clearance_mm + EPSILON >= 1.5 * task.motion_reference_height);
                const ExtrusionRole role = task.entity->role();
                const bool interface_role = role == erSupportMaterialInterface ||
                    role == erSupportMaterialInterfaceSublayer || role == erIroning;
                CHECK(task.wipe_root_kind == (interface_role ? HalfLayerWipeRootKind::SupportInterface :
                    HalfLayerWipeRootKind::SupportBody));
            }
            if (!retarget_probe_checked && task.support && task.object == print.get_object(size_t(0)) &&
                task.normal_tool.filament == 0) {
                HalfLayerExecutionTask probe = task;
                restore_half_layer_normal_snapshot(probe);
                const double normal_seconds = probe.extrusion_seconds;
                const double normal_volume = probe.wipe_into_volume_mm3;
                retarget_half_layer_task_material(probe, 1);
                CHECK(probe.tool.physical_tool == probe.normal_tool.physical_tool);
                CHECK(probe.tool.filament == 1);
                CHECK(probe.clearance_mm == Catch::Approx(0.8));
                CHECK(probe.extrusion_seconds > normal_seconds);
                CHECK(probe.wipe_into_volume_mm3 == Catch::Approx(normal_volume * destination_flow));
                restore_half_layer_normal_snapshot(probe);
                CHECK(probe.wipe_into_volume_mm3 == Catch::Approx(normal_volume));
                retarget_probe_checked = true;
            }
        }
        for (const HalfLayerPurgeTransaction &transaction : frame.plan.purge_transactions) {
            required_mm3 += transaction.required_volume_mm3;
            claimed_mm3 += transaction.claimed_volume_mm3;
            tower_mm3 += transaction.tower_volume_mm3;
            CHECK(transaction.tower_volume_mm3 == Catch::Approx(std::max(0.,
                transaction.required_volume_mm3 - transaction.claimed_volume_mm3)));
            for (size_t task_id : transaction.claimed_task_ids) {
                REQUIRE(task_id < frame.plan.tasks.size());
                CHECK(frame.plan.tasks[task_id].support);
                if (frame.plan.tasks[task_id].wipe_reservation_id != size_t(-1))
                    ++claimed_reassigned_support_tasks;
            }
        }
    }
    CHECK(eligible_support_tasks > 0);
    CHECK(reassigned_support_tasks > 0);
    CHECK(claimed_reassigned_support_tasks > 0);
    CHECK(retarget_probe_checked);
    CHECK(required_mm3 > 0.);
    CHECK(claimed_mm3 > 0.);
    CHECK(tower_mm3 < required_mm3);
    CHECK(print.get_object(size_t(0))->config().flush_into_support.value);
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
}

TEST_CASE("Half-layer purge credit matches emitted support volume under flow changes",
    "[HalfLayer][PurgeFlowAccounting]")
{
    const double process_ratio = GENERATE(0.5, 1.0, 1.25);
    const double filament_ratio = GENERATE(0.8, 1.0);
    const bool role_flow = GENERATE(false, true);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"support_half_layer_height", true}, {"enable_support", true},
        {"support_threshold_angle", 60}, {"support_interface_top_layers", 2},
        {"print_flow_ratio", process_ratio}, {"set_other_flow_ratios", role_flow},
        {"support_flow_ratio", 0.6}, {"support_interface_flow_ratio", 0.6}, {"first_layer_flow_ratio", 1.},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}, {"enable_arc_fitting", false}});
    config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{filament_ratio});
    Print print;
    Model model;
    init_print({TestMesh::overhang}, print, model, config);
    const std::string output = gcode(print);
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    double geometry_mm3 = 0., credited_mm3 = 0.;
    for (const auto &frame : print.half_layer_execution_plan()->frames)
        for (const auto &task : frame.plan.tasks)
            if (task.support) {
                const auto role = task.entity->role();
                const bool role_scaled = role_flow && (role == erSupportMaterial ||
                    role == erSupportMaterialInterface || role == erSupportMaterialInterfaceSublayer);
                geometry_mm3 += task.entity->total_volume() * (role_scaled ? 0.6 : 1.);
                credited_mm3 += task.wipe_into_volume_mm3;
            }
    const double expected_mm3 = geometry_mm3 * process_ratio * filament_ratio;
    GCodeReader reader;
    reader.apply_config(config);
    double emitted_mm3 = 0.;
    size_t segments = 0;
    const double filament_area_mm2 = 3.14159265358979323846 * 1.75 * 1.75 / 4.;
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        if (line.comment().find("support material") == std::string_view::npos ||
            !line.extruding(state) || line.dist_XY(state) <= 0.)
            return;
        emitted_mm3 += line.dist_E(state) * filament_area_mm2;
        ++segments;
    });
    CAPTURE(process_ratio, filament_ratio, role_flow, geometry_mm3, credited_mm3, emitted_mm3, segments);
    REQUIRE(segments > 0);
    // E is serialized to 5 decimal places; include float-reader accumulation, mm3.
    const double e_rounding_tolerance_mm3 = segments * 0.00003 * filament_area_mm2;
    CHECK(emitted_mm3 == Catch::Approx(expected_mm3).margin(e_rounding_tolerance_mm3));
    CHECK(credited_mm3 == Catch::Approx(expected_mm3));
}

TEST_CASE("Model purge capacity covers late flow and seam changes", "[HalfLayer][ModelPurgeEmission]")
{
    const int variant = GENERATE(0, 1, 2, 3, 4);
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const bool gap = variant == 1 || variant == 4;
    const bool compensation = variant == 2 || variant == 4;
    const bool scarf = variant == 3 || variant == 4;
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"outer_wall_half_layer_height", true}, {"wall_loops", 4},
        {"wall_generator", engine},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}, {"enable_arc_fitting", false},
        {"seam_gap", gap ? 0.3 : 0.}, {"seam_slope_type", scarf ? "all" : "none"},
        {"seam_slope_entire_loop", true}, {"scarf_joint_flow_ratio", 0.5},
        {"small_area_infill_flow_compensation", compensation},
        {"top_surface_pattern", "monotonic"}, {"bottom_surface_pattern", "monotonic"},
        {"internal_solid_infill_pattern", "rectilinear"}});
    config.set_key_value("small_area_infill_flow_compensation_model",
        new ConfigOptionStrings{"0, 0.5", "100, 1.0"});
    Print print;
    Model model;
    init_print({make_cube(10., 10., 3.)}, print, model, config);
    model.objects[0]->config.set("flush_into_objects", true);
    print.apply(model, config);
    print.process();
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    double capacity_mm3 = 0.;
    size_t unsafe_tasks = 0;
    for (const auto &frame : print.half_layer_execution_plan()->frames)
        for (const auto &task : frame.plan.tasks) {
            if (task.wipe_into_eligible)
                capacity_mm3 += task.wipe_into_volume_mm3;
            const auto role = task.entity->role();
            const bool loop = dynamic_cast<const ExtrusionLoop *>(task.entity) != nullptr;
            if (loop && gap)
                CHECK(task.wipe_into_volume_mm3 < task.entity->total_volume());
            const bool affected_loop = loop && scarf && task.logical_layer->id() > 0 && role == erExternalPerimeter;
            const bool affected_infill = compensation && (role == erTopSolidInfill || role == erSolidInfill);
            if (affected_loop || affected_infill) {
                ++unsafe_tasks;
                CHECK_FALSE(task.wipe_into_eligible);
            }
        }
    REQUIRE(capacity_mm3 > 0.);
    if (compensation || scarf)
        REQUIRE(unsafe_tasks > 0);
    const std::string output = gcode(print);
    GCodeReader reader;
    reader.apply_config(config);
    double emitted_mm3 = 0.;
    size_t segments = 0;
    const double filament_area_mm2 = 3.14159265358979323846 * 1.75 * 1.75 / 4.;
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        if (line.extruding(state) && line.dist_XY(state) > 0.) {
            emitted_mm3 += line.dist_E(state) * filament_area_mm2;
            ++segments;
        }
    });
    CAPTURE(variant, engine, capacity_mm3, emitted_mm3, segments);
    REQUIRE(segments > 0);
    const double e_rounding_tolerance_mm3 = segments * 0.00003 * filament_area_mm2;
    CHECK(capacity_mm3 <= emitted_mm3 + e_rounding_tolerance_mm3);
    if (variant == 0)
        CHECK(capacity_mm3 == Catch::Approx(emitted_mm3).margin(e_rounding_tolerance_mm3));

    const auto commands = [](const std::string &gcode_output) {
        std::istringstream input(gcode_output);
        std::string result, line;
        while (std::getline(input, line)) {
            const auto comment = line.find(';');
            if (comment != std::string::npos) line.resize(comment);
            const auto end = line.find_last_not_of(" \t\r");
            if (end != std::string::npos) result += line.substr(0, end + 1) + '\n';
        }
        return result;
    };
    model.objects[0]->config.set("flush_into_objects", false);
    print.apply(model, config);
    CHECK(commands(gcode(print)) == commands(output));
}

TEST_CASE("Late model flow changes invalidate and restore purge eligibility", "[HalfLayer][LateFlowInvalidation]")
{
    const int option = GENERATE(0, 1, 2);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"outer_wall_half_layer_height", true}, {"wall_loops", 4},
        {"seam_gap", 0.}, {"seam_slope_type", "none"}, {"small_area_infill_flow_compensation", false},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    Print print;
    Model model;
    init_print({make_cube(10., 10., 3.)}, print, model, config);
    model.objects[0]->config.set("flush_into_objects", true);
    print.apply(model, config);
    print.process();
    auto eligible_count = [&]() {
        size_t count = 0;
        for (const auto &frame : print.half_layer_execution_plan()->frames)
            for (const auto &task : frame.plan.tasks)
                if (task.wipe_into_eligible) ++count;
        return count;
    };
    auto eligible_volume = [&]() {
        double volume_mm3 = 0.;
        for (const auto &frame : print.half_layer_execution_plan()->frames)
            for (const auto &task : frame.plan.tasks)
                if (task.wipe_into_eligible) volume_mm3 += task.wipe_into_volume_mm3;
        return volume_mm3;
    };
    const size_t before = eligible_count();
    const double before_mm3 = eligible_volume();
    REQUIRE(before > 0);
    const auto original = config;
    if (option == 0) config.set_key_value("seam_gap", new ConfigOptionFloatOrPercent(0.3, false));
    if (option == 1) config.set_key_value("seam_slope_type", new ConfigOptionEnum<SeamScarfType>(SeamScarfType::All));
    if (option == 2) config.set_key_value("small_area_infill_flow_compensation", new ConfigOptionBool(true));
    print.apply(model, config);
    CAPTURE(option);
    REQUIRE(print.half_layer_execution_plan() == nullptr);
    print.process();
    if (option == 0)
        CHECK(eligible_volume() < before_mm3);
    else
        CHECK(eligible_count() < before);
    print.apply(model, original);
    REQUIRE(print.half_layer_execution_plan() == nullptr);
    print.process();
    CHECK(eligible_count() == before);
    CHECK(eligible_volume() == Catch::Approx(before_mm3));
}

TEST_CASE("Changing flow invalidates cached half-layer purge capacity", "[HalfLayer][PurgeFlowInvalidation]")
{
    const std::string key = GENERATE("filament_flow_ratio", "print_flow_ratio", "support_flow_ratio");
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"support_half_layer_height", true}, {"enable_support", true},
        {"support_threshold_angle", 60}, {"set_other_flow_ratios", true},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    Print print;
    Model model;
    init_print({TestMesh::overhang}, print, model, config);
    print.process();
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    auto support_body_capacity = [&]() {
        double volume = 0.;
        for (const auto &frame : print.half_layer_execution_plan()->frames)
            for (const auto &task : frame.plan.tasks)
                if (task.support && task.entity->role() == erSupportMaterial)
                    volume += task.wipe_into_volume_mm3;
        return volume;
    };
    const double old_volume = support_body_capacity();
    REQUIRE(old_volume > 0.);
    if (key == "filament_flow_ratio")
        config.set_key_value(key, new ConfigOptionFloats{0.5});
    else
        config.set_key_value(key, new ConfigOptionFloat(0.5));
    print.apply(model, config);
    CAPTURE(key);
    CHECK_FALSE(print.is_step_done(psWipeTower));
    CHECK(print.half_layer_execution_plan() == nullptr);
    print.process();
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    CHECK(support_body_capacity() == Catch::Approx(old_volume * 0.5));
}

TEST_CASE("Wipe-into never claims across separate physical nozzles",
    "[HalfLayer][WipeIntoMultiNozzle]")
{
    const bool type1 = GENERATE(false, true);
    auto config = multifilament_config(2);
    config.set_num_extruders(2);
    set_toolhead_nozzle_diameter(config, 0, 0.4);
    set_toolhead_nozzle_diameter(config, 1, 0.2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    config.set_deserialize_strict({{"single_extruder_multi_material", false},
        {"enable_prime_tower", true}, {"purge_in_prime_tower", true},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2}, {"wall_loops", 4},
        {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
        {"only_one_wall_first_layer", false}, {"detect_overhang_wall", false},
        {"flush_into_objects", false}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("flush_volumes_matrix", new ConfigOptionFloats{0., 100., 20., 0.});
    config.set_key_value("flush_multiplier", new ConfigOptionFloats{1., 1.});
    config.set_key_value("grab_length", new ConfigOptionFloats{0., 0.});
    std::vector<std::vector<ConfigBase::SetDeserializeItem>> overrides;
    for (int filament : {1, 2, 2}) {
        overrides.push_back({{"extruder", filament}});
        for (const char *key : {"outer_wall_filament_id", "inner_wall_filament_id",
                "sparse_infill_filament_id", "internal_solid_filament_id",
                "top_surface_filament_id", "bottom_surface_filament_id"})
            overrides.back().push_back({key, filament});
    }
    Print print;
    print.is_BBL_printer() = type1;
    Model model;
    init_print(std::vector<TriangleMesh>{cube(6.), cube(5.), cube(4.)}, print, model, config, &overrides);
    model.objects[1]->config.set("flush_into_objects", true);
    print.apply(model, config);
    CAPTURE(type1);
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames)
        for (const HalfLayerPurgeTransaction &transaction : frame.plan.purge_transactions) {
            CHECK(transaction.claimed_volume_mm3 == Catch::Approx(0.));
            CHECK(transaction.claimed_task_ids.empty());
        }
    CHECK(print.get_object(size_t(1))->config().flush_into_objects.value);
    std::string exported;
    REQUIRE_NOTHROW(exported = gcode(print));
    CHECK_FALSE(exported.empty());
}

TEST_CASE("WipeTower2 preserves repeated destination visit ordinals",
    "[HalfLayer][WipeTowerOrdinal]")
{
    auto config = multifilament_config(2);
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    config.set_deserialize_strict({{"single_extruder_multi_material", true},
        {"enable_prime_tower", true}, {"purge_in_prime_tower", true},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    WipeTower2 tower(print.config(), print.default_region_config(), 0, Vec3d::Zero(),
        {{0.f, 20.f}, {40.f, 0.f}}, 0);
    tower.set_extruder(0, print.config());
    tower.set_extruder(1, print.config());
    tower.plan_toolchange(0.2f, 0.2f, 0, 1, 20.f);
    tower.plan_toolchange(0.2f, 0.2f, 1, 0, 40.f);
    tower.plan_toolchange(0.2f, 0.2f, 0, 1, 300.f);
    std::vector<std::vector<WipeTower::ToolChangeResult>> results;
    tower.generate(results);
    REQUIRE(results.size() == 1);
    std::vector<double> extrusion_lengths;
    for (const WipeTower::ToolChangeResult &change : results.front()) {
        if (change.initial_tool == change.new_tool)
            continue;
        Vec2f previous = change.start_pos;
        double length = 0.;
        for (const WipeTower::Extrusion &extrusion : change.extrusions) {
            if (extrusion.width > 0.f)
                length += (extrusion.pos - previous).norm();
            previous = extrusion.pos;
        }
        extrusion_lengths.push_back(length);
    }
    REQUIRE(extrusion_lengths.size() == 3);
    CHECK(extrusion_lengths[2] > extrusion_lengths[0] + 50.);
}

TEST_CASE("Sequential copies receive independent half-layer execution plans", "[HalfLayer][InstancePlan]")
{
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"outer_wall_half_layer_height", true}, {"print_sequence", "by object"},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false},
        {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    Print print;
    Model model;
    init_print({cube(5.)}, print, model, config);
    ModelInstance *copy = model.objects.front()->add_instance();
    copy->set_offset(Vec3d(20., 0., 0.));
    print.apply(model, config);
    print.process();
    const PrintObject &object = *print.get_object(size_t(0));
    REQUIRE(object.instances().size() == 2);

    ToolOrdering first(object, 0, false, 0);
    first.sort_and_build_data(object, 0);
    ToolOrdering second(object, 0, false, 1);
    second.sort_and_build_data(object, 0);
    REQUIRE(first.half_layer_execution_plan() != nullptr);
    REQUIRE(second.half_layer_execution_plan() != nullptr);
    REQUIRE(first.half_layer_execution_plan()->frames.size() == second.half_layer_execution_plan()->frames.size());
    const double expected_shift_x = unscale<double>(object.instances()[1].shift.x() - object.instances()[0].shift.x());
    for (size_t frame_id = 0; frame_id < first.half_layer_execution_plan()->frames.size(); ++frame_id) {
        const auto &a = first.half_layer_execution_plan()->frames[frame_id];
        const auto &b = second.half_layer_execution_plan()->frames[frame_id];
        REQUIRE(a.valid());
        REQUIRE(b.valid());
        REQUIRE(a.plan.tasks.size() == b.plan.tasks.size());
        CHECK(a.plan.upper_begin == b.plan.upper_begin);
        for (size_t task_id = 0; task_id < a.plan.tasks.size(); ++task_id) {
            CHECK(a.plan.tasks[task_id].instance_id == 0);
            CHECK(b.plan.tasks[task_id].instance_id == 1);
            CHECK(b.plan.tasks[task_id].first_mm.x() - a.plan.tasks[task_id].first_mm.x() ==
                Catch::Approx(expected_shift_x).margin(0.000001));
        }
    }
    const std::string output = gcode(print);
    CHECK_FALSE(output.empty());
}

TEST_CASE("Half-layer skirt is consumed only by its scheduled filament", "[HalfLayer][GCodeInteractions][SkirtOwnershipRegression]")
{
    const char *engine = GENERATE("classic", "arachne");
    const bool support_half = GENERATE(false, true);
    const int skirt_height = GENERATE(1, 2);
    CAPTURE(engine, support_half, skirt_height);
    auto config = multifilament_config(2, {
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_generator", engine}, {"wall_loops", 4},
        {"outer_wall_filament_id", 1}, {"inner_wall_filament_id", 2},
        {"sparse_infill_filament_id", 2}, {"internal_solid_filament_id", 2},
        {"top_surface_filament_id", 2}, {"bottom_surface_filament_id", 2},
        {"outer_wall_half_layer_height", true}, {"support_half_layer_height", support_half},
        {"only_one_wall_first_layer", false}, {"enable_support", true},
        {"support_filament", 2}, {"support_interface_filament", 2}, {"support_threshold_angle", 60},
        {"skirt_type", "combined"}, {"skirt_loops", 2}, {"skirt_height", skirt_height},
        {"brim_type", "no_brim"}, {"enable_prime_tower", false},
        {"use_relative_e_distances", true}, {"layer_change_gcode", "G92 E0\n"},
    });
    config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
    Print print;
    Model model;
    init_print({TestMesh::overhang}, print, model, config);
    const std::string output = gcode(print);
    std::set<int> skirt_planes_um;
    std::string role;
    size_t skirt_moves = 0;
    GCodeReader reader;
    reader.apply_config(config);
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        if (role == "Skirt" && line.extruding(state) && line.dist_XY(state) > 0.) {
            ++skirt_moves;
            skirt_planes_um.insert(int(std::lround(line.new_Z(state) * 1000.)));
        }
    });
    // Config/footer strings containing "skirt" are not extrusion evidence.
    CHECK(skirt_moves > 0);
    CHECK(skirt_planes_um == (skirt_height == 1 ? std::set<int>{200} : std::set<int>{200, 400}));
}

TEST_CASE("Half-layer dispatcher retains auxiliary and sequential workflows", "[HalfLayer][GCodeInteractions]")
{
    SECTION("combined skirt and brim") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"wall_loops", 4}, {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
            {"only_one_wall_first_layer", false}, {"skirt_loops", 2}, {"brim_type", "outer_only"}, {"brim_width", 3.}});
        Print print;
        Model model;
        init_print({cube(5.)}, print, model, config);
        const std::string output = gcode(print);
        CHECK(output.find("skirt") != std::string::npos);
        CHECK(output.find("brim") != std::string::npos);
    }
    SECTION("support brim restores shifted object origin") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"outer_wall_half_layer_height", true}, {"support_half_layer_height", true},
            {"enable_support", true}, {"support_threshold_angle", 60}, {"brim_type", "outer_only"},
            {"brim_width", 3.}, {"skirt_loops", 0}});
        const auto support_min_x = [&](double shift_x) {
            Print print;
            Model model;
            init_print({TestMesh::overhang}, print, model, config);
            ModelInstance *instance = model.objects.front()->instances.front();
            instance->set_offset(instance->get_offset() + Vec3d(shift_x, 0., 0.));
            print.apply(model, config);
            const std::string output = gcode(print);
            double minimum_x = std::numeric_limits<double>::max();
            bool support = false;
            GCodeReader reader;
            reader.apply_config(config);
            reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
                const std::string_view comment = line.comment();
                if (comment.find("TYPE:") != std::string_view::npos)
                    support = comment.find("TYPE:Support") != std::string_view::npos;
                if (support && line.extruding(state) && line.dist_XY(state) > 0.)
                    minimum_x = std::min(minimum_x, double(line.new_X(state)));
            });
            REQUIRE(minimum_x < std::numeric_limits<double>::max());
            return minimum_x;
        };
        CHECK(support_min_x(20.) - support_min_x(0.) == Catch::Approx(20.).margin(0.001));
    }
    SECTION("sequential objects") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"wall_loops", 4}, {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
            {"only_one_wall_first_layer", false}, {"print_sequence", "by object"},
            {"gcode_label_objects", true}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
        TriangleMesh left = cube(5.);
        TriangleMesh right = cube(5.);
        right.translate(10.f, 0.f, 0.f);
        Print print;
        Model model;
        init_print({left, right}, print, model, config);
        const std::string output = gcode(print);
        CHECK_FALSE(output.empty());
        CHECK(output.find("printing object") != std::string::npos);
    }
    SECTION("low-temperature support interface") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"support_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
            {"support_interface_top_layers", 2}, {"support_interface_filament", 0},
            {"single_nozzle_low_temperature_interface", true}, {"support_interface_temperature", 170},
            {"support_interface_heating_time", 0.0}, {"support_interface_temperature_drop_tower", false},
            {"support_interface_auxiliary_fan_cooling_on_temperature_change", false},
            {"support_interface_nozzle_wiping_on_temperature_change", false},
            {"skirt_loops", 0}, {"brim_type", "no_brim"}});
        config.set_key_value("nozzle_temperature", new ConfigOptionInts({220}));
        config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({220}));
        Print print;
        Model model;
        init_print({TestMesh::overhang}, print, model, config);
        const std::string output = gcode(print);
        CHECK(output.find("low-temperature support interface begin") != std::string::npos);
        CHECK(output.find("low-temperature support interface end") != std::string::npos);
        size_t begins = 0, ends = 0;
        for (size_t pos = 0; (pos = output.find("low-temperature support interface begin", pos)) != std::string::npos;
             pos += sizeof("low-temperature support interface begin") - 1)
            ++begins;
        for (size_t pos = 0; (pos = output.find("low-temperature support interface end", pos)) != std::string::npos;
             pos += sizeof("low-temperature support interface end") - 1) {
            ++ends;
            const size_t restore = output.find("M104 S220", pos);
            const size_t next_type = output.find(";TYPE:", pos);
            REQUIRE(restore != std::string::npos);
            CHECK((next_type == std::string::npos || restore < next_type));
        }
        CHECK(ends == begins);
    }
    SECTION("traditional timelapse and wrapping hooks") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"wall_loops", 4}, {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
            {"only_one_wall_first_layer", false}, {"printer_structure", "i3"},
            {"time_lapse_gcode", ";HALF_LAYER_TIMELAPSE"}, {"timelapse_type", "0"},
            {"enable_wrapping_detection", true}, {"wrapping_detection_gcode", ";HALF_LAYER_WRAPPING"},
            {"skirt_loops", 0}, {"brim_type", "no_brim"}});
        Print print;
        Model model;
        init_print({cube(5.)}, print, model, config);
        const std::string output = gcode(print);
        CHECK(output.find(";HALF_LAYER_TIMELAPSE") != std::string::npos);
        CHECK(output.find(";HALF_LAYER_WRAPPING") != std::string::npos);
    }
    SECTION("variable parent heights") {
        auto config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
            {"wall_loops", 4}, {"outer_wall_half_layer_height", true}, {"only_one_wall_top", false},
            {"only_one_wall_first_layer", false}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
        Print print;
        Model model;
        init_print({cube(20.)}, print, model, config);
        model.objects.front()->layer_height_profile.set({0., 0.2, 6., 0.12, 12., 0.28, 20., 0.16});
        print.apply(model, config);
        const std::string output = gcode(print);
        REQUIRE_FALSE(output.empty());
        REQUIRE(print.half_layer_execution_plan() != nullptr);
        std::set<int> parent_heights_um;
        for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames)
            for (const HalfLayerExecutionTask &task : frame.plan.tasks)
                if (!task.support && task.physical_layer != task.logical_layer) {
                    parent_heights_um.insert(int(std::lround(task.motion_reference_height * 1000.)));
                    CHECK(task.physical_layer->height == Catch::Approx(0.5 * task.motion_reference_height));
                }
        CHECK(parent_heights_um.size() > 1);
    }
}

TEST_CASE("Bunny60 exports half-height support through every native generator", "[HalfLayer][.Bunny60]")
{
    const int generator = GENERATE(0, 1, 2, 3, 4, 5);
    const bool outer_half = GENERATE(false, true);
    const SupportType type = generator == 0 ? stNormalAuto : generator == 1 ? stNormalCuraAuto :
        generator == 4 ? stResinAuto : generator == 5 ? stMixedAuto : stTreeAuto;
    TriangleMesh bunny;
    const auto bunny_path = boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() /
        "resources/handy_models/Stanford_Bunny.drc";
    REQUIRE(load_drc(bunny_path.string().c_str(), &bunny));
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_loops", 4}, {"outer_wall_half_layer_height", outer_half}, {"support_half_layer_height", true},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}, {"enable_support", true},
        {"support_threshold_angle", 60}, {"support_interface_top_layers", 2},
        {"support_interface_bottom_layers", 2}, {"skirt_loops", 0}, {"brim_type", "no_brim"}});
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(type));
    if (generator == 2 || generator == 3)
        config.set_key_value("support_style", new ConfigOptionEnum<SupportMaterialStyle>(
            generator == 2 ? smsTreeStrong : smsTreeOrganic));
    Print print;
    Model model;
    init_print({bunny}, print, model, config);
    std::string output;
    CAPTURE(generator, outer_half);
    REQUIRE_NOTHROW(output = gcode(print));
    REQUIRE_FALSE(output.empty());
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    size_t support_tasks = 0;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames)
        for (const HalfLayerExecutionTask &task : frame.plan.tasks)
            if (task.support) {
                ++support_tasks;
                CHECK(task.physical_layer->height <= 0.100001);
                CHECK(task.motion_reference_height > 0.);
            }
    CHECK(support_tasks > 0);
}

// Explicitly selected only: immutable command counterfactuals are captured once
// before extracting the emitter's shared speed/flow calculation.
TEST_CASE("Shared speed calculation preserves captured machine commands", "[.SpeedBaseline]")
{
    const std::string engine = GENERATE(std::string("classic"), std::string("arachne"));
    const int variant = GENERATE(0, 1, 2, 3);
    auto config = variant == 0 ? DynamicPrintConfig::full_print_config() : multifilament_config(2);
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"wall_generator", engine}, {"wall_loops", 4}, {"skirt_loops", 0}, {"brim_type", "no_brim"},
        {"only_one_wall_top", false}, {"only_one_wall_first_layer", false}});
    if (variant > 0) {
        config.set_num_extruders(2);
        config.set_deserialize_strict({{"single_extruder_multi_material", false},
            {"use_smaller_nozzles_in_crisp_corners", true}, {"crisp_corner_small_nozzle_wall_count", 2}});
        set_toolhead_nozzle_diameter(config, 0, 0.4);
        set_toolhead_nozzle_diameter(config, 1, 0.2);
        config.set_key_value("filament_map", new ConfigOptionInts{1, 2});
        config.set_key_value("crisp_corner_small_nozzle_wall_speed", new ConfigOptionFloatsOrPercents{
            FloatOrPercent{50., true}, FloatOrPercent{20., false}});
        config.set_key_value("filament_max_volumetric_speed", new ConfigOptionFloats{12., 5.});
    }
    if (variant > 1) {
        config.set_deserialize_strict({{"print_flow_ratio", 1.1}, {"top_solid_infill_flow_ratio", 1.03},
            {"bottom_solid_infill_flow_ratio", 0.97}, {"set_other_flow_ratios", true},
            {"outer_wall_flow_ratio", 0.9}, {"inner_wall_flow_ratio", 1.05}, {"sparse_infill_flow_ratio", 1.02}});
        config.set_key_value("filament_flow_ratio", new ConfigOptionFloats{0.98, 1.02});
    }
    if (variant == 3) {
        config.set_deserialize_strict({{"outer_wall_speed", 0.}, {"inner_wall_speed", 0.},
            {"raft_layers", 3}, {"filament_adaptive_volumetric_speed", true}});
        config.set_key_value("volumetric_speed_coefficients", new ConfigOptionStrings{"0 0 0 0 0 9", "0 0 0 0 0 9"});
    }
    Print print;
    Model model;
    if (variant == 2)
        init_print({TestMesh::bridge}, print, model, config);
    else
        init_print({cube(5.)}, print, model, config);
    std::istringstream input(gcode(print));
    std::string commands, line;
    while (std::getline(input, line)) {
        const auto comment = line.find(';');
        if (comment != std::string::npos)
            line.resize(comment);
        const auto end = line.find_last_not_of(" \t\r");
        if (end != std::string::npos)
            commands += line.substr(0, end + 1) + '\n';
    }
    REQUIRE_FALSE(commands.empty());
    const std::string path = "half-speed-baseline-" + engine + "-" + std::to_string(variant) + ".commands";
    std::ifstream reference(path, std::ios::binary);
    CAPTURE(engine, variant, path);
    const char *capture = std::getenv("MAGPIE_HALF_LAYER_CAPTURE_SPEED_BASELINE");
    if (capture != nullptr && std::string(capture) == "1") {
        REQUIRE_FALSE(reference.is_open()); // Never overwrite an existing counterfactual.
        std::ofstream output(path, std::ios::binary);
        REQUIRE(output.is_open());
        output << commands;
        output.close();
        CHECK(output.good());
    } else {
        REQUIRE(reference.is_open());
        std::ostringstream expected;
        expected << reference.rdbuf();
        CHECK(commands == expected.str());
    }
}

TEST_CASE("Half-height support band ends never leave a sliver", "[HalfLayer][SupportBands]")
{
    // Model grid of a 0.2 mm layer: H/2 cuts every 0.1 mm. Band limits 0.05 to 0.1 mm.
    const std::vector<double> cuts_mm{0.1, 0.2, 0.3, 0.4};
    auto ends = [&](double bottom_mm, double top_mm) {
        return half_height_support_band_ends(cuts_mm, bottom_mm, top_mm, 0.05, 0.1);
    };
    auto same_ends = [](const std::vector<double> &a, const std::vector<double> &b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); ++i)
            if (std::abs(a[i] - b[i]) > 0.000001) // Endpoint tolerance, mm.
                return false;
        return true;
    };
    // Thin off-grid support layers straddling a cut stay whole (0.022 and 0.044 mm).
    CHECK(same_ends(ends(0.189, 0.211), {0.211}));
    CHECK(same_ends(ends(0.189, 0.233), {0.233}));
    // A bottom or top sliver shares the neighbouring band evenly instead.
    CHECK(same_ends(ends(0.189, 0.4), {0.2445, 0.3, 0.4}));
    CHECK(same_ends(ends(0.0, 0.211), {0.1, 0.1555, 0.211}));
    // Aligned layers and bands of exactly the minimum keep every grid cut.
    CHECK(same_ends(ends(0.0, 0.4), {0.1, 0.2, 0.3, 0.4}));
    CHECK(same_ends(ends(0.05, 0.25), {0.1, 0.2, 0.25}));
    // Without a minimum the grid cut is reproduced.
    CHECK(same_ends(half_height_support_band_ends(cuts_mm, 0.189, 0.211, 0., 0.1), {0.2, 0.211}));
}

TEST_CASE("Independent support layers under H/2 support produce no sliver bands", "[HalfLayer][SupportBands]")
{
    constexpr double h_mm = 0.2;
    auto config = DynamicPrintConfig::full_print_config();
    // An off-grid gap: independent support layers put the contact 0.15 mm under the model.
    config.set_deserialize_strict({{"layer_height", h_mm}, {"initial_layer_print_height", h_mm},
        {"support_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
        {"independent_support_layer_height", true}, {"support_top_z_distance", 0.15},
        {"support_bottom_z_distance", 0.15}, {"support_on_build_plate_only", true}});
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    TriangleMesh mesh = make_cube(5., 10., 6.);
    TriangleMesh ceiling = make_cube(20., 10., 1.2);
    ceiling.translate(0.f, 0.f, 6.f);
    mesh.merge(ceiling);
    Print print;
    Model model;
    init_print({mesh}, print, model, config);
    print.process();
    const auto &object = *print.get_object(size_t(0));
    REQUIRE_FALSE(object.support_layers().empty());
    // Physical band limits, mm: a quarter and a half of the 0.2 mm layer. The defect left 0.003 mm bands.
    for (const auto *layer : object.support_layers()) {
        CAPTURE(layer->print_z, layer->height);
        CHECK(layer->height >= 0.25 * h_mm - 0.000001);
        CHECK(layer->height <= 0.5 * h_mm + 0.000001);
    }
}

TEST_CASE("Half-height outer walls force half-height support", "[HalfLayer][SupportForced]")
{
    const bool outer_walls = GENERATE(false, true);
    const bool support = GENERATE(false, true);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"outer_wall_half_layer_height", outer_walls}, {"support_half_layer_height", support},
        {"enable_support", true}});
    Print print;
    Model model;
    init_print({make_cube(10., 10., 10.)}, print, model, config);
    CAPTURE(outer_walls, support);
    // Global settings: support follows the outer walls, and stays as configured otherwise.
    CHECK(print.get_object(0)->config().support_half_layer_height.value == (outer_walls || support));
    // A per-object override of the outer walls forces the object's support too.
    model.objects.front()->config.set("outer_wall_half_layer_height", true);
    print.apply(model, config);
    CHECK(print.get_object(0)->config().support_half_layer_height.value);
}

TEST_CASE("Half-height frames print upper-plane support after the lower outer-wall pass", "[HalfLayer][SupportOrder]")
{
    const int generator = GENERATE(0, 2);
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"outer_wall_half_layer_height", true}, {"enable_support", true}, {"support_threshold_angle", 60},
        {"support_on_build_plate_only", true}, {"layer_change_gcode", "G92 E0"}});
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(generator == 0 ? stNormalAuto : stTreeAuto));
    TriangleMesh mesh = make_cube(5., 10., 6.);
    TriangleMesh ceiling = make_cube(20., 10., 1.2);
    ceiling.translate(0.f, 0.f, 6.f);
    mesh.merge(ceiling);
    Print print;
    Model model;
    init_print({mesh}, print, model, config);
    print.process();
    REQUIRE(print.half_layer_execution_plan() != nullptr);
    CAPTURE(generator);
    size_t frames_with_both = 0;
    for (const HalfLayerExecutionFrame &frame : print.half_layer_execution_plan()->frames) {
        const auto &tasks = frame.plan.tasks;
        if (frame.plan.lower_begin == frame.plan.lower_end)
            continue;
        const double lower_z = tasks[frame.plan.lower_begin].physical_layer->print_z;
        bool support_above = false;
        // Physical Z never drops within a frame, so the nozzle never returns beside a taller deposit.
        for (size_t i = 1; i < tasks.size(); ++i) {
            CAPTURE(frame.print_z, i);
            CHECK(tasks[i].physical_layer->print_z >= tasks[i - 1].physical_layer->print_z - 0.000001);
        }
        for (size_t i = 0; i < frame.plan.lower_begin; ++i)
            CHECK(tasks[i].physical_layer->print_z <= lower_z + 0.000001);
        for (const auto &task : tasks)
            support_above |= task.support && task.physical_layer->print_z > lower_z + 0.000001;
        frames_with_both += support_above;
    }
    CHECK(frames_with_both > 0);
}
