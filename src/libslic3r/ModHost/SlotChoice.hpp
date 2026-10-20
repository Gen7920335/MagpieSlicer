#pragma once

#include "ModSet.hpp"

#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

class ConfigBase;
class DynamicPrintConfig;

namespace ModHost {

// The jobs of which the core runs one implementation, picked from the active mods' slot declarations by their
// "when" and "priority" (ModSet::choose_slot). Each is read where the job is done: support per object, walls per
// region, the layer time plan per print, so an object's or region's own parameter values pick for it.

// Orca's support generation is replaced by one implementation of these slots.
const std::vector<std::string> &support_slots();
// "wall.plan".
const std::vector<std::string> &wall_plan_slots();
// "gcode.time_plan".
const std::vector<std::string> &time_plan_slots();

// The choice among `slots` with the active mods, reading parameter values from the print's full config, the
// object's object_mod_settings and the region's region_mod_settings (either config may be null). With no active
// mods, nothing is picked.
struct ActiveSlotChoice
{
    std::shared_ptr<const ModSet> mods; // keeps the declarations alive
    ModSet::SlotChoice            choice;
    // Where the choice read its values (copied, so the choice outlives the configs' option objects but not the
    // full config).
    const DynamicPrintConfig     *print = nullptr;
    std::optional<std::string>    object_values, region_values;

    ModSet::ValueSources sources() const;

    // The implementation name, "" when none is picked.
    std::string impl() const { return choice.decl != nullptr ? choice.decl->impl : std::string(); }
    std::string mod_id() const { return choice.mod != nullptr ? choice.mod->id : std::string(); }
    // The picked implementation's setting `name`, or nothing (also when none is picked).
    std::optional<ExprValue> setting(const std::string &name, std::string *error = nullptr) const;
};
ActiveSlotChoice choose_active_slot(const std::vector<std::string> &slots, const DynamicPrintConfig &full_config,
                                    const ConfigBase *object_config, const ConfigBase *region_config);

// The implementation name picked for one job, "" for none (Orca's own). Throws Slic3r::SlicingError when the choice
// cannot be made (a condition fails, two mods at one priority).
std::string support_generator_name(const DynamicPrintConfig &full_config, const ConfigBase &object_config);
std::string wall_planner_name(const DynamicPrintConfig &full_config, const ConfigBase *object_config, const ConfigBase &region_config);
std::string time_planner_name(const DynamicPrintConfig &full_config);

} // namespace ModHost
} // namespace Slic3r
