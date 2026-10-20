// MAGPIE: Snapmaker U1 start G-code guard (resources/mods/snapmaker_u1.json, Magpie line 1).
#include <catch2/catch_all.hpp>

#include "libslic3r/Print.hpp"

#include "test_helpers.hpp"

#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

// A start G-code that moves before homing and lacks the U1's stock pre-print sequence.
const char *const unsafe_start = "G1 X10 Y10 F3000\nPRINT_START\nG1 Z5\n";

std::string start_section(bool guard, const std::string &printer_model)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "gcode_flavor", "klipper" },
        { "printer_model", printer_model },
        { "machine_start_gcode", unsafe_start },
        { "snapmaker_u1_start_guard", guard },
    });
    // Only the emitted start section; the config dump at the end repeats the configured start G-code.
    const std::string gcode = slice({ cube(20.) }, config);
    return gcode.substr(0, gcode.find(";LAYER_CHANGE"));
}

} // namespace

TEST_CASE("The U1 start guard puts back the stock pre-print sequence", "[SnapmakerU1]")
{
    const std::string guarded = start_section(true, "Snapmaker U1");
    CHECK(guarded.find("BED_MESH_CALIBRATE") != std::string::npos);
    CHECK(guarded.find("G28 Z I140 J140") != std::string::npos);
    CHECK(guarded.find("G1 X10 Y10 F3000") == std::string::npos);
}

TEST_CASE("Without the guard, or on another printer, the start G-code is kept as written", "[SnapmakerU1]")
{
    for (const std::string &gcode : { start_section(false, "Snapmaker U1"), start_section(true, "Snapmaker J1") }) {
        CHECK(gcode.find("G1 X10 Y10 F3000") != std::string::npos);
        CHECK(gcode.find("BED_MESH_CALIBRATE") == std::string::npos);
        CHECK(gcode.find("mandatory Snapmaker U1 homing") == std::string::npos);
    }
}

TEST_CASE("The U1 start guard says when it changed the start G-code", "[SnapmakerU1]")
{
    auto run = [](const std::string &start_gcode, bool guard) {
        DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
        config.set_deserialize_strict({
            { "gcode_flavor", "klipper" },
            { "printer_model", "Snapmaker U1" },
            { "machine_start_gcode", start_gcode },
            { "snapmaker_u1_start_guard", guard },
        });
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
