#include <catch2/catch_all.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <tuple>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Format/OBJ.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/CuraStyleSupport.hpp"
#include "libslic3r/Support/MixedSupportPlan.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

#include "test_helpers.hpp" // get access to init_print, etc
#include "test_utils.hpp"

namespace Slic3r {
std::vector<Polygons> buildplate_covered_by_object(const PrintObject &object);
}

using namespace Slic3r::Test;
using namespace Slic3r;

static Polygons support_test_rectangle(double min_x, double min_y, double max_x, double max_y)
{
    return Polygons{Polygon{
        Point(scale_(min_x), scale_(min_y)),
        Point(scale_(max_x), scale_(min_y)),
        Point(scale_(max_x), scale_(max_y)),
        Point(scale_(min_x), scale_(max_y))
    }};
}

static bool same_polygon_set(const Polygons &left, const Polygons &right)
{
    return diff(left, right).empty() && diff(right, left).empty();
}

TEST_CASE("Cura support joins nearby regions without deleting originals",
          "[SupportMaterial][Cura][Geometry]")
{
    const Polygons left = support_test_rectangle(0., 0., 10., 10.);
    const Polygons nearby_region = support_test_rectangle(11., 0., 21., 10.);
    const Polygons distant_region = support_test_rectangle(15., 0., 25., 10.);
    const coord_t half_min_feature = scale_(0.2);

    const Polygons disabled = join_cura_style_support_regions(
        left, nearby_region, 0, half_min_feature);
    CHECK(union_ex(disabled).size() == 2);

    const Polygons connected = join_cura_style_support_regions(
        left, nearby_region, scale_(2.), half_min_feature);
    CHECK(union_ex(connected).size() == 1);
    Polygons originals = left;
    append(originals, nearby_region);
    CHECK(diff(originals, connected).empty());

    const Polygons outside_range = join_cura_style_support_regions(
        left, distant_region, scale_(2.), half_min_feature);
    CHECK(union_ex(outside_range).size() == 2);

    const Polygons thin = support_test_rectangle(30., 0., 30.2, 10.);
    const Polygons with_thin_original = join_cura_style_support_regions(
        left, thin, scale_(2.), half_min_feature);
    CHECK(diff(thin, with_thin_original).empty());
}

TEST_CASE("Cura join distance connects separated overhang support in a processed print",
          "[SupportMaterial][Cura][Geometry][Integration]")
{
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(1., 1., 1.); // Keep the combined object on the bed.
        fixture.translate(0.f, 30.f, 0.f);
        TriangleMesh left_ceiling = make_cube(10., 10., 2.);
        left_ceiling.translate(0.f, 0.f, 10.f);
        TriangleMesh right_ceiling = make_cube(10., 10., 2.);
        right_ceiling.translate(11.f, 0.f, 10.f); // One millimetre unsupported gap.
        fixture.merge(left_ceiling);
        fixture.merge(right_ceiling);
        return fixture;
    };

    const auto component_counts = [&](double join_distance_mm) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalCuraAuto));
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
        config.set_key_value("cura_support_join_distance", new ConfigOptionFloat(join_distance_mm));
        config.set_key_value("support_expansion", new ConfigOptionFloat(0.));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
        REQUIRE(print.objects().size() == 1);
        std::vector<size_t> counts;
        for (const SupportLayer *layer : print.objects().front()->support_layers()) {
            if (layer->print_z < 2. || layer->print_z > 8. || layer->support_islands.empty())
                continue;
            counts.push_back(union_ex(layer->support_islands).size());
        }
        return counts;
    };

    const std::vector<size_t> unjoined = component_counts(0.);
    const std::vector<size_t> joined = component_counts(2.);
    REQUIRE_FALSE(unjoined.empty());
    REQUIRE_FALSE(joined.empty());
    CHECK(std::all_of(unjoined.begin(), unjoined.end(), [](size_t count) { return count == 2; }));
    CHECK(std::all_of(joined.begin(), joined.end(), [](size_t count) { return count == 1; }));
}

TEST_CASE("Cura and Classic Tree propagate cancellation from their long support stages",
          "[SupportMaterial][Cancellation]")
{
    TriangleMesh fixture = make_cube(8., 10., 10.);
    TriangleMesh ceiling = make_cube(20., 10., 2.);
    ceiling.translate(0.f, 0.f, 20.f);
    fixture.merge(ceiling);

    struct CancellationCase {
        SupportType type;
        SupportMaterialStyle style;
        int cancel_percent;
    };
    const std::array<CancellationCase, 2> cases {{
        { stNormalCuraAuto, smsDefault, 54 },
        { stTreeAuto, smsTreeStrong, 60 }
    }};

    for (const CancellationCase &test_case : cases) {
        CAPTURE(int(test_case.type), int(test_case.style), test_case.cancel_percent);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(test_case.type));
        config.set_key_value("support_style",
            new ConfigOptionEnum<SupportMaterialStyle>(test_case.style));
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

        Print print;
        Model model;
        init_print({ fixture }, print, model, config);
        bool cancellation_requested = false;
        print.set_status_callback([&](const PrintBase::SlicingStatus &status) {
            if (!cancellation_requested && status.percent >= test_case.cancel_percent) {
                cancellation_requested = true;
                print.cancel();
            }
        });

        REQUIRE_THROWS_AS(print.process(), CanceledException);
        CHECK(cancellation_requested);
        CHECK(print.canceled());
    }
}

static TriangleMesh resin_test_sloped_wall()
{
    constexpr double layer_height = 0.2;
    std::vector<ExPolygons> slices;
    slices.reserve(100);
    for (size_t layer_index = 0; layer_index < 100; ++layer_index) {
        const double shift = 0.1 * double(layer_index);
        slices.emplace_back(ExPolygons { ExPolygon(support_test_rectangle(
            shift, 0., shift + 30., 30.).front()) });
    }
    return TriangleMesh(slices_to_mesh(slices, 0., layer_height, layer_height));
}

TEST_CASE("Top interface footprints remain stable through their full thickness",
          "[SupportMaterial][InterfaceSmoothing]")
{
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayer intermediate;
    SupportGeneratorLayer interface;
    SupportGeneratorLayer base_interface;

    intermediate.layer_type = SupporLayerType::Intermediate;
    interface.layer_type = SupporLayerType::TopInterface;
    base_interface.layer_type = SupporLayerType::Base;
    intermediate.print_z = interface.print_z = base_interface.print_z = 1.0;
    intermediate.bottom_z = interface.bottom_z = base_interface.bottom_z = 0.8;
    intermediate.height = interface.height = base_interface.height = 0.2;

    const Polygons target = support_test_rectangle(0.0, 0.0, 10.0, 10.0);
    interface.polygons = support_test_rectangle(0.0, 0.0, 4.0, 10.0);
    intermediate.polygons = support_test_rectangle(4.0, 0.0, 8.0, 10.0);
    base_interface.polygons = support_test_rectangle(8.0, 0.0, 10.0, 10.0);

    SupportGeneratorLayersPtr intermediate_layers{&intermediate};
    SupportGeneratorLayersPtr interface_layers{&interface};
    SupportGeneratorLayersPtr base_interface_layers{&base_interface};
    const std::vector<Polygons> interface_targets{target};
    const std::vector<Polygons> base_targets(1);

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        interface_targets, base_targets, 0, storage);

    CHECK(same_polygon_set(interface.polygons, target));
    CHECK(intermediate.polygons.empty());
    CHECK(base_interface.polygons.empty());
}

TEST_CASE("Top interface smoothing never exceeds its printable support envelope",
          "[SupportMaterial][InterfaceSmoothing]")
{
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayer intermediate;
    SupportGeneratorLayer interface;

    intermediate.layer_type = SupporLayerType::Intermediate;
    interface.layer_type = SupporLayerType::TopInterface;
    intermediate.print_z = interface.print_z = 1.0;
    intermediate.bottom_z = interface.bottom_z = 0.8;
    intermediate.height = interface.height = 0.2;

    const Polygons printable = support_test_rectangle(0.0, 0.0, 10.0, 10.0);
    interface.polygons = support_test_rectangle(0.0, 0.0, 3.0, 10.0);
    intermediate.polygons = support_test_rectangle(3.0, 0.0, 10.0, 10.0);

    SupportGeneratorLayersPtr intermediate_layers{&intermediate};
    SupportGeneratorLayersPtr interface_layers{&interface};
    SupportGeneratorLayersPtr base_interface_layers;
    const std::vector<Polygons> targets{support_test_rectangle(-5.0, -5.0, 15.0, 15.0)};
    const std::vector<Polygons> base_targets(1);

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        targets, base_targets, scale_(1.0), storage);

    CHECK(same_polygon_set(interface.polygons, printable));
}

TEST_CASE("Top interface footprint is preserved on every requested thickness layer",
          "[SupportMaterial][InterfaceSmoothing]")
{
    constexpr size_t interface_depth = 5;
    constexpr size_t layer_count = interface_depth + 1;

    SupportGeneratorLayerStorage storage;
    std::array<SupportGeneratorLayer, layer_count> intermediate;
    std::array<SupportGeneratorLayer, layer_count> interface;
    SupportGeneratorLayersPtr intermediate_layers(layer_count);
    SupportGeneratorLayersPtr interface_layers(layer_count);
    SupportGeneratorLayersPtr base_interface_layers;
    std::vector<Polygons> targets(layer_count);
    std::vector<Polygons> base_targets(layer_count);
    const Polygons target = support_test_rectangle(0.0, 0.0, 10.0, 10.0);

    for (size_t idx = 0; idx < layer_count; ++idx) {
        const double print_z = 0.2 * double(idx + 1);
        intermediate[idx].layer_type = SupporLayerType::Intermediate;
        intermediate[idx].print_z = print_z;
        intermediate[idx].bottom_z = print_z - 0.2;
        intermediate[idx].height = 0.2;
        intermediate[idx].polygons = target;
        intermediate_layers[idx] = &intermediate[idx];
        interface_layers[idx] = nullptr;

        if (idx < interface_depth) {
            const double existing_width = 2.0 + double(idx);
            interface[idx].layer_type = SupporLayerType::TopInterface;
            interface[idx].print_z = print_z;
            interface[idx].bottom_z = print_z - 0.2;
            interface[idx].height = 0.2;
            interface[idx].polygons = support_test_rectangle(0.0, 0.0, existing_width, 10.0);
            intermediate[idx].polygons = support_test_rectangle(existing_width, 0.0, 10.0, 10.0);
            interface_layers[idx] = &interface[idx];
            targets[idx] = target;
        }
    }

    stabilize_top_interface_footprints(
        intermediate_layers, interface_layers, base_interface_layers,
        targets, base_targets, 0, storage);

    for (size_t idx = 0; idx < interface_depth; ++idx) {
        REQUIRE(interface_layers[idx] != nullptr);
        CHECK(same_polygon_set(interface_layers[idx]->polygons, target));
        CHECK(intermediate_layers[idx]->polygons.empty());
    }
    CHECK(interface_layers[interface_depth] == nullptr);
    CHECK(same_polygon_set(intermediate_layers[interface_depth]->polygons, target));
}

static bool collection_has_role(const ExtrusionEntityCollection &collection, ExtrusionRole role)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            if (collection_has_role(*children, role))
                return true;
        } else if (entity->role() == role) {
            return true;
        }
    }
    return false;
}

static bool collection_has_closed_path_with_role(
    const ExtrusionEntityCollection &collection, ExtrusionRole role)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            if (collection_has_closed_path_with_role(*children, role))
                return true;
        } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity);
                   loop != nullptr && loop->role() == role && !loop->paths.empty()) {
            return true;
        } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(entity);
                   multipath != nullptr && multipath->role() == role && !multipath->paths.empty() &&
                   multipath->paths.front().first_point().distance_to(multipath->paths.back().last_point()) <= scale_(1.)) {
            return true;
        } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                   path != nullptr && path->role() == role && path->polyline.points.size() >= 4 &&
                   path->first_point().distance_to(path->last_point()) <= scale_(1.)) {
            return true;
        }
    }
    return false;
}

static size_t maximum_grouped_path_count_with_role(
    const ExtrusionEntityCollection &collection, ExtrusionRole role)
{
    size_t direct_path_count = 0;
    size_t maximum_child_count = 0;
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            maximum_child_count = std::max(
                maximum_child_count, maximum_grouped_path_count_with_role(*children, role));
        } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity);
                   path != nullptr && path->role() == role) {
            ++direct_path_count;
        }
    }
    return std::max(direct_path_count, maximum_child_count);
}

static std::vector<size_t> support_layers_with_role(const Print &print, ExtrusionRole role)
{
    std::vector<size_t> result;
    const auto &layers = print.objects().front()->support_layers();
    for (size_t layer_idx = 0; layer_idx < layers.size(); ++layer_idx)
        if (collection_has_role(layers[layer_idx]->support_fills, role))
            result.push_back(layer_idx);
    return result;
}

static double normalized_undirected_angle(double angle)
{
    angle = std::fmod(angle, M_PI);
    return angle < 0. ? angle + M_PI : angle;
}

template <class PolylineType>
static void check_polyline_directions(
    const PolylineType &polyline, const std::array<double, 3> &expected_angles, size_t &segment_count)
{
    REQUIRE(polyline.points.size() >= 2);
    for (size_t point_idx = 1; point_idx < polyline.points.size(); ++point_idx) {
        const auto delta = polyline.points[point_idx] - polyline.points[point_idx - 1];
        const double angle = normalized_undirected_angle(
            std::atan2(double(delta.y()), double(delta.x())));
        double best_error = std::numeric_limits<double>::max();
        for (double expected : expected_angles) {
            const double direct_error = std::abs(angle - expected);
            best_error = std::min(best_error, std::min(direct_error, M_PI - direct_error));
        }
        CHECK(best_error < 0.002);
        ++segment_count;
    }
}

static void check_role_directions(
    const ExtrusionEntityCollection &collection,
    ExtrusionRole role,
    const std::array<double, 3> &expected_angles,
    size_t &segment_count)
{
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        if (const auto *children = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            check_role_directions(*children, role, expected_angles, segment_count);
        } else if (entity->role() == role) {
            if (const auto *path = dynamic_cast<const ExtrusionPath *>(entity))
                check_polyline_directions(path->polyline, expected_angles, segment_count);
            else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(entity))
                for (const ExtrusionPath &path : multipath->paths)
                    check_polyline_directions(path.polyline, expected_angles, segment_count);
            else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(entity))
                for (const ExtrusionPath &path : loop->paths)
                    check_polyline_directions(path.polyline, expected_angles, segment_count);
            else
                FAIL("Unexpected extrusion entity carrying the support-interface sublayer role");
        }
    }
}

static DynamicPrintConfig sublayer_config(
    SupportType support_type,
    bool enabled = true,
    int interface_layers = 5,
    int start_layer = 2,
    int end_layer = 4)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", interface_layers },
        { "support_interface_bottom_layers", 0 },
        { "support_interface_pattern", "rectilinear" },
        { "support_interface_spacing", 0.0 },
        { "support_interface_sublayer_pattern", enabled },
        { "support_interface_sublayer_start_layer", start_layer },
        { "support_interface_sublayer_end_layer", end_layer },
        { "support_interface_sublayer_pattern_type", "triangles" },
        { "support_interface_sublayer_angle", 17.0 },
        { "support_interface_sublayer_temperature", 170 }
    });
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(support_type));
    return config;
}

TEST_CASE("Cura normal support honors explicit geometry styles", "[SupportMaterial][CuraStyle]")
{
    CHECK(uses_cura_support_geometry(stNormalCuraAuto, smsDefault));
    CHECK(uses_cura_support_geometry(stNormalCura, smsDefault));
    CHECK(uses_cura_support_geometry(stNormalCuraAuto, smsGrid));
    CHECK(uses_cura_support_geometry(stNormalCuraAuto, smsSnug));

    CHECK(is_normal_support(stNormalAuto));
    CHECK(is_normal_support(stNormalCuraAuto));
    CHECK(is_normal_support(stNormal));
    CHECK(is_normal_support(stNormalCura));
    CHECK_FALSE(is_normal_support(stTreeAuto));
    CHECK_FALSE(is_normal_support(stTree));
}

TEST_CASE("Mixed support settings round trip with stable enum values", "[SupportMaterial][Mixed][Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "support_type", "mixed(auto)" },
        { "mixed_normal_support_generator", "cura" },
        { "mixed_tree_support_style", "tree_strong" },
        { "mixed_normal_coverage_threshold", "80%" },
        { "mixed_selective_merge", "1" }
    });

    REQUIRE(config.opt_enum<SupportType>("support_type") == stMixedAuto);
    CHECK(config.opt_enum<MixedNormalSupportGenerator>("mixed_normal_support_generator") == mnsgCura);
    CHECK(config.opt_enum<MixedTreeSupportStyle>("mixed_tree_support_style") == mtssStrong);
    CHECK(config.opt<ConfigOptionPercent>("mixed_normal_coverage_threshold")->value == Catch::Approx(80.));
    CHECK(config.opt_bool("mixed_selective_merge"));
    CHECK(is_auto(stMixedAuto));
    CHECK(uses_normal_channel(stMixedAuto));
    CHECK(uses_tree_channel(stMixedAuto));
    CHECK_FALSE(is_normal_support(stMixedAuto));
    CHECK_FALSE(is_tree(stMixedAuto));
    CHECK(mixed_tree_style_to_support_style(mtssOrganic) == smsTreeOrganic);
    CHECK(mixed_tree_style_to_support_style(mtssSlim) == smsTreeSlim);
    CHECK(mixed_tree_style_to_support_style(mtssStrong) == smsTreeStrong);
    CHECK(mixed_tree_style_to_support_style(mtssTreeHybrid) == smsTreeHybrid);

    DynamicPrintConfig restored = DynamicPrintConfig::full_print_config();
    for (const char *key : { "support_type", "mixed_normal_support_generator",
                             "mixed_tree_support_style", "mixed_normal_coverage_threshold",
                             "mixed_selective_merge" })
        restored.set_deserialize_strict(key, config.opt_serialize(key));
    CHECK(restored.opt_enum<SupportType>("support_type") == stMixedAuto);
    CHECK(restored.opt_enum<MixedNormalSupportGenerator>("mixed_normal_support_generator") == mnsgCura);
    CHECK(restored.opt_enum<MixedTreeSupportStyle>("mixed_tree_support_style") == mtssStrong);
    CHECK(restored.opt<ConfigOptionPercent>("mixed_normal_coverage_threshold")->value == Catch::Approx(80.));
    CHECK(restored.opt_bool("mixed_selective_merge"));
}

TEST_CASE("Removed Tsunami support settings migrate safely", "[SupportMaterial][Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    const auto &support_types = ConfigOptionEnum<SupportType>::get_enum_values();
    CHECK(support_types.find("tsunami(auto)") == support_types.end());
    CHECK(support_types.at("mixed(auto)") == stMixedAuto);
    CHECK(support_types.at("resin(auto)") == stResinAuto);

    REQUIRE_NOTHROW(config.set_deserialize_strict("support_type", "tsunami(auto)"));
    CHECK(config.opt_enum<SupportType>("support_type") == stNormalAuto);
    CHECK(config.opt_serialize("support_type") == "normal(auto)");

    for (const char *key : {
             "tsunami_branch_angle",
             "tsunami_micro_branch_enabled",
             "tsunami_micro_branch_angle",
             "tsunami_micro_branch_size",
             "tsunami_trunk_height",
             "tsunami_rib_spacing",
             "tsunami_trunk_thickness",
             "tsunami_min_bed_contact_area",
             "tsunami_max_bed_contact_area",
             "tsunami_branch_minimum_spacing",
         }) {
        CHECK_NOTHROW(config.set_deserialize_strict(key, "0"));
        CHECK(config.option(key) == nullptr);
    }
}

TEST_CASE("Normal and tree support walls plus Cura joining round trip independently",
          "[SupportMaterial][Config]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "support_wall_count", 2 },
        { "tree_support_wall_count", 5 },
        { "cura_support_join_distance", 1.75 }
    });

    CHECK(config.opt_int("support_wall_count") == 2);
    CHECK(config.opt_int("tree_support_wall_count") == 5);
    CHECK(config.opt_float("cura_support_join_distance") == Catch::Approx(1.75));

    DynamicPrintConfig restored = DynamicPrintConfig::full_print_config();
    for (const char *key : {
             "support_wall_count", "tree_support_wall_count", "cura_support_join_distance" })
        restored.set_deserialize_strict(key, config.opt_serialize(key));
    CHECK(restored.opt_int("support_wall_count") == 2);
    CHECK(restored.opt_int("tree_support_wall_count") == 5);
    CHECK(restored.opt_float("cura_support_join_distance") == Catch::Approx(1.75));

    DynamicPrintConfig legacy = DynamicPrintConfig::full_print_config();
    REQUIRE_NOTHROW(legacy.set_deserialize_strict("support_wall_loops", "3"));
    // The pre-2.5.0.0.7 Magpie contract named support_wall_loops as the Tree
    // support wall setting. Deserializing an old project must preserve that
    // channel instead of silently applying the value to normal support.
    CHECK(legacy.opt_int("tree_support_wall_count") == 3);
    CHECK(legacy.opt_int("support_wall_count") == 0);
}

TEST_CASE("Normal support wall count zero disables walls on the bed layer",
          "[SupportMaterial][NormalWallsAudit][Integration]")
{
    const auto support_has_closed_wall = [](int wall_count) {
        TriangleMesh fixture = make_cube(12., 20., 4.);
        TriangleMesh ceiling = make_cube(32., 20., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(smpRectilinear));
        config.set_key_value("support_wall_count", new ConfigOptionInt(wall_count));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

        Print print;
        init_and_process_print({ fixture }, print, config);
        REQUIRE(print.objects().size() == 1);
        const auto support_layers = print.objects().front()->support_layers();
        REQUIRE_FALSE(support_layers.empty());
        return std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
            return collection_has_closed_path_with_role(layer->support_fills, erSupportMaterial);
        });
    };

    CHECK_FALSE(support_has_closed_wall(0));
    CHECK(support_has_closed_wall(2));
}

TEST_CASE("Mixed support planner uses inclusive coverage boundaries", "[SupportMaterial][Mixed][Planner]")
{
    std::vector<Polygons> demand(2);
    demand[1] = support_test_rectangle(0., 0., 10., 10.);
    std::vector<Polygons> shadow(2);
    shadow[1] = support_test_rectangle(0., 0., 2., 10.); // 20 mm2 blocked, 80 mm2 reachable.
    const coord_t connection_width = scale_(0.4); // Support-demand connection distance in XY millimetres.

    const MixedSupportPlan exact = MixedSupportPlan::build_for_geometry(demand, shadow, connection_width, 80.);
    REQUIRE(exact.decisions().size() == 1);
    CHECK(exact.decisions().front().coverage_percent == Catch::Approx(80.).margin(1e-8));
    CHECK(exact.decisions().front().channel == MixedSupportChannel::Normal);
    CHECK_FALSE(exact.normal_mask()[1].empty());
    CHECK(exact.tree_mask()[1].empty());

    // Percentage-point boundary probes around the inclusive 80% comparison.
    const double threshold_epsilon = 1e-9;
    const MixedSupportPlan below = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 80. - threshold_epsilon);
    const MixedSupportPlan above = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 80. + threshold_epsilon);
    CHECK(below.decisions().front().channel == MixedSupportChannel::Normal);
    CHECK(above.decisions().front().channel == MixedSupportChannel::Tree);

    const MixedSupportPlan zero = MixedSupportPlan::build_for_geometry(demand, shadow, connection_width, 0.);
    CHECK(zero.decisions().front().channel == MixedSupportChannel::Normal);
    const MixedSupportPlan hundred = MixedSupportPlan::build_for_geometry(demand, shadow, connection_width, 100.);
    CHECK(hundred.decisions().front().channel == MixedSupportChannel::Tree);
    const MixedSupportPlan fully_reachable = MixedSupportPlan::build_for_geometry(
        demand, std::vector<Polygons>(2), connection_width, 100.);
    CHECK(fully_reachable.decisions().front().channel == MixedSupportChannel::Normal);
}

TEST_CASE("Mixed support planner preserves vanilla per-layer support islands", "[SupportMaterial][Mixed][Planner]")
{
    const coord_t connection_width = scale_(0.4); // Support-demand connection distance in XY millimetres.
    std::vector<Polygons> split_demand(2);
    split_demand[1] = support_test_rectangle(0., 0., 2., 2.);
    append(split_demand[1], support_test_rectangle(10., 0., 12., 2.));
    std::vector<Polygons> split_shadow(2);
    split_shadow[1] = support_test_rectangle(10., 0., 12., 2.);
    const MixedSupportPlan split_channels = MixedSupportPlan::build_for_geometry(
        split_demand, split_shadow, connection_width, 100.);
    REQUIRE(split_channels.decisions().size() == 2);
    CHECK(split_channels.has_normal_demand());
    CHECK(split_channels.has_tree_demand());

    std::vector<Polygons> bridged(3);
    bridged[1] = support_test_rectangle(0., 0., 2., 2.);
    append(bridged[1], support_test_rectangle(6., 0., 8., 2.));
    bridged[2] = support_test_rectangle(1.5, 0., 6.5, 2.);
    const MixedSupportPlan vanilla_islands = MixedSupportPlan::build_for_geometry(
        bridged, std::vector<Polygons>(3), connection_width, 100.);
    // The bridge on layer 2 must not reconnect the two vanilla islands on
    // layer 1 into a single Mixed decision.
    CHECK(vanilla_islands.decisions().size() == 3);

    std::vector<Polygons> reversed = bridged;
    std::reverse(reversed[1].begin(), reversed[1].end());
    const MixedSupportPlan reversed_plan = MixedSupportPlan::build_for_geometry(
        reversed, std::vector<Polygons>(3), connection_width, 100.);
    REQUIRE(reversed_plan.decisions().size() == vanilla_islands.decisions().size());
    for (size_t layer_id = 0; layer_id < bridged.size(); ++layer_id) {
        CHECK(same_polygon_set(reversed_plan.normal_mask()[layer_id], vanilla_islands.normal_mask()[layer_id]));
        CHECK(same_polygon_set(reversed_plan.tree_mask()[layer_id], vanilla_islands.tree_mask()[layer_id]));
    }

    std::vector<Polygons> skipped(3);
    skipped[0] = support_test_rectangle(0., 0., 2., 2.);
    skipped[2] = support_test_rectangle(0., 0., 2., 2.);
    const MixedSupportPlan two_components = MixedSupportPlan::build_for_geometry(
        skipped, std::vector<Polygons>(3), connection_width, 100.);
    CHECK(two_components.decisions().size() == 2);
}

TEST_CASE("Mixed selective merge gives reachable demand to normal support first", "[SupportMaterial][Mixed][Planner]")
{
    std::vector<Polygons> demand(2);
    demand[1] = support_test_rectangle(0., 0., 10., 10.);
    std::vector<Polygons> shadow(2);
    shadow[1] = support_test_rectangle(0., 0., 2., 10.); // 80% vertically reachable.
    const coord_t connection_width = scale_(0.4);

    const MixedSupportPlan disabled = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 81., false);
    REQUIRE(disabled.decisions().size() == 1);
    CHECK(disabled.decisions().front().channel == MixedSupportChannel::Tree);
    CHECK_FALSE(disabled.decisions().front().selectively_split);
    CHECK(disabled.normal_mask()[1].empty());
    CHECK(same_polygon_set(disabled.tree_mask()[1], demand[1]));

    const MixedSupportPlan enabled = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 81., true);
    REQUIRE(enabled.decisions().size() == 1);
    CHECK(enabled.decisions().front().channel == MixedSupportChannel::Mixed);
    CHECK(enabled.decisions().front().selectively_split);
    CHECK(same_polygon_set(enabled.normal_mask()[1], support_test_rectangle(2., 0., 10., 10.)));
    CHECK(same_polygon_set(enabled.tree_mask()[1], support_test_rectangle(0., 0., 2., 10.)));
    CHECK(intersection_ex(enabled.normal_mask()[1], enabled.tree_mask()[1]).empty());
    Polygons recombined = enabled.normal_mask()[1];
    append(recombined, enabled.tree_mask()[1]);
    CHECK(same_polygon_set(union_(recombined), demand[1]));

    const MixedSupportPlan exact = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 80., true);
    CHECK(exact.decisions().front().channel == MixedSupportChannel::Mixed);
    CHECK(exact.decisions().front().selectively_split);
    CHECK(same_polygon_set(exact.normal_mask()[1], support_test_rectangle(2., 0., 10., 10.)));
    CHECK(same_polygon_set(exact.tree_mask()[1], support_test_rectangle(0., 0., 2., 10.)));

    const MixedSupportPlan zero = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 0., true);
    CHECK(zero.decisions().front().channel == MixedSupportChannel::Mixed);
    CHECK(zero.decisions().front().selectively_split);
    CHECK(same_polygon_set(zero.normal_mask()[1], support_test_rectangle(2., 0., 10., 10.)));
    CHECK(same_polygon_set(zero.tree_mask()[1], support_test_rectangle(0., 0., 2., 10.)));
}

TEST_CASE("Mixed painted channels override automatic assignment safely", "[SupportMaterial][Mixed][Planner][Paint]")
{
    std::vector<Polygons> demand(2);
    demand[1] = support_test_rectangle(0., 0., 10., 10.);
    std::vector<Polygons> shadow(2);
    shadow[1] = support_test_rectangle(0., 0., 4., 10.);
    std::vector<Polygons> painted_normal(2);
    painted_normal[1] = support_test_rectangle(0., 0., 8., 10.);
    std::vector<Polygons> painted_tree(2);
    painted_tree[1] = support_test_rectangle(7., 0., 10., 10.);
    const coord_t connection_width = scale_(0.4); // Support-demand connection distance in XY millimetres.

    const MixedSupportPlan plan = MixedSupportPlan::build_for_geometry(
        demand, shadow, connection_width, 100., false, &painted_normal, &painted_tree);

    CHECK(same_polygon_set(plan.normal_mask()[1], support_test_rectangle(4., 0., 7., 10.)));
    Polygons expected_tree = support_test_rectangle(0., 0., 4., 10.);
    append(expected_tree, support_test_rectangle(7., 0., 10., 10.));
    CHECK(same_polygon_set(plan.tree_mask()[1], expected_tree));
    CHECK(same_polygon_set(plan.normal_paint_fallback()[1], support_test_rectangle(0., 0., 4., 10.)));
    CHECK(plan.has_normal_paint_fallback());
    CHECK(intersection_ex(plan.normal_mask()[1], plan.tree_mask()[1]).empty());
    Polygons recombined = plan.normal_mask()[1];
    append(recombined, plan.tree_mask()[1]);
    CHECK(same_polygon_set(union_(recombined), demand[1]));
}

TEST_CASE("Mixed selective merge generates both support channels",
          "[SupportMaterial][Mixed][Integration][MixedIndependent]")
{
    const auto make_fixture = []() {
        // Independent analytic fixture: a 20x10 mm ceiling starts at Z=20 mm. An 8x10 mm
        // pillar ends at Z=10 mm below its left side, so 60% of the ceiling demand is
        // vertically reachable from the bed and 40% is shadowed by the pillar.
        TriangleMesh fixture = make_cube(8., 10., 10.);
        TriangleMesh ceiling = make_cube(20., 10., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const MixedNormalSupportGenerator normal_generator : { mnsgPrusa, mnsgCura }) {
        for (const MixedTreeSupportStyle tree_style : {
                mtssOrganic, mtssSlim, mtssStrong, mtssTreeHybrid }) {
            CAPTURE(int(normal_generator), int(tree_style));
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
            config.set_key_value("mixed_normal_support_generator",
                new ConfigOptionEnum<MixedNormalSupportGenerator>(normal_generator));
            config.set_key_value("mixed_tree_support_style",
                new ConfigOptionEnum<MixedTreeSupportStyle>(tree_style));
            config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
            config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
            config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(false));
            config.set_key_value("support_expansion", new ConfigOptionFloat(0.));
            config.set_key_value("layer_height", new ConfigOptionFloat(0.4));
            config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
            config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

            Print print;
            init_and_process_print({ make_fixture() }, print, config);
            REQUIRE(print.objects().size() == 1);
            const auto support_layers = print.objects().front()->support_layers();
            REQUIRE_FALSE(support_layers.empty());
            CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
                return has_normal_channel(layer->support_type);
            }));
            CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
                return has_tree_channel(layer->support_type);
            }));
            CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
                return layer->support_type == stInnerMixed;
            }));

            const std::string output = gcode(print);
            CHECK_FALSE(output.empty());
            CHECK(output.find("support_type = mixed(auto)") != std::string::npos);
        }
    }
}

TEST_CASE("Mixed organic tree wall count changes emitted support paths",
          "[SupportMaterial][Mixed][Tree][Walls][Integration][MixedIndependent][MixedTreeWalls]")
{
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 10., 10.);
        TriangleMesh ceiling = make_cube(20., 10., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);
        return fixture;
    };
    struct WallMetrics {
        std::map<int, double> path_length_by_layer;
        size_t maximum_grouped_tree_wall_paths { 0 };
    };
    const auto support_metrics = [&](int wall_count, bool selective_merge) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        config.set_key_value("mixed_normal_support_generator",
            new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgCura));
        config.set_key_value("mixed_tree_support_style",
            new ConfigOptionEnum<MixedTreeSupportStyle>(mtssOrganic));
        config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
        config.set_key_value("mixed_selective_merge", new ConfigOptionBool(selective_merge));
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
        config.set_key_value("support_wall_count", new ConfigOptionInt(0));
        config.set_key_value("tree_support_wall_count", new ConfigOptionInt(wall_count));
        config.set_key_value("tree_support_with_infill", new ConfigOptionBool(false));
        config.set_key_value("tree_support_branch_diameter_organic", new ConfigOptionFloat(6.));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
        config.set_key_value("gcode_comments", new ConfigOptionBool(true));

        Print print;
        init_and_process_print({ make_fixture() }, print, config);
        REQUIRE(print.objects().size() == 1);
        WallMetrics metrics;
        for (const SupportLayer *layer : print.objects().front()->support_layers()) {
            if (!has_tree_channel(layer->support_type))
                continue;
            metrics.maximum_grouped_tree_wall_paths = std::max(
                metrics.maximum_grouped_tree_wall_paths,
                maximum_grouped_path_count_with_role(layer->support_fills, erSupportMaterial));
        }

        const std::string output = gcode(print);
        bool support = false;
        GCodeReader reader;
        reader.parse_buffer(output, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
            const std::string_view comment = line.comment();
            if (comment.find("TYPE:") != std::string_view::npos)
                support = comment.find("TYPE:Support") != std::string_view::npos &&
                          comment.find("interface") == std::string_view::npos;
            if (support && line.extruding(self))
                metrics.path_length_by_layer[int(std::lround(double(self.z()) * 1000.))] += line.dist_XY(self);
        });
        return metrics;
    };

    for (const bool selective_merge : { false, true }) {
        CAPTURE(selective_merge);
        const WallMetrics one_wall = support_metrics(1, selective_merge);
        const WallMetrics four_walls = support_metrics(4, selective_merge);
        double one_wall_length = 0.;
        double four_wall_length = 0.;
        double max_layer_ratio = 0.;
        for (const auto &[layer_z, length] : one_wall.path_length_by_layer) {
            one_wall_length += length;
            const auto four = four_walls.path_length_by_layer.find(layer_z);
            if (length > 0. && four != four_walls.path_length_by_layer.end())
                max_layer_ratio = std::max(max_layer_ratio, four->second / length);
        }
        for (const auto &[layer_z, length] : four_walls.path_length_by_layer)
            four_wall_length += length;
        INFO("Mixed one-wall support path length: " << one_wall_length);
        INFO("Mixed four-wall support path length: " << four_wall_length);
        INFO("Largest per-layer four/one wall path ratio: " << max_layer_ratio);
        INFO("One-wall maximum grouped support-wall paths: " << one_wall.maximum_grouped_tree_wall_paths);
        INFO("Four-wall maximum grouped support-wall paths: " << four_walls.maximum_grouped_tree_wall_paths);
        CHECK(one_wall.maximum_grouped_tree_wall_paths >= 1);
        CHECK(four_walls.maximum_grouped_tree_wall_paths >= 4);
        CHECK(four_wall_length > one_wall_length * 1.02);
        CHECK(max_layer_ratio > 1.1);
    }
}

TEST_CASE("Mixed auto dense-path stress remains responsive",
          "[.MixedPerformance][SupportMaterial][Mixed][Performance]")
{
    TriangleMesh fixture = make_cube(24., 60., 20.);
    TriangleMesh ceiling = make_cube(60., 60., 2.);
    ceiling.translate(-18.f, 0.f, 40.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("mixed_normal_support_generator",
        new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgPrusa));
    config.set_key_value("mixed_tree_support_style",
        new ConfigOptionEnum<MixedTreeSupportStyle>(mtssStrong));
    config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
    config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("support_base_pattern_spacing", new ConfigOptionFloat(0.8));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

    const auto started = std::chrono::steady_clock::now();
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    const double elapsed_seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - started).count();

    REQUIRE(print.objects().size() == 1);
    const auto support_layers = print.objects().front()->support_layers();
    REQUIRE_FALSE(support_layers.empty());
    CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
        return has_normal_channel(layer->support_type);
    }));
    CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
        return has_tree_channel(layer->support_type);
    }));
    CHECK(elapsed_seconds < 15.);
}

TEST_CASE("Mixed auto classifies Stanford Bunny vanilla islands independently",
          "[.MixedBunny][SupportMaterial][Mixed][Integration][Bunny]")
{
    const boost::filesystem::path repo_root =
        boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path();
    const boost::filesystem::path bunny_path =
        repo_root / "resources" / "handy_models" / "Stanford_Bunny.drc";
    TriangleMesh bunny;
    REQUIRE(load_drc(bunny_path.string().c_str(), &bunny));
    REQUIRE_FALSE(bunny.empty());

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("mixed_normal_support_generator",
        new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgCura));
    config.set_key_value("mixed_tree_support_style",
        new ConfigOptionEnum<MixedTreeSupportStyle>(mtssOrganic));
    config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(50.));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

    for (const bool selective_merge : { false, true }) {
        CAPTURE(selective_merge);
        config.set_key_value("mixed_selective_merge", new ConfigOptionBool(selective_merge));

        const auto started = std::chrono::steady_clock::now();
        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ bunny }, print, config));
        const double elapsed_seconds = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - started).count();

        REQUIRE(print.objects().size() == 1);
        const PrintObject *object = print.objects().front();
        const std::vector<Polygons> demand = detect_mixed_support_demand(*object);
        const std::vector<Polygons> shadow = buildplate_covered_by_object(*object);
        const MixedSupportPlan fifty = MixedSupportPlan::build_for_geometry(
            demand, shadow, scale_(0.4), 50., selective_merge);
        INFO("Stanford Bunny selective merge: " << selective_merge);
        INFO("Stanford Bunny Mixed processing seconds: " << elapsed_seconds);
        INFO("Stanford Bunny Mixed decision count: " << fifty.decisions().size());
        REQUIRE_FALSE(fifty.decisions().empty());
        CHECK(std::all_of(fifty.decisions().begin(), fifty.decisions().end(),
            [](const MixedSupportComponentDecision &decision) {
                return decision.polygon_ids.size() == 1;
            }));
        CHECK(fifty.has_normal_demand());
        CHECK(fifty.has_tree_demand());

        long double reachable_area = 0.;
        long double unassigned_demand_area = 0.;
        long double channel_overlap_area = 0.;
        long double reachable_missing_from_normal_area = 0.;
        long double reachable_assigned_to_tree_area = 0.;
        size_t reachable_layer_count = 0;
        for (size_t layer_id = 0; layer_id < demand.size(); ++layer_id) {
            const ExPolygons layer_demand = union_ex(demand[layer_id]);
            if (layer_demand.empty())
                continue;
            const Polygons layer_shadow = layer_id < shadow.size() ? shadow[layer_id] : Polygons{};
            const ExPolygons reachable = layer_shadow.empty() ? layer_demand :
                diff_ex(layer_demand, layer_shadow);
            const Polygons &normal = fifty.normal_mask()[layer_id];
            const Polygons &tree = fifty.tree_mask()[layer_id];
            Polygons assigned = normal;
            append(assigned, tree);
            assigned = union_(assigned);

            unassigned_demand_area += area(diff_ex(layer_demand, assigned));
            channel_overlap_area += area(intersection_ex(normal, tree));
            if (!reachable.empty()) {
                ++reachable_layer_count;
                reachable_area += area(reachable);
                const ExPolygons missing_from_normal = diff_ex(reachable, normal);
                const ExPolygons assigned_to_tree = intersection_ex(reachable, tree);
                reachable_missing_from_normal_area += area(missing_from_normal);
                reachable_assigned_to_tree_area += area(assigned_to_tree);
            }
        }
        const long double scaled_area_to_mm2 =
            static_cast<long double>(SCALING_FACTOR) * static_cast<long double>(SCALING_FACTOR);
        INFO("Stanford Bunny reachable layers: " << reachable_layer_count);
        INFO("Stanford Bunny bed-reachable demand mm2: " <<
            double(reachable_area * scaled_area_to_mm2));
        INFO("Stanford Bunny unassigned demand mm2: " <<
            double(unassigned_demand_area * scaled_area_to_mm2));
        INFO("Stanford Bunny normal/tree overlap mm2: " <<
            double(channel_overlap_area * scaled_area_to_mm2));
        INFO("Stanford Bunny bed-reachable area missing from normal mm2: " <<
            double(reachable_missing_from_normal_area * scaled_area_to_mm2));
        INFO("Stanford Bunny bed-reachable area assigned to tree mm2: " <<
            double(reachable_assigned_to_tree_area * scaled_area_to_mm2));
        // Clipper may leave sub-micron boundary dust when the same integer polygon
        // partition is unioned and differenced again. A 0.001 mm square is already
        // far below any printable 0.4 mm extrusion; enforce that total artifact
        // budget across the whole Bunny rather than requiring an unstable exact-empty test.
        constexpr long double geometry_artifact_tolerance_mm2 = 1e-5L;
        CHECK(unassigned_demand_area * scaled_area_to_mm2 <= geometry_artifact_tolerance_mm2);
        CHECK(channel_overlap_area * scaled_area_to_mm2 <= geometry_artifact_tolerance_mm2);
        if (selective_merge) {
            CHECK(reachable_missing_from_normal_area * scaled_area_to_mm2 <= geometry_artifact_tolerance_mm2);
            CHECK(reachable_assigned_to_tree_area * scaled_area_to_mm2 <= geometry_artifact_tolerance_mm2);
        }

        const auto support_layers = object->support_layers();
        REQUIRE_FALSE(support_layers.empty());
        CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
            return has_normal_channel(layer->support_type);
        }));
        CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
            return has_tree_channel(layer->support_type);
        }));

        const MixedSupportPlan zero = MixedSupportPlan::build_for_geometry(
            demand, shadow, scale_(0.4), 0., selective_merge);
        CHECK(zero.has_normal_demand());
        CHECK(zero.has_tree_demand() == selective_merge);
    }
}

TEST_CASE("Mixed auto worst-case Stanford Bunny planning stays bounded",
          "[.MixedBunnyWorstCase][SupportMaterial][Mixed][Performance][Bunny]")
{
    const boost::filesystem::path repo_root =
        boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path();
    const boost::filesystem::path bunny_path =
        repo_root / "resources" / "handy_models" / "Stanford_Bunny.drc";
    TriangleMesh bunny;
    REQUIRE(load_drc(bunny_path.string().c_str(), &bunny));

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("mixed_normal_support_generator",
        new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgCura));
    config.set_key_value("mixed_tree_support_style",
        new ConfigOptionEnum<MixedTreeSupportStyle>(mtssOrganic));
    config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(90.));
    config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.16));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("cura_solid_support_raft", new ConfigOptionBool(true));
    config.set_key_value("independent_support_layer_height", new ConfigOptionBool(true));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(2));
    config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(smpDefault));
    config.set_key_value("support_base_pattern_spacing", new ConfigOptionFloat(1.));
    config.set_key_value("support_interface_pattern", new ConfigOptionEnum<SupportMaterialInterfacePattern>(smipTriangles));
    config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.5));

    const auto total_started = std::chrono::steady_clock::now();
    Print print;
    Model model;
    init_print({ bunny }, print, model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *object = const_cast<PrintObject *>(print.objects().front());
    const auto slice_started = std::chrono::steady_clock::now();
    object->slice();

    const auto demand_started = std::chrono::steady_clock::now();
    const std::vector<Polygons> demand = detect_mixed_support_demand(*object);
    const auto shadow_started = std::chrono::steady_clock::now();
    const std::vector<Polygons> shadow = buildplate_covered_by_object(*object);
    const auto plan_started = std::chrono::steady_clock::now();
    const MixedSupportPlan plan = MixedSupportPlan::build_for_geometry(
        demand, shadow, scale_(0.4), 90., true);
    const auto finished = std::chrono::steady_clock::now();

    const auto seconds = [](auto begin, auto end) {
        return std::chrono::duration<double>(end - begin).count();
    };
    INFO("demand seconds: " << seconds(demand_started, shadow_started));
    INFO("shadow seconds: " << seconds(shadow_started, plan_started));
    INFO("plan seconds: " << seconds(plan_started, finished));
    INFO("layers: " << object->layer_count());
    INFO("decisions: " << plan.decisions().size());
    INFO("initialization seconds: " << seconds(total_started, slice_started));
    INFO("object slicing seconds: " << seconds(slice_started, demand_started));
    CHECK(plan.has_normal_demand());
    CHECK(plan.has_tree_demand());

    const auto process_started = std::chrono::steady_clock::now();
    REQUIRE_NOTHROW(print.process());
    const auto total_finished = std::chrono::steady_clock::now();
    const double demand_seconds = seconds(demand_started, shadow_started);
    const double shadow_seconds = seconds(shadow_started, plan_started);
    const double plan_seconds = seconds(plan_started, finished);
    const double process_seconds = seconds(process_started, total_finished);
    const double total_seconds = seconds(total_started, total_finished);
    INFO("full processing seconds: " << process_seconds);
    INFO("total setup, planning and processing seconds: " << total_seconds);
    REQUIRE_FALSE(object->support_layers().empty());
    // Release-test budgets in wall-clock seconds. process() performs demand
    // detection again as part of support generation, while the total budget also
    // includes the explicitly measured demand, shadow and plan construction above.
    constexpr double maximum_demand_seconds = 45.;
    constexpr double maximum_shadow_seconds = 5.;
    constexpr double maximum_plan_seconds = 5.;
    constexpr double maximum_process_seconds = 90.;
    constexpr double maximum_total_seconds = 120.;
    CHECK(demand_seconds < maximum_demand_seconds);
    CHECK(shadow_seconds < maximum_shadow_seconds);
    CHECK(plan_seconds < maximum_plan_seconds);
    CHECK(process_seconds < maximum_process_seconds);
    CHECK(total_seconds < maximum_total_seconds);
}

TEST_CASE("Mixed selective merge emits a single merged raft", "[SupportMaterial][Mixed][Integration][Raft]")
{
    struct GeneratorCombination {
        MixedNormalSupportGenerator normal;
        MixedTreeSupportStyle tree;
    };
    const GeneratorCombination combinations[] = {
        { mnsgPrusa, mtssOrganic },
        { mnsgCura, mtssStrong }
    };

    for (const GeneratorCombination combination : combinations) {
        CAPTURE(int(combination.normal), int(combination.tree));
        TriangleMesh fixture = make_cube(8., 10., 10.);
        TriangleMesh ceiling = make_cube(20., 10., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        config.set_key_value("mixed_normal_support_generator",
            new ConfigOptionEnum<MixedNormalSupportGenerator>(combination.normal));
        config.set_key_value("mixed_tree_support_style",
            new ConfigOptionEnum<MixedTreeSupportStyle>(combination.tree));
        config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
        config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
        config.set_key_value("raft_layers", new ConfigOptionInt(2));
        config.set_key_value("raft_expansion", new ConfigOptionFloat(2.));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.4));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
        REQUIRE(print.objects().size() == 1);
        const PrintObject *object = print.objects().front();
        const auto support_layers = object->support_layers();
        REQUIRE_FALSE(support_layers.empty());

        size_t raft_layer_count = 0;
        for (size_t layer_id = 0; layer_id < support_layers.size(); ++layer_id) {
            const SupportLayer *layer = support_layers[layer_id];
            if (layer_id > 0)
                CHECK(layer->print_z > support_layers[layer_id - 1]->print_z + EPSILON);
            if (layer->print_z <= object->slicing_parameters().raft_contact_top_z + EPSILON) {
                ++raft_layer_count;
                CHECK(layer->has_extrusions());
            }
        }
        CHECK(raft_layer_count >= object->slicing_parameters().raft_layers());

        const std::string output = gcode(print);
        CHECK_FALSE(output.empty());
        CHECK(output.find("support_type = mixed(auto)") != std::string::npos);
    }
}

TEST_CASE("Mixed selective merge supports common nozzle diameters", "[SupportMaterial][Mixed][Integration][Nozzle]")
{
    for (const double nozzle_diameter_mm : { 0.2, 0.4, 0.6, 0.8 }) {
        CAPTURE(nozzle_diameter_mm);
        TriangleMesh fixture = make_cube(16., 20., 4.);
        TriangleMesh ceiling = make_cube(40., 20., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        const double layer_height_mm = std::min(0.2, 0.5 * nozzle_diameter_mm);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        config.set_key_value("mixed_normal_support_generator",
            new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgPrusa));
        config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
        config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ nozzle_diameter_mm }));
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height_mm));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height_mm));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

        for (const MixedTreeSupportStyle tree_style : {
                mtssOrganic, mtssSlim, mtssStrong, mtssTreeHybrid }) {
            CAPTURE(int(tree_style));
            config.set_key_value("mixed_tree_support_style",
                new ConfigOptionEnum<MixedTreeSupportStyle>(tree_style));

            Print print;
            std::string slicing_error;
            try {
                init_and_process_print({ fixture }, print, config);
            } catch (const std::exception &error) {
                slicing_error = error.what();
            }
            CHECK(slicing_error.empty());
            if (!slicing_error.empty())
                continue;

            REQUIRE(print.objects().size() == 1);
            const auto support_layers = print.objects().front()->support_layers();
            REQUIRE_FALSE(support_layers.empty());
            CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
                return has_normal_channel(layer->support_type);
            }));
            CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
                return has_tree_channel(layer->support_type);
            }));

            const std::string output = gcode(print);
            CHECK_FALSE(output.empty());
            CHECK(output.find("support_type = mixed(auto)") != std::string::npos);
        }
    }
}

TEST_CASE("Resin style exposes current support controls without penetration",
          "[SupportMaterial][Resin][Config]")
{
    const ConfigOptionDef *support_type_def = print_config_def.get("support_type");
    REQUIRE(support_type_def != nullptr);
    REQUIRE(support_type_def->enum_keys_map != nullptr);
    REQUIRE(support_type_def->enum_values.size() == support_type_def->enum_labels.size());
    REQUIRE(support_type_def->enum_values.size() == 8);
    CHECK(support_type_def->enum_values[6] == "mixed(auto)");
    CHECK(support_type_def->enum_values[7] == "resin(auto)");
    CHECK(support_type_def->enum_keys_map->at(support_type_def->enum_values[6]) == stMixedAuto);
    CHECK(support_type_def->enum_keys_map->at(support_type_def->enum_values[7]) == stResinAuto);
    // Tsunami's retired numeric slot makes the final two enum values differ
    // from their combo-box indices. The GUI must resolve these through the
    // enum key map instead of storing selection indices directly.
    CHECK(stMixedAuto != 6);
    CHECK(stResinAuto != 7);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "support_type", "resin(auto)" },
        { "resin_support_tree_type", "branching" },
        { "resin_support_points_density_relative", "125" },
        { "resin_support_pillar_connection_mode", "cross" },
        { "resin_support_object_elevation", "7.5" },
        { "resin_branching_support_pillar_connection_mode", "zigzag" },
        { "resin_branching_support_object_elevation", "6.5" }
    });
    const std::array<const char *, 35> keys {
        "resin_support_tree_type",
        "resin_support_points_density_relative",
        "resin_support_enforcers_only",
        "resin_support_head_front_diameter",
        "resin_support_head_width",
        "resin_support_pillar_diameter",
        "resin_support_small_pillar_diameter_percent",
        "resin_support_max_bridges_on_pillar",
        "resin_support_max_weight_on_model",
        "resin_support_pillar_connection_mode",
        "resin_support_buildplate_only",
        "resin_support_pillar_widening_factor",
        "resin_support_base_diameter",
        "resin_support_base_height",
        "resin_support_base_safety_distance",
        "resin_support_critical_angle",
        "resin_support_max_bridge_length",
        "resin_support_max_pillar_link_distance",
        "resin_support_object_elevation",
        "resin_branching_support_head_front_diameter",
        "resin_branching_support_head_width",
        "resin_branching_support_pillar_diameter",
        "resin_branching_support_small_pillar_diameter_percent",
        "resin_branching_support_max_bridges_on_pillar",
        "resin_branching_support_max_weight_on_model",
        "resin_branching_support_pillar_connection_mode",
        "resin_branching_support_buildplate_only",
        "resin_branching_support_pillar_widening_factor",
        "resin_branching_support_base_diameter",
        "resin_branching_support_base_height",
        "resin_branching_support_base_safety_distance",
        "resin_branching_support_critical_angle",
        "resin_branching_support_max_bridge_length",
        "resin_branching_support_max_pillar_link_distance",
        "resin_branching_support_object_elevation"
    };
    for (const char *key : keys) {
        CAPTURE(key);
        CHECK(config.option(key) != nullptr);
    }
    CHECK(config.option("resin_support_head_penetration") == nullptr);
    CHECK(config.option("resin_branching_support_head_penetration") == nullptr);

    CHECK(config.opt_enum<SupportType>("support_type") == stResinAuto);
    CHECK(config.opt_enum<ResinSupportTreeType>("resin_support_tree_type") == rstBranching);
    CHECK(config.opt_int("resin_support_points_density_relative") == 125);
    CHECK(config.opt_enum<SLAPillarConnectionMode>("resin_support_pillar_connection_mode") == slapcmCross);
    CHECK(config.opt_float("resin_support_object_elevation") == Catch::Approx(7.5));
    CHECK(config.opt_enum<SLAPillarConnectionMode>("resin_branching_support_pillar_connection_mode") == slapcmZigZag);
    CHECK(config.opt_float("resin_branching_support_object_elevation") == Catch::Approx(6.5));
    CHECK(config.option<ConfigOptionEnum<SupportType>>("support_type")->serialize() == "resin(auto)");
    CHECK(is_auto(stResinAuto));
    CHECK(is_resin(stResinAuto));
    CHECK(uses_normal_channel(stResinAuto));
    CHECK_FALSE(uses_tree_channel(stResinAuto));

    DynamicPrintConfig restored = DynamicPrintConfig::full_print_config();
    restored.set_deserialize_strict("support_type", config.opt_serialize("support_type"));
    for (const char *key : keys)
        restored.set_deserialize_strict(key, config.opt_serialize(key));
    CHECK(restored.opt_enum<SupportType>("support_type") == stResinAuto);
    CHECK(restored.opt_enum<ResinSupportTreeType>("resin_support_tree_type") == rstBranching);
    CHECK(restored.opt_float("resin_support_object_elevation") == Catch::Approx(7.5));
    CHECK(restored.opt_float("resin_branching_support_object_elevation") == Catch::Approx(6.5));
}

TEST_CASE("Legacy process presets materialize every Resin editor option from schema",
          "[SupportMaterial][Resin][Config][Legacy]")
{
    const std::array<const char *, 35> keys {
        "resin_support_tree_type", "resin_support_points_density_relative",
        "resin_support_enforcers_only", "resin_support_head_front_diameter",
        "resin_support_head_width", "resin_support_pillar_diameter",
        "resin_support_small_pillar_diameter_percent", "resin_support_max_bridges_on_pillar",
        "resin_support_max_weight_on_model", "resin_support_pillar_connection_mode",
        "resin_support_buildplate_only", "resin_support_pillar_widening_factor",
        "resin_support_base_diameter", "resin_support_base_height",
        "resin_support_base_safety_distance", "resin_support_critical_angle",
        "resin_support_max_bridge_length", "resin_support_max_pillar_link_distance",
        "resin_support_object_elevation", "resin_branching_support_head_front_diameter",
        "resin_branching_support_head_width", "resin_branching_support_pillar_diameter",
        "resin_branching_support_small_pillar_diameter_percent",
        "resin_branching_support_max_bridges_on_pillar",
        "resin_branching_support_max_weight_on_model",
        "resin_branching_support_pillar_connection_mode",
        "resin_branching_support_buildplate_only",
        "resin_branching_support_pillar_widening_factor",
        "resin_branching_support_base_diameter", "resin_branching_support_base_height",
        "resin_branching_support_base_safety_distance", "resin_branching_support_critical_angle",
        "resin_branching_support_max_bridge_length",
        "resin_branching_support_max_pillar_link_distance",
        "resin_branching_support_object_elevation"
    };

    DynamicPrintConfig legacy;
    for (const char *key : keys) {
        CAPTURE(key);
        REQUIRE_FALSE(legacy.has(key));
        const ConfigOptionDef *definition = legacy.def()->get(key);
        REQUIRE(definition != nullptr);
        ConfigOption *created = legacy.option_throw(key, true);
        REQUIRE(created != nullptr);
        CHECK(created->type() == definition->type);
        CHECK(created->serialize() == definition->default_value->serialize());
    }
}

TEST_CASE("Resin style elevation remains active with support disabled",
          "[SupportMaterial][Resin][Elevation]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
    config.set_key_value("resin_support_tree_type",
        new ConfigOptionEnum<ResinSupportTreeType>(rstDefault));
    config.set_key_value("resin_support_object_elevation", new ConfigOptionFloat(5.));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ make_cube(20., 20., 10.) }, print, config));
    REQUIRE(print.objects().size() == 1);
    const PrintObject *object = print.objects().front();
    CHECK(object->slicing_parameters().object_print_z_min == Catch::Approx(5.));
    CHECK(object->support_layers().empty());
}

TEST_CASE("Support threshold uses one inclusive angle conversion",
          "[SupportMaterial][Threshold]")
{
    CHECK(support_overhang_offset_from_threshold(0.2, 0.) == 0);
    CHECK(support_overhang_offset_from_threshold(0., 45.) == 0);
    CHECK(unscale_(support_overhang_offset_from_threshold(0.2, 44.)) ==
          Catch::Approx(0.2).margin(1e-5));
    CHECK(support_overhang_offset_from_threshold(0.2, 80.) <
          support_overhang_offset_from_threshold(0.2, 30.));
}

TEST_CASE("Cura support demand uses Orca native overhang detection exactly",
          "[SupportMaterial][Cura][Threshold][Integration]")
{
    for (const int threshold_angle : { 30, 60, 80 }) {
        CAPTURE(threshold_angle);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalCuraAuto));
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(threshold_angle));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ resin_test_sloped_wall() }, print, config));
        REQUIRE(print.objects().size() == 1);
        const PrintObject &object = *print.objects().front();

        CuraStyleSupportGenerator cura_detector(&object, object.slicing_parameters());
        const std::vector<Polygons> cura_demand = cura_detector.detect_support_demand();

        SupportGeneratorLayerStorage storage;
        PrintObjectSupportMaterial orca_detector(&object, object.slicing_parameters());
        const SupportGeneratorLayersPtr contacts =
            orca_detector.detect_top_contact_layers(storage, true);
        std::vector<Polygons> orca_demand(object.layer_count());
        for (const SupportGeneratorLayer *contact : contacts) {
            REQUIRE(contact != nullptr);
            REQUIRE(contact->idx_object_layer_above < orca_demand.size());
            if (contact->overhang_polygons && !contact->overhang_polygons->empty())
                append(orca_demand[contact->idx_object_layer_above], *contact->overhang_polygons);
            else
                append(orca_demand[contact->idx_object_layer_above], contact->polygons);
        }
        for (Polygons &layer : orca_demand)
            if (!layer.empty())
                layer = union_(layer);

        REQUIRE(cura_demand.size() == orca_demand.size());
        for (size_t layer_idx = 0; layer_idx < cura_demand.size(); ++layer_idx) {
            CAPTURE(layer_idx);
            CHECK(same_polygon_set(cura_demand[layer_idx], orca_demand[layer_idx]));
        }
    }
}

TEST_CASE("Cura automatic demand preserves painted enforcers as a separate channel",
          "[SupportMaterial][Cura][Paint][Integration][CuraPaint]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalCuraAuto));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

    Model model;
    ModelObject *model_object = model.add_object();
    TriangleMesh bed_anchor = make_cube(1., 1., 1.);
    bed_anchor.translate(0.f, 30.f, 0.f);
    model_object->add_volume(std::move(bed_anchor));

    TriangleMesh automatic_ceiling = make_cube(10., 10., 2.);
    automatic_ceiling.translate(0.f, 0.f, 10.f);
    model_object->add_volume(std::move(automatic_ceiling));

    TriangleMesh painted_ceiling = make_cube(10., 10., 2.);
    painted_ceiling.translate(20.f, 0.f, 10.f);
    ModelVolume *painted_volume = model_object->add_volume(std::move(painted_ceiling));
    TriangleSelector selector(painted_volume->mesh());
    for (size_t facet_idx = 0; facet_idx < painted_volume->mesh().facets_count(); ++facet_idx)
        selector.set_facet(int(facet_idx), EnforcerBlockerType::ENFORCER);
    REQUIRE(painted_volume->supported_facets.set(selector));

    model_object->add_instance();
    model_object->ensure_on_bed();
    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.objects().size() == 1);

    const PrintObject &object = *print.objects().front();
    CuraStyleSupportGenerator detector(&object, object.slicing_parameters());
    const CuraSupportDemand demand = detector.analyze_support_demand();
    const auto channel_has_support = [](const std::vector<Polygons> &channel) {
        return std::any_of(channel.begin(), channel.end(), [](const Polygons &polygons) {
            return !polygons.empty();
        });
    };

    CHECK(channel_has_support(demand.automatic));
    CHECK(channel_has_support(demand.enforced));
}

TEST_CASE("Mixed Prusa demand preserves painted support enforcers",
          "[SupportMaterial][Mixed][Prusa][Paint][Integration][MixedPrusaPaint]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
    config.set_key_value("mixed_normal_support_generator",
        new ConfigOptionEnum<MixedNormalSupportGenerator>(mnsgPrusa));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

    Model model;
    ModelObject *model_object = model.add_object();
    TriangleMesh bed_anchor = make_cube(1., 1., 1.);
    bed_anchor.translate(0.f, 30.f, 0.f);
    model_object->add_volume(std::move(bed_anchor));

    TriangleMesh automatic_ceiling = make_cube(10., 10., 2.);
    automatic_ceiling.translate(0.f, 0.f, 10.f);
    model_object->add_volume(std::move(automatic_ceiling));

    TriangleMesh painted_ceiling = make_cube(10., 10., 2.);
    painted_ceiling.translate(20.f, 0.f, 10.f);
    ModelVolume *painted_volume = model_object->add_volume(std::move(painted_ceiling));
    TriangleSelector selector(painted_volume->mesh());
    for (size_t facet_idx = 0; facet_idx < painted_volume->mesh().facets_count(); ++facet_idx)
        selector.set_facet(int(facet_idx), EnforcerBlockerType::ENFORCER);
    REQUIRE(painted_volume->supported_facets.set(selector));

    model_object->add_instance();
    model_object->ensure_on_bed();
    Print print;
    print.auto_assign_extruders(model_object);
    print.apply(model, config);
    print.set_status_silent();
    REQUIRE_NOTHROW(print.process());
    REQUIRE(print.objects().size() == 1);

    const PrintObject &object = *print.objects().front();
    const std::vector<Polygons> mixed_demand = detect_mixed_support_demand(object);

    SupportGeneratorLayerStorage storage;
    PrintObjectSupportMaterial native_detector(&object, object.slicing_parameters());
    const SupportGeneratorLayersPtr contacts = native_detector.detect_top_contact_layers(storage, false);
    bool saw_painted_enforcer = false;
    for (const SupportGeneratorLayer *contact : contacts) {
        REQUIRE(contact != nullptr);
        REQUIRE(contact->idx_object_layer_above < mixed_demand.size());
        if (contact->enforcer_polygons && !contact->enforcer_polygons->empty()) {
            saw_painted_enforcer = true;
            CHECK(diff_ex(*contact->enforcer_polygons,
                          mixed_demand[contact->idx_object_layer_above]).empty());
        }
    }
    CHECK(saw_painted_enforcer);
}

TEST_CASE("Resin style automatic points obey the shared overhang threshold",
          "[SupportMaterial][Resin][Integration][Threshold]")
{
    const auto slice_with_threshold = [](int threshold_angle) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(threshold_angle));
        config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(100));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

        Print print;
        init_and_process_print({ resin_test_sloped_wall() }, print, config);
        return print.objects().front()->support_layers().size();
    };

    const size_t permissive_support_layers = slice_with_threshold(30);
    const size_t strict_support_layers = slice_with_threshold(80);
    CHECK(strict_support_layers > permissive_support_layers);
}

TEST_CASE("Resin style Default and Branching generate printable FFF support",
          "[SupportMaterial][Resin][Integration][Nozzle]")
{
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 18.);
        TriangleMesh ceiling = make_cube(28., 20., 2.);
        ceiling.translate(-10.f, -6.f, 18.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const ResinSupportTreeType strategy : { rstDefault, rstBranching }) {
      for (const double nozzle : { 0.2, 0.4, 0.6, 0.8 }) {
        CAPTURE(int(strategy), nozzle);
        const double layer_height = std::min(0.2, 0.5 * nozzle);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
        config.set_key_value("resin_support_tree_type",
            new ConfigOptionEnum<ResinSupportTreeType>(strategy));
        config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(50));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ nozzle }));
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
        REQUIRE(print.objects().size() == 1);
        const PrintObject *object = print.objects().front();
        CHECK(object->config().enable_support.value);
        CHECK(object->config().support_type.value == stResinAuto);
        CHECK(object->is_step_done(posSupportMaterial));
        const auto support_layers = object->support_layers();
        REQUIRE_FALSE(support_layers.empty());
        CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
            return layer->print_z < 15. && layer->has_extrusions();
        }));

        const std::string output = gcode(print);
        CHECK_FALSE(output.empty());
        CHECK(output.find("support_type = resin(auto)") != std::string::npos);
        CHECK(output.find("support material interface") != std::string::npos);
      }
    }
}

TEST_CASE("Resin style zero elevation omits bed-face points but keeps overhang support",
          "[SupportMaterial][Resin][Integration][Elevation]")
{
    TriangleMesh fixture = make_cube(8., 8., 8.);
    TriangleMesh ceiling = make_cube(28., 20., 2.);
    ceiling.translate(-10.f, -6.f, 18.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
    config.set_key_value("resin_support_tree_type",
        new ConfigOptionEnum<ResinSupportTreeType>(rstDefault));
    config.set_key_value("resin_support_object_elevation", new ConfigOptionFloat(0.));
    config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(50));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    REQUIRE(print.objects().size() == 1);
    const PrintObject *object = print.objects().front();
    CHECK(object->slicing_parameters().object_print_z_min == Catch::Approx(0.));
    CHECK_FALSE(object->support_layers().empty());
}

TEST_CASE("Resin style accepts the full configured pillar diameter range",
          "[SupportMaterial][Resin][Integration][PillarDiameter]")
{
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 12.);
        TriangleMesh ceiling = make_cube(24., 16., 2.);
        ceiling.translate(-8.f, -4.f, 12.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const ResinSupportTreeType strategy : { rstDefault, rstBranching }) {
        const char *pillar_key = strategy == rstBranching ?
            "resin_branching_support_pillar_diameter" : "resin_support_pillar_diameter";
        for (const double pillar_diameter_mm : { 0., 0.2, 0.4, 1., 2., 5., 10., 15. }) {
            CAPTURE(int(strategy), pillar_key, pillar_diameter_mm);
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
            config.set_key_value("resin_support_tree_type",
                new ConfigOptionEnum<ResinSupportTreeType>(strategy));
            config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(25));
            config.set_key_value(pillar_key, new ConfigOptionFloat(pillar_diameter_mm));
            config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

            Print print;
            REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
            REQUIRE(print.objects().size() == 1);
            CHECK(print.objects().front()->is_step_done(posSupportMaterial));
        }
    }
}

TEST_CASE("Resin style processes variable layer heights with both tree strategies",
          "[SupportMaterial][Resin][Integration][VariableLayer]")
{
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 12.);
        TriangleMesh ceiling = make_cube(24., 16., 2.);
        ceiling.translate(-8.f, -4.f, 12.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const ResinSupportTreeType strategy : { rstDefault, rstBranching }) {
        CAPTURE(int(strategy));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
        config.set_key_value("resin_support_tree_type",
            new ConfigOptionEnum<ResinSupportTreeType>(strategy));
        config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(50));
        config.set_key_value("resin_support_object_elevation", new ConfigOptionFloat(0.));
        config.set_key_value("resin_branching_support_object_elevation", new ConfigOptionFloat(0.));
        config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

        Model model;
        ModelObject *model_object = model.add_object();
        model_object->add_volume(make_fixture());
        model_object->add_instance();
        model_object->ensure_on_bed();
        model_object->layer_height_profile.set({
            0., 0.20,
            4., 0.12,
            8., 0.28,
            14., 0.16
        });

        Print print;
        print.auto_assign_extruders(model_object);
        print.apply(model, config);
        print.set_status_silent();
        REQUIRE_NOTHROW(print.process());
        REQUIRE(print.objects().size() == 1);
        const PrintObject *object = print.objects().front();
        REQUIRE_FALSE(object->layers().empty());
        std::set<int> layer_heights_um;
        for (const Layer *layer : object->layers())
            layer_heights_um.insert(int(std::lround(1000. * layer->height)));
        CHECK(layer_heights_um.size() > 1);
        CHECK(object->is_step_done(posSupportMaterial));
        CHECK_FALSE(object->support_layers().empty());
    }
}

TEST_CASE("Every exposed Resin strategy setting survives a slicing mutation",
          "[SupportMaterial][Resin][Integration][ResinSettingSweep]")
{
    const char *sweep_filter_env = std::getenv("MAGPIE_RESIN_SWEEP_FILTER");
    if (sweep_filter_env == nullptr || *sweep_filter_env == '\0')
        SKIP("Run through verify_resin_setting_sweep.ps1 so each mutation has an isolated timeout");
    const std::string sweep_filter(sweep_filter_env);
    size_t executed_mutations = 0;
    const std::array<std::pair<const char *, const char *>, 16> mutations {{
        { "head_front_diameter", "2" },
        { "head_width", "5" },
        { "pillar_diameter", "15" },
        { "small_pillar_diameter_percent", "1" },
        { "max_bridges_on_pillar", "50" },
        { "max_weight_on_model", "0" },
        { "pillar_connection_mode", "cross" },
        { "buildplate_only", "1" },
        { "pillar_widening_factor", "1" },
        { "base_diameter", "30" },
        { "base_height", "5" },
        { "base_safety_distance", "10" },
        { "critical_angle", "90" },
        { "max_bridge_length", "0" },
        { "max_pillar_link_distance", "0" },
        { "object_elevation", "15" }
    }};
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 12.);
        TriangleMesh ceiling = make_cube(24., 16., 2.);
        ceiling.translate(-8.f, -4.f, 12.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const ResinSupportTreeType strategy : { rstDefault, rstBranching }) {
        const std::string strategy_name = strategy == rstBranching ? "branching" : "default";
        const std::string prefix = strategy == rstBranching ?
            "resin_branching_support_" : "resin_support_";
        for (const auto &[suffix, value] : mutations) {
            const std::string key = prefix + suffix;
            if (sweep_filter != "all" && sweep_filter != strategy_name + ":" + key)
                continue;
            ++executed_mutations;
            CAPTURE(int(strategy), key, value);
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
            config.set_key_value("resin_support_tree_type",
                new ConfigOptionEnum<ResinSupportTreeType>(strategy));
            config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(25));
            config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));
            config.set_deserialize_strict(key, value);
            const std::string normalized_value = config.opt_serialize(key);

            Print print;
            REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
            REQUIRE(print.objects().size() == 1);
            CHECK(print.objects().front()->is_step_done(posSupportMaterial));
            CHECK(print.objects().front()->config().opt_serialize(key) == normalized_value);
        }
    }

    for (const auto &[key, value] : {
             std::pair<const char *, const char *>{ "resin_support_points_density_relative", "250" },
             std::pair<const char *, const char *>{ "resin_support_enforcers_only", "1" } }) {
        if (sweep_filter != "all" && sweep_filter != std::string("common:") + key)
            continue;
        ++executed_mutations;
        CAPTURE(key, value);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));
        config.set_deserialize_strict(key, value);
        const std::string normalized_value = config.opt_serialize(key);

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
        REQUIRE(print.objects().size() == 1);
        CHECK(print.objects().front()->is_step_done(posSupportMaterial));
        CHECK(print.objects().front()->config().opt_serialize(key) == normalized_value);
    }
    CHECK(executed_mutations > 0);
}

TEST_CASE("Resin style preserves dedicated body and interface filaments",
          "[SupportMaterial][Resin][Integration][MultiMaterial]")
{
    TriangleMesh fixture = make_cube(8., 8., 18.);
    TriangleMesh ceiling = make_cube(28., 20., 2.);
    ceiling.translate(-10.f, -6.f, 18.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stResinAuto));
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(3));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
    config.set_key_value("resin_support_points_density_relative", new ConfigOptionInt(50));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4, 0.4 }));
    config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1, 2, 3 }));
    config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({
        "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75, 1.75 }));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA", "PLA" }));
    config.set_key_value("filament_colour",
        new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));
    config.set_key_value("default_filament_colour",
        new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));
    config.set_key_value("flush_multiplier", new ConfigOptionFloats({ 1. }));
    config.set_key_value("flush_volumes_matrix",
        new ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0., 0. }));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 3 }));
    config.set_key_value("gcode_comments", new ConfigOptionBool(true));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    CHECK(print.support_material_extruders() == std::vector<unsigned int>{ 1, 2 });
    const std::string output = gcode(print);
    CHECK(output.find("support_type = resin(auto)") != std::string::npos);
    CHECK(output.find("\nT1") != std::string::npos);
    CHECK(output.find("\nT2 ; change extruder\n") != std::string::npos);
    CHECK(output.find("support material interface") != std::string::npos);
}

TEST_CASE("Tree support geometry width follows the body hotend instead of the interface hotend",
          "[SupportMaterial][Tree][MultiNozzle][TreeWidthAudit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(3));
    config.set_key_value("support_line_width", new ConfigOptionFloatOrPercent(105., true));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.6, 0.4 }));
    config.set_key_value("toolhead_support_line_width", new ConfigOptionFloatsOrPercents({
        FloatOrPercent(0.42, false), FloatOrPercent(0.63, false), FloatOrPercent(0.42, false) }));
    config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75, 1.75 }));
    config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA", "PLA" }));
    config.set_key_value("filament_colour",
        new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));
    config.set_key_value("default_filament_colour",
        new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));

    Print print;
    Model model;
    REQUIRE_NOTHROW(init_print({ make_cube(20., 20., 20.) }, print, model, config));
    REQUIRE(print.objects().size() == 1);

    const SupportParameters parameters(*print.objects().front());
    CHECK(parameters.support_material_flow.nozzle_diameter() == Catch::Approx(0.6));
    CHECK(parameters.support_material_flow.width() == Catch::Approx(0.63));
    CHECK(parameters.support_extrusion_width == Catch::Approx(parameters.support_material_flow.width()));
}

TEST_CASE("Tree support internal geometry resolves automatic support width",
          "[SupportMaterial][Tree][Flow][TreeWidthAudit]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stTreeAuto));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
    config.set_key_value("line_width", new ConfigOptionFloatOrPercent(0., false));
    config.set_key_value("support_line_width", new ConfigOptionFloatOrPercent(0., false));
    config.set_key_value("toolhead_line_width",
        new ConfigOptionFloatsOrPercents({ FloatOrPercent(0., false) }));
    config.set_key_value("toolhead_support_line_width",
        new ConfigOptionFloatsOrPercents({ FloatOrPercent(0., false) }));

    Print print;
    Model model;
    REQUIRE_NOTHROW(init_print({ make_cube(20., 20., 20.) }, print, model, config));
    REQUIRE(print.objects().size() == 1);

    const SupportParameters parameters(*print.objects().front());
    CHECK(parameters.support_material_flow.nozzle_diameter() == Catch::Approx(0.4));
    CHECK(parameters.support_material_flow.width() == Catch::Approx(0.4));
    CHECK(parameters.support_extrusion_width == Catch::Approx(parameters.support_material_flow.width()));
}

TEST_CASE("Mixed support preserves dedicated body and interface filaments",
          "[SupportMaterial][Mixed][Integration][MultiMaterial]")
{
    struct GeneratorCombination {
        MixedNormalSupportGenerator normal;
        MixedTreeSupportStyle tree;
    };
    const GeneratorCombination combinations[] = {
        { mnsgPrusa, mtssOrganic },
        { mnsgCura, mtssStrong }
    };

    for (const GeneratorCombination combination : combinations) {
        CAPTURE(int(combination.normal), int(combination.tree));
        TriangleMesh fixture = make_cube(16., 20., 4.);
        TriangleMesh ceiling = make_cube(40., 20., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_num_extruders(3);
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stMixedAuto));
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        config.set_key_value("mixed_normal_support_generator",
            new ConfigOptionEnum<MixedNormalSupportGenerator>(combination.normal));
        config.set_key_value("mixed_tree_support_style",
            new ConfigOptionEnum<MixedTreeSupportStyle>(combination.tree));
        config.set_key_value("mixed_normal_coverage_threshold", new ConfigOptionPercent(80.));
        config.set_key_value("mixed_selective_merge", new ConfigOptionBool(true));
        config.set_key_value("support_filament", new ConfigOptionInt(2));
        config.set_key_value("support_interface_filament", new ConfigOptionInt(3));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4, 0.4 }));
        config.set_key_value("printer_extruder_id", new ConfigOptionInts({ 1, 2, 3 }));
        config.set_key_value("printer_extruder_variant", new ConfigOptionStrings({
            "Direct Drive Standard", "Direct Drive Standard", "Direct Drive Standard" }));
        config.set_key_value("filament_diameter", new ConfigOptionFloats({ 1.75, 1.75, 1.75 }));
        config.set_key_value("filament_type", new ConfigOptionStrings({ "PLA", "PLA", "PLA" }));
        config.set_key_value("filament_colour",
            new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));
        config.set_key_value("default_filament_colour",
            new ConfigOptionStrings({ "#808080", "#00AAFF", "#FFAA00" }));
        config.set_key_value("flush_multiplier", new ConfigOptionFloats({ 1. }));
        config.set_key_value("flush_volumes_matrix",
            new ConfigOptionFloats({ 0., 0., 0., 0., 0., 0., 0., 0., 0. }));
        config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
        config.set_key_value("filament_map", new ConfigOptionInts({ 1, 2, 3 }));
        config.set_key_value("gcode_comments", new ConfigOptionBool(true));

        Print print;
        REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
        CHECK(print.support_material_extruders() == std::vector<unsigned int>{ 1, 2 });

        const std::string output = gcode(print);
        CHECK(output.find("support_type = mixed(auto)") != std::string::npos);
        CHECK(output.find("\nT1 ; change extruder\n") != std::string::npos);
        CHECK(output.find("\nT2 ; change extruder\n") != std::string::npos);
        CHECK(output.find("support material interface") != std::string::npos);
    }
}

TEST_CASE("Cura hollow support emits walls without sparse base fill", "[SupportMaterial][CuraStyle][Hollow]")
{
    const auto support_length = [](SupportMaterialPattern pattern, bool solid_raft) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", true },
            { "support_threshold_angle", 90.0 },
            { "support_interface_top_layers", 0 },
            { "support_interface_bottom_layers", 0 },
            { "cura_solid_support_raft", solid_raft }
        });
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalCuraAuto));
        config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(pattern));

        Print print;
        init_and_process_print({ TestMesh::overhang }, print, config);
        const std::string output = gcode(print);

        const auto axis_value = [](const std::string &line, char axis, double fallback) {
            const std::string marker { ' ', axis };
            const size_t position = line.find(marker);
            return position == std::string::npos ? fallback : std::strtod(line.c_str() + position + 2, nullptr);
        };

        bool in_support = false;
        double x = 0.;
        double y = 0.;
        double length = 0.;
        std::istringstream lines(output);
        for (std::string line; std::getline(lines, line);) {
            if (line.rfind(";TYPE:", 0) == 0) {
                in_support = line == ";TYPE:Support";
            } else if (line.rfind("G1 ", 0) == 0) {
                const double next_x = axis_value(line, 'X', x);
                const double next_y = axis_value(line, 'Y', y);
                const double extrusion = axis_value(line, 'E', 0.);
                if (in_support && extrusion > 0.)
                    length += std::hypot(next_x - x, next_y - y);
                x = next_x;
                y = next_y;
            }
        }
        return length;
    };

    const double rectilinear_length = support_length(smpRectilinear, false);
    const double hollow_length = support_length(smpNone, false);
    const double solid_raft_length = support_length(smpNone, true);

    REQUIRE(rectilinear_length > 0.);
    REQUIRE(hollow_length > 0.);
    CHECK(hollow_length < rectilinear_length);
    CHECK(solid_raft_length > hollow_length);
}

TEST_CASE("Support interface sublayer range is contact-first and clamped", "[SupportMaterial][Sublayer]")
{
    const auto selected = [](bool enabled, int start, int end, int number, int total) {
        return support_interface_sublayer_selected(enabled, start, end, number, total);
    };

    CHECK_FALSE(selected(false, 2, 4, 2, 5));
    CHECK_FALSE(selected(true, 2, 4, 1, 5));
    CHECK_FALSE(selected(true, 2, 4, 2, 1));
    CHECK_FALSE(selected(true, 2, 4, 5, 5));
    CHECK(selected(true, 2, 4, 2, 5));
    CHECK(selected(true, 2, 4, 3, 5));
    CHECK(selected(true, 2, 4, 4, 5));

    CHECK(selected(true, -20, 99, 2, 5));
    CHECK(selected(true, -20, 99, 5, 5));
    CHECK_FALSE(selected(true, -20, 99, 1, 5));
    CHECK_FALSE(selected(true, -20, 99, 6, 5));

    CHECK(selected(true, 99, 1, 5, 5));
    CHECK_FALSE(selected(true, 99, 1, 4, 5));
}

TEST_CASE("Normal and tree support emit configured interface sublayers", "[SupportMaterial][Sublayer]")
{
    const auto verify = [](SupportType support_type) {
        Print print;
        const DynamicPrintConfig config = sublayer_config(support_type);
        init_and_process_print({ TestMesh::overhang }, print, config);

        const std::vector<size_t> sublayers =
            support_layers_with_role(print, erSupportMaterialInterfaceSublayer);
        const std::vector<size_t> contact_layers =
            support_layers_with_role(print, erSupportMaterialInterface);
        CAPTURE(int(support_type), sublayers, contact_layers);
        REQUIRE_FALSE(sublayers.empty());
        REQUIRE_FALSE(contact_layers.empty());
        CHECK(sublayers.size() == 3);

        constexpr double angle = 17. * M_PI / 180.;
        const std::array<double, 3> expected_angles {
            normalized_undirected_angle(angle),
            normalized_undirected_angle(angle + M_PI / 3.),
            normalized_undirected_angle(angle + 2. * M_PI / 3.)
        };
        size_t segment_count = 0;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            check_role_directions(layer->support_fills, erSupportMaterialInterfaceSublayer, expected_angles, segment_count);
        CHECK(segment_count > 100);

        const std::string output = gcode(print);
        CHECK(output.find("support material interface sublayer") != std::string::npos);
        const size_t low_m104 = output.find("M104 S170");
        const size_t low_m109 = output.find("M109 S170");
        const size_t low_temperature_position = std::min(low_m104, low_m109);
        REQUIRE(low_temperature_position != std::string::npos);

        const size_t restore_m104 = output.find("M104 S", low_temperature_position + 1);
        const size_t restore_m109 = output.find("M109 S", low_temperature_position + 1);
        const size_t restore_position = std::min(restore_m104, restore_m109);
        REQUIRE(restore_position != std::string::npos);
        CHECK(output.substr(restore_position, 12).find("S170") == std::string::npos);
    };

    SECTION("normal support") { verify(stNormalAuto); }
    SECTION("tree support")   { verify(stTreeAuto); }
}

TEST_CASE("Disabled interface sublayers preserve ordinary interface roles", "[SupportMaterial][Sublayer]")
{
    for (const SupportType support_type : { stNormalAuto, stTreeAuto }) {
        Print print;
        const DynamicPrintConfig config = sublayer_config(support_type, false);
        init_and_process_print({ TestMesh::overhang }, print, config);
        CAPTURE(int(support_type));
        CHECK(support_layers_with_role(print, erSupportMaterialInterfaceSublayer).empty());
        CHECK_FALSE(support_layers_with_role(print, erSupportMaterialInterface).empty());
    }
}

TEST_CASE("Sublayer range boundaries select exact generated layers", "[SupportMaterial][Sublayer][Boundary]")
{
    struct RangeCase {
        int interface_layers;
        int start_layer;
        int end_layer;
        size_t expected_sublayers;
    };
    const std::array<RangeCase, 5> cases {{
        { 1, 2, 4, 0 },
        { 5, 2, 2, 1 },
        { 5, 2, 4, 3 },
        { 5, 3, 99, 3 },
        { 5, 99, 1, 1 }
    }};

    for (const SupportType support_type : { stNormalAuto, stTreeAuto }) {
        for (const RangeCase &range : cases) {
            Print print;
            const DynamicPrintConfig config = sublayer_config(
                support_type, true, range.interface_layers, range.start_layer, range.end_layer);
            init_and_process_print({ TestMesh::overhang }, print, config);
            const std::vector<size_t> sublayers =
                support_layers_with_role(print, erSupportMaterialInterfaceSublayer);
            CAPTURE(int(support_type), range.interface_layers, range.start_layer, range.end_layer, sublayers);
            CHECK(sublayers.size() == range.expected_sublayers);
        }
    }
}

static std::string low_temperature_interface_gcode(bool auxiliary_fan_toggle, bool wiping_toggle,
                                                    bool auxiliary_fan_supported = true, bool temperature_drop_tower = false)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", 0 },
        { "single_nozzle_low_temperature_interface", true },
        { "support_interface_temperature", 170 },
        { "support_interface_auxiliary_fan_speed", 100 },
        { "support_interface_heating_time", 0.0 },
        { "support_interface_auxiliary_fan_cooling_on_temperature_change", auxiliary_fan_toggle },
        { "support_interface_nozzle_wiping_on_temperature_change", wiping_toggle },
        { "support_interface_temperature_drop_tower", temperature_drop_tower },
        { "auxiliary_fan", auxiliary_fan_supported },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 }
    });
    config.set_key_value("support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value("support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    return slice({ TestMesh::overhang }, config);
}

struct TemperatureDropTowerSettings {
    int normal_temperature { 220 };
    int interface_temperature { 170 };
    bool tower_enabled { true };
    bool low_temperature_interface_enabled { true };
    int interface_filament { 0 };
    PrintSequence print_sequence { PrintSequence::ByLayer };
    bool configured_wiper { false };
    bool support_enabled { true };
    double tower_x { -1.0 };
    double tower_y { -1.0 };
    double nozzle_diameter { 0.4 };
    double layer_height { 0.2 };
    std::vector<Vec2d> printable_area {
        Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.)
    };
    std::vector<std::vector<Vec2d>> extruder_printable_areas;
    std::vector<Vec2d> extruder_offsets;
    std::vector<int> filament_map;
};

static DynamicPrintConfig temperature_drop_tower_config(const TemperatureDropTowerSettings &settings)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", settings.support_enabled },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", settings.interface_filament },
        { "single_nozzle_low_temperature_interface", settings.low_temperature_interface_enabled },
        { "support_interface_temperature", settings.interface_temperature },
        { "support_interface_temperature_drop_tower", settings.tower_enabled },
        { "support_interface_auxiliary_fan_cooling_on_temperature_change", false },
        { "support_interface_nozzle_wiping_on_temperature_change", settings.configured_wiper },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 }
    });
    config.set_key_value(
        "nozzle_temperature", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value(
        "nozzle_temperature_initial_layer", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value(
        "nozzle_diameter", new ConfigOptionFloats({ settings.nozzle_diameter }));
    config.set_key_value(
        "layer_height", new ConfigOptionFloat(settings.layer_height));
    config.set_key_value(
        "initial_layer_print_height", new ConfigOptionFloat(settings.layer_height));
    config.set_key_value(
        "print_sequence", new ConfigOptionEnum<PrintSequence>(settings.print_sequence));
    config.set_key_value(
        "printable_area", new ConfigOptionPoints(settings.printable_area));
    if (!settings.extruder_printable_areas.empty())
        config.set_key_value(
            "extruder_printable_area", new ConfigOptionPointsGroups(settings.extruder_printable_areas));
    if (!settings.extruder_offsets.empty())
        config.set_key_value("extruder_offset", new ConfigOptionPoints(settings.extruder_offsets));
    if (!settings.filament_map.empty())
        config.set_key_value("filament_map", new ConfigOptionInts(settings.filament_map));
    config.set_key_value(
        "support_interface_temperature_drop_tower_x", new ConfigOptionFloats({ settings.tower_x }));
    config.set_key_value(
        "support_interface_temperature_drop_tower_y", new ConfigOptionFloats({ settings.tower_y }));
    config.set_key_value(
        "support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value(
        "support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    return config;
}

static std::string temperature_drop_tower_gcode(const TemperatureDropTowerSettings &settings)
{
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    return slice({ TestMesh::overhang }, config);
}

static std::string temperature_drop_tower_gcode_for_plate(
    DynamicPrintConfig config, int plate_index)
{
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(mesh(TestMesh::overhang));
    init_print(std::move(meshes), print, model, config, nullptr, false);
    print.set_plate_index(plate_index);
    return gcode(print);
}

static std::optional<double> gcode_word(const std::string &line, char letter)
{
    for (size_t pos = 0; pos < line.size(); ++pos) {
        if (line[pos] != letter || (pos > 0 && !std::isspace(static_cast<unsigned char>(line[pos - 1]))))
            continue;
        const char *begin = line.c_str() + pos + 1;
        char *end = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin)
            return value;
    }
    return std::nullopt;
}

static size_t count_substring(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

static size_t count_extruding_moves_between(
    const std::string &gcode, const std::string &begin_marker, const std::string &end_marker)
{
    const size_t begin = gcode.find(begin_marker);
    const size_t end = begin == std::string::npos ? std::string::npos : gcode.find(end_marker, begin);
    if (begin == std::string::npos || end == std::string::npos)
        return 0;

    size_t count = 0;
    std::istringstream stream(gcode.substr(begin, end - begin));
    for (std::string line; std::getline(stream, line);) {
        const bool linear_move =
            line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0 ||
            line == "G0" || line == "G1";
        if (linear_move && gcode_word(line, 'E'))
            ++count;
    }
    return count;
}

struct TemperatureDropTowerMetrics {
    size_t cooling_passes { 0 };
    std::vector<double> cooling_pass_z;
    double fast_length { 0.0 };
    double slow_length { 0.0 };
    double min_fast_feedrate { std::numeric_limits<double>::max() };
    double max_fast_feedrate { std::numeric_limits<double>::lowest() };
    double min_slow_feedrate { std::numeric_limits<double>::max() };
    double max_slow_feedrate { std::numeric_limits<double>::lowest() };
    double min_x { std::numeric_limits<double>::max() };
    double min_y { std::numeric_limits<double>::max() };
    double max_x { std::numeric_limits<double>::lowest() };
    double max_y { std::numeric_limits<double>::lowest() };
};

static TemperatureDropTowerMetrics analyze_temperature_drop_tower(const std::string &gcode)
{
    enum class Section { None, Fast, Slow };
    TemperatureDropTowerMetrics metrics;
    Section section = Section::None;
    bool analyze_first_pass = false;
    double x = 0.0;
    double y = 0.0;
    double z = 0.0;
    double feedrate = 0.0;

    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.find("temperature drop tower cooling pass") != std::string::npos &&
            line.find("pass end") == std::string::npos) {
            ++metrics.cooling_passes;
            metrics.cooling_pass_z.push_back(z);
            analyze_first_pass = metrics.cooling_passes == 1;
            section = Section::None;
        } else if (analyze_first_pass && line.find("cooling fast section") != std::string::npos) {
            section = Section::Fast;
        } else if (analyze_first_pass && line.find("cooling final 10 mm") != std::string::npos) {
            section = Section::Slow;
        } else if (analyze_first_pass && line.find("cooling pass end") != std::string::npos) {
            analyze_first_pass = false;
            section = Section::None;
        }

        const bool linear_move =
            line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0 ||
            line == "G0" || line == "G1";
        if (!linear_move)
            continue;

        const double previous_x = x;
        const double previous_y = y;
        if (const auto value = gcode_word(line, 'F'))
            feedrate = *value;
        if (const auto value = gcode_word(line, 'X'))
            x = *value;
        if (const auto value = gcode_word(line, 'Y'))
            y = *value;
        if (const auto value = gcode_word(line, 'Z'))
            z = *value;

        if (!analyze_first_pass || section == Section::None || !gcode_word(line, 'E'))
            continue;

        const double length = std::hypot(x - previous_x, y - previous_y);
        if (length <= 1e-6)
            continue;
        metrics.min_x = std::min({ metrics.min_x, previous_x, x });
        metrics.min_y = std::min({ metrics.min_y, previous_y, y });
        metrics.max_x = std::max({ metrics.max_x, previous_x, x });
        metrics.max_y = std::max({ metrics.max_y, previous_y, y });
        if (section == Section::Fast) {
            metrics.min_fast_feedrate = std::min(metrics.min_fast_feedrate, feedrate);
            metrics.max_fast_feedrate = std::max(metrics.max_fast_feedrate, feedrate);
            metrics.fast_length += length;
        } else {
            metrics.min_slow_feedrate = std::min(metrics.min_slow_feedrate, feedrate);
            metrics.max_slow_feedrate = std::max(metrics.max_slow_feedrate, feedrate);
            metrics.slow_length += length;
        }
    }
    return metrics;
}

static void check_temperature_drop_tower_passes_are_on_distinct_layers(
    const TemperatureDropTowerMetrics &metrics)
{
    REQUIRE(metrics.cooling_passes == 3);
    REQUIRE(metrics.cooling_pass_z.size() == metrics.cooling_passes);
    std::vector<double> sorted_z = metrics.cooling_pass_z;
    std::sort(sorted_z.begin(), sorted_z.end());
    for (size_t idx = 1; idx < sorted_z.size(); ++idx)
        CHECK(sorted_z[idx] - sorted_z[idx - 1] > 0.05);
}

TEST_CASE("Temperature drop tower scales and slows only its final 10 mm",
          "[SupportMaterial][TemperatureDropTower]")
{
    struct TemperatureCase {
        int normal_temperature;
        int interface_temperature;
        double expected_size;
    };
    const std::array<TemperatureCase, 4> cases {{
        { 200, 170, 50.0 },
        { 201, 170, 51.0 },
        { 220, 170, 70.0 },
        { 260, 170, 80.0 }
    }};

    for (const TemperatureCase &temperature : cases) {
        TemperatureDropTowerSettings settings;
        settings.normal_temperature = temperature.normal_temperature;
        settings.interface_temperature = temperature.interface_temperature;
        const std::string output = temperature_drop_tower_gcode(settings);
        const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(output);
        CAPTURE(temperature.normal_temperature, temperature.interface_temperature, metrics.cooling_passes);

        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.fast_length > metrics.slow_length);
        CHECK(metrics.slow_length == Catch::Approx(10.0).margin(0.03));
        CHECK(metrics.min_fast_feedrate > 600.0);
        CHECK(metrics.max_fast_feedrate == Catch::Approx(metrics.min_fast_feedrate).margin(0.1));
        CHECK(metrics.min_slow_feedrate == Catch::Approx(600.0).margin(0.1));
        CHECK(metrics.max_slow_feedrate == Catch::Approx(600.0).margin(0.1));
        CHECK(metrics.max_x - metrics.min_x == Catch::Approx(temperature.expected_size).margin(0.03));
        CHECK(metrics.max_y - metrics.min_y == Catch::Approx(temperature.expected_size).margin(0.03));
        CHECK(metrics.min_x > 3.0);
        CHECK(metrics.max_y < 197.0);
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);

        const std::string set_temperature = "M104 S" + std::to_string(temperature.interface_temperature);
        const std::string wait_temperature = "M109 S" + std::to_string(temperature.interface_temperature);
        const size_t begin = output.find("low-temperature support interface begin");
        const size_t nonblocking_temperature = output.find(set_temperature, begin);
        const size_t brim_begin = output.find("temperature drop tower brim begin");
        const size_t brim_end = output.find("temperature drop tower brim end", brim_begin);
        const size_t first_tower_layer = output.find("; temperature drop tower layer");
        const size_t cooling_pass = output.find("temperature drop tower cooling pass", begin);
        const size_t cooling_end = output.find("temperature drop tower cooling pass end", cooling_pass);
        const size_t interface = output.find("support material interface", cooling_end);
        REQUIRE(begin != std::string::npos);
        REQUIRE(nonblocking_temperature != std::string::npos);
        REQUIRE(brim_begin != std::string::npos);
        REQUIRE(brim_end != std::string::npos);
        REQUIRE(first_tower_layer != std::string::npos);
        REQUIRE(cooling_pass != std::string::npos);
        REQUIRE(cooling_end != std::string::npos);
        REQUIRE(interface != std::string::npos);
        CHECK(begin < nonblocking_temperature);
        CHECK(brim_begin < brim_end);
        CHECK(brim_end < first_tower_layer);
        CHECK(brim_end < cooling_pass);
        CHECK(cooling_pass < cooling_end);
        CHECK(cooling_end < interface);
        CHECK(output.find(wait_temperature, nonblocking_temperature) > interface);
        CHECK(count_extruding_moves_between(
                  output,
                  "; temperature drop tower brim begin",
                  "; temperature drop tower brim end") >= TEMPERATURE_DROP_TOWER_BRIM_LINE_COUNT);
        CHECK(count_substring(output, "; temperature drop tower brim begin") == 1);
        CHECK(count_substring(output, "; temperature drop tower brim end") == 1);
    }
}

TEST_CASE("Temperature drop tower keeps its brim across a 36-setting slicing matrix",
          "[SupportMaterial][TemperatureDropTower][Matrix]")
{
    struct NozzleCase {
        double diameter;
        double layer_height;
    };
    const std::array<int, 3> normal_temperatures {{ 200, 220, 260 }};
    const std::array<int, 2> interface_temperatures {{ 170, 182 }};
    const std::array<NozzleCase, 3> nozzle_cases {{
        { 0.15, 0.08 },
        { 0.4, 0.2 },
        { 0.8, 0.3 }
    }};
    const std::array<bool, 2> manual_position_cases {{ false, true }};

    size_t sliced_combinations = 0;
    for (const int normal_temperature : normal_temperatures) {
        for (const int interface_temperature : interface_temperatures) {
            for (const NozzleCase &nozzle : nozzle_cases) {
                for (const bool manual_position : manual_position_cases) {
                    TemperatureDropTowerSettings settings;
                    settings.normal_temperature = normal_temperature;
                    settings.interface_temperature = interface_temperature;
                    settings.nozzle_diameter = nozzle.diameter;
                    settings.layer_height = nozzle.layer_height;
                    if (manual_position) {
                        settings.tower_x = 20.0;
                        settings.tower_y = 25.0;
                    }

                    const std::string output = temperature_drop_tower_gcode(settings);
                    const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(output);
                    const double temperature_delta = normal_temperature - interface_temperature;
                    const double expected_size = temperature_delta <= 30.0
                        ? 50.0
                        : std::min(80.0, 50.0 + temperature_delta - 30.0);
                    const size_t brim_begin = output.find("; temperature drop tower brim begin");
                    const size_t brim_end = output.find("; temperature drop tower brim end", brim_begin);
                    const size_t first_tower_layer = output.find("; temperature drop tower layer");

                    CAPTURE(normal_temperature, interface_temperature, nozzle.diameter,
                            nozzle.layer_height, manual_position, expected_size);
                    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
                    REQUIRE(brim_begin != std::string::npos);
                    REQUIRE(brim_end != std::string::npos);
                    REQUIRE(first_tower_layer != std::string::npos);
                    CHECK(brim_begin < brim_end);
                    CHECK(brim_end < first_tower_layer);
                    CHECK(count_substring(output, "; temperature drop tower brim begin") == 1);
                    CHECK(count_substring(output, "; temperature drop tower brim end") == 1);
                    CHECK(count_substring(output, "; temperature drop tower brim line") ==
                          TEMPERATURE_DROP_TOWER_BRIM_LINE_COUNT);
                    CHECK(count_extruding_moves_between(
                              output,
                              "; temperature drop tower brim begin",
                              "; temperature drop tower brim end") >=
                          TEMPERATURE_DROP_TOWER_BRIM_LINE_COUNT);
                    CHECK(std::isfinite(metrics.min_x));
                    CHECK(std::isfinite(metrics.min_y));
                    CHECK(std::isfinite(metrics.max_x));
                    CHECK(std::isfinite(metrics.max_y));
                    CHECK(metrics.min_x >= 3.0 - 0.03);
                    CHECK(metrics.min_y >= 3.0 - 0.03);
                    CHECK(metrics.max_x <= 197.0 + 0.03);
                    CHECK(metrics.max_y <= 197.0 + 0.03);
                    CHECK(metrics.max_x - metrics.min_x == Catch::Approx(expected_size).margin(0.03));
                    CHECK(metrics.max_y - metrics.min_y == Catch::Approx(expected_size).margin(0.03));
                    CHECK(metrics.slow_length == Catch::Approx(10.0).margin(0.03));
                    ++sliced_combinations;
                }
            }
        }
    }
    CHECK(sliced_combinations == 36);
}

TEST_CASE("Temperature drop tower stays inside rear-left bed bounds",
          "[SupportMaterial][TemperatureDropTower]")
{
    const std::array<std::tuple<std::vector<Vec2d>, double, double, double, double>, 2> beds {{
        {
            { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 70.), Vec2d(0., 70.) },
            0.0, 200.0, 67.0, 64.0
        },
        {
            { Vec2d(-10., -20.), Vec2d(190., -20.), Vec2d(190., 180.), Vec2d(-10., 180.) },
            -10.0, 190.0, 177.0, 70.0
        }
    }};

    for (const auto &[bed, bed_min_x, bed_max_x, expected_max_y, expected_size] : beds) {
        TemperatureDropTowerSettings settings;
        settings.printable_area = bed;
        const TemperatureDropTowerMetrics metrics =
            analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));
        CAPTURE(bed_min_x, bed_max_x, expected_max_y, expected_size);
        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.min_x >= bed_min_x + 3.0 - 0.03);
        CHECK(metrics.max_x <= bed_max_x - 3.0 + 0.03);
        CHECK(metrics.max_y < expected_max_y);
        CHECK(metrics.max_x - metrics.min_x <= expected_size + 0.03);
        CHECK(metrics.max_y - metrics.min_y <= expected_size + 0.03);
    }
}

TEST_CASE("Temperature drop tower keeps its configured position when a model overlaps it",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.tower_x = 5.0;
    settings.tower_y = 145.0;
    const TemperatureDropTowerMetrics baseline =
        analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    TriangleMesh overhang = mesh(TestMesh::overhang);
    const BoundingBoxf3 original_bbox = overhang.bounding_box();
    const double target_min_x = 5.0;
    const double target_max_y = 195.0;
    overhang.translate(
        target_min_x - original_bbox.min.x(),
        target_max_y - original_bbox.max.y(),
        -original_bbox.min.z());
    const BoundingBoxf3 placed_bbox = overhang.bounding_box();

    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.emplace_back(std::move(overhang));
    init_print(std::move(meshes), print, model, config, nullptr, false);
    const TemperatureDropTowerMetrics metrics =
        analyze_temperature_drop_tower(gcode(print));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x >= 3.0 - 0.03);
    CHECK(metrics.max_x <= 197.0 + 0.03);
    CHECK(metrics.min_y >= 3.0 - 0.03);
    CHECK(metrics.max_y <= 197.0 + 0.03);
    const bool overlaps =
        metrics.max_x > placed_bbox.min.x() &&
        metrics.min_x < placed_bbox.max.x() &&
        metrics.max_y > placed_bbox.min.y() &&
        metrics.min_y < placed_bbox.max.y();
    CHECK(overlaps);
    CHECK(metrics.min_x == Catch::Approx(baseline.min_x).margin(0.03));
    CHECK(metrics.max_x == Catch::Approx(baseline.max_x).margin(0.03));
    CHECK(metrics.min_y == Catch::Approx(baseline.min_y).margin(0.03));
    CHECK(metrics.max_y == Catch::Approx(baseline.max_y).margin(0.03));
}

TEST_CASE("Temperature drop tower G-code follows a manual project move exactly",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings before_move;
    before_move.tower_x = 20.0;
    before_move.tower_y = 25.0;
    const std::string before_output = temperature_drop_tower_gcode(before_move);
    const TemperatureDropTowerMetrics before =
        analyze_temperature_drop_tower(before_output);

    TemperatureDropTowerSettings after_move = before_move;
    after_move.tower_x = 42.0;
    after_move.tower_y = 31.0;
    const std::string after_output = temperature_drop_tower_gcode(after_move);
    const TemperatureDropTowerMetrics after =
        analyze_temperature_drop_tower(after_output);

    check_temperature_drop_tower_passes_are_on_distinct_layers(before);
    check_temperature_drop_tower_passes_are_on_distinct_layers(after);
    CHECK(after.min_x - before.min_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.max_x - before.max_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.min_y - before.min_y == Catch::Approx(6.0).margin(0.03));
    CHECK(after.max_y - before.max_y == Catch::Approx(6.0).margin(0.03));
    CHECK(count_substring(before_output, "; temperature drop tower brim line") == 5);
    CHECK(count_substring(after_output, "; temperature drop tower brim line") == 5);
}

TEST_CASE("Temperature drop tower keeps five brim lines across nozzle diameters",
          "[SupportMaterial][TemperatureDropTower]")
{
    const std::array<std::pair<double, double>, 3> nozzle_cases {{
        { 0.15, 0.08 },
        { 0.4, 0.2 },
        { 0.8, 0.3 }
    }};
    double previous_body_offset = 0.0;
    for (const auto &[nozzle_diameter, layer_height] : nozzle_cases) {
        TemperatureDropTowerSettings settings;
        settings.tower_x = 20.0;
        settings.tower_y = 25.0;
        settings.nozzle_diameter = nozzle_diameter;
        settings.layer_height = layer_height;
        const std::string output = temperature_drop_tower_gcode(settings);
        const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(output);
        const double body_offset = metrics.min_x - settings.tower_x;

        CAPTURE(nozzle_diameter, body_offset);
        check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
        CHECK(metrics.max_x - metrics.min_x == Catch::Approx(70.0).margin(0.03));
        CHECK(metrics.max_y - metrics.min_y == Catch::Approx(70.0).margin(0.03));
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);
        CHECK(body_offset > previous_body_offset);
        previous_body_offset = body_offset;
    }
}

TEST_CASE("Temperature drop tower clamps a moved position to printable bed bounds",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.tower_x = 1000.0;
    settings.tower_y = 1000.0;
    const TemperatureDropTowerMetrics metrics =
        analyze_temperature_drop_tower(temperature_drop_tower_gcode(settings));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 3.0);
    CHECK(metrics.min_y > 3.0);
    CHECK(metrics.max_x < 197.0);
    CHECK(metrics.max_y < 197.0);
}

TEST_CASE("Temperature drop tower automatic placement respects the mapped extruder area",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.filament_map = { 2 };
    settings.extruder_printable_areas = {
        settings.printable_area,
        { Vec2d(24., 12.), Vec2d(176., 12.), Vec2d(176., 188.), Vec2d(24., 188.) }
    };
    settings.tower_x = -1.0;
    settings.tower_y = -1.0;

    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    config.set_num_extruders(2);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4 }));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 2 }));
    config.set_key_value(
        "extruder_printable_area", new ConfigOptionPointsGroups(settings.extruder_printable_areas));
    const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(
        slice({ TestMesh::overhang }, config));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 24.0);
    CHECK(metrics.min_y > 12.0);
    CHECK(metrics.max_x < 176.0);
    CHECK(metrics.max_y < 188.0);
}

TEST_CASE("Temperature drop tower uses the mapped physical extruder nozzle",
          "[SupportMaterial][TemperatureDropTower][ToolMapping]")
{
    TemperatureDropTowerSettings reference_settings;
    reference_settings.tower_x = 20.0;
    reference_settings.tower_y = 25.0;
    reference_settings.nozzle_diameter = 0.8;
    const TemperatureDropTowerMetrics reference = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode(reference_settings));

    TemperatureDropTowerSettings mapped_settings;
    mapped_settings.tower_x = reference_settings.tower_x;
    mapped_settings.tower_y = reference_settings.tower_y;
    DynamicPrintConfig config = temperature_drop_tower_config(mapped_settings);
    config.set_num_extruders(2);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.15, 0.8 }));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 2 }));

    const TemperatureDropTowerMetrics mapped = analyze_temperature_drop_tower(
        slice({ TestMesh::overhang }, config));

    check_temperature_drop_tower_passes_are_on_distinct_layers(mapped);
    CHECK(mapped.min_x == Catch::Approx(reference.min_x).margin(0.03));
    CHECK(mapped.min_y == Catch::Approx(reference.min_y).margin(0.03));
    CHECK(mapped.max_x == Catch::Approx(reference.max_x).margin(0.03));
    CHECK(mapped.max_y == Catch::Approx(reference.max_y).margin(0.03));
}

TEST_CASE("Temperature drop tower follows the standard extruder offset transform",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    settings.extruder_offsets = { Vec2d(-32.0, 0.0) };
    const TemperatureDropTowerMetrics metrics = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode(settings));

    check_temperature_drop_tower_passes_are_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 34.0);
    CHECK(metrics.max_x < 109.0);
    CHECK(metrics.min_y > 2.0);
    CHECK(metrics.max_y < 198.0);
}

TEST_CASE("Temperature drop tower selects and serializes independent plate positions",
          "[SupportMaterial][TemperatureDropTower]")
{
    TemperatureDropTowerSettings settings;
    DynamicPrintConfig config = temperature_drop_tower_config(settings);
    config.set_key_value(
        "support_interface_temperature_drop_tower_x", new ConfigOptionFloats({ 20.0, 65.0 }));
    config.set_key_value(
        "support_interface_temperature_drop_tower_y", new ConfigOptionFloats({ 25.0, 40.0 }));

    const std::string serialized_x =
        config.opt_serialize("support_interface_temperature_drop_tower_x");
    const std::string serialized_y =
        config.opt_serialize("support_interface_temperature_drop_tower_y");
    DynamicPrintConfig restored = DynamicPrintConfig::full_print_config();
    restored.set_deserialize_strict(
        "support_interface_temperature_drop_tower_x", serialized_x);
    restored.set_deserialize_strict(
        "support_interface_temperature_drop_tower_y", serialized_y);
    const auto *restored_x = restored.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_x");
    const auto *restored_y = restored.option<ConfigOptionFloats>(
        "support_interface_temperature_drop_tower_y");
    REQUIRE(restored_x != nullptr);
    REQUIRE(restored_y != nullptr);
    REQUIRE(restored_x->values.size() == 2);
    REQUIRE(restored_y->values.size() == 2);
    CHECK(restored_x->get_at(0) == Catch::Approx(20.0));
    CHECK(restored_x->get_at(1) == Catch::Approx(65.0));
    CHECK(restored_y->get_at(0) == Catch::Approx(25.0));
    CHECK(restored_y->get_at(1) == Catch::Approx(40.0));

    const TemperatureDropTowerMetrics plate_zero = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode_for_plate(config, 0));
    const TemperatureDropTowerMetrics plate_one = analyze_temperature_drop_tower(
        temperature_drop_tower_gcode_for_plate(config, 1));
    check_temperature_drop_tower_passes_are_on_distinct_layers(plate_zero);
    check_temperature_drop_tower_passes_are_on_distinct_layers(plate_one);
    CHECK(plate_one.min_x - plate_zero.min_x == Catch::Approx(45.0).margin(0.03));
    CHECK(plate_one.max_x - plate_zero.max_x == Catch::Approx(45.0).margin(0.03));
    CHECK(plate_one.min_y - plate_zero.min_y == Catch::Approx(15.0).margin(0.03));
    CHECK(plate_one.max_y - plate_zero.max_y == Catch::Approx(15.0).margin(0.03));
}

TEST_CASE("Temperature drop tower honors every activation guard",
          "[SupportMaterial][TemperatureDropTower]")
{
    std::vector<std::pair<std::string, TemperatureDropTowerSettings>> disabled_cases;
    TemperatureDropTowerSettings settings;
    settings.tower_enabled = false;
    disabled_cases.emplace_back("tower toggle off", settings);
    settings = {};
    settings.low_temperature_interface_enabled = false;
    disabled_cases.emplace_back("low-temperature mode off", settings);
    settings = {};
    settings.interface_filament = 1;
    disabled_cases.emplace_back("dedicated interface filament", settings);
    settings = {};
    settings.print_sequence = PrintSequence::ByObject;
    disabled_cases.emplace_back("print by object", settings);
    settings = {};
    settings.configured_wiper = true;
    disabled_cases.emplace_back("configured nozzle wiper", settings);
    settings = {};
    settings.normal_temperature = settings.interface_temperature;
    disabled_cases.emplace_back("no temperature drop", settings);
    settings = {};
    settings.support_enabled = false;
    disabled_cases.emplace_back("support disabled", settings);

    for (const auto &[name, disabled] : disabled_cases) {
        CAPTURE(name);
        const std::string output = temperature_drop_tower_gcode(disabled);
        CHECK(output.find("temperature drop tower layer") == std::string::npos);
        CHECK(output.find("temperature drop tower cooling pass") == std::string::npos);
    }

    TemperatureDropTowerSettings wiper_fallback;
    wiper_fallback.configured_wiper = true;
    const std::string fallback = temperature_drop_tower_gcode(wiper_fallback);
    CHECK(fallback.find("M109 S170") != std::string::npos);
    CHECK(fallback.find("temperature drop tower") == std::string::npos);
}

TEST_CASE("Low temperature interface hardware actions follow their toggles", "[SupportMaterial][GCode]")
{
    constexpr std::string_view begin_marker = "low-temperature support interface begin";
    constexpr std::string_view auxiliary_fan_marker = "low-temperature support interface auxiliary cooling";
    constexpr std::string_view wiping_marker = "low-temperature support interface nozzle brushing";

    SECTION("both actions disabled") {
        const std::string gcode = low_temperature_interface_gcode(false, false);
        REQUIRE(gcode.find(begin_marker) != std::string::npos);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }

    SECTION("only auxiliary fan cooling enabled") {
        const std::string gcode = low_temperature_interface_gcode(true, false);
        CHECK(gcode.find(auxiliary_fan_marker) != std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }

    SECTION("only nozzle wiping enabled") {
        const std::string gcode = low_temperature_interface_gcode(false, true);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) != std::string::npos);
    }

    SECTION("both actions enabled in cooling then wiping order") {
        const std::string gcode = low_temperature_interface_gcode(true, true);
        const size_t auxiliary_fan_position = gcode.find(auxiliary_fan_marker);
        const size_t wiping_position = gcode.find(wiping_marker);
        REQUIRE(auxiliary_fan_position != std::string::npos);
        REQUIRE(wiping_position != std::string::npos);
        CHECK(auxiliary_fan_position < wiping_position);
    }

    SECTION("unsupported auxiliary fan suppresses cooling even if enabled") {
        const std::string gcode = low_temperature_interface_gcode(true, false, false);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
    }

    SECTION("temperature drop tower is printed during cooling when wiping is disabled") {
        const std::string gcode = low_temperature_interface_gcode(false, false, true, true);
        const size_t begin_position = gcode.find(begin_marker);
        const size_t tower_position = gcode.find("temperature drop tower cooling pass");
        const size_t interface_position = gcode.find("support material interface");
        REQUIRE(begin_position != std::string::npos);
        REQUIRE(tower_position != std::string::npos);
        REQUIRE(interface_position != std::string::npos);
        CHECK(begin_position < tower_position);
        CHECK(tower_position < interface_position);
    }
}

TEST_CASE("Three raft layers are created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ cube(20) }, print, {
        { "enable_support", 1 },
        { "raft_layers",    3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

TEST_CASE("Cura raft does not enable automatic support when support is off", "[SupportMaterial][CuraStyle]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    config.set_key_value("enforce_support_layers", new ConfigOptionInt(0));
    config.set_key_value("raft_layers", new ConfigOptionInt(1));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalCuraAuto));
    config.set_key_value("cura_solid_support_raft", new ConfigOptionBool(true));

    Slic3r::Print print;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, print, config);

    REQUIRE(print.objects().front()->support_layers().size() == 1);
}

TEST_CASE("Enforced support layers are generated", "[SupportMaterial]")
{
    // enforce_support_layers forces support on the first N layers even with support off.
    Slic3r::Print baseline;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, baseline, {
        { "enable_support",         0 },
        { "enforce_support_layers", 0 }
    });
    REQUIRE(baseline.objects().front()->support_layers().empty());

    Slic3r::Print enforced;
    Slic3r::Test::init_and_process_print({ TestMesh::overhang }, enforced, {
        { "enable_support",         0 },
        { "enforce_support_layers", 100 }
    });
    REQUIRE(enforced.objects().front()->support_layers().size() > 0);
}

SCENARIO("Support layer Z honors contact distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}
	};

    GIVEN("A print object having one modelObject") {
        WHEN("Layer height = 0.2 and first layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.4 },
                { "dont_support_bridges",       false },
			});
			bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
        WHEN("Layer height = 0.2 and first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
                { "enable_support",             1 },
                { "layer_height",               0.2 },
                { "initial_layer_print_height", 0.3 },
                { "dont_support_bridges",       false },
            });
            bool first_layer_ok, layer_min_ok, layer_max_ok;
            check(print, first_layer_ok, layer_min_ok, layer_max_ok);
            THEN("First layer height is honored")			{ REQUIRE(first_layer_ok == true); }
            THEN("No null or negative support layers")		{ REQUIRE(layer_min_ok == true); }
            THEN("No layers thicker than nozzle diameter")	{ REQUIRE(layer_max_ok == true); }
        }
    }
}
