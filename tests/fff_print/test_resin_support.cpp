#include <libslic3r/ModHost/SlotChoice.hpp>
#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Config.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"

#include "libslic3r/Format/DRC.hpp"
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
    // PERMANENT DIAGNOSTIC: which mod failed to load and why.
    for (const std::string &error : mods.errors)
        UNSCOPED_INFO(error);
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

TEST_CASE("Resin support on a raft lays the raft under the whole object without contacts", "[ResinSupport][Raft]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // Enforcers only and none painted: the generator places no contact. Seeds 2218, 2310 and 2347 of the U1 random
    // test printed the object over an empty raft.
    DynamicPrintConfig config = resin_config({{"resin_support.enforcers_only", true}, {"resin_support.default_object_elevation", 0.}});
    config.set_key_value("raft_layers", new ConfigOptionInt(2));
    Print print;
    REQUIRE_NOTHROW(init_and_process_print({column_under_ceiling(8.)}, print, config));
    const PrintObject &object = *print.objects().front();
    REQUIRE(ModHost::support_generator_name(print.full_print_config(), object.config()) == "resin");
    REQUIRE_FALSE(object.support_layers().empty());
    // The bed layer of the raft covers the column's 8 x 8 mm foot, in mm2.
    const double column_foot_mm2 = 8. * 8.;
    const SupportLayer &raft_bottom = *object.support_layers().front();
    CHECK(raft_bottom.has_extrusions());
    Polygons covered;
    raft_bottom.support_fills.polygons_covered_by_width(covered, float(SCALED_EPSILON));
    CHECK(area(union_(covered)) * SCALING_FACTOR * SCALING_FACTOR >= column_foot_mm2);
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
        for (const auto &[name, value] : mutations) {
            nlohmann::json values = {{"resin_support.tree_type", tree_type}, {std::string("resin_support.") + tree_type + "_" + name, value}};
            // A head as wide as a 15 mm pillar does not fit under the default 5 mm lift (at least 8 mm with the default
            // tree), and a bottom without heads is refused; lift it by the pillar's width.
            if (std::string(name) == "pillar_diameter")
                values[std::string("resin_support.") + tree_type + "_object_elevation"] = 15.;
            cases.push_back(values);
        }
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

TEST_CASE("Turning on a mod that takes the support over slices the lifted object again", "[ResinSupport][Elevation]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // Resin support lifts the object by its elevation; mixed support (priority 2, its enable declares
    // posSupportMaterial only) outranks it once on, and the lift ends with it. Before (review 2026-10-22) the slice
    // kept the lift: the object stayed in the air over support that was no longer there.
    DynamicPrintConfig config = resin_config({{"resin_support.default_object_elevation", 5.}});
    Print print;
    Model model;
    init_print({column_under_ceiling(8.)}, print, model, config);
    print.process();
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.slicing_parameters().object_print_z_min > 4.);
    nlohmann::json values = nlohmann::json::parse(config.opt_string("mod_settings"));
    values["mixed_support.enable"] = true;
    config.set_key_value("mod_settings", new ConfigOptionString(values.dump()));
    print.apply(model, config);
    CHECK_FALSE(object.is_step_done(posSlice));
    print.process();
    REQUIRE(ModHost::support_generator_name(print.full_print_config(), object.config()) == "mixed");
    CHECK_THAT(object.slicing_parameters().object_print_z_min, Catch::Matchers::WithinAbs(0., 1e-9));
}

namespace {

// Seed 6828 of the U1 random test: branching resin support of the 20 mm Orca cube, with that seed's whole config (its
// four toolheads, widths, support and mod settings, as the CLI wrote them into the G-code) lifted elevation_mm, with
// layer_mm layers and the given nozzles (mm). Returns the slicing error, empty when it slices. The data leaves out the
// six enum keys (extruder_type, nozzle_type, nozzle_volume_type, overhang_fan_threshold, retract_lift_enforce,
// z_hop_types): load_from_ini crashed on each of them alone here, and none of them sizes the support heads.
std::string lifted_cube_error(double elevation_mm, double layer_mm, const std::vector<double> &nozzles_mm)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.load_from_ini(std::string(TEST_DATA_DIR) + "/resin_support_seed6828.ini", ForwardCompatibilitySubstitutionRule::Enable);
    nlohmann::json mod_values = nlohmann::json::parse(config.opt_string("mod_settings"));
    mod_values["resin_support.branching_object_elevation"] = elevation_mm;
    config.set_key_value("mod_settings", new ConfigOptionString(mod_values.dump()));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats(nozzles_mm));
    std::vector<double> max_layer_mm;
    for (double nozzle_mm : nozzles_mm)
        max_layer_mm.push_back(0.75 * nozzle_mm);
    config.set_key_value("max_layer_height", new ConfigOptionFloats(max_layer_mm));
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_mm));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_mm));
    const boost::filesystem::path cube_path = boost::filesystem::path(PROFILES_DIR) / ".." / "handy_models" / "OrcaCube_v2.drc";
    TriangleMesh cube;
    if (!load_drc(cube_path.string().c_str(), &cube))
        return "cannot load " + cube_path.string();
    Print print;
    try {
        init_and_process_print({cube}, print, config);
    } catch (const std::exception &e) {
        return e.what();
    }
    return {};
}

} // namespace

TEST_CASE("A lifted object whose bottom gets no head is refused with the elevation it needs", "[ResinSupport][Elevation][Regression]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    // The seed's absolute 0.58 mm support width scales to 1.16 mm on its 0.8 mm nozzle (the nozzles differ in size), so
    // its heads need at least 2.4 mm of lift; with one nozzle size it stays 0.58 mm and 1.7 mm is enough (the warning
    // logged before the refusal printed 1.16 mm; the CLI measured both lifts).
    SECTION("no head fits under the bottom") {
        // Before: "No support was generated", without what to change.
        const std::string error = lifted_cube_error(2.0, 0.26, {0.8, 0.4, 0.4, 0.6});
        CAPTURE(error);
        CHECK(error.find("would print in the air") != std::string::npos);
        CHECK(error.find("at least") != std::string::npos);
    }
    SECTION("support elsewhere on the bed does not hide the floating bottom") {
        // Before: with 0.2 mm layers the letters' overhangs got pillars on the bed, so the slice passed with most of
        // the bottom face in the air.
        const std::string error = lifted_cube_error(2.0, 0.2, {0.8, 0.4, 0.4, 0.6});
        CAPTURE(error);
        CHECK(error.find("would print in the air") != std::string::npos);
        // All nine bottom points are headless; the letters' five heads (numbered as branching tree leaves) once
        // counted as heads of bottom points 0-4, giving "4 of the 9".
        CHECK(error.find("9 of the 9") != std::string::npos);
    }
    SECTION("a higher lift fits the heads") {
        const std::string error = lifted_cube_error(3.0, 0.26, {0.8, 0.4, 0.4, 0.6});
        CAPTURE(error);
        CHECK(error.empty());
    }
    SECTION("a smaller support nozzle fits the heads in the same lift") {
        const std::string error = lifted_cube_error(2.0, 0.26, {0.4, 0.4, 0.4, 0.4});
        CAPTURE(error);
        CHECK(error.empty());
    }
}
