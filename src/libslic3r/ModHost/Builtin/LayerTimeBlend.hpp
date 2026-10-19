#pragma once

#include "../SlotRegistry.hpp"

#include <vector>

namespace Slic3r {
namespace ModHost {
namespace LayerTimeBlend {

// The blending settings, as the hidden layer_time_blend_* settings carry them.
struct Settings
{
    bool   log           = false; // limit the ratio of neighbouring layer times instead of their difference
    double max_diff_s    = 5.;    // linear: largest difference between neighbouring layer times, s
    double max_ratio_pct = 20.;   // log: largest increase from the shorter to the longer of two neighbours, %
    // Layers over which the ramp's slope changes at its start and end: the slope changes by at most
    // limit / (ease_layers + 1) per layer (curvature limit, convex bends only). 0 keeps sharp bends.
    int    ease_layers   = 0;
    // First layer (1-based, counted per chain, so per object in a by-object print) whose time is
    // blended; the constraint to the layer below it is cut, and layers below it get no wait.
    int    start_layer   = 2;
    double wait_cap_s    = 0.;    // largest single wait, s; 0 = no cap
    // Whether waits may park: a wait at least park_min_s long (s) and at least the site's park trip parks
    // the head instead of waiting in place. Only sites with a park trip can park.
    bool   park          = false;
    double park_min_s    = 0.;
    double warn_total_pct = 15.;  // warn when the waits add more than this share of the print time, %
    double warn_single_s  = 60.;  // warn when one wait is longer than this, s
};

// The target layer times: the pointwise smallest T >= t with each neighbour pair within the limit
// (difference D, or ratio in log space) and, with ease_layers > 0, every convex bend's second
// difference within D / (ease_layers + 1). `links[i]` false cuts the constraints between i-1 and i
// (links[0] is ignored). Exposed for the tests.
std::vector<double> target_times(const std::vector<double> &t, const std::vector<bool> &links, const Settings &settings);

// The plan for the sites: target_times over the sites from start_layer on, waits = T - t (capped),
// park choice and warnings. `print_time_s` is the whole print's estimated time, for the share warning.
WaitPlan plan(const std::vector<WaitSite> &sites, const Settings &settings, double print_time_s);

// Reads the settings from the hidden layer_time_blend_* settings.
Settings settings_from(const PrintConfig &config);

TimePlanner make_planner();

} // namespace LayerTimeBlend
} // namespace ModHost
} // namespace Slic3r
