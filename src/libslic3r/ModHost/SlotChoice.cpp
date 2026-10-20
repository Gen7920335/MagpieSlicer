#include "SlotChoice.hpp"

#include "libslic3r/Exception.hpp"
#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {
namespace ModHost {

const std::vector<std::string> &support_slots()
{
    static const std::vector<std::string> slots = { "support.areas", "support.compose", "support.contacts" };
    return slots;
}

const std::vector<std::string> &wall_plan_slots()
{
    static const std::vector<std::string> slots = { "wall.plan" };
    return slots;
}

const std::vector<std::string> &time_plan_slots()
{
    static const std::vector<std::string> slots = { "gcode.time_plan" };
    return slots;
}

ModSet::ValueSources ActiveSlotChoice::sources() const
{
    ModSet::ValueSources s;
    s.print         = print;
    s.object_values = object_values ? &*object_values : nullptr;
    s.region_values = region_values ? &*region_values : nullptr;
    return s;
}

std::optional<ExprValue> ActiveSlotChoice::setting(const std::string &name, std::string *error) const
{
    if (!mods || choice.decl == nullptr)
        return std::nullopt;
    return mods->slot_setting(choice, name, sources(), error);
}

static std::optional<std::string> string_value(const ConfigBase *config, const char *key)
{
    if (config == nullptr)
        return std::nullopt;
    const auto *opt = dynamic_cast<const ConfigOptionString *>(config->option(key));
    return opt ? std::optional<std::string>(opt->value) : std::nullopt;
}

ActiveSlotChoice choose_active_slot(const std::vector<std::string> &slots, const DynamicPrintConfig &full_config,
                                    const ConfigBase *object_config, const ConfigBase *region_config)
{
    ActiveSlotChoice out;
    out.mods = active_mods();
    if (!out.mods)
        return out;
    // Nothing to read when no mod declares one of the slots, which keeps a print without such mods free of the
    // parameter lookups.
    bool declared = false;
    for (const Mod &mod : out.mods->mods())
        for (const ModSlotDecl &decl : mod.slots)
            declared = declared || (decl.when && std::find(slots.begin(), slots.end(), decl.slot) != slots.end());
    if (!declared)
        return out;
    out.print         = &full_config;
    out.object_values = string_value(object_config, "object_mod_settings");
    out.region_values = string_value(region_config, "region_mod_settings");
    out.choice        = out.mods->choose_slot(slots, out.sources());
    return out;
}

static std::string picked_name(const ActiveSlotChoice &c)
{
    if (!c.choice.error.empty())
        throw Slic3r::SlicingError(c.choice.error);
    return c.impl();
}

std::string support_generator_name(const DynamicPrintConfig &full_config, const ConfigBase &object_config)
{
    return picked_name(choose_active_slot(support_slots(), full_config, &object_config, nullptr));
}

std::string wall_planner_name(const DynamicPrintConfig &full_config, const ConfigBase *object_config, const ConfigBase &region_config)
{
    return picked_name(choose_active_slot(wall_plan_slots(), full_config, object_config, &region_config));
}

std::string time_planner_name(const DynamicPrintConfig &full_config)
{
    return picked_name(choose_active_slot(time_plan_slots(), full_config, nullptr, nullptr));
}

} // namespace ModHost
} // namespace Slic3r
