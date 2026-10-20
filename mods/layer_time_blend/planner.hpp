// The layer time blending planner: from the estimated time between wait sites, the waits that keep
// neighbouring layer times within a limit. Plain C++ over numbers; layer_time_blend.cpp connects it to
// the core's "gcode.time_plan" slot (magpie_slot_time_plan.h), and the mod's own tests call it directly.
#pragma once

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace LayerTimeBlend {

// One wait site (mp_wait_site, as numbers the planner works with).
struct WaitSite
{
    size_t layer_id = 0;      // G-code layer printed just before the site (the layer whose time interval_s is)
    // Estimated time from the previous site to this one, s. The first site's is the time before it and is
    // not a layer time.
    double interval_s = 0.;
    double park_trip_s = 0.;  // time a park trip from this site takes, s; 0 = the site cannot park
    // Starts a new chain, like site 0: the next object of a by-object print. Its interval is not a layer
    // time, and no constraint links the layers on either side of it.
    bool   constraint_break = false;
    bool   uncertain = false;        // the interval holds time the estimator only guesses (heating waits, tool changes)
    // A layer time tower block around the site: the time from the block's start to the site, and the whole
    // block's time, s (0 when there is none). Dropping the blocks from some site on shortens the layers there.
    double tower_pre_s   = 0.;
    double tower_block_s = 0.;
};

struct WaitWarning
{
    enum class Kind { TotalShare, SingleWait, Uncertain };
    Kind   kind;
    double value = 0.;   // TotalShare: added share of the print time, %; SingleWait: longest wait, s; Uncertain: site count
    // Sites (indices into the plan's input): the layer whose length spreads the waits, and the first and
    // last site of the waits it causes. Uncertain: first and last uncertain site, source = first.
    size_t source_site = 0, first_site = 0, last_site = 0;
};

struct WaitPlan
{
    std::vector<double>      dwell_s;  // G4 time per site, s (the wait less any park trip)
    std::vector<bool>        use_park; // per site: park the head for this wait
    std::vector<double>      added_s;  // time added per site, s (dwell plus park trip)
    std::vector<double>      target_s; // planned layer time per site, s (interval_s + added_s)
    std::vector<WaitWarning> warnings;
    // The first site whose tower block is left out (no wait at or after it needs the tower); the number of
    // sites keeps every block.
    size_t                   tower_end = 0;
};

// The blending settings, as the mod's parameters carry them.
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

// Where the planner reports a failure path it took (a PERMANENT DIAGNOSTIC); the mod sends it to the
// application log. Nothing by default.
void set_diagnostic_sink(std::function<void(const std::string &)> sink);

// The target layer times: the pointwise smallest T >= t with each neighbour pair within the limit
// (difference D, or ratio in log space) and, with ease_layers > 0, every convex bend's second
// difference within D / (ease_layers + 1). `links[i]` false cuts the constraints between i-1 and i
// (links[0] is ignored).
std::vector<double> target_times(const std::vector<double> &t, const std::vector<bool> &links, const Settings &settings);

// The plan for the sites: target_times over the sites from start_layer on, waits = T - t (capped),
// park choice and warnings. `print_time_s` is the whole print's estimated time, for the share warning.
WaitPlan plan(const std::vector<WaitSite> &sites, const Settings &settings, double print_time_s);

} // namespace LayerTimeBlend
