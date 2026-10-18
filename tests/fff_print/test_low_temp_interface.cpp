// MAGPIE: low temperature support interface (resources/mods/low_temp_interface.json), ported from Magpie line 1's
// test_support_material.cpp low temperature interface cases.
#include <catch2/catch_all.hpp>

#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/TemperatureDropTower.hpp"

#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
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
    // Compare line by line so a failure names the first differing line.
    std::istringstream a(without_timestamp(off)), b(without_timestamp(slice({ TestMesh::overhang }, orca)));
    std::string        line_a, line_b;
    size_t             line_no = 0;
    while (true) {
        const bool more_a = bool(std::getline(a, line_a)), more_b = bool(std::getline(b, line_b));
        ++line_no;
        if (!more_a || !more_b) {
            CHECK(more_a == more_b);
            break;
        }
        // Object labels carry a per-process object id (golden.py normalizes them the same way).
        const auto id_pos_a = line_a.find(" id:"), id_pos_b = line_b.find(" id:");
        if (line_a.rfind("; ", 0) == 0 && id_pos_a != std::string::npos && id_pos_b != std::string::npos &&
            line_a.substr(0, id_pos_a) == line_b.substr(0, id_pos_b))
            continue;
        if (line_a != line_b) {
            INFO("line " << line_no);
            CHECK(line_a == line_b);
            break;
        }
    }
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

// ---- Temperature drop tower (Magpie line 1 test_support_material.cpp) ----

namespace {

struct TowerSettings
{
    int                normal_temperature { 220 };   // degC
    int                interface_temperature { 170 }; // degC
    bool               tower_enabled { true };
    bool               low_temperature_interface_enabled { true };
    int                interface_filament { 0 };
    PrintSequence      print_sequence { PrintSequence::ByLayer };
    bool               configured_wiper { false };
    bool               support_enabled { true };
    double             tower_x { -1.0 }; // mm
    double             tower_y { -1.0 }; // mm
    double             nozzle_diameter { 0.4 }; // mm
    double             layer_height { 0.2 };    // mm
    std::vector<Vec2d> extruder_offsets;        // mm; empty keeps the default
    std::vector<Vec2d> printable_area { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 200.), Vec2d(0., 200.) };
};

DynamicPrintConfig tower_config(const TowerSettings &settings)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support", settings.support_enabled },
        { "support_interface_top_layers", 2 },
        { "support_interface_filament", settings.interface_filament },
        { "single_nozzle_low_temperature_interface", settings.low_temperature_interface_enabled },
        { "support_interface_temperature", settings.interface_temperature },
        { "support_interface_temperature_drop_tower", settings.tower_enabled },
        { "support_interface_nozzle_wiping_on_temperature_change", settings.configured_wiper },
        { "support_interface_brush_repetitions", 2 },
        { "support_interface_brush_speed", 80.0 },
        { "layer_height", settings.layer_height },
        { "initial_layer_print_height", settings.layer_height },
    });
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ settings.nozzle_diameter }));
    config.set_key_value("nozzle_temperature", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value("nozzle_temperature_initial_layer", new ConfigOptionInts({ settings.normal_temperature }));
    config.set_key_value("print_sequence", new ConfigOptionEnum<PrintSequence>(settings.print_sequence));
    config.set_key_value("printable_area", new ConfigOptionPoints(settings.printable_area));
    config.set_key_value("support_interface_temperature_drop_tower_x", new ConfigOptionFloats({ settings.tower_x }));
    config.set_key_value("support_interface_temperature_drop_tower_y", new ConfigOptionFloats({ settings.tower_y }));
    config.set_key_value("support_interface_brush_start", new ConfigOptionPoint(Vec2d(10.0, 10.0)));
    config.set_key_value("support_interface_brush_end", new ConfigOptionPoint(Vec2d(20.0, 10.0)));
    if (!settings.extruder_offsets.empty())
        config.set_key_value("extruder_offset", new ConfigOptionPoints(settings.extruder_offsets));
    return config;
}

std::string tower_gcode(const TowerSettings &settings)
{
    return slice({ TestMesh::overhang }, tower_config(settings));
}

std::optional<double> gcode_word(const std::string &line, char letter)
{
    for (size_t pos = 0; pos < line.size(); ++pos) {
        if (line[pos] != letter || (pos > 0 && !std::isspace(static_cast<unsigned char>(line[pos - 1]))))
            continue;
        const char  *begin = line.c_str() + pos + 1;
        char        *end   = nullptr;
        const double value = std::strtod(begin, &end);
        if (end != begin)
            return value;
    }
    return std::nullopt;
}

size_t count_substring(const std::string &text, const std::string &needle)
{
    size_t count = 0;
    for (size_t pos = 0; (pos = text.find(needle, pos)) != std::string::npos; pos += needle.size())
        ++count;
    return count;
}

// Extruding moves between two markers.
size_t count_extruding_moves_between(const std::string &gcode, const std::string &begin_marker, const std::string &end_marker)
{
    const size_t begin = gcode.find(begin_marker);
    const size_t end   = begin == std::string::npos ? std::string::npos : gcode.find(end_marker, begin);
    if (begin == std::string::npos || end == std::string::npos)
        return 0;
    size_t             count = 0;
    std::istringstream stream(gcode.substr(begin, end - begin));
    for (std::string line; std::getline(stream, line);) {
        const bool linear_move = line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0 || line == "G0" || line == "G1";
        if (linear_move && gcode_word(line, 'E'))
            ++count;
    }
    return count;
}

// The first cooling pass: its fast and slow lengths (mm), feed rates (mm/min) and extent (mm); and every pass's Z.
struct TowerMetrics
{
    size_t              cooling_passes { 0 };
    std::vector<double> cooling_pass_z;
    double              fast_length { 0.0 }, slow_length { 0.0 };
    double              min_fast_feedrate { std::numeric_limits<double>::max() }, max_fast_feedrate { std::numeric_limits<double>::lowest() };
    double              min_slow_feedrate { std::numeric_limits<double>::max() }, max_slow_feedrate { std::numeric_limits<double>::lowest() };
    double              min_x { std::numeric_limits<double>::max() }, min_y { std::numeric_limits<double>::max() };
    double              max_x { std::numeric_limits<double>::lowest() }, max_y { std::numeric_limits<double>::lowest() };
};

TowerMetrics analyze_tower(const std::string &gcode)
{
    enum class Section { None, Fast, Slow };
    TowerMetrics       metrics;
    Section            section    = Section::None;
    bool               first_pass = false;
    double             x = 0.0, y = 0.0, z = 0.0, feedrate = 0.0;
    std::istringstream stream(gcode);
    for (std::string line; std::getline(stream, line);) {
        if (line.find("temperature drop tower cooling pass") != std::string::npos && line.find("pass end") == std::string::npos) {
            ++metrics.cooling_passes;
            metrics.cooling_pass_z.push_back(z);
            first_pass = metrics.cooling_passes == 1;
            section    = Section::None;
        } else if (first_pass && line.find("cooling fast section") != std::string::npos) {
            section = Section::Fast;
        } else if (first_pass && line.find("cooling final 10 mm") != std::string::npos) {
            section = Section::Slow;
        } else if (first_pass && line.find("cooling pass end") != std::string::npos) {
            first_pass = false;
            section    = Section::None;
        }
        const bool linear_move = line.rfind("G0 ", 0) == 0 || line.rfind("G1 ", 0) == 0;
        if (!linear_move)
            continue;
        const double previous_x = x, previous_y = y;
        if (const auto v = gcode_word(line, 'F')) feedrate = *v;
        if (const auto v = gcode_word(line, 'X')) x = *v;
        if (const auto v = gcode_word(line, 'Y')) y = *v;
        if (const auto v = gcode_word(line, 'Z')) z = *v;
        if (!first_pass || section == Section::None || !gcode_word(line, 'E'))
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

void check_passes_on_distinct_layers(const TowerMetrics &metrics)
{
    REQUIRE(metrics.cooling_passes >= 2);
    std::vector<double> z = metrics.cooling_pass_z;
    std::sort(z.begin(), z.end());
    // One cooling pass per layer, mm.
    for (size_t idx = 1; idx < z.size(); ++idx)
        CHECK(z[idx] - z[idx - 1] > 0.05);
}

} // namespace

TEST_CASE("Temperature drop tower scales and slows only its final 10 mm", "[LowTemperatureInterface][TemperatureDropTower]")
{
    // Arm length in mm for the temperature drop: 80 mm up to 30 degC, then one mm per degree, at most 110.
    const auto [normal_temperature, expected_size] = GENERATE(table<int, double>({ { 200, 80.0 }, { 201, 81.0 }, { 220, 100.0 }, { 260, 110.0 } }));
    DYNAMIC_SECTION("normal " << normal_temperature) {
        TowerSettings settings;
        settings.normal_temperature = normal_temperature;
        const std::string  output  = tower_gcode(settings);
        const TowerMetrics metrics = analyze_tower(output);
        check_passes_on_distinct_layers(metrics);
        CHECK(metrics.fast_length > metrics.slow_length);
        CHECK_THAT(metrics.slow_length, Catch::Matchers::WithinAbs(10.0, 0.03));
        // Feed rates in mm/min: the tail at 10 mm/s, the rest faster and constant.
        CHECK(metrics.min_fast_feedrate > 600.0);
        CHECK_THAT(metrics.max_fast_feedrate, Catch::Matchers::WithinAbs(metrics.min_fast_feedrate, 0.1));
        CHECK_THAT(metrics.min_slow_feedrate, Catch::Matchers::WithinAbs(600.0, 0.1));
        CHECK_THAT(metrics.max_slow_feedrate, Catch::Matchers::WithinAbs(600.0, 0.1));
        CHECK_THAT(metrics.max_x - metrics.min_x, Catch::Matchers::WithinAbs(expected_size, 0.03));
        CHECK_THAT(metrics.max_y - metrics.min_y, Catch::Matchers::WithinAbs(expected_size, 0.03));
        // Rear left of the 200 mm bed, 3 mm from its edge.
        CHECK(metrics.min_x > 3.0);
        CHECK(metrics.max_y < 197.0);
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);
        CHECK(count_substring(output, "; temperature drop tower brim begin") == 1);

        // Cooling: M104 (no wait) to the interface temperature, the cooling pass, then the interface; no M109 wait.
        const std::string set_temperature  = "M104 S" + std::to_string(settings.interface_temperature);
        const std::string wait_temperature = "M109 S" + std::to_string(settings.interface_temperature);
        const size_t      begin            = output.find("low-temperature support interface begin");
        const size_t      nonblocking      = output.find(set_temperature, begin);
        const size_t      brim_end         = output.find("temperature drop tower brim end");
        const size_t      cooling_pass     = output.find("temperature drop tower cooling pass", begin);
        const size_t      cooling_end      = output.find("temperature drop tower cooling pass end", cooling_pass);
        const size_t      interface        = output.find("support material interface", cooling_end);
        REQUIRE(begin != std::string::npos);
        REQUIRE(nonblocking != std::string::npos);
        REQUIRE(brim_end != std::string::npos);
        REQUIRE(cooling_pass != std::string::npos);
        REQUIRE(cooling_end != std::string::npos);
        REQUIRE(interface != std::string::npos);
        CHECK(begin < nonblocking);
        CHECK(brim_end < cooling_pass);
        CHECK(cooling_end < interface);
        CHECK(output.find(wait_temperature, nonblocking) > interface);
    }
}

TEST_CASE("Temperature drop tower stays inside rear-left bed bounds", "[LowTemperatureInterface][TemperatureDropTower]")
{
    // Bed, its x range, the highest tower y and the largest tower size, mm.
    const std::array<std::tuple<std::vector<Vec2d>, double, double, double, double>, 2> beds{ {
        { { Vec2d(0., 0.), Vec2d(200., 0.), Vec2d(200., 70.), Vec2d(0., 70.) }, 0.0, 200.0, 67.0, 64.0 },
        { { Vec2d(-10., -20.), Vec2d(190., -20.), Vec2d(190., 180.), Vec2d(-10., 180.) }, -10.0, 190.0, 177.0, 100.0 },
    } };
    for (const auto &[bed, bed_min_x, bed_max_x, expected_max_y, expected_size] : beds) {
        TowerSettings settings;
        settings.printable_area    = bed;
        const TowerMetrics metrics = analyze_tower(tower_gcode(settings));
        CAPTURE(bed_min_x, bed_max_x, expected_max_y, expected_size);
        check_passes_on_distinct_layers(metrics);
        CHECK(metrics.min_x >= bed_min_x + 3.0 - 0.03);
        CHECK(metrics.max_x <= bed_max_x - 3.0 + 0.03);
        CHECK(metrics.max_y < expected_max_y);
        CHECK(metrics.max_x - metrics.min_x <= expected_size + 0.03);
        CHECK(metrics.max_y - metrics.min_y <= expected_size + 0.03);
    }
}

TEST_CASE("Temperature drop tower clamps a configured position to the bed", "[LowTemperatureInterface][TemperatureDropTower]")
{
    TowerSettings settings;
    settings.tower_x           = 500.0;
    settings.tower_y           = 100.0;
    const TowerMetrics metrics = analyze_tower(tower_gcode(settings));
    check_passes_on_distinct_layers(metrics);
    // Pushed against the right edge of the 200 mm bed, 3 mm margin.
    CHECK(metrics.max_x <= 197.0 + 0.03);
    CHECK(metrics.max_x > 150.0);
}

TEST_CASE("Temperature drop tower honors every activation guard", "[LowTemperatureInterface][TemperatureDropTower]")
{
    std::vector<std::pair<std::string, TowerSettings>> disabled_cases;
    TowerSettings                                      settings;
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
        const std::string output = tower_gcode(disabled);
        CHECK(output.find("temperature drop tower layer") == std::string::npos);
        CHECK(output.find("temperature drop tower cooling pass") == std::string::npos);
    }
    TowerSettings wiper_fallback;
    wiper_fallback.configured_wiper = true;
    const std::string fallback      = tower_gcode(wiper_fallback);
    CHECK(fallback.find("M109 S170") != std::string::npos);
    CHECK(fallback.find("temperature drop tower") == std::string::npos);
}

// Ported from Magpie line 1 (the P4 exit matrix: 3 normal x 2 interface temperatures x 3 nozzles x 2 positions).
TEST_CASE("Temperature drop tower keeps its brim across a 36-setting slicing matrix",
          "[LowTemperatureInterface][TemperatureDropTower][Matrix]")
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
                    TowerSettings settings;
                    settings.normal_temperature = normal_temperature;
                    settings.interface_temperature = interface_temperature;
                    settings.nozzle_diameter = nozzle.diameter;
                    settings.layer_height = nozzle.layer_height;
                    if (manual_position) {
                        settings.tower_x = 20.0;
                        settings.tower_y = 25.0;
                    }

                    const std::string output = tower_gcode(settings);
                    const TowerMetrics metrics = analyze_tower(output);
                    const double temperature_delta = normal_temperature - interface_temperature;
                    const double expected_size = temperature_delta <= 30.0
                        ? 80.0
                        : std::min(110.0, 80.0 + temperature_delta - 30.0);
                    const size_t brim_begin = output.find("; temperature drop tower brim begin");
                    const size_t brim_end = output.find("; temperature drop tower brim end", brim_begin);
                    const size_t first_tower_layer = output.find("; temperature drop tower layer");

                    CAPTURE(normal_temperature, interface_temperature, nozzle.diameter,
                            nozzle.layer_height, manual_position, expected_size);
                    check_passes_on_distinct_layers(metrics);
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

TEST_CASE("Temperature drop tower keeps five brim lines across nozzle diameters",
          "[LowTemperatureInterface][TemperatureDropTower]")
{
    const std::array<std::pair<double, double>, 3> nozzle_cases {{
        { 0.15, 0.08 },
        { 0.4, 0.2 },
        { 0.8, 0.3 }
    }};
    double previous_body_offset = 0.0;
    for (const auto &[nozzle_diameter, layer_height] : nozzle_cases) {
        TowerSettings settings;
        settings.tower_x = 20.0;
        settings.tower_y = 25.0;
        settings.nozzle_diameter = nozzle_diameter;
        settings.layer_height = layer_height;
        const std::string output = tower_gcode(settings);
        const TowerMetrics metrics = analyze_tower(output);
        const double body_offset = metrics.min_x - settings.tower_x;

        CAPTURE(nozzle_diameter, body_offset);
        check_passes_on_distinct_layers(metrics);
        CHECK(metrics.max_x - metrics.min_x == Catch::Approx(100.0).margin(0.03)); // 220 - 170 degC: 100 mm arms
        CHECK(metrics.max_y - metrics.min_y == Catch::Approx(100.0).margin(0.03));
        CHECK(count_substring(output, "; temperature drop tower brim line") == 5);
        CHECK(body_offset > previous_body_offset);
        previous_body_offset = body_offset;
    }
}

TEST_CASE("Temperature drop tower G-code follows a manual project move exactly",
          "[LowTemperatureInterface][TemperatureDropTower]")
{
    TowerSettings before_move;
    before_move.tower_x = 20.0;
    before_move.tower_y = 25.0;
    const std::string before_output = tower_gcode(before_move);
    const TowerMetrics before =
        analyze_tower(before_output);

    TowerSettings after_move = before_move;
    after_move.tower_x = 42.0;
    after_move.tower_y = 31.0;
    const std::string after_output = tower_gcode(after_move);
    const TowerMetrics after =
        analyze_tower(after_output);

    check_passes_on_distinct_layers(before);
    check_passes_on_distinct_layers(after);
    CHECK(after.min_x - before.min_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.max_x - before.max_x == Catch::Approx(22.0).margin(0.03));
    CHECK(after.min_y - before.min_y == Catch::Approx(6.0).margin(0.03));
    CHECK(after.max_y - before.max_y == Catch::Approx(6.0).margin(0.03));
    CHECK(count_substring(before_output, "; temperature drop tower brim line") == 5);
    CHECK(count_substring(after_output, "; temperature drop tower brim line") == 5);
}

TEST_CASE("Temperature drop tower follows the standard extruder offset transform",
          "[LowTemperatureInterface][TemperatureDropTower]")
{
    TowerSettings settings;
    settings.extruder_offsets = { Vec2d(-32.0, 0.0) };
    const TowerMetrics metrics = analyze_tower(
        tower_gcode(settings));

    check_passes_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 34.0);
    CHECK(metrics.max_x < 139.0); // 100 mm arms (220 - 170 degC)
    CHECK(metrics.min_y > 2.0);
    CHECK(metrics.max_y < 198.0);
}

TEST_CASE("Temperature drop tower keeps its configured position when a model overlaps it",
          "[LowTemperatureInterface][TemperatureDropTower]")
{
    TowerSettings settings;
    settings.tower_x = 5.0;
    settings.tower_y = 145.0;
    const TowerMetrics baseline =
        analyze_tower(tower_gcode(settings));
    DynamicPrintConfig config = tower_config(settings);
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
    const TowerMetrics metrics =
        analyze_tower(gcode(print));

    check_passes_on_distinct_layers(metrics);
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

namespace {

// Nozzle temperature (degC) in force for every model extrusion (wall or infill), and every M104/M109 target.
struct TemperatureTrace
{
    std::vector<int> targets;
    std::vector<int> model_temperatures;
};

TemperatureTrace trace_temperatures(const std::string &gcode, const DynamicPrintConfig &config)
{
    TemperatureTrace trace;
    GCodeReader      reader;
    reader.apply_config(config);
    int         temperature = 0;
    std::string role;
    reader.parse_buffer(gcode, [&](GCodeReader &state, const GCodeReader::GCodeLine &line) {
        const std::string raw(line.raw());
        if (raw.rfind(";TYPE:", 0) == 0)
            role = raw.substr(6);
        float target;
        if ((line.cmd_is("M104") || line.cmd_is("M109")) && line.has_value('S', target)) {
            temperature = int(std::lround(target));
            trace.targets.push_back(temperature);
        }
        if (line.extruding(state) && line.dist_XY(state) > 0.f &&
            (role.find("wall") != std::string::npos || role.find("infill") != std::string::npos))
            trace.model_temperatures.push_back(temperature);
    });
    return trace;
}

DynamicPrintConfig sublayer_temperature_config(bool low_temperature_interface, int sublayer_temperature_c)
{
    DynamicPrintConfig config = low_temperature_config(low_temperature_interface);
    config.set_deserialize_strict({
        { "support_type", "normal(auto)" },
        { "support_interface_top_layers", 4 },
        { "support_interface_temperature", 200 },
        { "support_interface_sublayer_start", 2 },
        { "support_interface_sublayer_end", 3 },
        { "support_interface_sublayer_temperature", sublayer_temperature_c },
    });
    return config;
}

} // namespace

TEST_CASE("Interface sublayers print at their own temperature and the model at the filament temperature", "[LowTemperatureInterface][Sublayer]")
{
    const bool low_temperature_interface = GENERATE(false, true);
    CAPTURE(low_temperature_interface);
    const DynamicPrintConfig config = sublayer_temperature_config(low_temperature_interface, 185);
    Print print;
    init_and_process_print({ TestMesh::overhang }, print, config);
    const TemperatureTrace trace = trace_temperatures(Slic3r::Test::gcode(print), config);

    // 185 degC is set for the sublayers; with the low temperature interface the contact layer uses 200 degC.
    CHECK(std::count(trace.targets.begin(), trace.targets.end(), 185) > 0);
    CHECK((std::count(trace.targets.begin(), trace.targets.end(), 200) > 0) == low_temperature_interface);
    REQUIRE_FALSE(trace.model_temperatures.empty());
    for (const int temperature_c : trace.model_temperatures)
        CHECK(temperature_c == 220);
}


// MAGPIE: line 1 (Beta 7) temperature drop tower tests for printers with several physical tools.
TEST_CASE("Tower placement intersects only participating physical tool areas", "[LowTemperatureInterface][TemperatureDropTower]")
{
    // Rectangles spanning x = left..right mm and y = 0..200 mm.
    auto rectangle = [](double left, double right) {
        return Polygons{ Polygon{ Points{ Point::new_scale(left, 0.), Point::new_scale(right, 0.), Point::new_scale(right, 200.),
                                          Point::new_scale(left, 200.) } } };
    };
    const Polygons              bed = rectangle(0., 200.);
    const std::vector<Polygons> areas{ rectangle(0., 150.), rectangle(50., 200.) };
    const double                mm2 = SCALING_FACTOR * SCALING_FACTOR; // scaled area to mm2
    for (const auto &tools : { std::vector<size_t>{ 0, 1 }, std::vector<size_t>{ 1, 0 }, std::vector<size_t>{ 0, 1, 1 } }) {
        const Polygons common = temperature_drop_tower_common_area(bed, areas, tools);
        CHECK(area(common) * mm2 == Catch::Approx(20000.)); // 100 x 200 mm
        CHECK(diff_ex(common, rectangle(50., 150.)).empty());
    }
    CHECK(temperature_drop_tower_common_area(bed, { rectangle(0., 80.), rectangle(120., 200.) }, { 0, 1 }).empty());
    CHECK(area(temperature_drop_tower_common_area(bed, areas, { 0 })) * mm2 == Catch::Approx(30000.));
    CHECK(area(temperature_drop_tower_common_area(bed, {}, { 0, 1 })) * mm2 == Catch::Approx(40000.));
}

TEST_CASE("Temperature drop tower automatic placement respects the mapped extruder area", "[LowTemperatureInterface][TemperatureDropTower]")
{
    TowerSettings settings; // automatic placement (tower_x, tower_y = -1)
    const std::vector<std::vector<Vec2d>> extruder_areas{
        settings.printable_area,
        { Vec2d(24., 12.), Vec2d(176., 12.), Vec2d(176., 188.), Vec2d(24., 188.) } // mm
    };
    DynamicPrintConfig config = tower_config(settings);
    config.set_num_extruders(2);
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats({ 0.4, 0.4 }));
    config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode", true)->value = fmmManual;
    config.set_key_value("filament_map", new ConfigOptionInts({ 2 }));
    config.set_key_value("extruder_printable_area", new ConfigOptionPointsGroups(extruder_areas));

    const TowerMetrics metrics = analyze_tower(slice({ TestMesh::overhang }, config));
    check_passes_on_distinct_layers(metrics);
    CHECK(metrics.min_x > 24.0);
    CHECK(metrics.min_y > 12.0);
    CHECK(metrics.max_x < 176.0);
    CHECK(metrics.max_y < 188.0);
}

TEST_CASE("Multi-nozzle tower fill preserves its fixed physical band", "[LowTemperatureInterface][TemperatureDropTower]")
{
    for (double maximum : { 0.4, 0.8, 1.2 })                    // largest nozzle, mm
        for (double nozzle : { 0.15, 0.2, 0.4, 0.6, 0.8, 1.2 })  // mm
            for (double height : { 0.04, 0.08, 0.12, 0.2, 0.28 }) { // mm
                if (nozzle > maximum || height >= nozzle)
                    continue;
                CAPTURE(maximum, nozzle, height);
                const double band = 4. * temperature_drop_tower_multi_pitch(maximum); // mm
                const Flow   nominal(Flow::auto_extrusion_width(frPerimeter, float(nozzle)), float(height), float(nozzle));
                const auto   fill = temperature_drop_tower_fill(nominal, band);
                CHECK(fill.line_count >= 4);
                CHECK(fill.flow.width() + (fill.line_count - 1) * double(fill.flow.spacing()) == Catch::Approx(band).margin(0.000002));
                CHECK(fill.flow.mm3_per_mm() > 0.);
                CHECK(fill.flow.width() > 0.9 * nozzle);
                CHECK(fill.flow.width() < 1.5 * nozzle);
            }
    const Flow flow(0.45f, 0.2f, 0.4f);
    CHECK_THROWS(temperature_drop_tower_fill(flow, 0.));
    CHECK_THROWS(temperature_drop_tower_fill(flow, std::numeric_limits<double>::infinity()));
}
