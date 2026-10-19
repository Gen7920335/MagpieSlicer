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

std::string slice_mesh(TriangleMesh mesh, const DynamicPrintConfig &config)
{
    Print print;
    Model model;
    init_print({std::move(mesh)}, print, model, config);
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
    std::vector<double> out;
    for (size_t k = 1; k < times.size(); ++k)
        out.push_back(times[k].time_s - times[k - 1].time_s);
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
