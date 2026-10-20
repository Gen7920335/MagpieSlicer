#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"

namespace Slic3r {

class DynamicPrintConfig;
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

// gcode.time_plan: decides how long to wait at each wait site of a print's G-code. A wait site sits
// at the start of a full layer, before its first extrusion; the core marks the sites while exporting,
// estimates their times, asks the planner and fills the waits in. Chosen by the layer_time_planner
// setting; an empty setting marks no sites. Input and output are numbers only, so a planner never
// touches G-code or geometry. Planners are native mods filling the C slot of
// mods/sdk/include/magpie_slot_time_plan.h; these structures are the core's view of its data.
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
    // Slicing warnings the planner's mod raised through the host (host->warn), in call order.
    std::vector<std::string> mod_warnings;
};

struct TimePlanner
{
    std::string name;                 // the layer_time_planner value that selects it
    std::string mod_id, mod_version;  // the native mod that fills the slot
    // The plan for the sites of one print. `config` is the print's full configuration, which holds the mod's
    // parameters; `cancelled` says the user stopped the slice; `print_time_s` is the print's estimated time
    // before waits. A failing planner (an error status, a crash, a host->warn error, a plan that is not finite
    // or not for these sites) throws Slic3r::SlicingError naming the mod; nothing falls back to no waits.
    std::function<WaitPlan(const std::vector<WaitSite> &sites, const DynamicPrintConfig &config,
                           const std::function<bool()> &cancelled, double print_time_s)> plan;
};

// The planner of the active native mods registered under `name` (gcode.time_plan v1), or nullptr. The
// planner keeps the native mod set it came from alive.
std::shared_ptr<const TimePlanner> find_time_planner(const std::string &name);

} // namespace ModHost
} // namespace Slic3r
