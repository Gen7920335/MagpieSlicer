#pragma once

// MAGPIE: an Orca dropdown option with the choices of mod parameters merged into it (ModParamUi::merge_into). The
// settings tab (Tab::create_merged_setting) and the calibration form (ModCalibrationDialog) show the same one list
// from these functions, so a choice a mod adds is offered in both or in neither.

#include <map>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {

class ConfigOptionDef;
class DynamicPrintConfig;

namespace ModHost {

class ModSet;

// One entry of the list: Orca's value (param empty) or a parameter's value.
struct MergedChoice
{
    std::string                        param; // "<mod id>.<name>", or "" for Orca's own value
    std::string                        value;
    std::string                        label;  // English: Orca's enum label (gettext translates it) or the mod's label
    std::map<std::string, std::string> labels; // a mod option's translations ("label_<language>")
};

struct MergedChoices
{
    std::vector<std::string>  params;  // the parameters merged into the option, in load order
    std::vector<MergedChoice> entries; // Orca's choices, then each parameter's own
    // A parameter's option that is one of Orca's (same value and name): (parameter key, value) -> Orca's entry.
    std::map<std::pair<std::string, std::string>, int> aliases;
};

// The string and number parameters of `mods` merged into the Orca option `option`, in load order.
std::vector<std::string> merged_params(const ModSet &mods, const std::string &option);
// The list of an Orca dropdown (`def`, a coEnum) and the string parameters `params` merged into it.
MergedChoices merged_choices(const ModSet &mods, const ConfigOptionDef &def, const std::vector<std::string> &params);
// The entry a print config shows: a merged parameter holding a value of its own in `stored` (the config's
// mod_settings JSON), else Orca's value of `option` in `print`; -1 for none.
int merged_selection(const MergedChoices &choices, const std::string &stored, const DynamicPrintConfig &print, const std::string &option);
// What choosing `entry` writes: Orca's option (`option_value`, empty for none) and each merged parameter's value
// (a JSON value, or "" for its default, which keeps Orca's).
struct MergedWrite
{
    std::string                                      option_value;
    std::vector<std::pair<std::string, std::string>> values;
};
MergedWrite merged_write(const MergedChoices &choices, size_t entry);

} // namespace ModHost
} // namespace Slic3r
