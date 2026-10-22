#pragma once

// MAGPIE: the core's side of the wall.plan slot (mods/sdk/include/magpie_slot_wall_plan.h). A wall planner splits a
// region's walls between nozzles of different sizes; native mods provide planners, picked per region by the
// wall_planner setting (an empty setting keeps Orca's walls). The core keeps what any planner's walls need: the
// detail flow on the planner's nozzle (resolved by the core's line width rules), the Classic and Arachne generators
// that print a WallPlan, the tags that route each wall to its nozzle, and the line width checks.

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"

#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ExPolygon;
class PerimeterGenerator;
class PrintConfig;
class PrintObjectConfig;
class PrintRegionConfig;
struct WallPlan;

namespace ModHost {

class NativeModSet;
struct NativeEntry;

// A filament and the toolhead that prints it, as a planner chose them.
struct WallTool
{
    unsigned int filament_1based { 0 };
    unsigned int toolhead_1based { 0 };
    double       nozzle_diameter { 0. }; // mm

    explicit operator bool() const { return filament_1based > 0 && toolhead_1based > 0 && nozzle_diameter > 0.; }
};

// A filament a planner may print a region's walls with, for the line width checks.
struct WallToolUse
{
    unsigned int filament_1based { 0 };
    unsigned int outer_walls { 0 }; // outermost walls it prints, 0 for none
    bool         inner_walls { false };
};

// One planner opened for one region of one print. Its functions may be called from several threads; a call
// that fails in the mod throws SlicingError naming the mod.
class WallPlanner
{
public:
    ~WallPlanner();
    WallPlanner(const WallPlanner &) = delete;
    WallPlanner &operator=(const WallPlanner &) = delete;

    const std::string &name() const { return m_name; }
    const std::string &mod_id() const;

    // The planner's detail nozzle on layer layer_id, or nothing.
    WallTool detail_tool(size_t layer_id) const;
    // The detail nozzle's wall flow on the generator's layer, or nothing when the layer has no detail nozzle (then
    // plan() is not asked and the walls are Orca's).
    std::optional<Flow> detail_flow(const PerimeterGenerator &g) const;
    // The plan for one island of a layer with a detail flow, given the wall count Orca would print
    // (base_loop_number + 1).
    WallPlan plan(const PerimeterGenerator &g, const ExPolygon &island, int base_loop_number) const;
    // The physical nozzle (1-based toolhead) printing every wall of layer layer_id, so their widths are resolved on
    // it; 0 keeps the region's own nozzle.
    unsigned int layer_wall_toolhead(size_t layer_id) const;
    // The filament (1-based) printing walls tagged `hint` on layer layer_id where Orca would use
    // base_filament_1based; 0 keeps Orca's choice.
    unsigned int wall_filament(size_t layer_id, ExtrusionToolHint hint, unsigned int base_filament_1based) const;
    // The filaments it may print the region's walls with, and whether the region's own inner wall filament prints
    // inner walls even where the region asks for few walls.
    std::vector<WallToolUse> tool_uses(bool &base_inner_walls) const;
    // What the mod warned about when it opened for the region, translated (Print::validate shows them).
    const std::vector<std::string> &warnings() const { return m_warnings; }

private:
    friend std::unique_ptr<const WallPlanner> open_wall_planner(const DynamicPrintConfig &, const PrintConfig &,
                                                                const PrintObjectConfig *, const PrintRegionConfig &, bool);
    WallPlanner() = default;

    std::string                          m_name;
    std::shared_ptr<const NativeModSet>  m_mods; // keeps the library and its table loaded
    const NativeEntry                   *m_entry { nullptr };
    void                                *m_state { nullptr };
    std::vector<std::string>             m_warnings;
};

// The id of the loaded mod whose planner the region uses, or "" for none (wall_planner_name, SlotChoice.hpp).
std::string wall_planner_mod_id(const DynamicPrintConfig &full_config, const PrintObjectConfig *object_config,
                                const PrintRegionConfig &region_config);

// Opens the planner the region uses (wall_planner_name: the active mods' wall.plan declaration whose "when" holds for
// the region). full_config is the print's full config (it holds the mod parameters); print_config the print's own,
// object_config the region's object (or null); is_bbl_printer as Print::is_BBL_printer() (the planner is told the
// nozzle each filament prints on, printing_nozzle_index). Nullptr when the region uses none; throws SlicingError when
// the choice fails, no loaded mod provides the planner or the mod refuses the region.
std::unique_ptr<const WallPlanner> open_wall_planner(const DynamicPrintConfig &full_config, const PrintConfig &print_config,
                                                     const PrintObjectConfig *object_config, const PrintRegionConfig &region_config,
                                                     bool is_bbl_printer);

} // namespace ModHost
} // namespace Slic3r
