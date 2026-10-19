// MAGPIE: layer time blending (gcode.time_plan slot, resources/mods/layer_time_blend.json): wait sites,
// in-place waits filled in after the export, and the re-estimate of the G-code as written.
#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// A 4 x 4 mm pillar, 16 mm tall, with a 30 x 30 x 3 mm slab around it from z = 6 to 9 mm: short layers, then
// long ones, then short again.
TriangleMesh pillar_with_slab()
{
    TriangleMesh pillar = make_cube(4., 4., 16.);
    TriangleMesh slab   = make_cube(30., 30., 3.);
    slab.translate(-13.f, -13.f, 6.f);
    pillar.merge(slab);
    return pillar;
}

DynamicPrintConfig blend_config(const std::string &planner, double max_diff_s)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"layer_height", "0.3"},
        {"initial_layer_print_height", "0.3"},
        {"wall_loops", "2"},
        {"sparse_infill_density", "15%"},
        {"enable_support", "0"},
        {"skirt_loops", "0"},
        {"brim_type", "no_brim"},
        // No cooling slowdown, which would stretch the short layers towards each other.
        {"slow_down_for_layer_cooling", "0"},
        {"layer_time_planner", planner},
        {"layer_time_blend_max_diff", std::to_string(max_diff_s)},
        {"layer_time_blend_warn_total", "0"},
        {"layer_time_blend_warn_single", "0"},
    });
    return config;
}

// Slices `mesh` placed at the middle of the default 200 x 200 mm bed (not arranged, which would centre it
// on the origin, half off the bed), so there is room for a tower and a park position around it.
std::string slice_mesh(TriangleMesh mesh, const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    mesh.translate(100.f, 100.f, 0.f);
    std::vector<TriangleMesh> meshes;
    meshes.push_back(std::move(mesh));
    init_print(std::move(meshes), print, model, config, nullptr, /*arrange=*/false);
    return gcode(print);
}

// The layer times between wait sites as a fresh processor estimates them from the G-code text: the time
// from each site to the next (site 0 is the first layer's start).
std::vector<double> measured_intervals(const std::string &gcode)
{
    ScopedTemporaryFile file(".gcode");
    {
        std::ofstream out(file.string(), std::ios::binary);
        out << gcode;
    }
    GCodeProcessor processor;
    processor.process_file(file.string());
    const std::vector<GCodeProcessor::WaitSiteTime> times = processor.wait_site_times();
    std::vector<double> sites;
    for (const GCodeProcessor::WaitSiteTime &mark : times)
        if (mark.kind == GCodeProcessor::WaitMarkKind::Site)
            sites.push_back(mark.time_s);
    std::vector<double> out;
    for (size_t k = 1; k < sites.size(); ++k)
        out.push_back(sites[k] - sites[k - 1]);
    return out;
}

std::string without_lines_starting(const std::string &gcode, const std::string &prefix)
{
    std::istringstream in(gcode);
    std::string        line, out;
    while (std::getline(in, line))
        if (line.compare(0, prefix.size(), prefix) != 0)
            out += line + '\n';
    return out;
}

size_t count_lines_starting(const std::string &gcode, const std::string &prefix)
{
    std::istringstream in(gcode);
    std::string        line;
    size_t             n = 0;
    while (std::getline(in, line))
        n += line.compare(0, prefix.size(), prefix) == 0;
    return n;
}

const std::string site_tag = ";MAGPIE_WAIT_SITE";

} // namespace

TEST_CASE("Without a planner no wait site is written", "[LayerTimeBlend]")
{
    const std::string out = slice_mesh(pillar_with_slab(), blend_config("", 2.));
    CHECK(count_lines_starting(out, site_tag) == 0);
    CHECK(count_lines_starting(out, "G4 ") == 0);
}

TEST_CASE("Waits of zero leave the G-code as it was apart from the site tags", "[LayerTimeBlend]")
{
    const std::string plain   = slice_mesh(pillar_with_slab(), blend_config("", 2.));
    // A limit no layer pair reaches, s: nothing waits.
    const std::string blended = slice_mesh(pillar_with_slab(), blend_config("blend", 1e6));
    // 16 mm at 0.3 mm is 53 whole layers (the last 0.1 mm is not printed); a site starts each.
    CHECK(count_lines_starting(blended, site_tag) == 53);
    CHECK(count_lines_starting(blended, "G4 ") == 0);
    CHECK(without_lines_starting(blended, site_tag) == plain);
}

TEST_CASE("In-place waits blend the layer times around a long layer", "[LayerTimeBlend]")
{
    const double max_diff_s = 2.;
    // The unblended times come from the same G-code with a limit no pair reaches.
    const std::vector<double> t = measured_intervals(slice_mesh(pillar_with_slab(), blend_config("blend", 1e6)));
    const std::string         out = slice_mesh(pillar_with_slab(), blend_config("blend", max_diff_s));
    const std::vector<double> T   = measured_intervals(out);
    REQUIRE(t.size() == 52); // one interval per layer below the top one (53 layers)
    REQUIRE(T.size() == t.size());
    // The fixture works only if the slab's layers are much longer than the pillar's, s.
    const double longest  = *std::max_element(t.begin(), t.end());
    const double shortest = *std::min_element(t.begin() + 1, t.end());
    REQUIRE(longest - shortest > 5. * max_diff_s);

    // Interval i is the time of layer i + 1; layer 1 (index 0) is below start_layer 2 and keeps its time.
    CHECK_THAT(T[0], WithinAbs(t[0], 0.05));
    // Expected: the smallest times >= t with neighbours within max_diff_s, over layers 2 on, written out as
    // the envelope of cones on every layer time.
    for (size_t i = 1; i < t.size(); ++i) {
        double expected = t[i];
        for (size_t j = 1; j < t.size(); ++j)
            expected = std::max(expected, t[j] - max_diff_s * std::abs(double(i) - double(j)));
        // Within 50 ms of the planned time, the accuracy the design asks of the re-estimate.
        CHECK_THAT(T[i], WithinAbs(expected, 0.05));
        if (i > 1)
            CHECK(std::abs(T[i] - T[i - 1]) <= max_diff_s + 0.05);
    }
    // Every wait sits right before its site: a lift, the G4 and the return, or a bare G4 for a short one.
    const size_t waits = count_lines_starting(out, "G4 P");
    CHECK(waits > 0);
    std::istringstream in(out);
    std::string        line, prev1, prev2;
    size_t             tagged = 0;
    while (std::getline(in, line)) {
        if (line == site_tag && (prev1.rfind("G4 P", 0) == 0 || (prev2.rfind("G4 P", 0) == 0 && prev1.rfind("G1 Z", 0) == 0)))
            ++tagged;
        prev2 = prev1;
        prev1 = line;
    }
    CHECK(tagged == waits);
}

TEST_CASE("An in-place wait lifts the nozzle and returns it to the same height", "[LayerTimeBlend]")
{
    const std::string out = slice_mesh(pillar_with_slab(), blend_config("blend", 2.));
    std::vector<std::string> lines;
    std::vector<double>      z_before; // Z in force before each line, from the G0/G1 moves (absolute XYZ)
    {
        std::istringstream in(out);
        std::string        line;
        double             z = 0.;
        while (std::getline(in, line)) {
            lines.push_back(line);
            z_before.push_back(z);
            if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0) {
                const size_t at = line.find(" Z");
                if (at != std::string::npos && line.find(';') > at)
                    z = std::stod(line.substr(at + 2));
            }
        }
    }
    auto z_of = [](const std::string &line) { return std::stod(line.substr(line.find(" Z") + 2)); };
    size_t checked = 0;
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        if (lines[i].rfind("G4 P", 0) != 0)
            continue;
        if (lines[i - 1].rfind("G1 Z", 0) != 0) {
            // A bare G4 is a short wait (under 1 s, 1000 ms), which does not lift.
            CHECK(std::stoll(lines[i].substr(4)) < 1000);
            continue;
        }
        REQUIRE(lines[i + 1].rfind("G1 Z", 0) == 0);
        // Up to 1 mm over the new layer, then down to the new layer's Z, which the next travel would have
        // raised the nozzle to anyway: never below where the nozzle was.
        CHECK_THAT(z_of(lines[i - 1]) - z_of(lines[i + 1]), WithinAbs(1., 1e-6));
        CHECK(z_of(lines[i + 1]) >= z_before[i - 1] - 1e-6);
        CHECK(z_of(lines[i + 1]) - z_before[i - 1] <= 0.3 + 1e-6); // at most one layer (0.3 mm) up
        ++checked;
    }
    CHECK(checked > 0);
}

namespace {

DynamicPrintConfig wait_at_config(const std::string &wait_at, double max_diff_s)
{
    DynamicPrintConfig config = blend_config("blend", max_diff_s);
    config.set_deserialize_strict({{"layer_time_blend_wait_at", wait_at}, {"use_relative_e_distances", "1"}});
    return config;
}

// The G-code's lines without the progress lines (M73) the post-processor puts wherever the time crosses a
// minute or a percent, inside wait blocks too.
std::vector<std::string> lines_of(const std::string &gcode)
{
    std::vector<std::string> out;
    std::istringstream       in(gcode);
    std::string              line;
    while (std::getline(in, line))
        if (line.rfind("M73", 0) != 0)
            out.push_back(line);
    return out;
}

// The value after " <axis>" in a G0/G1 line, or nothing.
std::optional<double> axis_value(const std::string &line, char axis)
{
    if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
        return std::nullopt;
    const size_t comment = line.find(';');
    const size_t at      = line.find(std::string(" ") + axis);
    if (at == std::string::npos || at > comment)
        return std::nullopt;
    return std::stod(line.substr(at + 2));
}

void check_blended(const std::vector<double> &T, double max_diff_s)
{
    // Layer 1 (interval 0) is below start_layer; from layer 2 on neighbours stay within the limit, s.
    for (size_t i = 2; i < T.size(); ++i)
        CHECK(std::abs(T[i] - T[i - 1]) <= max_diff_s + 0.05);
}

} // namespace

TEST_CASE("Tower waits happen over the tower, which stops above the last wait", "[LayerTimeBlend]")
{
    // 10 s: the waits ramp down from the slab's ~140 s layers within about 14 layers (4 mm), well below the
    // pillar's top, so the tower can stop there.
    const double              max_diff_s = 10.;
    const std::string         out        = slice_mesh(pillar_with_slab(), wait_at_config("tower", max_diff_s));
    const std::vector<std::string> lines = lines_of(out);
    const std::string begin_tag = ";MAGPIE_WAIT_TOWER_BEGIN", end_tag = ";MAGPIE_WAIT_TOWER_END";
    size_t blocks = 0, sites = 0, last_wait_site = 0, last_block_site = 0;
    bool   in_block = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line == begin_tag) {
            CHECK_FALSE(in_block);
            in_block = true;
            ++blocks;
            // Blocks come one per layer from the first, without a gap.
            CHECK(blocks == sites + 1);
        } else if (line == end_tag) {
            CHECK(in_block);
            in_block = false;
        } else if (line == site_tag) {
            if (in_block)
                last_block_site = sites;
            if (lines[i - 1].rfind("G4 P", 0) == 0) {
                // The wait is over the tower: inside the block, right after the travel there, not lifted.
                CHECK(in_block);
                CHECK(lines[i - 2].rfind("G1 Z", 0) != 0);
                last_wait_site = sites;
            }
            ++sites;
        }
    }
    CHECK_FALSE(in_block);
    // 53 layers; the waits stop before the top, and so does the tower, but not below its last wait.
    REQUIRE(blocks > 0);
    CHECK(blocks < 53);
    CHECK(last_block_site >= last_wait_site);
    CHECK(count_lines_starting(out, "G4 P") > 0);
    check_blended(measured_intervals(out), max_diff_s);

    // Every extrusion is at its layer's height: leaving the blocks out kept the moves after them valid.
    double z = 0., layer_z = 0.;
    size_t extrusions = 0;
    for (const std::string &line : lines) {
        if (line.rfind(";Z:", 0) == 0)
            layer_z = std::stod(line.substr(3));
        if (std::optional<double> v = axis_value(line, 'Z'))
            z = *v;
        if (axis_value(line, 'E') && *axis_value(line, 'E') > 0. && (axis_value(line, 'X') || axis_value(line, 'Y'))) {
            CHECK_THAT(z, WithinAbs(layer_z, 1e-6));
            ++extrusions;
        }
    }
    CHECK(extrusions > 0);
}

TEST_CASE("Parked waits go to the park position and come back to where they left", "[LayerTimeBlend]")
{
    const double       max_diff_s = 2.;
    DynamicPrintConfig config     = wait_at_config("park", max_diff_s);
    config.set_deserialize_strict({{"layer_time_blend_park_x", "10"}, {"layer_time_blend_park_y", "190"}, {"layer_time_blend_in_place_max", "2"}});
    const std::string              out   = slice_mesh(pillar_with_slab(), config);
    const std::vector<std::string> lines = lines_of(out);
    double x = 0., y = 0.;
    size_t parked = 0;
    for (size_t i = 0; i + 4 < lines.size(); ++i) {
        if (std::optional<double> v = axis_value(lines[i], 'X'))
            x = *v;
        if (std::optional<double> v = axis_value(lines[i], 'Y'))
            y = *v;
        // lift, out, G4, back, down, site
        if (lines[i].rfind("G1 Z", 0) != 0 || lines[i + 2].rfind("G4 P", 0) != 0 || !axis_value(lines[i + 1], 'X'))
            continue;
        CHECK_THAT(*axis_value(lines[i + 1], 'X'), WithinAbs(10., 1e-3));
        CHECK_THAT(*axis_value(lines[i + 1], 'Y'), WithinAbs(190., 1e-3));
        CHECK_THAT(*axis_value(lines[i + 3], 'X'), WithinAbs(x, 1e-3));
        CHECK_THAT(*axis_value(lines[i + 3], 'Y'), WithinAbs(y, 1e-3));
        CHECK(lines[i + 5] == site_tag);
        ++parked;
    }
    CHECK(parked > 0);
    check_blended(measured_intervals(out), max_diff_s);
}

TEST_CASE("Waiting at a purge tower without one parks", "[LayerTimeBlend]")
{
    DynamicPrintConfig config = wait_at_config("purge_tower", 2.);
    config.set_deserialize_strict({{"layer_time_blend_park_x", "10"}, {"layer_time_blend_park_y", "190"}});
    const std::string out = slice_mesh(pillar_with_slab(), config);
    CHECK(out.find("G1 X10 Y190") != std::string::npos);
    check_blended(measured_intervals(out), 2.);
}

TEST_CASE("The bundled mod loads and drives the planner", "[LayerTimeBlend]")
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    DynamicPrintConfig config = blend_config("", 2.);
    // Through the mod: linear, D = 2 s, waiting in place.
    config.set_key_value("mod_settings", new ConfigOptionString(
        R"json({"layer_time_blend.blend_type": "linear", "layer_time_blend.max_diff": 2, "layer_time_blend.wait_at": "in_place",
                "layer_time_blend.warn_total_pct": 0, "layer_time_blend.warn_single": 0})json"));
    const std::string out = slice_mesh(pillar_with_slab(), config);
    CHECK(count_lines_starting(out, site_tag) == 53);
    CHECK(count_lines_starting(out, "G4 P") > 0);
    check_blended(measured_intervals(out), 2.);
    // Off through the mod: nothing.
    config.set_key_value("mod_settings", new ConfigOptionString(R"json({"layer_time_blend.blend_type": "off"})json"));
    CHECK(count_lines_starting(slice_mesh(pillar_with_slab(), config), site_tag) == 0);
}

namespace {

// Estimated time of a hand-written G-code, s, with layer time blending around the low temperature interface
// on or off.
double estimated_time(const std::string &gcode, bool blending, std::vector<GCodeProcessor::WaitSiteTime> *marks = nullptr)
{
    PrintConfig config;
    config.layer_time_planner.value                      = blending ? "blend" : "";
    config.layer_time_blend_heat_rate.value              = 2.; // degC/s
    config.layer_time_blend_cool_rate.value              = 1.; // degC/s
    GCodeProcessor processor;
    processor.reset();
    processor.initialize_result_moves();
    processor.apply_config(config);
    processor.set_temperature_wait_estimate(blending);
    processor.initialize("estimate.gcode");
    processor.process_buffer(gcode);
    processor.finalize(false);
    if (marks)
        *marks = processor.wait_site_times();
    return processor.get_time(PrintEstimatedStatistics::ETimeMode::Normal);
}

} // namespace

TEST_CASE("Temperature waits count while blending around the low temperature interface", "[LayerTimeBlend]")
{
    // Heat to 200 degC (100 s at 2 degC/s), set 170 degC and move for 10 s (the nozzle cools 10 degC at
    // 1 degC/s), then wait for 170 degC: 20 s more.
    const std::string gcode = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\nM104 S170\nG1 X100 F600\n;MAGPIE_WAIT_SITE\nM109 R170\n"
                              "G1 X0 F600\n;MAGPIE_WAIT_SITE\n";
    const double off = estimated_time(gcode, false);
    std::vector<GCodeProcessor::WaitSiteTime> marks;
    const double on = estimated_time(gcode, true, &marks);
    // The moves alone: 100 mm and back at 10 mm/s, plus acceleration; Orca counts M109 as no time.
    CHECK(off < 25.);
    CHECK_THAT(on - off, WithinAbs(100. + 20., 0.5));
    // The first site comes after the heat-up; the second after the 20 s cool-down, which it is flagged for.
    REQUIRE(marks.size() == 2);
    CHECK(marks[0].uncertain);
    CHECK(marks[1].uncertain);
    // Between them: the move back (half the moves' time) and the 20 s wait.
    CHECK_THAT(marks[1].time_s - marks[0].time_s, WithinAbs(20. + off / 2., 0.5));
}

TEST_CASE("Under H/2 a wait site starts each full layer only", "[LayerTimeBlend]")
{
    DynamicPrintConfig config = blend_config("blend", 1e6);
    config.set_deserialize_strict({{"outer_wall_half_layer_height", "1"}});
    const std::string out = slice_mesh(pillar_with_slab(), config);
    // The 53 layers of the plain slice: half-height passes are no layers of their own.
    CHECK(count_lines_starting(out, site_tag) == 53);
}
