// MAGPIE: Snapmaker U1 start G-code guard (the native mod mods/snapmaker_u1, gcode.events start G-code filter,
// Magpie line 1). OfficialMods makes its printer parameter readable.
#include <catch2/catch_all.hpp>

#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A start G-code that moves before homing and lacks the U1's stock pre-print sequence.
const char *const unsafe_start = "G1 X10 Y10 F3000\nPRINT_START\nG1 Z5\n";

// The guard's switch, the printer's mod parameter snapmaker_u1.start_guard.
void set_guard(DynamicPrintConfig &config, bool guard)
{
    config.set_key_value("printer_mod_settings", new ConfigOptionString(nlohmann::json{ { "snapmaker_u1.start_guard", guard } }.dump()));
}

std::string start_section(bool guard, const std::string &printer_model)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor", "klipper" },
        { "printer_model", printer_model },
        { "machine_start_gcode", unsafe_start },
    });
    set_guard(config, guard);
    // Only the emitted start section; the config dump at the end repeats the configured start G-code.
    const std::string gcode = slice({ cube(20.) }, config);
    return gcode.substr(0, gcode.find(";LAYER_CHANGE"));
}

} // namespace

TEST_CASE("The U1 start guard puts back the stock pre-print sequence", "[SnapmakerU1]")
{
    OfficialMods mods;
    const std::string guarded = start_section(true, "Snapmaker U1");
    CHECK(guarded.find("BED_MESH_CALIBRATE") != std::string::npos);
    CHECK(guarded.find("G28 Z I140 J140") != std::string::npos);
    CHECK(guarded.find("G1 X10 Y10 F3000") == std::string::npos);
}

TEST_CASE("Without the guard, or on another printer, the start G-code is kept as written", "[SnapmakerU1]")
{
    OfficialMods mods;
    for (const std::string &gcode : { start_section(false, "Snapmaker U1"), start_section(true, "Snapmaker J1") }) {
        CHECK(gcode.find("G1 X10 Y10 F3000") != std::string::npos);
        CHECK(gcode.find("BED_MESH_CALIBRATE") == std::string::npos);
        CHECK(gcode.find("mandatory Snapmaker U1 homing") == std::string::npos);
    }
}

TEST_CASE("The U1 start guard says when it changed the start G-code", "[SnapmakerU1]")
{
    OfficialMods mods;
    auto run = [](const std::string &start_gcode, bool guard) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "gcode_flavor", "klipper" },
            { "printer_model", "Snapmaker U1" },
            { "machine_start_gcode", start_gcode },
        });
        set_guard(config, guard);
        Print print;
        Model model;
        init_print({ cube(20.) }, print, model, config);
        const std::string out = gcode(print);
        std::string warnings;
        for (const PrintStateBase::Warning &w : print.step_state_with_warnings(psGCodeExport).warnings)
            warnings += w.message + "\n";
        return std::make_pair(out.substr(0, out.find(";LAYER_CHANGE")), warnings);
    };
    // Replaced: a comment where the start G-code begins, and a slicing warning.
    const auto [replaced, replaced_warnings] = run(unsafe_start, true);
    CHECK(replaced.find("; Magpie: the start G-code lacked the Snapmaker U1 pre-print sequence") != std::string::npos);
    CHECK(replaced_warnings.find("stock U1 start G-code was used instead") != std::string::npos);
    // Guard off: neither.
    const auto [kept, kept_warnings] = run(unsafe_start, false);
    CHECK(kept.find("; Magpie:") == std::string::npos);
    CHECK(kept_warnings.find("Snapmaker U1") == std::string::npos);
}

TEST_CASE("The U1 start guard is on unless turned off", "[SnapmakerU1]")
{
    OfficialMods mods;
    // Gen, 2026-10-19: on by default. No parameter stored: the guard acts.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "gcode_flavor", "klipper" }, { "printer_model", "Snapmaker U1" }, { "machine_start_gcode", unsafe_start } });
    const std::string gcode = slice({ cube(20.) }, config);
    const std::string start = gcode.substr(0, gcode.find(";LAYER_CHANGE"));
    CHECK(start.find("BED_MESH_CALIBRATE") != std::string::npos);
    CHECK(start.find("G1 X10 Y10 F3000") == std::string::npos);
    // It names itself in the G-code it changed.
    CHECK(gcode.find("; mods: snapmaker_u1 2.0.0") != std::string::npos);
}

TEST_CASE("The U1 start guard homes a start G-code that would move before homing", "[SnapmakerU1]")
{
    OfficialMods mods;
    // The configured text has the stock sequence, so it is not replaced, but a placeholder condition takes it out:
    // once filled in, the start G-code moves after PRINT_START without homing, and the guard adds a G28 there.
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({ { "gcode_flavor", "klipper" }, { "printer_model", "Snapmaker U1" } });
    std::string start = "PRINT_START\n{if 0 > 1}M140 S60\nG28 X Y\nG28 Z I140 J140\nM190 S60\nG28 Z\nBED_MESH_CALIBRATE\nM109 S200\n"
                        "G1 X185 E15 F360\n{endif}G1 X10 Y10 F3000\n";
    config.set_key_value("machine_start_gcode", new ConfigOptionString(start));
    const std::string gcode = slice({ cube(20.) }, config);
    const std::string section = gcode.substr(0, gcode.find(";LAYER_CHANGE"));
    CHECK(section.find("; Magpie: the start G-code moved before homing; a G28 was added after PRINT_START") != std::string::npos);
    CHECK(section.find("; Magpie: mandatory Snapmaker U1 homing after PRINT_START\nG28\n") != std::string::npos);
}
