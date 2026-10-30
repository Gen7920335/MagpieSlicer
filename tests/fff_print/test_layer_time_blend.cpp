// MAGPIE: layer time blending (gcode.time_plan slot, the bundled native mod mods/layer_time_blend): wait
// sites, in-place waits filled in after the export, and the re-estimate of the G-code as written. Every
// test turns the mod on through its parameters, as the settings page does, with the bundled mods loaded.
#include <catch2/catch_all.hpp>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/LayerTimeTower.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_helpers.hpp"
#include "test_utils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

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

// The mod's parameters as JSON members ("layer_time_blend.<name>": <value>), joined after `first`.
std::string mod_json(const std::string &first, const std::vector<std::pair<std::string, std::string>> &params)
{
    std::string out = "{" + first;
    for (const auto &[name, value] : params)
        out += std::string(out.size() > 1 ? ", " : "") + "\"layer_time_blend." + name + "\": " + value;
    return out + "}";
}

// Blending linear with limit max_diff_s (s) when `planner` is "blend", off when it is empty; `params` and
// `printer_params` add mod parameters of the process and printer scopes (values as JSON).
DynamicPrintConfig blend_config(const std::string &planner, double max_diff_s,
                                const std::vector<std::pair<std::string, std::string>> &params = {},
                                const std::vector<std::pair<std::string, std::string>> &printer_params = {})
{
    std::vector<std::pair<std::string, std::string>> process = {
        {"blend_type", planner == "blend" ? "\"linear\"" : "\"off\""},
        {"max_diff", std::to_string(max_diff_s)},
        // The default waits over a tower; these tests choose where they wait.
        {"wait_at", "\"in_place\""},
        {"warn_total_pct", "0"},
        {"warn_single", "0"},
    };
    for (const auto &p : params) {
        auto it = std::find_if(process.begin(), process.end(), [&p](const auto &q) { return q.first == p.first; });
        if (it == process.end())
            process.push_back(p);
        else
            it->second = p.second;
    }
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
    });
    config.set_key_value("mod_settings", new ConfigOptionString(mod_json("", process)));
    config.set_key_value("printer_mod_settings", new ConfigOptionString(mod_json("", printer_params)));
    return config;
}

// Slices `mesh` placed at the middle of the default 200 x 200 mm bed (not arranged, which would centre it
// on the origin, half off the bed), so there is room for a tower and a park position around it. The
// bundled mods are active meanwhile: their rules and the planner's library.
std::string slice_mesh(TriangleMesh mesh, const DynamicPrintConfig &config)
{
    BundledMods mods;
    REQUIRE(mods.errors.empty());
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
    // The header names the native mod the blended G-code used, and only there.
    CHECK(count_lines_starting(blended, "; mods: layer_time_blend ") == 1);
    CHECK(count_lines_starting(plain, "; mods:") == 0);
    // The header's wall-clock time differs when the two slices straddle a second (seen under a loaded ctest -j 8).
    CHECK(without_lines_starting(without_lines_starting(without_lines_starting(blended, site_tag), "; mods:"), "; generated by") ==
          without_lines_starting(plain, "; generated by"));
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
    // The moves around a wait; the time estimator's M73 progress lines (Orca places them by the estimated time)
    // can fall between a move and the wait and say nothing about the nozzle.
    auto move_before = [&lines](size_t i) {
        while (i > 0 && lines[i - 1].rfind("M73 ", 0) == 0)
            --i;
        return i - 1;
    };
    auto move_after = [&lines](size_t i) {
        while (i + 2 < lines.size() && lines[i + 1].rfind("M73 ", 0) == 0)
            ++i;
        return i + 1;
    };
    size_t checked = 0;
    for (size_t i = 1; i + 1 < lines.size(); ++i) {
        if (lines[i].rfind("G4 P", 0) != 0)
            continue;
        const size_t up = move_before(i), down = move_after(i);
        if (lines[up].rfind("G1 Z", 0) != 0) {
            // A bare G4 is a short wait (under 1 s, 1000 ms), which does not lift.
            // PERMANENT DIAGNOSTIC: the lines around a long wait that was not lifted.
            INFO("before: " << lines[up] << " | wait: " << lines[i] << " | after: " << lines[down]);
            CHECK(std::stoll(lines[i].substr(4)) < 1000);
            continue;
        }
        REQUIRE(lines[down].rfind("G1 Z", 0) == 0);
        // Up to 1 mm over the new layer, then down to the new layer's Z, which the next travel would have
        // raised the nozzle to anyway: never below where the nozzle was.
        CHECK_THAT(z_of(lines[up]) - z_of(lines[down]), WithinAbs(1., 1e-6));
        CHECK(z_of(lines[down]) >= z_before[up] - 1e-6);
        CHECK(z_of(lines[down]) - z_before[up] <= 0.3 + 1e-6); // at most one layer (0.3 mm) up
        ++checked;
    }
    CHECK(checked > 0);
}

namespace {

DynamicPrintConfig wait_at_config(const std::string &wait_at, double max_diff_s,
                                  const std::vector<std::pair<std::string, std::string>> &params = {},
                                  const std::vector<std::pair<std::string, std::string>> &printer_params = {})
{
    std::vector<std::pair<std::string, std::string>> all = {{"wait_at", "\"" + wait_at + "\""}};
    all.insert(all.end(), params.begin(), params.end());
    DynamicPrintConfig config = blend_config("blend", max_diff_s, all, printer_params);
    config.set_deserialize_strict({{"use_relative_e_distances", "1"}});
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

namespace {

// The filament each forward E move pushes (extrusions and unretracts), mm, in G-code order, as the printer
// reads them: relative E as written, absolute E as the step from the last E (G92 included).
std::vector<double> extrusion_steps(const std::vector<std::string> &lines, bool absolute_e)
{
    std::vector<double> steps;
    double              e = 0.;
    for (const std::string &line : lines) {
        if (line.rfind("G92 ", 0) == 0) {
            const size_t at = line.find(" E");
            if (at != std::string::npos)
                e = std::stod(line.substr(at + 2));
            continue;
        }
        const std::optional<double> v = axis_value(line, 'E');
        if (!v)
            continue;
        const double step = absolute_e ? *v - e : *v;
        if (absolute_e)
            e = *v;
        if (step > 0.)
            steps.push_back(step);
    }
    return steps;
}

size_t count_lines(const std::vector<std::string> &lines, const std::string &line)
{
    return size_t(std::count(lines.begin(), lines.end(), line));
}

} // namespace

TEST_CASE("With absolute E the tower is cut above the last wait as with relative E, and E stays continuous", "[LayerTimeBlend]")
{
    // As in the tower test: 10 s ramps end well below the pillar's top, so the tower can stop there.
    const double       max_diff_s = 10.;
    const std::string  relative   = slice_mesh(pillar_with_slab(), wait_at_config("tower", max_diff_s));
    DynamicPrintConfig config     = wait_at_config("tower", max_diff_s);
    config.set_deserialize_strict({{"use_relative_e_distances", "0"}});
    const std::string  absolute   = slice_mesh(pillar_with_slab(), config);
    const std::vector<std::string> rel = lines_of(relative), abs = lines_of(absolute);
    const std::string begin_tag = ";MAGPIE_WAIT_TOWER_BEGIN";
    // The same cut with either E mode: fewer blocks than the 53 layers.
    const size_t blocks = count_lines(rel, begin_tag);
    REQUIRE(blocks > 0);
    CHECK(blocks < 53);
    CHECK(count_lines(abs, begin_tag) == blocks);
    // Leaving a block out must not hand its filament to the next move (the unretract after it): the largest
    // forward step is the relative output's (a left-out block's E would add a few mm to one move), up to the
    // rounding of E to 5 decimals on both sides, mm.
    const std::vector<double> rel_steps = extrusion_steps(rel, false), abs_steps = extrusion_steps(abs, true);
    REQUIRE(!rel_steps.empty());
    REQUIRE(abs_steps.size() == rel_steps.size());
    CHECK(*std::max_element(abs_steps.begin(), abs_steps.end()) <= *std::max_element(rel_steps.begin(), rel_steps.end()) + 1e-4);
    check_blended(measured_intervals(absolute), max_diff_s);
}

TEST_CASE("Purge tower waits happen at the temperature drop tower's visit and blend the model's layer times", "[LayerTimeBlend]")
{
    // The slab's underside needs support; the low temperature interface prints a drop tower on every layer up to
    // the last interface. The sites stay at the layer starts, so the blended intervals are the model's layer
    // times; a wait for a layer whose layer before visited the tower is written at that visit, over the tower.
    const double       max_diff_s = 2.;
    DynamicPrintConfig config     = wait_at_config("purge_tower", max_diff_s, {}, {{"park_x", "10"}, {"park_y", "190"}});
    config.set_deserialize_strict({{"enable_support", "1"}, {"support_interface_top_layers", "2"}});
    const std::string mod_settings = config.opt_string("mod_settings");
    config.set_key_value("mod_settings", new ConfigOptionString(mod_settings.substr(0, mod_settings.size() - 1) +
        R"(, "low_temp_interface.enable": true, "low_temp_interface.temperature": 170, "low_temp_interface.tower": true})"));
    const std::string              out   = slice_mesh(pillar_with_slab(), config);
    const std::vector<std::string> lines = lines_of(out);
    REQUIRE(out.find("; temperature drop tower layer") != std::string::npos);
    const std::string visit_tag = ";MAGPIE_WAIT_VISIT";
    size_t visits = 0, tower_waits = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (lines[i] != visit_tag)
            continue;
        ++visits;
        // At the drop tower: its marker comes before the next extrusion.
        bool at_tower = false;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (lines[j].rfind("; temperature drop tower", 0) == 0 || lines[j].rfind("; low-temperature support interface temperature drop tower", 0) == 0) {
                at_tower = true;
                break;
            }
            if (axis_value(lines[j], 'E') && (axis_value(lines[j], 'X') || axis_value(lines[j], 'Y')))
                break;
        }
        CHECK(at_tower);
        if (lines[i - 1].rfind("G4 P", 0) == 0) {
            // A bare wait where the nozzle already is: no lift and no trip before it.
            CHECK(lines[i - 2].rfind("G1 Z", 0) != 0);
            const bool parked = axis_value(lines[i - 2], 'X').has_value() && lines[i - 3].rfind("G1 Z", 0) == 0;
            CHECK_FALSE(parked);
            ++tower_waits;
        }
    }
    CHECK(count_lines_starting(out, site_tag) == 53);
    CHECK(visits > 0);
    CHECK(tower_waits > 0);
    // The intervals between the layer-start sites, the model's layer times, are the ones blended.
    check_blended(measured_intervals(out), max_diff_s);
}

TEST_CASE("Parked waits go to the park position and come back to where they left", "[LayerTimeBlend]")
{
    const double       max_diff_s = 2.;
    DynamicPrintConfig config     = wait_at_config("park", max_diff_s, {{"in_place_max", "2"}}, {{"park_x", "10"}, {"park_y", "190"}});
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
    DynamicPrintConfig config = wait_at_config("purge_tower", 2., {}, {{"park_x", "10"}, {"park_y", "190"}});
    const std::string out = slice_mesh(pillar_with_slab(), config);
    CHECK(out.find("G1 X10 Y190") != std::string::npos);
    check_blended(measured_intervals(out), 2.);
}

TEST_CASE("The bundled mod loads and drives the planner", "[LayerTimeBlend]")
{
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
    GCodeProcessor processor;
    processor.reset();
    processor.initialize_result_moves();
    processor.apply_config(config);
    processor.set_temperature_wait_estimate(blending);
    processor.set_temperature_rates(2., 1.); // degC/s
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

// Without blending, a wait inside the print (after the first layer change) still takes its time; the start
// G-code heat-up before it stays at none, as in Orca.
TEST_CASE("Temperature waits inside the print count without blending", "[LayerTimeBlend][GCodeProcessor]")
{
    // The layer change tag as the processor reads it (it differs between Bambu and other printers).
    const std::string layer_change = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // Heat to 200 degC before the first layer (not counted), set 170 degC and move for 10 s (the nozzle cools
    // 10 degC at 1 degC/s), then wait for 170 degC: 20 s more.
    const std::string inside = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\n" + layer_change + "M104 S170\nG1 X100 F600\nM109 R170\n"
                               "G1 X0 F600\n";
    const std::string before = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\nM104 S170\nG1 X100 F600\nM109 R170\n"
                               "G1 X0 F600\n";
    const double moves_only = estimated_time(before, false);
    CHECK(moves_only < 25.);
    CHECK_THAT(estimated_time(inside, false) - moves_only, WithinAbs(20., 0.5));
    // An M109 that only confirms the temperature already reached adds nothing.
    const std::string reached = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\n" + layer_change + "G1 X100 F600\nM109 S200\nG1 X0 F600\n";
    CHECK_THAT(estimated_time(reached, false), WithinAbs(moves_only, 0.5));
}

// Two 10 mm cubes, 12 mm tall, one at 0.3 mm layers and one at 0.2 mm: G-code layers follow the pattern of both
// heights, so their times alternate with it although each cube's own time between its layers does not. A site
// starts only a layer where both cubes start one, every 0.6 mm, so each interval is a whole period of the pattern.
TEST_CASE("With objects of different layer heights a wait site starts only layers every object starts", "[LayerTimeBlend]")
{
    // A limit no pair reaches: no waits, the intervals as sliced.
    DynamicPrintConfig config = blend_config("blend", 1e6);
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    meshes.push_back(make_cube(10., 10., 12.));
    meshes.push_back(make_cube(10., 10., 12.));
    const std::vector<std::vector<ConfigBase::SetDeserializeItem>> per_object = { {}, { { "layer_height", "0.2" } } };
    init_print(std::move(meshes), print, model, config, &per_object);
    const std::string out = gcode(print);

    // The heights where every cube still printing (between its first and last layer) has a layer, from the
    // sliced objects.
    REQUIRE(print.objects().size() == 2);
    size_t common = 0, any = 0;
    std::set<long long> heights; // print_z, um
    for (const PrintObject *object : print.objects())
        for (const Layer *layer : object->layers())
            heights.insert(std::llround(layer->print_z * 1000.));
    for (long long z_um : heights) {
        const double z = double(z_um) / 1000.; // mm
        ++any;
        bool all = true;
        for (const PrintObject *object : print.objects())
            if (z <= object->layers().back()->print_z + EPSILON)
                all = all && object->get_layer_at_printz(z, EPSILON) != nullptr;
        common += all;
    }
    CHECK(common < any);
    CHECK(count_lines_starting(out, site_tag) == common);
    // Each interval is a whole period of the pattern, the same work every time: between the bottom and top shells
    // (five periods, 3 mm, from either end) they differ by little, 29.8 to 31.9 s on 2026-10-27; the period after
    // the bottom shells took 33.4 s. Between G-code layers they alternated by the time of a whole cube layer.
    const std::vector<double> intervals = measured_intervals(out);
    REQUIRE(intervals.size() >= 12);
    for (size_t k = 6; k + 4 < intervals.size(); ++k)
        CHECK_THAT(intervals[k], WithinRel(intervals[k - 1], 0.1)); // within 10 % of the period before
}

TEST_CASE("Under H/2 a wait site starts each full layer only", "[LayerTimeBlend]")
{
    DynamicPrintConfig config = blend_config("blend", 1e6);
    Slic3r::Test::half_layer_params(config, true);
    const std::string out = slice_mesh(pillar_with_slab(), config);
    // The 53 layers of the plain slice: half-height passes are no layers of their own.
    CHECK(count_lines_starting(out, site_tag) == 53);
}

// The tower block comes at the start of a layer, before the H/2 frame's first band: its travels used to find no
// logical layer and stop the slice ("Half-height travel requires an explicit logical parent layer").
TEST_CASE("Under H/2 the tower waits happen over the tower, at the layer's height", "[LayerTimeBlend]")
{
    DynamicPrintConfig config = wait_at_config("tower", 10.);
    Slic3r::Test::half_layer_params(config, true);
    const std::string              out   = slice_mesh(pillar_with_slab(), config);
    const std::vector<std::string> lines = lines_of(out);
    const std::string begin_tag = ";MAGPIE_WAIT_TOWER_BEGIN", end_tag = ";MAGPIE_WAIT_TOWER_END";
    size_t blocks = 0, waits_in_block = 0, block_extrusions = 0;
    bool   in_block = false;
    double z = 0., layer_z = 0.;
    for (size_t i = 0; i < lines.size(); ++i) {
        const std::string &line = lines[i];
        if (line.rfind(";Z:", 0) == 0)
            layer_z = std::stod(line.substr(3));
        if (std::optional<double> v = axis_value(line, 'Z'))
            z = *v;
        if (line == begin_tag) {
            in_block = true;
            ++blocks;
        } else if (line == end_tag) {
            in_block = false;
        } else if (in_block) {
            if (line == site_tag && lines[i - 1].rfind("G4 P", 0) == 0)
                ++waits_in_block;
            const bool xy = axis_value(line, 'X') || axis_value(line, 'Y');
            // Never below the layer: the travel over to the tower keeps at least the layer's height.
            if (xy)
                CHECK(z >= layer_z - 1e-6);
            if (xy && axis_value(line, 'E') && *axis_value(line, 'E') > 0.) {
                CHECK_THAT(z, WithinAbs(layer_z, 1e-6));
                ++block_extrusions;
            }
        }
    }
    CHECK_FALSE(in_block);
    CHECK(blocks > 0);
    CHECK(waits_in_block > 0);
    CHECK(block_extrusions > 0);
    // One site per full layer, each in its layer's block, as without H/2; the blocks left out above the last wait
    // take their sites along (where the cut falls follows the layer times, which H/2 changes).
    CHECK(count_lines_starting(out, site_tag) == blocks);
    CHECK(blocks < 53);
}

namespace {

// The lowest X and Y the G-code extrudes at inside the first tower block (its brim's outer loop), mm.
Vec2d tower_brim_min(const std::vector<std::string> &lines)
{
    Vec2d min(std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    bool  in_block = false;
    for (const std::string &line : lines) {
        if (line == ";MAGPIE_WAIT_TOWER_BEGIN")
            in_block = true;
        else if (line == ";MAGPIE_WAIT_TOWER_END")
            break;
        else if (in_block && axis_value(line, 'E') && *axis_value(line, 'E') > 0.) {
            if (std::optional<double> x = axis_value(line, 'X'))
                min.x() = std::min(min.x(), *x);
            if (std::optional<double> y = axis_value(line, 'Y'))
                min.y() = std::min(min.y(), *y);
        }
    }
    return min;
}

} // namespace

// MAGPIE: the 3D scene draws and drags the layer time tower where place_layer_time_tower puts it from the model
// instances' footprints and the plate's stored position (GLCanvas3D::load_layer_time_tower_previews); the export
// prints it from the sliced objects through the same function. On a plain plate the two agree: the brim's outer loop
// starts where the footprint the function gives puts it.
TEST_CASE("The layer time tower prints where the 3D scene shows it", "[LayerTimeBlend]")
{
    const double stored_x = GENERATE(-1., 20.); // mm, -1: automatic
    CAPTURE(stored_x);
    DynamicPrintConfig config = wait_at_config("tower", 10.);
    if (stored_x >= 0.) {
        config.set_key_value("layer_time_blend_tower_x", new ConfigOptionFloats{ stored_x });
        config.set_key_value("layer_time_blend_tower_y", new ConfigOptionFloats{ 30. });
    }
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    Print print;
    Model model;
    TriangleMesh mesh = pillar_with_slab();
    mesh.translate(100.f, 100.f, 0.f);
    std::vector<TriangleMesh> meshes;
    meshes.push_back(std::move(mesh));
    init_print(std::move(meshes), print, model, config, nullptr, /*arrange=*/false);
    const std::string out = gcode(print);

    // What the 3D scene computes, from the model.
    const double nozzle = config.option<ConfigOptionFloats>("nozzle_diameter")->get_at(0); // mm
    std::vector<LayerTimeTowerObject> objects;
    for (const ModelObject *object : model.objects)
        for (size_t i = 0; i < object->instances.size(); ++i) {
            const BoundingBoxf3 box = object->instance_bounding_box(i);
            objects.push_back({ BoundingBoxf(Vec2d(box.min.x(), box.min.y()), Vec2d(box.max.x(), box.max.y())), config.opt_float("brim_width") });
        }
    const std::optional<LayerTimeTowerSpot> spot =
        place_layer_time_tower(config.option<ConfigOptionPoints>("printable_area")->values, 15., layer_time_tower_brim(nozzle),
                               layer_time_tower_skirt(config.opt_int("skirt_loops"), config.opt_float("skirt_distance"), nozzle), objects,
                               std::nullopt, stored_x, stored_x >= 0. ? 30. : -1.);
    REQUIRE(spot);
    REQUIRE(spot->fits);
    if (stored_x >= 0.) {
        CHECK_THAT(spot->origin.x(), WithinAbs(20., 1e-9));
        CHECK_THAT(spot->origin.y(), WithinAbs(30., 1e-9));
    }
    // The brim's outer loop on the first layer (0.3 mm): its five lines at that layer's spacing out from the tower's
    // outline, which sits a brim (laid out at half the nozzle's height) inside the footprint; minus half a line, mm.
    // The G-code has three decimals.
    const Flow   first(Flow::auto_extrusion_width(frPerimeter, float(nozzle)), 0.3f, float(nozzle));
    const double inset   = layer_time_tower_brim(nozzle) - (LAYER_TIME_TOWER_BRIM_LINES * double(first.spacing()) - 0.5 * double(first.width()));
    const Vec2d  printed = tower_brim_min(lines_of(out));
    CHECK_THAT(printed.x(), WithinAbs(spot->origin.x() + inset, 2e-3));
    CHECK_THAT(printed.y(), WithinAbs(spot->origin.y() + inset, 2e-3));
}

TEST_CASE("A stored layer time tower position over an object does not print", "[LayerTimeBlend]")
{
    const std::vector<Vec2d> bed = { { 0., 0. }, { 200., 0. }, { 200., 200. }, { 0., 200. } };
    const std::vector<LayerTimeTowerObject> objects = { { BoundingBoxf(Vec2d(90., 90.), Vec2d(110., 110.)), 0. } };
    const double side = 15., brim = 2.; // mm
    // Stored over the object: shown where it was put, not printed.
    std::optional<LayerTimeTowerSpot> spot = place_layer_time_tower(bed, side, brim, 0., objects, std::nullopt, 95., 95.);
    REQUIRE(spot);
    CHECK_THAT(spot->origin.x(), WithinAbs(95., 1e-9));
    CHECK_THAT(spot->origin.y(), WithinAbs(95., 1e-9));
    CHECK_FALSE(spot->fits);
    // Stored past the bed's edge: clamped onto it (AUX_TOWER_BED_MARGIN inside).
    spot = place_layer_time_tower(bed, side, brim, 0., objects, std::nullopt, 500., 10.);
    REQUIRE(spot);
    CHECK_THAT(spot->origin.x(), WithinAbs(200. - AUX_TOWER_BED_MARGIN - (side + 2. * brim), 1e-9));
    CHECK_THAT(spot->origin.y(), WithinAbs(10., 1e-9));
    CHECK(spot->fits);
    // Automatic: right of the objects, 20 mm off their box, centred on it; a prime tower there pushes it left.
    spot = place_layer_time_tower(bed, side, brim, 0., objects, std::nullopt, -1., -1.);
    REQUIRE(spot);
    CHECK_THAT(spot->origin.x(), WithinAbs(130., 1e-9));
    CHECK_THAT(spot->origin.y(), WithinAbs(100. - 0.5 * (side + 2. * brim), 1e-9));
    spot = place_layer_time_tower(bed, side, brim, 0., objects, LayerTimeTowerPrimeTower{ Vec2d(135., 95.), 10.f, 10.f, 2.f }, -1., -1.);
    REQUIRE(spot);
    CHECK(spot->origin.x() < 90.);
    CHECK(spot->fits);
}

TEST_CASE("A tower that does not fit parks instead", "[LayerTimeBlend]")
{
    // Review 2026-10-22: the core parked, but the planner read wait_at "tower" and planned in-place waits.
    DynamicPrintConfig config = wait_at_config("tower", 2., {{"tower_size", "400"}, {"in_place_max", "2"}}, {{"park_x", "10"}, {"park_y", "190"}});
    const std::string  out    = slice_mesh(pillar_with_slab(), config);
    CHECK(out.find("G1 X10 Y190") != std::string::npos);
    check_blended(measured_intervals(out), 2.);
}

TEST_CASE("In a by-object print the waits park above the objects already printed", "[LayerTimeBlend]")
{
    // The tower is not printed by object, so the waits park; a trip from the second object must clear the
    // first, which is finished (16 mm tall), by the in-place lift (1 mm).
    DynamicPrintConfig config = wait_at_config("tower", 2., {{"in_place_max", "2"}}, {{"park_x", "10"}, {"park_y", "190"}});
    config.set_deserialize_strict({{"print_sequence", "by object"}});
    BundledMods mods;
    REQUIRE(mods.errors.empty());
    Print print;
    Model model;
    std::vector<TriangleMesh> meshes;
    for (const float x : {60.f, 140.f}) {
        TriangleMesh mesh = pillar_with_slab();
        mesh.translate(x, 100.f, 0.f);
        meshes.push_back(std::move(mesh));
    }
    init_print(std::move(meshes), print, model, config, nullptr, /*arrange=*/false);
    const std::vector<std::string> lines = lines_of(gcode(print));
    double max_layer_z = 0.;
    size_t parked = 0, parked_over_first = 0;
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (lines[i].rfind(";Z:", 0) == 0)
            max_layer_z = std::max(max_layer_z, std::stod(lines[i].substr(3)));
        if (lines[i].rfind("G1 Z", 0) != 0 || lines[i + 1].find("X10 Y190") == std::string::npos)
            continue;
        CHECK(*axis_value(lines[i], 'Z') >= max_layer_z + 1. - 1e-3);
        ++parked;
        parked_over_first += max_layer_z > 15.;
    }
    CHECK(parked > 0);
    CHECK(parked_over_first > 0);
}

TEST_CASE("The tower catches up on a raft in loops no taller than the nozzle's tallest layer", "[LayerTimeBlend]")
{
    // Review 2026-10-22: the tower's layer was as tall as the step since its last one, and the first site comes
    // with the first object layer, on top of a 12-layer raft (3.6 mm): a 3.6 mm bead has no flow (Flow::spacing()
    // threw and the slice stopped). Now the first block prints that height in loops of at most max_layer_height
    // (0.3 mm here, set below), each at its own Z; the blocks above print one loop as before.
    DynamicPrintConfig config = wait_at_config("tower", 10.);
    config.set_deserialize_strict({{"raft_layers", "12"}, {"max_layer_height", "0.3"}});
    const std::vector<std::string> lines = lines_of(slice_mesh(pillar_with_slab(), config));
    const std::string begin_tag = ";MAGPIE_WAIT_TOWER_BEGIN", end_tag = ";MAGPIE_WAIT_TOWER_END";
    std::vector<size_t> loops_per_block; // tower loops (the "layer time tower" extrusions) of each block
    std::vector<double> z_per_block;     // the Z the block's last loop is printed at
    std::vector<double> z_first_loop;    // the Z the block's first loop is printed at
    bool   in_block = false;
    double z        = 0.;
    for (const std::string &line : lines) {
        if (line == begin_tag) {
            in_block = true;
            loops_per_block.push_back(0);
            z_per_block.push_back(0.);
            z_first_loop.push_back(-1.);
        } else if (line == end_tag) {
            in_block = false;
        } else if (in_block) {
            if (const std::optional<double> value = axis_value(line, 'Z'))
                z = *value;
            // Every extrusion segment of a loop carries the comment (gcode_comments is on in the tests): a loop is
            // a square of four segments; the Z moves between loops say "loop height" and are not counted.
            if (line.find("; layer time tower") != std::string::npos && line.find("brim") == std::string::npos &&
                line.find("loop height") == std::string::npos && line.rfind("G1 ", 0) == 0) {
                ++loops_per_block.back();
                z_per_block.back() = z;
                if (z_first_loop.back() < 0.)
                    z_first_loop.back() = z;
            }
        }
    }
    REQUIRE(loops_per_block.size() > 2);
    for (size_t &segments : loops_per_block)
        segments /= 4;
    // The first block spans the raft: at least 3.6 mm / 0.3 mm loops; the next ones one loop each.
    CHECK(loops_per_block.front() >= 12);
    CHECK(loops_per_block[1] == 1);
    // The loops of the first block climb from the first loop's height to the layer's Z, each at its own Z (not all
    // at the layer's Z in the air): at least 11 loops of 0.3 mm between the first and the last.
    CHECK(z_per_block.front() - z_first_loop.front() >= 11 * 0.3 - 1e-3);
    CHECK(z_first_loop.front() <= 0.3 + 1e-3);
    // Each block ends at its layer's Z: the blocks' Z values rise by one layer (0.3 mm) from the first.
    CHECK_THAT(z_per_block[1] - z_per_block[0], WithinAbs(0.3, 1e-3));
}

TEST_CASE("The layer time tower is printed in the width of the nozzle printing it", "[LayerTimeBlend]")
{
    // The tower read its nozzle through the print's filament map (cur_extruder_index()), the writer through its own:
    // sliced for a Snapmaker U1 the print's map was 1,1,1,1 and the writer's 1,2,3,4, so filament 4 on its 0.2 mm
    // toolhead printed the tower 0.9 mm wide, nozzle 1's 0.8 mm automatic perimeter width (OrcaCube, 2026-10-23).
    // This fixture's two maps agree (1,2), so it states the width the tower must have rather than reproducing that
    // split; the OrcaCube run is the counterfactual.
    DynamicPrintConfig config = multifilament_config(2, {
        {"outer_wall_filament_id", 2}, {"inner_wall_filament_id", 2}, {"sparse_infill_filament_id", 2},
        {"internal_solid_filament_id", 2}, {"top_surface_filament_id", 2}, {"bottom_surface_filament_id", 2},
        {"layer_height", 0.3}, {"initial_layer_print_height", 0.3}, // mm
        {"wall_loops", 2}, {"sparse_infill_density", 15}, {"enable_support", false}, {"skirt_loops", 0},
        {"brim_type", "no_brim"}, {"slow_down_for_layer_cooling", false}, {"use_relative_e_distances", true},
        {"enable_prime_tower", false},
    });
    config.set_num_extruders(2);
    config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
    const double first_nozzle_mm = 0.8, second_nozzle_mm = 0.4;
    config.set_key_value("nozzle_diameter", new ConfigOptionFloats{first_nozzle_mm, second_nozzle_mm});
    size_flush_to_nozzles(config);
    const DynamicPrintConfig blend = wait_at_config("tower", 10.);
    for (const char *key : {"mod_settings", "printer_mod_settings"})
        config.set_key_value(key, blend.option(key)->clone());

    const std::vector<std::string> lines = lines_of(slice_mesh(pillar_with_slab(), config));
    std::set<double> tower_widths; // mm, the widths the tower blocks extrude at
    bool             in_block = false;
    double           width    = 0.; // mm, the last ;WIDTH: tag (written only when the width changes)
    for (const std::string &line : lines) {
        if (line == ";MAGPIE_WAIT_TOWER_BEGIN")
            in_block = true;
        else if (line == ";MAGPIE_WAIT_TOWER_END")
            in_block = false;
        else if (line.rfind(";WIDTH:", 0) == 0)
            width = std::stod(line.substr(7));
        else if (in_block && axis_value(line, 'E') && *axis_value(line, 'E') > 0. && (axis_value(line, 'X') || axis_value(line, 'Y')))
            tower_widths.insert(width);
    }
    REQUIRE_FALSE(tower_widths.empty());
    // Flow's automatic perimeter width is 1.125 x the nozzle.
    for (const double width : tower_widths)
        CHECK_THAT(width, WithinAbs(1.125 * second_nozzle_mm, 1e-3));
}

TEST_CASE("The layer time tower is printed by the chosen toolhead, its wait at its visit", "[LayerTimeBlend]")
{
    // tower_toolhead (Gen 2026-10-30): the tower's own tool. ToolOrdering plans that tool into every layer, the
    // export prints the tower once the tool is in and the wait happens at its visit tag; the site tag stays at the
    // layer start, so the intervals are still the model's layer times. 0 keeps the tool in use: one filament
    // prints, so no tool changes at all.
    const int tower_toolhead = GENERATE(0, 2);
    DYNAMIC_SECTION("tower toolhead " << tower_toolhead) {
        // Two toolheads of 0.4 mm; the model prints with filament 1 (toolhead 1) alone.
        DynamicPrintConfig config = multifilament_config(2, {
            {"layer_height", 0.3}, {"initial_layer_print_height", 0.3}, // mm
            {"wall_loops", 2}, {"sparse_infill_density", 15}, {"enable_support", false}, {"skirt_loops", 0},
            {"brim_type", "no_brim"}, {"slow_down_for_layer_cooling", false}, {"use_relative_e_distances", true},
            {"enable_prime_tower", false},
        });
        config.set_num_extruders(2);
        config.set_key_value("single_extruder_multi_material", new ConfigOptionBool(false));
        config.set_key_value("nozzle_diameter", new ConfigOptionFloats{0.4, 0.4});
        size_flush_to_nozzles(config);
        const DynamicPrintConfig blend = wait_at_config("tower", 10., {{"tower_toolhead", std::to_string(tower_toolhead)}});
        for (const char *key : {"mod_settings", "printer_mod_settings"})
            config.set_key_value(key, blend.option(key)->clone());

        const std::string              out   = slice_mesh(pillar_with_slab(), config);
        const std::vector<std::string> lines = lines_of(out);
        const int tower_tool = tower_toolhead == 0 ? 0 : tower_toolhead - 1; // 0-based tool the blocks must print with
        int    tool = 0; // the tool in (T lines), 0 before the first
        size_t blocks = 0, waits = 0, visits = 0, sites = 0;
        bool   in_block = false;
        for (size_t i = 0; i < lines.size(); ++i) {
            const std::string &line = lines[i];
            if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
                tool = std::stoi(line.substr(1));
            } else if (line == ";MAGPIE_WAIT_TOWER_BEGIN") {
                in_block = true;
                ++blocks;
                CHECK(tool == tower_tool);
            } else if (line == ";MAGPIE_WAIT_TOWER_END") {
                in_block = false;
            } else if (line == ";MAGPIE_WAIT_VISIT") {
                ++visits;
                CHECK(in_block);
            } else if (line == site_tag) {
                ++sites;
                // With the tower's own tool the site tag is at the layer start, outside the block.
                CHECK(in_block == (tower_toolhead == 0));
            } else if (line.rfind("G4 P", 0) == 0) {
                ++waits;
                // Every wait is over the tower: right before the tag the block puts after the travel there.
                CHECK(in_block);
                REQUIRE(i + 1 < lines.size());
                CHECK(lines[i + 1] == (tower_toolhead == 0 ? site_tag : std::string(";MAGPIE_WAIT_VISIT")));
            } else if (!in_block && axis_value(line, 'E') && *axis_value(line, 'E') > 0. && (axis_value(line, 'X') || axis_value(line, 'Y'))) {
                // The model is filament 1's.
                CHECK(tool == 0);
            }
        }
        REQUIRE(blocks > 0);
        CHECK(waits > 0);
        CHECK(sites >= blocks);
        CHECK(visits == (tower_toolhead == 0 ? 0 : blocks));
        // Toolhead 2 is planned into every layer with a block: a change to it and back; toolhead 0 changes nothing.
        CHECK(count_lines_starting(out, "T1") == (tower_toolhead == 0 ? 0 : blocks));
        check_blended(measured_intervals(out), 10.);
    }
}

TEST_CASE("On a later plate the tower and the park position stay where they are on the first", "[LayerTimeBlend]")
{
    // Review 2026-10-22: the tower's spot, the park position and the printable area are in plate coordinates;
    // the objects are on the bed, the plate's origin further, and the writer takes the origin off every move.
    // The tower and the park position went out without it: on plate 2 they came out left of the bed (X < 0).
    // Same model at the same place on its plate: the moves are the same.
    const char *wait_at = GENERATE("tower", "park");
    CAPTURE(wait_at);
    const DynamicPrintConfig config = wait_at_config(wait_at, 2., {{"in_place_max", "2"}}, {{"park_x", "10"}, {"park_y", "190"}});
    auto moves_on_plate = [&config](const Vec3d &plate_origin) {
        BundledMods mods;
        REQUIRE(mods.errors.empty());
        Print print;
        Model model;
        TriangleMesh mesh = pillar_with_slab();
        mesh.translate(float(100. + plate_origin.x()), float(100. + plate_origin.y()), 0.f);
        std::vector<TriangleMesh> meshes;
        meshes.push_back(std::move(mesh));
        init_print(std::move(meshes), print, model, config, nullptr, /*arrange=*/false);
        print.set_plate_origin(plate_origin);
        std::vector<std::string> moves;
        for (const std::string &line : lines_of(gcode(print)))
            if (line.rfind("G1 ", 0) == 0 || line.rfind("G0 ", 0) == 0)
                moves.push_back(line);
        return moves;
    };
    const std::vector<std::string> first = moves_on_plate(Vec3d::Zero()), second = moves_on_plate(Vec3d(324., 0., 0.));
    REQUIRE(!first.empty());
    CHECK(first == second);
    // No move left of the bed.
    for (const std::string &line : second)
        CHECK(line.find(" X-") == std::string::npos);
    if (std::string(wait_at) == "park")
        CHECK(std::any_of(second.begin(), second.end(), [](const std::string &line) { return line.find("X10 Y190") != std::string::npos; }));
}

TEST_CASE("A temperature set for another tool leaves the printing nozzle's model alone", "[LayerTimeBlend][GCodeProcessor]")
{
    // Review 2026-10-22: an M104 with a T word (a preheat or cool-down of another tool, as the slicer writes for
    // a tool change ahead) set the current filament's target, so its model cooled to the other tool's temperature
    // and the next M109 on the printing tool counted a heat-up. The layer change tag as the processor reads it.
    const std::string layer_change = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // Inside the print: cool tool 1 to 150 degC, move 10 s, confirm 200 degC on the printing tool: nothing to wait for.
    const std::string other_tool = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\n" + layer_change + "M104 T1 S150\nG1 X100 F600\nM109 S200\nG1 X0 F600\n";
    const std::string none       = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\n" + layer_change + "G1 X100 F600\nM109 S200\nG1 X0 F600\n";
    CHECK_THAT(estimated_time(other_tool, false), WithinAbs(estimated_time(none, false), 0.5));
    // The same cool-down of the printing tool itself: 10 s of cooling at 1 degC/s, heated back at 2 degC/s (5 s).
    const std::string own_tool = "G90\nM83\nG1 X0 Y0 F600\nM109 S200\n" + layer_change + "M104 S150\nG1 X100 F600\nM109 S200\nG1 X0 F600\n";
    CHECK(estimated_time(own_tool, false) > estimated_time(none, false) + 4.);
}

TEST_CASE("The reheat of a tool after its idle cool-down counts as no time", "[LayerTimeBlend][GCodeProcessor]")
{
    // Gen, 2026-10-22: as in Orca, a tool-change wait counts as none; only a printing tool's own temperature change
    // (the low temperature interface) is a wait. Cool tool 1 while tool 0 prints, then wait for tool 1 at 220 degC.
    const std::string layer_change = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    const std::string idle_reheat = "G90\nM83\nG1 X0 Y0 F600\nM109 S220\n" + layer_change + "M104 T1 S70\nG1 X100 F600\nM104 T1 S220\nM109 T1 S220\nG1 X0 F600\n";
    const std::string none        = "G90\nM83\nG1 X0 Y0 F600\nM109 S220\n" + layer_change + "G1 X100 F600\nG1 X0 F600\n";
    CHECK_THAT(estimated_time(idle_reheat, false), WithinAbs(estimated_time(none, false), 0.5));
}

TEST_CASE("The preview shows each wait on the layer it was planned for", "[LayerTimeBlend][GCodeViewer]")
{
    // Moves of layers 0, 1 and 2 with times 1 2 | 3 4 5 | 6 s. The site of layer 1 at 7 s lies 4 s past the layer's
    // start (3 s): that time moves down to layer 0. The site of layer 2 at 13 s lies 2 s before its layer's start
    // (15 s), as when the estimate booked a dwell asked for after the layer change on an earlier move: 2 s move up.
    GCodeProcessorResult result;
    const std::vector<std::pair<unsigned int, float>> moves = {{0, 1.f}, {0, 2.f}, {1, 3.f}, {1, 4.f}, {1, 5.f}, {2, 6.f}};
    for (const auto &[layer, time] : moves) {
        GCodeProcessorResult::MoveVertex move;
        move.layer_id = layer;
        move.time     = {time, 7.f};
        result.moves.push_back(move);
    }
    const std::vector<float> own = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    // Blending off: no sites, the moves' own times.
    CHECK(layer_time_blend_preview_times(result) == own);

    result.layer_time_blend.active = true;
    result.layer_time_blend.sites  = {{1, 7.}, {2, 13.}};
    const std::vector<float> preview = layer_time_blend_preview_times(result);
    REQUIRE(preview.size() == own.size());
    const std::vector<float> expected = {1.f, 6.f, 0.f, 3.f, 3.f, 8.f};
    for (size_t i = 0; i < own.size(); ++i)
        CHECK_THAT(preview[i], WithinAbs(expected[i], 1e-6));
    // The layers run from site to site: layer 1 from 7 s to 13 s.
    CHECK_THAT(preview[2] + preview[3] + preview[4], WithinAbs(13. - 7., 1e-6));
    // The result itself keeps its times, in both modes.
    CHECK(result.moves[2].time[0] == 3.f);
    CHECK(result.moves[2].time[1] == 7.f);
}

TEST_CASE("The preview's layer times are the planner's when waiting over the tower", "[LayerTimeBlend][GCodeViewer]")
{
    // Over the tower the wait comes after the next layer's layer change, so by the moves' layers it falls on the
    // layer above the one it lengthens.
    const double         max_diff_s = 10.;
    GCodeProcessorResult result;
    {
        BundledMods mods;
        REQUIRE(mods.errors.empty());
        Print        print;
        Model        model;
        TriangleMesh mesh = pillar_with_slab();
        mesh.translate(100.f, 100.f, 0.f);
        std::vector<TriangleMesh> meshes;
        meshes.push_back(std::move(mesh));
        init_print(std::move(meshes), print, model, wait_at_config("tower", max_diff_s), nullptr, /*arrange=*/false);
        gcode(print, &result);
    }
    const GCodeProcessorResult::LayerTimeBlendInfo &blend = result.layer_time_blend;
    REQUIRE(blend.active);
    REQUIRE(blend.sites.size() > 10);
    REQUIRE_FALSE(blend.layers.empty());

    const size_t        normal = static_cast<size_t>(PrintEstimatedStatistics::ETimeMode::Normal);
    const unsigned int  layers = result.moves.back().layer_id + 1;
    std::vector<double> own(layers, 0.), shown(layers, 0.);
    const std::vector<float> preview = layer_time_blend_preview_times(result);
    double own_total = 0., shown_total = 0.;
    for (size_t i = 0; i < result.moves.size(); ++i) {
        own[result.moves[i].layer_id] += result.moves[i].time[normal];
        shown[result.moves[i].layer_id] += preview[i];
        own_total += result.moves[i].time[normal];
        shown_total += preview[i];
    }
    CHECK_THAT(shown_total, WithinAbs(own_total, 0.01));
    // Each layer between two sites shows the time from its site to the next (the first layer holds the start G-code
    // too); by the moves' own layers the waits land a layer up, so some layer's own time is off by more than a second.
    size_t off = 0;
    for (size_t k = 2; k < blend.sites.size(); ++k) {
        const unsigned int layer = blend.sites[k - 1].layer_id;
        REQUIRE(blend.sites[k].layer_id == layer + 1);
        const double interval = blend.sites[k].time_s - blend.sites[k - 1].time_s;
        CHECK_THAT(shown[layer], WithinAbs(interval, 0.01));
        off += std::abs(own[layer] - interval) > 1.;
    }
    CHECK(off > 0);
    // The slider's hover tooltip: a waiting layer's time before its wait plus the wait is the time it shows.
    for (const GCodeProcessorResult::LayerTimeBlendLayer &layer : blend.layers) {
        REQUIRE(layer.layer_id < layers);
        CHECK_THAT(shown[layer.layer_id], WithinAbs(double(layer.original_s + layer.added_s), 0.05));
    }
}

TEST_CASE("A site right after a tool change counts the tool change's time", "[LayerTimeBlend][GCodeProcessor]")
{
    // A tool change's time is booked on a block of its own that takes the line of the G1 before it, and the G1
    // time cache leaves such blocks out: a site after the change and before the next G1 read the time from before
    // the change while the booking was taken out, 5 s early here (random slice 5752, a wait over the wait cap).
    FullPrintConfig config;
    // Klipper books extra time on the oldest queued block as soon as one is queued, so the swap's time lands on
    // its own block, which takes the G1 line before it (the Snapmaker U1 of that slice).
    config.gcode_flavor.value                 = gcfKlipper;
    config.filament_diameter.values           = {1.75, 1.75};
    config.filament_map.values                = {1, 1}; // one extruder: T1 is a filament swap, unload + load
    config.machine_load_filament_time.value   = 3.;     // s
    config.machine_unload_filament_time.value = 2.;     // s
    const std::string layer_change = ";" + GCodeProcessor::reserved_tag(GCodeProcessor::ETags::Layer_Change) + "\n";
    // 100 mm at 10 mm/s on either side of the swap: 10 s each, plus the acceleration at its ends. Moves follow the
    // last site, as in any print: the estimator leaves a lone last block unprocessed.
    auto sites = [&](const std::string &between) {
        GCodeProcessor processor;
        processor.reset();
        processor.initialize_result_moves();
        processor.apply_config(config);
        processor.initialize("estimate.gcode");
        processor.process_buffer("G90\nM83\nT0\nG1 X0 Y0 F600\n" + layer_change + "G1 X100 F600\nT1\n" + between +
                                 ";MAGPIE_WAIT_SITE\nG1 X0 F600\n;MAGPIE_WAIT_SITE\nG1 Y10 F600\nG1 Y0 F600\n");
        processor.finalize(false);
        std::vector<double> out;
        for (const GCodeProcessor::WaitSiteTime &mark : processor.wait_site_times())
            out.push_back(mark.time_s);
        return out;
    };
    const std::vector<double> direct = sites("");
    // The same with a G1 between the swap and the site, which the cache does hold: the reference.
    const std::vector<double> after_g1 = sites("G1 F600\nG1 X100.001 F600\n");
    REQUIRE(direct.size() == 2);
    REQUIRE(after_g1.size() == 2);
    // The swap's 5 s lie before the first site; the second site is one 10 s move after it.
    CHECK(direct[0] > 15. - 0.5);
    CHECK_THAT(direct[0], WithinAbs(after_g1[0], 0.05));
    CHECK_THAT(direct[1] - direct[0], WithinAbs(after_g1[1] - after_g1[0], 0.05));
    CHECK(direct[1] - direct[0] < 10.5);
}
