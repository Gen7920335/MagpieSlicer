#pragma once

// MAGPIE: `--mod-test <id>`, the check a mod's own repository runs on every change (docs/HLSD/native-mods.md).
//
// 1. The mod loaded: its manifest checked with the others, and for native code the library's query
//    (id, version, API, floating-point mode "precise") accepted and its slots registered.
// 2. Each case in the package's tests/ folder sliced twice. A case is <name>.json:
//      {"args": [CLI arguments, paths relative to tests/], "contains": [...], "not_contains": [...]}
//    or a bare <name>.3mf (sliced as is). Both slices must give the same G-code (determinism), the
//    G-code must pass check_gcode(), and it must contain every "contains" text and none of
//    "not_contains". Expected texts are the case author's own statement of the result, not values
//    computed by the mod.

#include <functional>
#include <string>
#include <vector>

namespace Slic3r {
namespace ModHost {

// The machine limits check_gcode() holds the G-code to.
struct GCodeBounds
{
    double min_x_mm = 0., max_x_mm = 0., min_y_mm = 0., max_y_mm = 0.; // the printable area
    double max_z_mm      = 0.;   // printable height
    double slack_mm      = 1.;   // moves may touch the edge by this much, mm
    double max_temp_c    = 350.; // highest believable nozzle or bed temperature, degC
    double max_e_move_mm = 50.;  // filament per single relative-E move, mm
};

// Problems in a G-code text, one line each ("<line>: <what>: <G-code>"), at most `limit`: XY off the
// bed, Z below 0 or above the printable height, a temperature outside 0..max, a non-finite number, a
// single extrusion longer than max_e_move_mm. The same checks as scripts/magpie/sweep/random_sweep.py,
// which checks earlier runs' files without the application and keeps its own copy.
std::vector<std::string> check_gcode(const std::string &gcode, const GCodeBounds &bounds, size_t limit = 20);

// The G-code with the lines that differ between identical runs normalised: the producer and time line
// and object ids (golden.py does the same).
std::string normalize_gcode_for_comparison(const std::string &gcode);

struct ModTestCase
{
    std::string              name;
    std::vector<std::string> args; // CLI arguments
    std::vector<std::string> contains, not_contains;
};

// The cases in <package>/tests. Throws ModError on a malformed case file.
std::vector<ModTestCase> read_mod_test_cases(const std::string &package_dir);

// Slices one case: the G-code text, or throws with the reason. `run` is the 1st or 2nd slice.
using ModTestSlice = std::function<std::string(const ModTestCase &c, int run)>;
// The bounds of the printer a case's G-code was sliced for, read from its config block.
GCodeBounds bounds_from_gcode(const std::string &gcode);

struct ModTestReport
{
    bool                     passed = true;
    std::vector<std::string> lines;
};

// Runs `exe` with `args` and waits for it; its standard output and error go to `output_file`.
// Returns its exit code, or -1 when it could not be started.
int run_child_process(const std::string &exe, const std::vector<std::string> &args, const std::string &output_file);

// Runs the checks of mod `id` against the active mods. `slice` is how a case is sliced (the CLI runs
// itself as a separate process for each slice).
ModTestReport run_mod_test(const std::string &id, const ModTestSlice &slice);

} // namespace ModHost
} // namespace Slic3r
