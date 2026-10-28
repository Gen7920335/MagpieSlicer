#include "MergedSettings.hpp"

#include "ModSet.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>

namespace Slic3r {
namespace ModHost {

std::vector<std::string> merged_params(const ModSet &mods, const std::string &option)
{
    std::vector<std::string> out;
    for (const Mod &mod : mods.mods())
        for (const ModParam &p : mod.params)
            if (p.ui.merge_into == option)
                out.push_back(mod.id + "." + p.name);
    return out;
}

MergedChoices merged_choices(const ModSet &mods, const ConfigOptionDef &def, const std::vector<std::string> &params)
{
    MergedChoices out;
    out.params = params;
    for (size_t i = 0; i < def.enum_values.size(); ++i)
        out.entries.push_back({ std::string(), def.enum_values[i], i < def.enum_labels.size() ? def.enum_labels[i] : def.enum_values[i], {} });
    for (const std::string &k : params)
        if (const ModParam *p = mods.find_param(k); p != nullptr && p->type == "string")
            for (const ModParamOption &o : p->options) {
                if (o.value.empty())
                    continue;
                // An option with Orca's value and name is Orca's choice (toggle test, support_extras' concentric
                // interface: the same G-code as Orca's on six cases): listed once, as Orca's.
                const auto orca = std::find(def.enum_values.begin(), def.enum_values.end(), o.value);
                if (orca != def.enum_values.end()) {
                    const size_t i = size_t(orca - def.enum_values.begin());
                    if (i < def.enum_labels.size() && def.enum_labels[i] == o.label) {
                        out.aliases[{ k, o.value }] = int(i);
                        continue;
                    }
                }
                out.entries.push_back({ k, o.value, o.label.empty() ? o.value : o.label, o.labels });
            }
    return out;
}

int merged_selection(const MergedChoices &choices, const std::string &stored_text, const DynamicPrintConfig &print, const std::string &option)
{
    const nlohmann::json stored    = nlohmann::json::parse(stored_text.empty() ? std::string("{}") : stored_text, nullptr, false);
    int                  selection = -1;
    // A merged parameter holding a value of its own shows it; else Orca's value.
    for (size_t i = 0; i < choices.entries.size() && selection == -1; ++i) {
        const MergedChoice &e = choices.entries[i];
        if (!e.param.empty() && stored.is_object())
            if (auto it = stored.find(e.param); it != stored.end() && it->is_string() && it->get<std::string>() == e.value)
                selection = int(i);
    }
    for (const auto &[alias, index] : choices.aliases)
        if (selection == -1 && stored.is_object())
            if (auto it = stored.find(alias.first); it != stored.end() && it->is_string() && it->get<std::string>() == alias.second)
                selection = index;
    if (selection == -1 && print.has(option)) {
        const std::string value = print.opt_serialize(option);
        for (size_t i = 0; i < choices.entries.size(); ++i)
            if (choices.entries[i].param.empty() && choices.entries[i].value == value)
                selection = int(i);
    }
    return selection;
}

MergedWrite merged_write(const MergedChoices &choices, size_t entry)
{
    MergedWrite out;
    if (entry >= choices.entries.size())
        return out;
    const MergedChoice &e = choices.entries[entry];
    // Orca's value: the merged parameters back to their default (Orca's); a parameter's value: that one.
    if (e.param.empty()) {
        out.option_value = e.value;
        for (const std::string &p : choices.params)
            out.values.emplace_back(p, std::string());
    } else
        out.values.emplace_back(e.param, nlohmann::json(e.value).dump());
    return out;
}

} // namespace ModHost
} // namespace Slic3r
