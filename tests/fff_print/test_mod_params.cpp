// MAGPIE: mod parameters in a print: what a changed value redoes, and how object and region values
// reach a region (ModHost/ModSet.hpp, PrintApply.cpp, PrintObject.cpp).
#include <catch2/catch_all.hpp>

#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <memory>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A demo mod for the test's lifetime: parameters for each kind of "invalidates", and a rule.
struct DemoMod
{
    std::shared_ptr<const ModHost::ModSet> previous = ModHost::active_mods();
    DemoMod()
    {
        ModHost::set_active_mods(std::make_shared<const ModHost::ModSet>(ModHost::ModSet::build({ModHost::ModSet::parse_mod(R"({
            "id": "demo", "version": "1",
            "params": {
                "support": {"type": "float", "default": 1, "invalidates": ["posSupportMaterial"]},
                "walls":   {"type": "int",   "default": 2, "invalidates": ["none"]},
                "silent":  {"type": "float", "default": 1},
                "gap":     {"type": "float", "default": 1, "scope": "region", "invalidates": ["posPerimeters"]},
                "lift":    {"type": "float", "default": 1, "scope": "object", "invalidates": ["posSupportMaterial"]}
            },
            "rules": [ {"target": "wall_loops", "value": "demo.walls"} ]
        })")})));
    }
    ~DemoMod() { ModHost::set_active_mods(previous); }
};

struct Steps
{
    bool slice, perimeters, support;
};
Steps done(const PrintObject &object)
{
    return {object.is_step_done(posSlice), object.is_step_done(posPerimeters), object.is_step_done(posSupportMaterial)};
}

} // namespace

TEST_CASE("A changed mod parameter redoes the steps it declares and no more", "[ModHost]")
{
    DemoMod            demo;
    Print              print;
    Model              model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}, {"wall_loops", 2}});
    init_print({cube(20.)}, print, model, config);
    print.process();
    auto apply_values = [&](const std::string &values) {
        config.set_key_value("mod_settings", new ConfigOptionString(values));
        print.apply(model, config);
        return done(*print.objects().front());
    };

    // "support" declares posSupportMaterial: slices and walls stay.
    Steps s = apply_values(R"({"demo.support": 2})");
    CHECK(s.slice);
    CHECK(s.perimeters);
    CHECK_FALSE(s.support);
    print.process();

    // "walls" acts through its rule only: wall_loops 2 -> 3 redoes the walls (Orca's own invalidation of
    // wall_loops), the slices stay.
    s = apply_values(R"({"demo.support": 2, "demo.walls": 3})");
    CHECK(s.slice);
    CHECK_FALSE(s.perimeters);
    print.process();

    // The same value written again changes nothing.
    s = apply_values(R"({"demo.walls": 3, "demo.support": 2})");
    CHECK(s.slice);
    CHECK(s.perimeters);
    CHECK(s.support);

    // "silent" declares nothing, so everything is redone from the slices.
    s = apply_values(R"({"demo.support": 2, "demo.walls": 3, "demo.silent": 5})");
    CHECK_FALSE(s.slice);
}

TEST_CASE("Object and region mod values reach the object, region values joining per parameter", "[ModHost]")
{
    DemoMod            demo;
    Print              print;
    Model              model;
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({{"layer_height", 0.2}});
    init_print({cube(20.)}, print, model, config);
    ModelObject *object = model.objects.front();
    object->config.set_key_value("object_mod_settings", new ConfigOptionString(R"({"demo.lift": 2})"));
    object->config.set_key_value("region_mod_settings", new ConfigOptionString(R"({"demo.gap": 2, "other.x": 1})"));
    object->volumes.front()->config.set_key_value("region_mod_settings", new ConfigOptionString(R"({"demo.gap": 3})"));
    print.apply(model, config);
    print.process();
    const PrintObject &print_object = *print.objects().front();
    CHECK(print_object.config().object_mod_settings.value == R"({"demo.lift": 2})");
    REQUIRE(print_object.num_printing_regions() == 1);
    // The part's gap replaces the object's, the object's other value stays.
    CHECK(print_object.printing_region(0).config().region_mod_settings.value == R"({"demo.gap":3,"other.x":1})");

    // A region value redoes what the parameter declares (walls), not the slices.
    object->volumes.front()->config.set_key_value("region_mod_settings", new ConfigOptionString(R"({"demo.gap": 4})"));
    print.apply(model, config);
    Steps s = done(*print.objects().front());
    CHECK(s.slice);
    CHECK_FALSE(s.perimeters);
    print.process();

    // An object value likewise (support).
    object->config.set_key_value("object_mod_settings", new ConfigOptionString(R"({"demo.lift": 3})"));
    print.apply(model, config);
    s = done(*print.objects().front());
    CHECK(s.slice);
    CHECK(s.perimeters);
    CHECK_FALSE(s.support);
}
