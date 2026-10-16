// MAGPIE: low temperature support interface (resources/mods/low_temp_interface.json), ported from Magpie line 1's
// test_support_material.cpp low temperature interface cases.
#include <catch2/catch_all.hpp>

#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

constexpr std::string_view begin_marker         = "low-temperature support interface begin";
constexpr std::string_view end_marker           = "low-temperature support interface end";
constexpr std::string_view auxiliary_fan_marker = "low-temperature support interface auxiliary cooling";
constexpr std::string_view wiping_marker        = "low-temperature support interface nozzle brushing";

DynamicPrintConfig low_temperature_config(bool enabled, bool auxiliary_fan_toggle = false, bool wiping_toggle = false,
                                          bool auxiliary_fan_supported = true)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", true },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", 0 },
        { "single_nozzle_low_temperature_interface", enabled },
        { "support_interface_temperature", 170 },
        { "support_interface_auxiliary_fan_speed", 100 },
        { "support_interface_heating_time", 0.0 },
        { "support_interface_auxiliary_fan_cooling_on_temperature_change", auxiliary_fan_toggle },
        { "support_interface_nozzle_wiping_on_temperature_change", wiping_toggle },
        { "auxiliary_fan", auxiliary_fan_supported },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 },
        { "nozzle_temperature", 220 },
        { "nozzle_temperature_initial_layer", 220 },
    });
    config.set_key_value("support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value("support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    return config;
}

struct LayerTrace
{
    std::vector<std::string> roles;        // ;TYPE: roles in print order
    std::vector<int>         temperatures; // M104/M109 targets in order, degC
};

// Per layer: the extrusion roles in order and the nozzle temperature commands.
std::vector<LayerTrace> trace_layers(const std::string &gcode)
{
    std::vector<LayerTrace> layers;
    GCodeReader             reader;
    std::string             role;
    reader.parse_buffer(gcode, [&](GCodeReader &self, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        if (raw == ";LAYER_CHANGE") {
            layers.emplace_back();
            role.clear();
            return;
        }
        if (layers.empty())
            return;
        if (raw.rfind(";TYPE:", 0) == 0) {
            role = raw.substr(6);
            return;
        }
        float temperature = 0.f;
        if ((line.cmd_is("M104") || line.cmd_is("M109")) && line.has_value('S', temperature))
            layers.back().temperatures.push_back(int(temperature));
        else if (line.extruding(self) && line.dist_XY(self) > 0 && (layers.back().roles.empty() || layers.back().roles.back() != role))
            layers.back().roles.push_back(role);
    });
    return layers;
}

} // namespace

TEST_CASE("Low temperature interface prints the interface last at its own temperature", "[LowTemperatureInterface]")
{
    const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true));
    REQUIRE(gcode.find(begin_marker) != std::string::npos);
    REQUIRE(gcode.find(end_marker) != std::string::npos);

    int interface_layers = 0;
    for (const LayerTrace &layer : trace_layers(gcode)) {
        const auto it = std::find(layer.roles.begin(), layer.roles.end(), "Support interface");
        if (it == layer.roles.end())
            continue;
        ++interface_layers;
        // Nothing but interface (and support ironing) after the first interface path of the layer.
        for (auto next = it; next != layer.roles.end(); ++next)
            CHECK(*next == "Support interface");
        // Down to 170 degC for the interface and back to 220 degC.
        REQUIRE(layer.temperatures.size() >= 2);
        CHECK(std::find(layer.temperatures.begin(), layer.temperatures.end(), 170) != layer.temperatures.end());
        CHECK(layer.temperatures.back() == 220);
    }
    CHECK(interface_layers > 0);
}

TEST_CASE("Low temperature interface off leaves support and temperatures to Orca", "[LowTemperatureInterface]")
{
    const std::string off = slice({ TestMesh::overhang }, low_temperature_config(false));
    CHECK(off.find(begin_marker) == std::string::npos);
    DynamicPrintConfig orca = low_temperature_config(false);
    // The feature's own keys at their defaults: the G-code must not change.
    for (const char *key : { "support_interface_temperature", "support_interface_heating_time", "support_interface_auxiliary_fan_speed" })
        orca.set_key_value(key, FullPrintConfig::defaults().option(key)->clone());
    // The header's generation time may tick between the two slices.
    auto without_timestamp = [](std::string gcode) {
        const size_t begin = gcode.find("; generated by");
        if (begin != std::string::npos)
            gcode.erase(begin, gcode.find('\n', begin) - begin);
        return gcode;
    };
    CHECK(without_timestamp(off) == without_timestamp(slice({ TestMesh::overhang }, orca)));
}

TEST_CASE("Low temperature interface hardware actions follow their toggles", "[LowTemperatureInterface]")
{
    SECTION("both actions disabled") {
        const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true, false, false));
        REQUIRE(gcode.find(begin_marker) != std::string::npos);
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }
    SECTION("only auxiliary fan cooling enabled") {
        const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true, true, false));
        CHECK(gcode.find(auxiliary_fan_marker) != std::string::npos);
        CHECK(gcode.find(wiping_marker) == std::string::npos);
    }
    SECTION("only nozzle wiping enabled") {
        const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true, false, true));
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
        CHECK(gcode.find(wiping_marker) != std::string::npos);
    }
    SECTION("both actions enabled in cooling then wiping order") {
        const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true, true, true));
        const size_t auxiliary_fan_position = gcode.find(auxiliary_fan_marker);
        const size_t wiping_position        = gcode.find(wiping_marker);
        REQUIRE(auxiliary_fan_position != std::string::npos);
        REQUIRE(wiping_position != std::string::npos);
        CHECK(auxiliary_fan_position < wiping_position);
    }
    SECTION("unsupported auxiliary fan suppresses cooling even if enabled") {
        const std::string gcode = slice({ TestMesh::overhang }, low_temperature_config(true, true, false, false));
        CHECK(gcode.find(auxiliary_fan_marker) == std::string::npos);
    }
}

TEST_CASE("The low temperature interface mod drives the same G-code as the core keys", "[LowTemperatureInterface][Mods]")
{
    const std::string by_keys = slice({ TestMesh::overhang }, low_temperature_config(true));

    BundledMods mods;
    REQUIRE(mods.errors.empty());
    DynamicPrintConfig by_mod = low_temperature_config(false);
    by_mod.set_key_value("mod_settings", new ConfigOptionString(nlohmann::json{
        { "low_temp_interface.enable", true }, { "low_temp_interface.heating_time", 0 } }.dump()));
    const std::string mod_output = slice({ TestMesh::overhang }, by_mod);
    CHECK(trace_layers(mod_output).size() == trace_layers(by_keys).size());
    CHECK(mod_output.find(begin_marker) != std::string::npos);
    for (size_t i = 0; i < trace_layers(by_keys).size(); ++i) {
        INFO("layer " << i + 1);
        CHECK(trace_layers(mod_output)[i].temperatures == trace_layers(by_keys)[i].temperatures);
    }
}

TEST_CASE("Low temperature interface targets stay within extrudable filament limits", "[LowTemperatureInterface]")
{
    auto validate = [](bool enabled, int interface_c) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "enable_support", true },
            { "single_nozzle_low_temperature_interface", enabled },
            { "support_interface_filament", 0 },
            { "support_interface_temperature", interface_c },
            // Relative E needs the per-layer reset; keep that unrelated preflight error out.
            { "layer_change_gcode", "G92 E0" } });
        config.set_key_value("nozzle_temperature_range_high", new ConfigOptionInts({ 240 }));
        Print print;
        Model model;
        init_print({ make_cube(10., 10., 10.) }, print, model, config);
        return print.validate();
    };
    // Firmware cold-extrusion guard: 170 degC.
    CHECK(validate(true, 170).string == "");
    CHECK(validate(true, 169).opt_key == "support_interface_temperature");
    CHECK(validate(true, 1).opt_key == "support_interface_temperature");
    // Filament maximum (this fixture: 240 degC).
    CHECK(validate(true, 240).string == "");
    CHECK(validate(true, 241).opt_key == "support_interface_temperature");
    // Feature off: the interface target is never emitted, so it is not judged.
    CHECK(validate(false, 1).string == "");
}
