#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Format/DRC.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/Builtin/MixedSupport.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem/path.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "test_helpers.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
namespace Mixed = Slic3r::ModHost::MixedSupport;

namespace {

Polygons rectangle(double min_x, double min_y, double max_x, double max_y) // mm
{
    return {Polygon{Point::new_scale(min_x, min_y), Point::new_scale(max_x, min_y), Point::new_scale(max_x, max_y),
                    Point::new_scale(min_x, max_y)}};
}

bool same_area(const Polygons &a, const Polygons &b) { return diff(a, b).empty() && diff(b, a).empty(); }

// A 20 x 10 mm ceiling at Z = 20 mm over an 8 x 10 mm pillar 10 mm tall under its left side:
// 60 % of the ceiling's overhang reaches the bed straight down, 40 % is over the pillar.
TriangleMesh pillar_under_ceiling()
{
    TriangleMesh fixture = make_cube(8., 10., 10.);
    TriangleMesh ceiling = make_cube(20., 10., 2.);
    ceiling.translate(0.f, 0.f, 20.f);
    fixture.merge(ceiling);
    return fixture;
}

DynamicPrintConfig mixed_config(nlohmann::json mod_values)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("enable_support", new ConfigOptionBool(true));
    config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(stNormalAuto));
    mod_values["mixed_support.enable"] = true;
    config.set_key_value("mod_settings", new ConfigOptionString(mod_values.dump()));
    return config;
}

bool any_layer(const PrintObject &object, SupportInnerType type)
{
    const auto layers = object.support_layers();
    return std::any_of(layers.begin(), layers.end(), [type](const SupportLayer *l) { return l->support_type == type; });
}

} // namespace

TEST_CASE("Mixed support coverage threshold is inclusive", "[MixedSupport]")
{
    std::vector<Polygons> demand(2), shadow(2);
    demand[1] = rectangle(0., 0., 10., 10.);
    shadow[1] = rectangle(0., 0., 2., 10.); // 20 mm2 over the object, 80 mm2 (80 %) reachable

    const Mixed::Plan exact = Mixed::plan(demand, shadow, 80., false);
    REQUIRE(exact.decisions.size() == 1);
    CHECK(exact.decisions.front() == Mixed::Channel::Normal);
    CHECK(same_area(exact.normal_mask[1], demand[1]));
    CHECK(exact.tree_mask[1].empty());

    // Threshold just below and just above 80 percent.
    CHECK(Mixed::plan(demand, shadow, 80. - 1e-9, false).decisions.front() == Mixed::Channel::Normal);
    CHECK(Mixed::plan(demand, shadow, 80. + 1e-9, false).decisions.front() == Mixed::Channel::Tree);
    CHECK(Mixed::plan(demand, shadow, 0., false).decisions.front() == Mixed::Channel::Normal);
    CHECK(Mixed::plan(demand, shadow, 100., false).decisions.front() == Mixed::Channel::Tree);
    CHECK(Mixed::plan(demand, std::vector<Polygons>(2), 100., false).decisions.front() == Mixed::Channel::Normal);
}

TEST_CASE("Mixed support judges each layer's islands on their own", "[MixedSupport]")
{
    std::vector<Polygons> split(2), shadow(2);
    split[1] = rectangle(0., 0., 2., 2.);
    append(split[1], rectangle(10., 0., 12., 2.));
    shadow[1] = rectangle(10., 0., 12., 2.);
    const Mixed::Plan two = Mixed::plan(split, shadow, 100., false);
    CHECK(two.decisions.size() == 2);
    CHECK(two.has_normal());
    CHECK(two.has_tree());

    // A bridge on layer 2 over both islands of layer 1 does not join them into one decision.
    std::vector<Polygons> bridged(3);
    bridged[1] = rectangle(0., 0., 2., 2.);
    append(bridged[1], rectangle(6., 0., 8., 2.));
    bridged[2] = rectangle(1.5, 0., 6.5, 2.);
    const Mixed::Plan per_layer = Mixed::plan(bridged, std::vector<Polygons>(3), 100., false);
    CHECK(per_layer.decisions.size() == 3);

    // The order of the input polygons does not change the result.
    std::vector<Polygons> reversed = bridged;
    std::reverse(reversed[1].begin(), reversed[1].end());
    const Mixed::Plan reversed_plan = Mixed::plan(reversed, std::vector<Polygons>(3), 100., false);
    REQUIRE(reversed_plan.decisions.size() == per_layer.decisions.size());
    for (size_t i = 0; i < bridged.size(); ++i) {
        CHECK(same_area(reversed_plan.normal_mask[i], per_layer.normal_mask[i]));
        CHECK(same_area(reversed_plan.tree_mask[i], per_layer.tree_mask[i]));
    }
}

TEST_CASE("Mixed support selective merge gives the reachable part to normal support", "[MixedSupport]")
{
    std::vector<Polygons> demand(2), shadow(2);
    demand[1] = rectangle(0., 0., 10., 10.);
    shadow[1] = rectangle(0., 0., 2., 10.); // 80 % reachable

    const Mixed::Plan off = Mixed::plan(demand, shadow, 81., false);
    REQUIRE(off.decisions.size() == 1);
    CHECK(off.decisions.front() == Mixed::Channel::Tree);
    CHECK(off.normal_mask[1].empty());
    CHECK(same_area(off.tree_mask[1], demand[1]));

    for (const double threshold : {0., 80., 81.}) {
        CAPTURE(threshold);
        const Mixed::Plan on = Mixed::plan(demand, shadow, threshold, true);
        REQUIRE(on.decisions.size() == 1);
        CHECK(on.decisions.front() == Mixed::Channel::Split);
        CHECK(same_area(on.normal_mask[1], rectangle(2., 0., 10., 10.)));
        CHECK(same_area(on.tree_mask[1], rectangle(0., 0., 2., 10.)));
        CHECK(intersection_ex(on.normal_mask[1], on.tree_mask[1]).empty());
    }
}

TEST_CASE("Mixed support prints normal and tree support in one object", "[MixedSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const std::string normal = GENERATE("orca", "cura");
    const std::string style  = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    CAPTURE(normal, style);
    DynamicPrintConfig config = mixed_config({{"mixed_support.normal_generator", normal},
                                              {"mixed_support.tree_style", style},
                                              {"mixed_support.coverage_threshold", 80.},
                                              {"mixed_support.selective_merge", true}});
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(false));
    config.set_key_value("support_expansion", new ConfigOptionFloat(0.));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.4));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({pillar_under_ceiling()}, print, config));
    const PrintObject &object = *print.objects().front();
    CHECK(object.config().support_generator.value == "mixed");
    REQUIRE_FALSE(object.support_layers().empty());
    // Normal support stands on the bed beside the pillar; the tree reaches over the pillar. They
    // meet on shared layers.
    CHECK((any_layer(object, stInnerNormal) || any_layer(object, stInnerMixed)));
    CHECK((any_layer(object, stInnerTree) || any_layer(object, stInnerMixed)));
    CHECK(any_layer(object, stInnerMixed));
    CHECK_FALSE(gcode(print).empty());
}

TEST_CASE("Mixed support prints one raft", "[MixedSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const auto [normal, style] = GENERATE(table<const char *, const char *>({{"orca", "organic"}, {"cura", "tree_strong"}}));
    CAPTURE(normal, style);
    DynamicPrintConfig config = mixed_config({{"mixed_support.normal_generator", normal},
                                              {"mixed_support.tree_style", style},
                                              {"mixed_support.coverage_threshold", 80.},
                                              {"mixed_support.selective_merge", true}});
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("raft_layers", new ConfigOptionInt(2));
    config.set_key_value("raft_expansion", new ConfigOptionFloat(2.));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.4));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({pillar_under_ceiling()}, print, config));
    const PrintObject &object = *print.objects().front();
    const auto         layers = object.support_layers();
    REQUIRE_FALSE(layers.empty());
    size_t raft_layers = 0;
    for (size_t i = 0; i < layers.size(); ++i) {
        // Strictly rising Z: no two layers (one per generator) at the same height.
        if (i > 0)
            CHECK(layers[i]->print_z > layers[i - 1]->print_z + EPSILON);
        if (layers[i]->print_z <= object.slicing_parameters().raft_contact_top_z + EPSILON) {
            ++raft_layers;
            CHECK(layers[i]->has_extrusions());
        }
    }
    CHECK(raft_layers >= object.slicing_parameters().raft_layers());
}

TEST_CASE("Mixed support works with common nozzle diameters", "[MixedSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const double      nozzle_mm = GENERATE(0.2, 0.4, 0.6, 0.8);
    const std::string style     = GENERATE("organic", "tree_slim", "tree_strong", "tree_hybrid");
    CAPTURE(nozzle_mm, style);
    TriangleMesh fixture = make_cube(16., 20., 4.);
    TriangleMesh ceiling = make_cube(40., 20., 2.);
    ceiling.translate(0.f, 0.f, 20.f);
    fixture.merge(ceiling);

    const double       layer_height_mm = std::min(0.2, 0.5 * nozzle_mm);
    DynamicPrintConfig config          = mixed_config(
        {{"mixed_support.tree_style", style}, {"mixed_support.coverage_threshold", 80.}, {"mixed_support.selective_merge", true}});
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({nozzle_mm}));
    config.set_key_value("layer_height", new ConfigOptionFloat(layer_height_mm));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(layer_height_mm));
    config.set_key_value("support_interface_top_layers", new ConfigOptionInt(0));
    config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(0));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({fixture}, print, config));
    const PrintObject &object = *print.objects().front();
    REQUIRE_FALSE(object.support_layers().empty());
    CHECK((any_layer(object, stInnerNormal) || any_layer(object, stInnerMixed)));
    CHECK((any_layer(object, stInnerTree) || any_layer(object, stInnerMixed)));
}

// Hidden: slices the full Stanford Bunny twice (about a minute). Run with "[MixedBunny]".
TEST_CASE("Mixed support splits the Stanford Bunny's demand without gaps or overlaps", "[.MixedBunny][MixedSupport]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    const boost::filesystem::path bunny_path = boost::filesystem::path(PROFILES_DIR) / ".." / "handy_models" / "Stanford_Bunny.drc";
    TriangleMesh bunny;
    REQUIRE(load_drc(bunny_path.string().c_str(), &bunny));
    const bool selective = GENERATE(false, true);
    CAPTURE(selective);

    DynamicPrintConfig config = mixed_config({{"mixed_support.normal_generator", "cura"},
                                              {"mixed_support.tree_style", "organic"},
                                              {"mixed_support.coverage_threshold", 50.},
                                              {"mixed_support.selective_merge", selective}});
    config.set_key_value("support_on_build_plate_only", new ConfigOptionBool(true));
    config.set_key_value("support_threshold_angle", new ConfigOptionInt(60));
    config.set_key_value("layer_height", new ConfigOptionFloat(0.2));
    config.set_key_value("initial_layer_print_height", new ConfigOptionFloat(0.2));

    Print print;
    REQUIRE_NOTHROW(init_and_process_print({bunny}, print, config));
    const PrintObject          &object = *print.objects().front();
    const Mixed::ModSettings    s      = Mixed::read_settings(object);
    const std::vector<Polygons> demand = Mixed::detect_demand(object, s);
    const std::vector<Polygons> shadow = buildplate_covered_by_object(object);
    const Mixed::Plan           p      = Mixed::plan(demand, shadow, 50., selective);
    CHECK(p.has_normal());
    CHECK(p.has_tree());

    // Demand left unassigned, and assigned to both, summed over all layers (scaled area).
    long double unassigned = 0., overlap = 0., reachable_not_normal = 0.;
    for (size_t i = 0; i < demand.size(); ++i) {
        const ExPolygons layer_demand = union_ex(demand[i]);
        if (layer_demand.empty())
            continue;
        Polygons assigned = p.normal_mask[i];
        append(assigned, p.tree_mask[i]);
        unassigned += area(diff_ex(layer_demand, union_(assigned)));
        overlap += area(intersection_ex(p.normal_mask[i], p.tree_mask[i]));
        if (selective)
            reachable_not_normal += area(diff_ex(shadow[i].empty() ? layer_demand : diff_ex(layer_demand, shadow[i]), p.normal_mask[i]));
    }
    const long double to_mm2 = static_cast<long double>(SCALING_FACTOR) * static_cast<long double>(SCALING_FACTOR);
    // Clipper rounding dust, in mm2 over the whole Bunny: far below one 0.4 mm extrusion square.
    constexpr long double dust_mm2 = 1e-5L;
    CHECK(unassigned * to_mm2 <= dust_mm2);
    CHECK(overlap * to_mm2 <= dust_mm2);
    CHECK(reachable_not_normal * to_mm2 <= dust_mm2);
    CHECK((any_layer(object, stInnerNormal) || any_layer(object, stInnerMixed)));
    CHECK((any_layer(object, stInnerTree) || any_layer(object, stInnerMixed)));
}
