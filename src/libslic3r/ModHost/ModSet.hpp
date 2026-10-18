#pragma once

#include "Expr.hpp"

#include <map>
#include <memory>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace boost { namespace filesystem { class path; } }

namespace Slic3r {

class DynamicPrintConfig;

namespace ModHost {

class ModError : public std::runtime_error
{
public:
    using std::runtime_error::runtime_error;
};

// A variable a mod adds, addressed in expressions as "<mod id>.<name>".
// The text for `language` (a locale such as "ko_KR"): its exact entry, else its first two letters,
// else `fallback`. Mods translate their own names and labels this way, since gettext only sees the app.
const std::string &localized_text(const std::map<std::string, std::string> &translations, const std::string &language,
                                  const std::string &fallback);

struct ModParam
{
    std::string              name;
    std::string              type; // "float", "int", "bool" or "string"
    // The preset that stores the value: "process" (mod_settings), "printer" (printer_mod_settings)
    // or "filament" (filament_mod_settings, one value per filament, read as "<mod id>.<name>[i]").
    std::string              scope = "process";
    ExprValue                default_value;
    std::string              label;
    // Translations of the label: "label_<language>" keys, by language ("ko", "pt_BR", ...).
    std::map<std::string, std::string> labels;
    std::string              unit;
    std::string              page;
    std::vector<std::string> invalidates; // slicing steps to redo when the value changes
};

// "Set <target> to <value> when <when> holds." Rules sit in the mod-rule layer: above the printer,
// filament and process presets, below a value the user changed in a preset.
struct ModRule
{
    std::string           target;
    Expr                  value;
    std::optional<Expr>   when;
    int                   priority = 0;    // higher is applied later, so it wins over lower
    std::optional<size_t> index;           // one element of a vector setting; all elements when absent
    std::string           mod_id;
};

struct Mod
{
    std::string           id;
    std::string           name;
    // Translations of the name: "name_<language>" keys.
    std::map<std::string, std::string> names;
    std::string           version;
    std::vector<ModParam> params;
    std::vector<ModRule>  rules;
};

// The loaded mods, checked against each other and put in evaluation order. Loading rejects a
// rule on an unknown setting, a name no setting or mod parameter provides, a cycle between
// rules, and two rules on one setting with the same priority.
class ModSet
{
public:
    // Parses one mod definition. JSON with // and /* */ comments.
    static Mod parse_mod(const std::string &json_text);
    // Checks the mods together and orders their rules. Throws ModError.
    static ModSet build(std::vector<Mod> mods);
    // Every *.json directly in `dir`, in file-name order. A file that fails to parse or check is
    // skipped and its error appended to `errors`; a missing directory gives an empty set.
    static ModSet load_dir(const boost::filesystem::path &dir, std::vector<std::string> &errors);
    // Every *.json in each directory in turn (file-name order within a directory), checked as one set.
    static ModSet load_dirs(const std::vector<boost::filesystem::path> &dirs, std::vector<std::string> &errors);

    bool empty() const { return m_order.empty(); }
    const std::vector<Mod> &mods() const { return m_mods; }

    // Applies the rules to a full print config. Parameter values come from the config's
    // mod_settings, printer_mod_settings and filament_mod_settings, else from the defaults;
    // `param_values` overrides both, keyed "<mod id>.<name>". A setting listed in the config's different_settings_to_system
    // (a value the user changed in a preset) is left as the user set it. A rule whose expression
    // cannot be evaluated (division by zero, an index past the end) is not applied and its error
    // is returned; the caller must not slice while any are returned.
    std::vector<std::string> resolve(DynamicPrintConfig &config, const std::map<std::string, ExprValue> &param_values = {}) const;

private:
    // Indices rather than pointers, so a ModSet stays valid when copied or moved.
    using Ref = std::pair<size_t, size_t>; // (mod, rule or param)
    std::vector<Mod>           m_mods;
    std::vector<Ref>           m_order;  // rules in evaluation order
    std::map<std::string, Ref> m_params; // "<mod id>.<name>"
};

// The mods the running application slices with. Empty unless a mods directory was loaded, in
// which case Print::apply leaves the config exactly as it came.
std::shared_ptr<const ModSet> active_mods();
void set_active_mods(std::shared_ptr<const ModSet> mods);
// Loads the mods bundled in <resources>/mods, then <data_dir>/mods, and makes them the active set;
// a mod that fails to load is logged and left out.
void load_active_mods_from_data_dir();

// The value of a process- or printer-scope parameter of an active mod: the one stored in `config`
// (mod_settings / printer_mod_settings), else the parameter's default. Nothing when no active mod
// declares the parameter or the stored value has the wrong type.
std::optional<ExprValue> param_value(const DynamicPrintConfig &config, const std::string &key);

} // namespace ModHost
} // namespace Slic3r
