#include "SupportMenu.hpp"

#include "SlotChoice.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <set>

namespace Slic3r {
namespace ModHost {

namespace {

std::string support_type_key(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionEnum<SupportType>>("support_type");
    const SupportType type = opt != nullptr ? opt->value : stNormalAuto;
    for (const auto &[key, value] : ConfigOptionEnum<SupportType>::get_enum_values())
        if (value == int(type))
            return key;
    return "normal(auto)";
}

nlohmann::json stored_values(const DynamicPrintConfig &config)
{
    const auto *opt = config.option<ConfigOptionString>("mod_settings");
    if (opt == nullptr || opt->value.empty())
        return nlohmann::json::object();
    try {
        nlohmann::json j = nlohmann::json::parse(opt->value);
        return j.is_object() ? j : nlohmann::json::object();
    } catch (const nlohmann::json::exception &) {
        return nlohmann::json::object();
    }
}

} // namespace

std::vector<SupportMenuItem> support_menu(const ModSet *mods)
{
    std::vector<SupportMenuItem> items;
    if (const ConfigOptionDef *def = print_config_def.get("support_type"); def != nullptr)
        for (size_t i = 0; i < def->enum_values.size(); ++i)
            items.push_back({ "", "", def->enum_values[i], i < def->enum_labels.size() ? def->enum_labels[i] : def->enum_values[i], {} });
    if (mods == nullptr)
        return items;
    const std::vector<std::string> &slots = support_slots();
    for (const Mod &mod : mods->mods())
        for (const ModSlotDecl &decl : mod.slots) {
            if (decl.menu.empty() || std::find(slots.begin(), slots.end(), decl.slot) == slots.end())
                continue;
            for (const ModSupportMenuEntry &entry : decl.menu)
                items.push_back({ mod.id, mod.id + "." + decl.menu_param, entry.support_type, entry.label, entry.labels });
        }
    return items;
}

size_t support_menu_selection(const ModSet *mods, const std::vector<SupportMenuItem> &items, const DynamicPrintConfig &config)
{
    const std::string type = support_type_key(config);
    std::string picked;
    if (mods != nullptr) {
        ModSet::ValueSources sources;
        sources.print = &config;
        const ModSet::SlotChoice choice = mods->choose_slot(support_slots(), sources);
        if (choice.mod != nullptr)
            picked = choice.mod->id;
    }
    size_t first_of_picked = items.size(), orca = items.size();
    for (size_t i = 0; i < items.size(); ++i) {
        if (items[i].mod_id != picked)
            continue;
        if (items[i].support_type == type)
            return i;
        if (first_of_picked == items.size())
            first_of_picked = i;
    }
    if (first_of_picked < items.size())
        return first_of_picked;
    // A mod picked that adds no entry, or a type no item has: Orca's first item of the type, else the first.
    for (size_t i = 0; i < items.size() && orca == items.size(); ++i)
        if (items[i].mod_id.empty() && items[i].support_type == type)
            orca = i;
    return orca < items.size() ? orca : 0;
}

std::vector<std::string> apply_support_menu_item(const ModSet *mods, const std::vector<SupportMenuItem> &items, size_t index,
                                                 DynamicPrintConfig &config)
{
    std::vector<std::string> changed;
    if (index >= items.size())
        return changed;
    const SupportMenuItem &item = items[index];

    const auto  &values = ConfigOptionEnum<SupportType>::get_enum_values();
    if (const auto it = values.find(item.support_type); it != values.end() && support_type_key(config) != item.support_type) {
        config.set_key_value("support_type", new ConfigOptionEnum<SupportType>(SupportType(it->second)));
        changed.push_back("support_type");
    }

    nlohmann::json        stored = stored_values(config);
    const nlohmann::json  before = stored;
    std::set<std::string> keys;
    for (const SupportMenuItem &i : items)
        if (!i.param_key.empty())
            keys.insert(i.param_key);
    for (const std::string &key : keys) {
        if (key == item.param_key) {
            stored[key] = true;
            continue;
        }
        // Off: removed where off is the default, so the file carries no value it does not need.
        const ModParam *param       = mods != nullptr ? mods->find_param(key) : nullptr;
        const bool      default_on  = param != nullptr && std::holds_alternative<bool>(param->default_value) && std::get<bool>(param->default_value);
        if (default_on)
            stored[key] = false;
        else
            stored.erase(key);
    }
    if (stored != before) {
        config.set_key_value("mod_settings", new ConfigOptionString(stored.empty() ? std::string() : stored.dump()));
        changed.push_back("mod_settings");
    }
    return changed;
}

std::vector<std::string> support_choices(const ModSet *mods, const std::string &slot)
{
    std::vector<std::string> out { "orca" };
    if (mods == nullptr)
        return out;
    for (const Mod &mod : mods->mods())
        for (const ModSlotDecl &decl : mod.slots)
            if (decl.slot == slot && std::find(out.begin(), out.end(), decl.impl) == out.end())
                out.push_back(decl.impl);
    return out;
}

const Mod *slot_provider(const ModSet *mods, const std::string &slot, const std::string &impl)
{
    if (mods == nullptr)
        return nullptr;
    for (const Mod &mod : mods->mods())
        for (const ModSlotDecl &decl : mod.slots)
            if (decl.slot == slot && decl.impl == impl)
                return &mod;
    return nullptr;
}

bool is_support_menu_param(const Mod &mod, const std::string &name)
{
    return std::any_of(mod.slots.begin(), mod.slots.end(), [&name](const ModSlotDecl &decl) { return !decl.menu.empty() && decl.menu_param == name; });
}

} // namespace ModHost
} // namespace Slic3r
