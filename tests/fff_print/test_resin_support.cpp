#include <libslic3r/ModHost/SlotChoice.hpp>
#include <catch2/catch_all.hpp>

#include "libslic3r/Config.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// An 8 x 8 mm column under a 24 x 16 mm ceiling plate: the ceiling overhangs on every side.
TriangleMesh column_under_ceiling(double column_height_mm)
{
    TriangleMesh fixture = make_cube(8., 8., column_height_mm);
    TriangleMesh ceiling = make_cube(24., 16., 2.);
    ceiling.translate(-8.f, -4.f, float(column_height_mm));
    fixture.merge(ceiling);
    return fixture;
}

// A process config with resin support on and the given resin_support mod values.
DynamicPrintConfig resin_config(nlohmann::json mod_values)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(1));
    mod_values["resin_support.enable"] = true;
    config.set_key_value("mod_settings", new ConfigOptionString(mod_values.dump()));
    return config;
}

} // namespace

TEST_CASE("The bundled mods load without errors", "[ResinSupport]")
{
    BundledMods mods;
    CHECK(mods.errors.empty());
    CHECK(ModHost::active_mods()->mods().size() >= 2);
}

TEST_CASE("Resin support Default and Branching print support under an overhang", "[ResinSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const std::string tree_type = GENERATE("default", "branching");
    const double      nozzle_mm = GENERATE(0.4, 0.6);
    CAPTURE(tree_type, nozzle_mm);

    DynamicPrintConfig config = resin_config({{"resin_support.tree_type", tree_type}, {"resin_support.points_density", 50}});
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({nozzle_mm}));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(2));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(18.)}, print, config));
    const PrintObject &object = *print.objects().front();
    CHECK(ModHost::support_generator_name(print.full_print_config(), object.config()) == "resin");
    // The default 5 mm elevation lifts the object.
    CHECK(object.slicing_parameters().object_print_z_min > 4.9);
    const auto layers = object.support_layers();
    REQUIRE_FALSE(layers.empty());
    // Support under the ceiling, which starts 18 mm above the object's bottom (lifted 5 mm).
    CHECK(std::any_of(layers.begin(), layers.end(), [](const SupportLayer *l) { return l->print_z > 15. && l->has_extrusions(); }));
    const std::string output = gcode(print);
    CHECK(output.find("support material interface") != std::string::npos);
}

TEST_CASE("Resin support fills the elevation gap with printable support layers", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const std::string tree_type = GENERATE("default", "branching");
    const int         raft      = GENERATE(0, 2);
    CAPTURE(tree_type, raft);

    DynamicPrintConfig config = resin_config({{"resin_support.tree_type", tree_type},
                                              {"resin_support.default_object_elevation", 1.2},
                                              {"resin_support.branching_object_elevation", 1.2}});
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));
    config.set_key_value("raft_layers", new ConfigOptionInt(raft));
    // Support layers may be up to max_layer_height thick; hold it at the layer height.
    config.set_key_value("max_layer_height", new ConfigOptionFloats({0.2}));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({make_cube(8., 8., 2.)}, print, config));
    const PrintObject &object       = *print.objects().front();
    const double       model_bottom = object.layers().front()->bottom_z(); // mm
    // Support layer steps below the object: one 0.2 mm layer, or a 0.3 mm raft layer at most.
    const double max_step_mm  = raft ? 0.3 : 0.2;
    const double tolerance_mm = 1e-5;
    double       previous_top = 0.;
    size_t       below_model  = 0;
    for (const SupportLayer *layer : object.support_layers()) {
        if (!layer->has_extrusions() || layer->print_z >= model_bottom)
            continue;
        CAPTURE(previous_top, layer->print_z);
        CHECK(layer->print_z - previous_top <= max_step_mm + tolerance_mm);
        previous_top = layer->print_z;
        ++below_model;
    }
    CHECK(below_model >= 3);
    // No gap below the object larger than one layer plus the 0.2 mm top Z distance.
    CHECK(model_bottom - previous_top <= max_step_mm + 0.2 + tolerance_mm);
}

TEST_CASE("Resin support with zero elevation keeps the object on the bed", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    DynamicPrintConfig config = resin_config({{"resin_support.default_object_elevation", 0.}, {"resin_support.points_density", 50}});
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(8.)}, print, config));
    const PrintObject &object = *print.objects().front();
    CHECK_THAT(object.slicing_parameters().object_print_z_min, Catch::Matchers::WithinAbs(0., 1e-9));
    CHECK_FALSE(object.support_layers().empty());
}

TEST_CASE("Resin support lifts the object only while support is on", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    DynamicPrintConfig config = resin_config({});
    config.set_key_value("enable_support", new ConfigOptionBool(false));
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(8.)}, print, config));
    CHECK_THAT(print.objects().front()->slicing_parameters().object_print_z_min, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Resin support lifts only an object whose support it generates", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // Mixed support's slot declaration outranks resin support's, so mixed support is picked for the object: the
    // resin lift must not apply, as Mixed fills no gap under a lifted object.
    DynamicPrintConfig config = resin_config({{"mixed_support.enable", true}, {"resin_support.default_object_elevation", 5.}});
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(8.)}, print, config));
    const PrintObject &object = *print.objects().front();
    REQUIRE(ModHost::support_generator_name(print.full_print_config(), object.config()) == "mixed");
    CHECK_THAT(object.slicing_parameters().object_print_z_min, Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Resin support with enforcers only keeps the object on the bed", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // Enforcers only gives the object's bottom no support points, so a lifted object would hang in the air.
    DynamicPrintConfig config = resin_config({{"resin_support.enforcers_only", true}, {"resin_support.default_object_elevation", 5.}});
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(8.)}, print, config));
    const PrintObject &object = *print.objects().front();
    REQUIRE(ModHost::support_generator_name(print.full_print_config(), object.config()) == "resin");
    CHECK_THAT(object.slicing_parameters().object_print_z_min, Catch::Matchers::WithinAbs(0., 1e-9));
    // Its first layer is printed: nothing between the bed and the object is left empty.
    CHECK_THAT(object.layers().front()->bottom_z(), Catch::Matchers::WithinAbs(0., 1e-9));
}

TEST_CASE("Resin support automatic points follow the support threshold angle", "[ResinSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // A 30 x 30 x 20 mm block whose top is shifted 10 mm in X: one side overhangs at 63 degrees
    // from the horizontal. A 30 degree threshold leaves it unsupported, an 80 degree one does not.
    const auto slice_with_threshold = [](int threshold_deg) {
        indexed_triangle_set its;
        its.vertices = {{0.f, 0.f, 0.f},   {30.f, 0.f, 0.f},  {30.f, 30.f, 0.f},  {0.f, 30.f, 0.f},
                        {10.f, 0.f, 20.f}, {40.f, 0.f, 20.f}, {40.f, 30.f, 20.f}, {10.f, 30.f, 20.f}};
        its.indices  = {{0, 2, 1}, {0, 3, 2}, {4, 5, 6}, {4, 6, 7}, {0, 1, 5}, {0, 5, 4},
                        {1, 2, 6}, {1, 6, 5}, {2, 3, 7}, {2, 7, 6}, {3, 0, 4}, {3, 4, 7}};
        TriangleMesh wall(std::move(its));
        DynamicPrintConfig config = resin_config({{"resin_support.default_object_elevation", 0.}});
        config.set_key_value("support_threshold_angle", new ConfigOptionInt(threshold_deg));
        Print print;
        init_and_process_print({wall}, print, config);
        size_t layers_with_support = 0;
        for (const SupportLayer *layer : print.objects().front()->support_layers())
            layers_with_support += layer->has_extrusions();
        return layers_with_support;
    };
    CHECK(slice_with_threshold(80) > slice_with_threshold(30));
}

TEST_CASE("Every resin support setting survives a slicing mutation", "[ResinSupport][Mutation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // Extreme values of each per-strategy setting, as the resin_support mod defines them.
    static const std::array<std::pair<const char *, nlohmann::json>, 16> mutations{{
        {"head_front_diameter", 2.},
        {"head_width", 5.},
        {"pillar_diameter", 15.},
        {"small_pillar_diameter_percent", 1.},
        {"max_bridges_on_pillar", 50},
        {"max_weight_on_model", 0.},
        {"pillar_connection_mode", "cross"},
        {"buildplate_only", true},
        {"pillar_widening_factor", 1.},
        {"base_diameter", 30.},
        {"base_height", 5.},
        {"base_safety_distance", 10.},
        {"critical_angle", 90.},
        {"max_bridge_length", 0.},
        {"max_pillar_link_distance", 0.},
        {"object_elevation", 15.},
    }};
    std::vector<nlohmann::json> cases;
    for (const char *tree_type : {"default", "branching"})
        for (const auto &[name, value] : mutations)
            cases.push_back({{"resin_support.tree_type", tree_type}, {std::string("resin_support.") + tree_type + "_" + name, value}});
    cases.push_back({{"resin_support.points_density", 250}});
    cases.push_back({{"resin_support.enforcers_only", true}});
    REQUIRE(cases.size() == 34);

    for (const nlohmann::json &values : cases) {
        CAPTURE(values.dump());
        nlohmann::json mod_values = values;
        if (!mod_values.contains("resin_support.points_density"))
            mod_values["resin_support.points_density"] = 25;
        Print print;
        REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(12.)}, print, resin_config(mod_values)));
        CHECK(print.objects().front()->is_step_done(posSupportMaterial));
        // Every mutation still supports the ceiling, except enforcers-only with no enforcer.
        if (!mod_values.contains("resin_support.enforcers_only"))
            CHECK_FALSE(print.objects().front()->support_layers().empty());
    }
}
