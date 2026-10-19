// MAGPIE: Magpie line 1 (Beta 7) support tests, run on this build's support modules with line 1's
// settings set through the line 1 migration (line1_settings.hpp).
#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Slicing.hpp"
#include "libslic3r/SlicesToTriangleMesh.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

#include "line1_settings.hpp"
#include <nlohmann/json.hpp>
#include "test_helpers.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

static Polygons support_test_rectangle(double min_x, double min_y, double max_x, double max_y)
{
    return Polygons{Polygon{
        Point(scale_(min_x), scale_(min_y)),
        Point(scale_(max_x), scale_(min_y)),
        Point(scale_(max_x), scale_(max_y)),
        Point(scale_(min_x), scale_(max_y))
    }};
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

static void mixed_audit_paths(const ExtrusionEntity &entity, std::map<float, Polygons> &by_height)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const auto *child : collection->entities)
            mixed_audit_paths(*child, by_height);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        append(by_height[path->height], path->polygons_covered_by_width());
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const auto &path : loop->paths) mixed_audit_paths(path, by_height);
    } else if (const auto *multi = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const auto &path : multi->paths) mixed_audit_paths(path, by_height);
    } else {
        FAIL("Unsupported Mixed audit extrusion entity");
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

TEST_CASE("Resin style Default and Branching generate printable FFF support",
          "[ResinSupport][Line1][Integration][Nozzle]")
{
    BundledMods mods;
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 18.);
        TriangleMesh ceiling = make_cube(28., 20., 2.);
        ceiling.translate(-10.f, -6.f, 18.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const Line1ResinTree strategy : { rstDefault, rstBranching }) {
      for (const double nozzle : { 0.2, 0.4, 0.6, 0.8 }) {
        CAPTURE(int(strategy), nozzle);
        const double layer_height = std::min(0.2, 0.5 * nozzle);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        line1(config, "support_type", "resin(auto)");
        line1(config, "resin_support_tree_type", line1_name(strategy));
        line1(config, "resin_support_points_density_relative", 50);
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
        CHECK(object->config().support_generator.value == "resin");
        CHECK(object->is_step_done(posSupportMaterial));
        const auto support_layers = object->support_layers();
        REQUIRE_FALSE(support_layers.empty());
        CHECK(std::any_of(support_layers.begin(), support_layers.end(), [](const SupportLayer *layer) {
            return layer->print_z < 15. && layer->has_extrusions();
        }));

        const std::string output = gcode(print);
        CHECK_FALSE(output.empty());
        CHECK(output.find("support material interface") != std::string::npos);
      }
    }
}

TEST_CASE("Resin style accepts the full configured pillar diameter range",
          "[ResinSupport][Line1][Integration][PillarDiameter]")
{
    BundledMods mods;
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 12.);
        TriangleMesh ceiling = make_cube(24., 16., 2.);
        ceiling.translate(-8.f, -4.f, 12.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const Line1ResinTree strategy : { rstDefault, rstBranching }) {
        const char *pillar_key = strategy == rstBranching ?
            "resin_branching_support_pillar_diameter" : "resin_support_pillar_diameter";
        for (const double pillar_diameter_mm : { 0., 0.2, 0.4, 1., 2., 5., 10., 15. }) {
            CAPTURE(int(strategy), pillar_key, pillar_diameter_mm);
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            line1(config, "support_type", "resin(auto)");
            line1(config, "resin_support_tree_type", line1_name(strategy));
            line1(config, "resin_support_points_density_relative", 25);
            line1(config, pillar_key, pillar_diameter_mm);
            config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

            Print print;
            REQUIRE_NOTHROW(init_and_process_print({ make_fixture() }, print, config));
            REQUIRE(print.objects().size() == 1);
            CHECK(print.objects().front()->is_step_done(posSupportMaterial));
        }
    }
}

TEST_CASE("Resin style automatic points obey the shared overhang threshold",
          "[ResinSupport][Line1][Integration][Threshold]")
{
    BundledMods mods;
    const auto slice_with_threshold = [](int threshold_angle) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        line1(config, "support_type", "resin(auto)");
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(threshold_angle));
        line1(config, "resin_support_points_density_relative", 100);
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

        Print print;
        init_and_process_print({ resin_test_sloped_wall() }, print, config);
        return print.objects().front()->support_layers().size();
    };

    const size_t permissive_support_layers = slice_with_threshold(30);
    const size_t strict_support_layers = slice_with_threshold(80);
    CHECK(strict_support_layers > permissive_support_layers);
}

TEST_CASE("Resin style elevation applies only while support is enabled",
          "[ResinSupport][Line1][Elevation]")
{
    BundledMods mods;
    // Before: the object floated 5 mm with nothing under it and slicing stopped with
    // "The object has empty layers". User decision 2026-10-05: elevation follows Enable support.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    line1(config, "support_type", "resin(auto)");
    line1(config, "resin_support_tree_type", line1_name(rstDefault));
    line1(config, "resin_support_object_elevation", 5.);

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ make_cube(20., 20., 10.) }, print, config));
    REQUIRE(print.objects().size() == 1);
    const PrintObject *object = print.objects().front();
    CHECK(object->slicing_parameters().object_print_z_min == Catch::Approx(0.));
    CHECK(object->support_layers().empty());
    CHECK(object->layers().front()->bottom_z() == Catch::Approx(0.));

    // Turning support on in place re-slices the object at its elevation.
    DynamicPrintConfig enabled = config;
    enabled.set_key_value("enable_support", new ConfigOptionBool(true));
    print.apply(print.model(), enabled);
    REQUIRE_NOTHROW(print.process());
    CHECK(print.objects().front()->slicing_parameters().object_print_z_min == Catch::Approx(5.));
    CHECK(print.objects().front()->layers().front()->bottom_z() == Catch::Approx(5.));
}

TEST_CASE("Resin style zero elevation omits bed-face points but keeps overhang support",
          "[ResinSupport][Line1][Integration][Elevation]")
{
    BundledMods mods;
    TriangleMesh fixture = make_cube(8., 8., 8.);
    TriangleMesh ceiling = make_cube(28., 20., 2.);
    ceiling.translate(-10.f, -6.f, 18.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    line1(config, "support_type", "resin(auto)");
    line1(config, "resin_support_tree_type", line1_name(rstDefault));
    line1(config, "resin_support_object_elevation", 0.);
    line1(config, "resin_support_points_density_relative", 50);
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    REQUIRE(print.objects().size() == 1);
    const PrintObject *object = print.objects().front();
    CHECK(object->slicing_parameters().object_print_z_min == Catch::Approx(0.));
    CHECK_FALSE(object->support_layers().empty());
}

TEST_CASE("Resin support keeps every raft layer under an elevated object", "[ResinSupport][Line1][Raft]")
{
    BundledMods mods;
    // Before: with 3 raft layers the raft interface layer under a Resin-elevated object had no
    // contact area, so it was dropped. Slicing stopped with "empty layers between 0.25 and 0.86".
    const auto strategy = GENERATE(rstDefault, rstBranching);
    const int raft_layers = GENERATE(2, 3, 4);
    TriangleMesh fixture = make_cube(5., 10., 6.);
    TriangleMesh ceiling = make_cube(20., 10., 1.2);
    ceiling.translate(0.f, 0.f, 6.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.24}, {"initial_layer_print_height", 0.25},
        {"enable_support", true}, {"raft_layers", raft_layers}});
    line1(config, "support_type", "resin(auto)");
    line1(config, "resin_support_tree_type", line1_name(strategy));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    const PrintObject &object = *print.objects().front();
    const SlicingParameters &slicing = object.slicing_parameters();
    CAPTURE(int(strategy), raft_layers);
    REQUIRE(object.layers().front()->bottom_z() > slicing.raft_contact_top_z); // The object floats.

    const auto support_layers = object.support_layers();
    REQUIRE(support_layers.size() > size_t(raft_layers));
    size_t raft_with_paths = 0;
    for (size_t k = 0; k < support_layers.size(); ++k) {
        const SupportLayer &layer = *support_layers[k];
        if (layer.print_z > slicing.raft_interface_top_z + EPSILON)
            break;
        raft_with_paths += layer.has_extrusions();
    }
    // Every raft layer below the columns. The raft contact layer is not generated under a floating object.
    CHECK(raft_with_paths == size_t(slicing.raft_layers() - 1));
    // No vertical hole from the bed up to the first column layer above the raft.
    for (size_t k = 1; k < support_layers.size() && support_layers[k - 1]->print_z <= slicing.raft_interface_top_z + EPSILON; ++k) {
        CAPTURE(k, support_layers[k - 1]->print_z, support_layers[k]->print_z, support_layers[k]->height);
        CHECK(support_layers[k]->print_z - support_layers[k]->height <= support_layers[k - 1]->print_z + EPSILON);
    }
}

TEST_CASE("Resin style processes variable layer heights with both tree strategies",
          "[ResinSupport][Line1][Integration][VariableLayer]")
{
    BundledMods mods;
    const auto make_fixture = []() {
        TriangleMesh fixture = make_cube(8., 8., 12.);
        TriangleMesh ceiling = make_cube(24., 16., 2.);
        ceiling.translate(-8.f, -4.f, 12.f);
        fixture.merge(ceiling);
        return fixture;
    };

    for (const Line1ResinTree strategy : { rstDefault, rstBranching }) {
        CAPTURE(int(strategy));
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        line1(config, "support_type", "resin(auto)");
        line1(config, "resin_support_tree_type", line1_name(strategy));
        line1(config, "resin_support_points_density_relative", 50);
        line1(config, "resin_support_object_elevation", 0.);
        line1(config, "resin_branching_support_object_elevation", 0.);
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


TEST_CASE("Mixed selective merge generates both support channels",
          "[MixedSupport][Line1][Integration][MixedIndependent]")
{
    BundledMods mods;
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

    for (const Line1MixedNormal normal_generator : { mnsgPrusa, mnsgCura }) {
        for (const Line1MixedTree tree_style : {
                mtssOrganic, mtssSlim, mtssStrong, mtssTreeHybrid }) {
            CAPTURE(int(normal_generator), int(tree_style));
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            line1(config, "support_type", "mixed(auto)");
            line1(config, "mixed_normal_support_generator", line1_name(normal_generator));
            line1(config, "mixed_tree_support_style", line1_name(tree_style));
            line1(config, "mixed_normal_coverage_threshold", 80.);
            line1(config, "mixed_selective_merge", true);
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
            CHECK(output.find("support_type = normal(auto)") != std::string::npos);
        }
    }
}

TEST_CASE("Mixed organic tree wall count changes emitted support paths",
          "[MixedSupport][Line1][Tree][Walls][Integration][MixedIndependent][MixedTreeWalls]")
{
    BundledMods mods;
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
        line1(config, "support_type", "mixed(auto)");
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        line1(config, "mixed_normal_support_generator", line1_name(mnsgCura));
        line1(config, "mixed_tree_support_style", line1_name(mtssOrganic));
        line1(config, "mixed_normal_coverage_threshold", 80.);
        line1(config, "mixed_selective_merge", selective_merge);
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4 }));
        line1(config, "support_wall_count", 0);
        // Line 1 read tree_support_wall_count for organic trees; here the support extras mod carries it.
        mod_param(config, "support_extras.tree_wall_loops", wall_count);
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
          "[.MixedPerformance][MixedSupport][Line1][Performance]")
{
    BundledMods mods;
    TriangleMesh fixture = make_cube(24., 60., 20.);
    TriangleMesh ceiling = make_cube(60., 60., 2.);
    ceiling.translate(-18.f, 0.f, 40.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    line1(config, "support_type", "mixed(auto)");
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    line1(config, "mixed_normal_support_generator", line1_name(mnsgPrusa));
    line1(config, "mixed_tree_support_style", line1_name(mtssStrong));
    line1(config, "mixed_normal_coverage_threshold", 80.);
    line1(config, "mixed_selective_merge", true);
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

TEST_CASE("Mixed selective merge emits a single merged raft", "[MixedSupport][Line1][Integration][Raft]")
{
    BundledMods mods;
    struct GeneratorCombination {
        Line1MixedNormal normal;
        Line1MixedTree tree;
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
        line1(config, "support_type", "mixed(auto)");
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        line1(config, "mixed_normal_support_generator", line1_name(combination.normal));
        line1(config, "mixed_tree_support_style", line1_name(combination.tree));
        line1(config, "mixed_normal_coverage_threshold", 80.);
        line1(config, "mixed_selective_merge", true);
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
        CHECK(output.find("support_type = normal(auto)") != std::string::npos);
    }
}

TEST_CASE("Mixed selective merge supports common nozzle diameters", "[MixedSupport][Line1][Integration][Nozzle]")
{
    BundledMods mods;
    for (const double nozzle_diameter_mm : { 0.2, 0.4, 0.6, 0.8 }) {
        CAPTURE(nozzle_diameter_mm);
        TriangleMesh fixture = make_cube(16., 20., 4.);
        TriangleMesh ceiling = make_cube(40., 20., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        const double layer_height_mm = std::min(0.2, 0.5 * nozzle_diameter_mm);
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        line1(config, "support_type", "mixed(auto)");
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        line1(config, "mixed_normal_support_generator", line1_name(mnsgPrusa));
        line1(config, "mixed_normal_coverage_threshold", 80.);
        line1(config, "mixed_selective_merge", true);
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ nozzle_diameter_mm }));
        config.set_key_value("layer_height", new ConfigOptionFloat(layer_height_mm));
        config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height_mm));
        config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
        config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

        for (const Line1MixedTree tree_style : {
                mtssOrganic, mtssSlim, mtssStrong, mtssTreeHybrid }) {
            CAPTURE(int(tree_style));
            line1(config, "mixed_tree_support_style", line1_name(tree_style));

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
            CHECK(output.find("support_type = normal(auto)") != std::string::npos);
        }
    }
}

TEST_CASE("Mixed support preserves dedicated body and interface filaments",
          "[MixedSupport][Line1][Integration][MultiMaterial]")
{
    BundledMods mods;
    struct GeneratorCombination {
        Line1MixedNormal normal;
        Line1MixedTree tree;
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
        line1(config, "support_type", "mixed(auto)");
        config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
        line1(config, "mixed_normal_support_generator", line1_name(combination.normal));
        line1(config, "mixed_tree_support_style", line1_name(combination.tree));
        line1(config, "mixed_normal_coverage_threshold", 80.);
        line1(config, "mixed_selective_merge", true);
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
        CHECK(output.find("support_type = normal(auto)") != std::string::npos);
        CHECK(output.find("\nT1 ; change extruder\n") != std::string::npos);
        CHECK(output.find("\nT2 ; change extruder\n") != std::string::npos);
        CHECK(output.find("support material interface") != std::string::npos);
    }
}


TEST_CASE("Mixed Bunny60 different Z channels do not intersect",
          "[.MixedCrossZ][Mixed][Bunny]")
{
    BundledMods mods;
    const auto bunny_path = boost::filesystem::path(TEST_DATA_DIR).parent_path().parent_path() /
        "resources/handy_models/Stanford_Bunny.drc";
    TriangleMesh bunny;
    REQUIRE(load_drc(bunny_path.string().c_str(), &bunny));
    REQUIRE_FALSE(bunny.empty());
    for (const int variant : {0, 1, 2, 3}) {
        const char *selected_variant = std::getenv("MAGPIE_MIXED_AUDIT_VARIANT");
        if (selected_variant != nullptr && variant != std::atoi(selected_variant)) continue;
        const bool normal_control = std::getenv("MAGPIE_MIXED_AUDIT_NORMAL_CONTROL") != nullptr;
        const bool independent = variant != 0;
        const double model_height = variant == 2 ? 0.12 : variant == 3 ? 0.28 : 0.2;
        const double contact_gap = variant == 2 ? 0.17 : variant == 3 ? 0.13 : 0.2;
        for (const auto generator : {mnsgCura, mnsgPrusa}) {
            if (normal_control && generator != mnsgPrusa) continue;
            CAPTURE(independent, int(generator), model_height, contact_gap);
            DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
            config.set_key_value("enable_support", new ConfigOptionBool(true));
            line1(config, "support_type", normal_control ? "normal(auto)" : "mixed(auto)");
            config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
            line1(config, "mixed_normal_support_generator", line1_name(generator));
            line1(config, "mixed_tree_support_style", line1_name(mtssOrganic));
            line1(config, "mixed_normal_coverage_threshold", 50.);
            line1(config, "mixed_selective_merge", true);
            config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
            config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
            config.set_key_value("layer_height", new ConfigOptionFloat(model_height));
            config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
            config.set_key_value("support_top_z_distance", new ConfigOptionFloat(contact_gap));
            config.set_key_value("support_bottom_z_distance", new ConfigOptionFloat(contact_gap));
            config.set_key_value("independent_support_layer_height", new ConfigOptionBool(independent));
            Print print;
            REQUIRE_NOTHROW(init_and_process_print({bunny}, print, config));
            REQUIRE(print.objects().size() == 1);
            const auto &layers = print.objects().front()->support_layers();
            REQUIRE_FALSE(layers.empty());
            std::vector<std::map<float, Polygons>> coverage(layers.size());
            double max_path_height = 0.;
            for (size_t i=0; i<layers.size(); ++i) {
                mixed_audit_paths(layers[i]->support_fills, coverage[i]);
                for (auto &[height, polygons] : coverage[i]) {
                    max_path_height = std::max(max_path_height, double(height));
                    polygons = union_(polygons);
                }
            }
            double overlap_mm3 = 0.;
            double channel_overlap_mm3 = 0.;
            size_t pairs = 0;
            // Positive vertical penetration threshold: 1 micrometre, in mm.
            constexpr double min_penetration_mm = 0.000001;
            for (size_t i=0; i<layers.size(); ++i) {
                for (size_t j=i+1; j<layers.size(); ++j) {
                    if (layers[j]->print_z - layers[i]->print_z > max_path_height) break;
                    for (const auto &[hi, pi] : coverage[i]) {
                        for (const auto &[hj, pj] : coverage[j]) {
                            const double depth = layers[i]->print_z - std::max(layers[i]->print_z-hi, layers[j]->print_z-hj);
                            if (depth <= min_penetration_mm) continue;
                            const double mm2 = area(intersection_ex(pi,pj)) * SCALING_FACTOR * SCALING_FACTOR;
                            if (mm2 > 0.) {
                                overlap_mm3 += mm2*depth; ++pairs;
                                if (layers[i]->support_type != stInnerNormal || layers[j]->support_type != stInnerNormal)
                                    channel_overlap_mm3 += mm2*depth;
                                std::cout << "CROSS_Z_PAIR normal_control=" << normal_control
                                    << " zi=" << layers[i]->print_z << " zj=" << layers[j]->print_z
                                    << " hi=" << hi << " hj=" << hj << " type_i=" << int(layers[i]->support_type)
                                    << " type_j=" << int(layers[j]->support_type) << " area_mm2=" << mm2
                                    << " depth_mm=" << depth << std::endl;
                            }
                        }
                    }
                }
            }
            std::cout << "MIXED_CROSS_Z independent=" << independent << " generator=" << int(generator)
                      << " model_height=" << model_height << " contact_gap=" << contact_gap
                      << " layers=" << layers.size() << " pairs=" << pairs << " overlap_mm3=" << overlap_mm3
                      << " cross_channel_mm3=" << channel_overlap_mm3 << std::endl;
            // Total numerical volume dust budget, in cubic mm (one 0.01 mm cube).
            constexpr double overlap_dust_mm3 = 0.000001;
            // The 0.12 mm / 0.17 mm Prusa case also has six normal-only slivers
            // totaling 0.000453103 mm3 with Mixed disabled. Keep that separate
            // from the cross-channel regression; do not widen its tolerance.
            CHECK(channel_overlap_mm3 <= overlap_dust_mm3);
        }
    }
}

TEST_CASE("Support interface sublayer range is contact-first and clamped", "[SupportExtras][Line1]")
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

TEST_CASE("Normal support wall count zero keeps the upstream bed-layer sheath only",
          "[SupportExtras][Line1][Integration]")
{
    BundledMods mods;
    const auto support_has_closed_wall = [](int wall_count) {
        TriangleMesh fixture = make_cube(12., 20., 4.);
        TriangleMesh ceiling = make_cube(32., 20., 2.);
        ceiling.translate(0.f, 0.f, 20.f);
        fixture.merge(ceiling);

        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_key_value("enable_support", new ConfigOptionBool(true));
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
        config.set_key_value("support_base_pattern", new ConfigOptionEnum<SupportMaterialPattern>(smpRectilinear));
        line1(config, "support_wall_count", wall_count);
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
        // {bed layer has a closed wall, any layer above the bed has a closed wall}
        const bool bed = collection_has_closed_path_with_role(support_layers.front()->support_fills, erSupportMaterial);
        const bool above = std::any_of(support_layers.begin() + 1, support_layers.end(), [](const SupportLayer *layer) {
            return collection_has_closed_path_with_role(layer->support_fills, erSupportMaterial);
        });
        return std::make_pair(bed, above);
    };

    // Upstream always sheathes the bed flange; the wall count governs the layers above it.
    const auto zero = support_has_closed_wall(0);
    CHECK(zero.first);
    CHECK_FALSE(zero.second);
    const auto two = support_has_closed_wall(2);
    CHECK(two.first);
    CHECK(two.second);
}

TEST_CASE("Resin style preserves dedicated body and interface filaments",
          "[ResinSupport][Line1][Integration][MultiMaterial]")
{
    BundledMods mods;
    TriangleMesh fixture = make_cube(8., 8., 18.);
    TriangleMesh ceiling = make_cube(28., 20., 2.);
    ceiling.translate(-10.f, -6.f, 18.f);
    fixture.merge(ceiling);

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(3);
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    line1(config, "support_type", "resin(auto)");
    config.set_key_value("support_filament", new ConfigOptionInt(2));
    config.set_key_value("support_interface_filament", new ConfigOptionInt(3));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));
    line1(config, "resin_support_points_density_relative", 50);
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
    CHECK(output.find("\nT1") != std::string::npos);
    CHECK(output.find("\nT2 ; change extruder\n") != std::string::npos);
    CHECK(output.find("support material interface") != std::string::npos);
}

TEST_CASE("Elevated Resin support remains continuous when synchronized with a prime tower",
          "[ResinSupport][Line1][ElevatedSupportRegression]")
{
    BundledMods mods;
    const auto strategy = GENERATE(rstDefault, rstBranching);
    const bool synchronized = GENERATE(false, true);
    const bool half = GENERATE(false, true);
    const int raft = GENERATE(0, 2);
    const bool dense = GENERATE(false, true);
    const int support_walls = GENERATE(0, 1);
    const int interface_layers = GENERATE(0, 3);
    auto config = multifilament_config(2, {
        {"enable_support", true},
        {"support_filament", 2}, {"support_interface_filament", 2},
        {"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"support_half_layer_height", half}, {"raft_layers", raft},
        {"enable_prime_tower", synchronized}, {"independent_support_layer_height", !synchronized},
        {"single_extruder_multi_material", true}, {"use_relative_e_distances", true},
        {"layer_change_gcode", "G92 E0\n"}, {"skirt_loops", 0}, {"brim_type", "no_brim"},
    });
    line1(config, "support_type", "resin(auto)");
    line1(config, "resin_support_object_elevation", 1.2);
    line1(config, "resin_branching_support_object_elevation", 1.2);
    line1(config, "resin_support_tree_type", line1_name(strategy));
    config.set_key_value("filament_map", new ConfigOptionInts{1, 1});
    config.set_key_value("max_layer_height", new ConfigOptionFloats{0.2, 0.2});
    // Nightly's default tower position lies off the test bed (the H/2 tests place it the same way).
    config.set_key_value("wipe_tower_x", new ConfigOptionFloats{50.});
    config.set_key_value("wipe_tower_y", new ConfigOptionFloats{50.});
    line1(config, "support_wall_count", support_walls);
    config.set("support_interface_top_layers", interface_layers);
    if (dense) {
        config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
        config.set_key_value("support_base_pattern_spacing", new ConfigOptionFloat(0.));
    }
    Print print;
    Model model;
    init_print({make_cube(8., 8., 2.)}, print, model, config);
    CAPTURE(int(strategy), synchronized, half, raft, dense, support_walls, interface_layers);
    REQUIRE_NOTHROW(print.process());
    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());
    const double model_bottom_mm = object.layers().front()->bottom_z();
    double previous_top_mm = 0.;
    size_t below_model_layers = 0;
    // This fixture uses 0.2 mm layers; auto raft layers may be 0.3 mm.
    const double maximum_step_mm = raft ? 0.3 : 0.2;
    constexpr double z_tolerance_mm = 0.00001;
    for (const auto *layer : object.support_layers()) {
        INFO("support plane Z=" << layer->print_z << " H=" << layer->height << " extrusions=" << layer->has_extrusions());
        if (!layer->has_extrusions() || layer->print_z >= model_bottom_mm) continue;
        CAPTURE(previous_top_mm, layer->print_z, layer->height, model_bottom_mm);
        CHECK(layer->print_z - previous_top_mm <= maximum_step_mm + z_tolerance_mm);
        previous_top_mm = layer->print_z;
        ++below_model_layers;
    }
    CHECK(below_model_layers >= 3);
    // Allow the configured model-contact air gap, but not the elevation void.
    CHECK(model_bottom_mm - previous_top_mm <= maximum_step_mm + 0.2 + z_tolerance_mm);
    std::string output;
    REQUIRE_NOTHROW(output = gcode(print));
    REQUIRE_FALSE(output.empty());
    GCodeReader reader;
    reader.apply_config(config);
    std::set<float> support_z;
    reader.parse_buffer(output, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        if (line.extruding(state) && line.dist_XY(state) > 0. &&
            std::string(line.comment()).find("support material") != std::string::npos && line.new_Z(state) < model_bottom_mm)
            support_z.insert(line.new_Z(state));
    });
    REQUIRE(support_z.size() >= 3);
    double last_z_mm = 0.;
    for (const double z_mm : support_z) {
        CHECK(z_mm - last_z_mm <= maximum_step_mm + z_tolerance_mm);
        last_z_mm = z_mm;
    }
}

TEST_CASE("Consecutive support exports keep independent G-code state", "[SupportMaterial][Line1][StateIsolation]")
{
    const std::string raft = slice({ cube(20) }, {
        { "initial_layer_line_width", 0 }, { "enable_support", true }, { "raft_layers", 3 },
    });
    CHECK(raft.find("support material") != std::string::npos);

    const std::string overhang = slice({ TestMesh::overhang }, {
        { "layer_height", 0.4 }, { "initial_layer_print_height", 0.4 },
        { "enable_support", true }, { "skirt_loops", 1 }, { "skirt_distance", 0 },
        { "brim_type", "outer_only" }, { "brim_width", 5 },
    });
    CHECK(overhang.find("support material") != std::string::npos);
}

TEST_CASE("Cura support prints a full raft under the object", "[CuraSupport][Raft]")
{
    BundledMods mods;
    // Before: the Cura generator gave the raft no contact layer, so every raft layer came out empty
    // and the object, lifted by the raft height, printed in the air. Line 1 built the contact from
    // the first object layer, as the classic generator does.
    const int  raft_layers = GENERATE(2, 3);
    const bool overhang    = GENERATE(false, true);
    TriangleMesh fixture = make_cube(8., 8., 8.);
    if (overhang) {
        TriangleMesh ceiling = make_cube(24., 8., 1.2);
        ceiling.translate(-8.f, 0.f, 8.f);
        fixture.merge(ceiling);
    }
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"enable_support", true}, {"raft_layers", raft_layers}});
    line1(config, "support_type", "normal_cura(auto)");

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    const PrintObject &object = *print.objects().front();
    const SlicingParameters &slicing = object.slicing_parameters();
    CAPTURE(raft_layers, overhang);
    REQUIRE(object.config().support_generator.value == "cura");
    size_t raft_with_paths = 0;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z <= slicing.raft_contact_top_z + EPSILON)
            raft_with_paths += layer->has_extrusions();
    CHECK(raft_with_paths == size_t(raft_layers));
}

TEST_CASE("A raft without support stays a raft with a support module on", "[SupportGenerator][Raft]")
{
    BundledMods mods;
    // Before: a generator module ran whenever the support step did, also for "raft on, support off":
    // Resin replaced the raft footprint with its (empty) demand and Cura had no raft at all.
    const char *support_type = GENERATE("normal_cura(auto)", "resin(auto)");
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"initial_layer_print_height", 0.2},
        {"enable_support", false}, {"raft_layers", 3}});
    line1(config, "support_type", support_type);

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ make_cube(10., 10., 5.) }, print, config));
    const PrintObject &object = *print.objects().front();
    const SlicingParameters &slicing = object.slicing_parameters();
    CAPTURE(support_type);
    // Not lifted beyond the raft, and every raft layer prints.
    CHECK(object.layers().front()->bottom_z() == Catch::Approx(slicing.raft_contact_top_z + slicing.gap_raft_object));
    size_t raft_with_paths = 0;
    for (const SupportLayer *layer : object.support_layers())
        if (layer->print_z <= slicing.raft_contact_top_z + EPSILON)
            raft_with_paths += layer->has_extrusions();
    CHECK(raft_with_paths == 3);
}

TEST_CASE("A changed support module parameter regenerates the support", "[SupportGenerator][Invalidation]")
{
    BundledMods mods;
    // Before: the module parameters live in mod_settings, which no static config owns, so a change
    // only invalidated G-code export and the old support was exported.
    TriangleMesh fixture = make_cube(8., 8., 8.);
    TriangleMesh ceiling = make_cube(24., 8., 1.2);
    ceiling.translate(-8.f, 0.f, 8.f);
    fixture.merge(ceiling);
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    line1(config, "support_type", "normal_cura(auto)");

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({ fixture }, print, config));
    REQUIRE(print.objects().front()->is_step_done(posSupportMaterial));

    DynamicPrintConfig changed = print.full_print_config();
    const std::string stored = changed.opt_string("mod_settings");
    nlohmann::json settings = stored.empty() ? nlohmann::json::object() : nlohmann::json::parse(stored);
    settings["cura_support.join_distance"] = 7.5;
    changed.set_key_value("mod_settings", new ConfigOptionString(settings.dump()));
    print.apply(print.model(), changed);
    CHECK_FALSE(print.objects().front()->is_step_done(posSupportMaterial));
    CHECK(print.objects().front()->is_step_done(posSlice));
}
