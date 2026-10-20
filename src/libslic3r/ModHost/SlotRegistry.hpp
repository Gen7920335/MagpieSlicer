#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"

namespace Slic3r {

class ExPolygon;
class PerimeterGenerator;
class PrintConfig;
class PrintObject;
class PrintRegionConfig;
struct WallPlan;

namespace ModHost {

// support.generator: replaces Orca's support generation for an object. It fills the object's
// support layers the way PrintObject::generate_orca_support_material() does, and is chosen per
// object by the support_generator setting. An empty setting keeps Orca's own generator.
struct SupportGenerator
{
    std::string                        name;  // the support_generator value that selects it
    std::string                        label; // shown in the settings
    std::function<void(PrintObject &)> generate;
};

// The generator registered under `name`, or nullptr.
const SupportGenerator *find_support_generator(const std::string &name);
// Every registered generator, in name order.
std::vector<const SupportGenerator *> support_generators();
// Registers a generator; a later registration under the same name replaces the earlier one.
void register_support_generator(SupportGenerator generator);

// wall.plan: splits a region's walls between its own nozzle and a second, detail nozzle. Chosen per
// region by the wall_planner setting; an empty setting keeps Orca's walls.
struct WallPlanner
{
    std::string name;
    std::string label;
    // The detail nozzle's wall flow on this generator's layer, or nothing when the layer has no detail
    // nozzle (then plan is not asked and the walls are Orca's).
    std::function<std::optional<Flow>(const PerimeterGenerator &)> detail_flow;
    // The plan for one island, given the wall count Orca would print (base_loop_number + 1).
    std::function<WallPlan(const PerimeterGenerator &, const ExPolygon &island, int base_loop_number)> plan;
    // The filament (1-based) that prints walls tagged `hint` on layer layer_id of a region whose walls
    // would otherwise use base_filament_1based; 0 keeps Orca's choice.
    std::function<unsigned int(const PrintConfig &, const PrintRegionConfig &, size_t layer_id, ExtrusionToolHint hint,
                               unsigned int base_filament_1based)> wall_filament;
    // The physical nozzle (1-based toolhead) that prints every wall of layer layer_id when the planner moves the
    // whole layer's walls to another nozzle, so their widths are resolved on it; 0 keeps the region's own nozzle.
    std::function<unsigned int(const PrintConfig &, const PrintRegionConfig &, size_t layer_id)> layer_wall_toolhead;
};

const WallPlanner *find_wall_planner(const std::string &name);
void register_wall_planner(WallPlanner planner);

// gcode.time_plan: decides how long to wait at each wait site of a print's G-code. A wait site sits
// at the start of a full layer, before its first extrusion; the core marks the sites while exporting,
// estimates their times, asks the planner and fills the waits in. Chosen by the layer_time_planner
// setting; an empty setting marks no sites. Input and output are numbers only, so a planner never
// touches G-code or geometry.
struct WaitSite
{
    size_t layer_id = 0;      // G-code layer printed just before the site (the layer whose time interval_s is)
    // Estimated time from the previous site to this one, s, including any wait already at this site.
    // The first site's is the time before it and is not a layer time.
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

struct TimePlanner
{
    std::string name;
    std::string label;
    // The plan for the sites of one print; `print_time_s` is the print's estimated time before waits.
    std::function<WaitPlan(const std::vector<WaitSite> &sites, const PrintConfig &config, double print_time_s)> plan;
};

const TimePlanner *find_time_planner(const std::string &name);
void register_time_planner(TimePlanner planner);

} // namespace ModHost
} // namespace Slic3r
