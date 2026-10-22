#pragma once

// MAGPIE: the support type dropdown with the support generators of the loaded mods. Orca's support_type values
// come first; every support slot declaration with a "menu" (ModSlotDecl::menu) adds its entries after them, in the
// mods' load order, so a support mod installed later appears without a new build of the application. Picking a
// mod's entry writes support_type (the Orca value the entry stands on) and switches the mod's menu parameter on and
// every other support mod's off: one generator, as the dropdown shows. The slot choice that slices
// (support_generator_name) is unchanged; a config that turns several on (a file, the CLI) still picks by priority
// and is warned about in Print::validate.

#include "ModSet.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;

namespace ModHost {

struct SupportMenuItem
{
    // Empty for an Orca support type; else the mod whose implementation the entry picks.
    std::string mod_id;
    // "<mod id>.<param>": the mod's bool parameter the entry switches on (empty for Orca's).
    std::string param_key;
    // The support_type value written, as its config key ("normal(auto)", "tree(manual)", ...).
    std::string support_type;
    // English label: Orca's own (translated by the application) or the mod's.
    std::string label;
    // The mod's translations ("label_<language>"); empty for Orca's.
    std::map<std::string, std::string> labels;
};

// Orca's support types, then every menu entry of the support slot declarations of `mods` (may be null).
std::vector<SupportMenuItem> support_menu(const ModSet *mods);

// The item `config` (a print config holding support_type and mod_settings) selects: the entry of the mod its
// support slot choice picks, the one standing on the config's support_type, else that mod's first; with no mod
// picked, Orca's item of the config's support_type. Index into `items`.
size_t support_menu_selection(const ModSet *mods, const std::vector<SupportMenuItem> &items, const DynamicPrintConfig &config);

// Writes item `index` into `config`: support_type, and in mod_settings the item's parameter on and every other
// item's parameter off (removed where its default is off). Returns the config keys whose values changed.
std::vector<std::string> apply_support_menu_item(const ModSet *mods, const std::vector<SupportMenuItem> &items, size_t index,
                                                 DynamicPrintConfig &config);

// The choices of a string parameter with "choices_from" (ModParam::choices_from), as values: "orca", then the
// implementation name each loaded mod registers for that slot, in load order, each once.
std::vector<std::string> support_choices(const ModSet *mods, const std::string &slot);
// The mod registering implementation `impl` for `slot`, or nullptr ("orca").
const Mod *slot_provider(const ModSet *mods, const std::string &slot, const std::string &impl);

// Whether `name` is the parameter a support menu of `mod` switches (the dropdown sets it, so the Mods page does not
// show it).
bool is_support_menu_param(const Mod &mod, const std::string &name);

} // namespace ModHost
} // namespace Slic3r
