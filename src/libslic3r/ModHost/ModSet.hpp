#pragma once

#include "Expr.hpp"

#include <cstdint>
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

// One choice of a string parameter's dropdown.
struct ModParamOption
{
    std::string                        value;
    std::string                        label; // the value itself when the mod gives none
    std::map<std::string, std::string> labels;
};

struct ModParam
{
    std::string              name;
    // "float", "int", "bool", "string", or "float_or_percent" (a number, or a string "50%"; a rule
    // writing it to a setting that takes a percentage passes the percentage on).
    std::string              type;
    // Where the value is stored and how far down it can differ:
    // "process" (mod_settings), "printer" (printer_mod_settings), "filament" (filament_mod_settings,
    // one value per filament, read as "<mod id>.<name>[i]"), "object" (mod_settings, overridden per
    // object by object_mod_settings) or "region" (mod_settings, overridden per object, layer range,
    // part or modifier by region_mod_settings). Rules read the process value of object and region
    // parameters; a native mod reads the value of the object or region it is called for.
    std::string              scope = "process";
    ExprValue                default_value;
    std::string              label;
    // Translations of the label: "label_<language>" keys, by language ("ko", "pt_BR", ...).
    std::map<std::string, std::string> labels;
    std::string              unit;
    std::string              page;
    // Slicing steps to redo when the value changes, by step name ("posSupportMaterial",
    // "psGCodeExport", see mod_step_names()). An empty list redoes everything from slicing, so a mod
    // that leaves it out gets a slower but never a stale result. "none" says the value changes the
    // print only through the settings its rules write, which invalidate what they change themselves.
    std::vector<std::string> invalidates;
    // A string parameter with options is a dropdown and takes only these values; a stored value
    // outside them is an error, like a value of the wrong type.
    std::vector<ModParamOption> options;
    // The settings tab shows the row only while this holds. Display only: rules read the value
    // whether or not the row is shown, and a hidden row keeps its stored value.
    std::optional<Expr>      visible;
};

// Whether a parameter's row is shown, reading other parameters through `lookup`. A condition that
// cannot be evaluated shows the row, so a broken condition never hides a setting.
bool param_visible(const ModParam &param, const ExprLookup &lookup);
// Whether `value` is allowed for `param`: any value without options, else one of them.
bool param_allows(const ModParam &param, const ExprValue &value);

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

// A slot implementation a native mod registers ("slots" in the manifest).
struct ModSlotDecl
{
    std::string slot;        // "layer.bands"
    uint32_t    version = 0; // the slot interface version
    std::string impl;        // the name the mod registers the implementation under
    // For the slots the core picks one implementation of (ModSet::choose_slot): the implementation is picked
    // where `when` holds, and of several that hold, the one with the highest `priority`. Without `when` it is
    // never picked that way; slots whose every implementation is asked (gcode.events, layer.bands, ...) need none.
    std::optional<Expr>         when;
    int                         priority = 0;
    // Values the core reads for the picked implementation, by the names its slot gives ("settings").
    std::map<std::string, Expr> settings;
};

// "legacy_keys": a setting of an earlier build (Magpie line 1, or this line before the feature was a mod) that loads
// as a parameter of the mod (LegacySettings.hpp). The value converts by the parameter's type.
struct ModLegacyKey
{
    std::string                param;          // the parameter name in this mod; for a point, the name before _x/_y
    // The key's default in the earlier build, which every file of it saved and which carries nothing.
    std::optional<std::string> saved_default;
    // A position "XxY" or "X,Y" (mm) that becomes the two parameters <param>_x and <param>_y; a negative coordinate
    // was the automatic position and carries nothing.
    bool                       point = false;
};

// "depends": another mod (or "core", the mod API) that must be loaded first, at least `min`.
struct ModDependency
{
    std::string id;
    std::string min_version;
};

// Declarative GUI ("gui" in the manifest). The core GUI draws each item; a mod never runs wx code.
struct ModGuiItem
{
    std::string                        id;    // identifier, unique within its list
    std::string                        label;
    std::map<std::string, std::string> labels; // "label_<language>"
    std::string                        color;  // paint channels: "#RRGGBB"
    std::string                        unit;   // legends
};

struct ModGui
{
    // Support painting channels (at most two: the paint tool stores channel 0 and 1 besides plain
    // enforcers, ModelVolume::support_channel_facets), shown while paint_when holds (always without it).
    std::vector<ModGuiItem> paint_channels;
    std::optional<Expr>     paint_when;
    std::vector<ModGuiItem> aux_towers;     // auxiliary towers the core places, previews and drags
    std::vector<ModGuiItem> legends;        // per-layer numbers the preview legend shows
    std::vector<ModGuiItem> calibrations;   // Calibration menu entries
    // Parameters of object or region scope shown in an object's settings, in this order. Empty
    // shows every object and region parameter.
    std::vector<std::string> object_params;
};

// Where a mod was found. Bundled mods ship with the application in <resources>/mods; installed
// mods are under <data dir>/mods.
enum class ModOrigin { Bundled, Installed };

struct Mod
{
    std::string           id;
    std::string           name;
    // Translations of the name: "name_<language>" keys.
    std::map<std::string, std::string> names;
    std::string           version;
    std::vector<ModParam> params;
    std::vector<ModRule>  rules;

    // Package manifest fields (docs/HLSD/mods.md). A single-file JSON mod has none of them.
    int                                api_major     = 0; // "api": {"major", "min_minor"}; 0 = no native code
    int                                api_min_minor = 0;
    std::string                        binary;            // library name under bin/<platform>/, empty = none
    std::vector<ModSlotDecl>           slots;
    std::vector<ModDependency>         depends;
    std::vector<std::string>           conflicts;
    bool                               thread_safe = false;
    std::map<std::string, ModLegacyKey> legacy_keys; // old setting key -> how it loads
    ModGui                             gui;

    // Set by the loader, not read from the file.
    ModOrigin   origin = ModOrigin::Installed;
    // Bundled, or signed with an official key. Only official mods may write G-code settings
    // (machine_start_gcode and the other *_gcode keys) and run native code without a verified key.
    bool        official = false;
    bool        verified = false; // signed with an official or a verified key
    std::string dir;              // the package folder; empty for a single JSON file
};

// The step names an "invalidates" list may use, PrintObjectStep and PrintStep names, plus "none".
const std::vector<std::string> &mod_step_names();
// Whether a setting holds G-code a printer runs (machine_start_gcode, layer_change_gcode, ...).
bool is_gcode_setting(const std::string &key);
// Compares two dotted version strings numerically ("1.10.0" > "1.9"); missing parts count as 0.
int compare_versions(const std::string &a, const std::string &b);

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
    // Every *.json directly in `dir` and every package folder <dir>/<id>/mod.json, in file-name order.
    // A file that fails to parse or check is skipped and its error appended to `errors`; a missing
    // directory gives an empty set. The mods count as bundled with the application (official): this
    // is how the bundled set and tests load. Native code is not loaded; the application's loader
    // (load_active_mods_from_data_dir, ModPackage.hpp) does that and checks installed mods' signatures.
    static ModSet load_dir(const boost::filesystem::path &dir, std::vector<std::string> &errors);
    // The same for each directory in turn, checked as one set.
    static ModSet load_dirs(const std::vector<boost::filesystem::path> &dirs, std::vector<std::string> &errors);

    // No mods (a set of mods without rules is not empty: their parameters still change what they slice).
    bool empty() const { return m_mods.empty(); }
    const std::vector<Mod> &mods() const { return m_mods; }

    // Applies the rules to a full print config. Parameter values come from the config's
    // mod_settings, printer_mod_settings and filament_mod_settings, else from the defaults;
    // `param_values` overrides both, keyed "<mod id>.<name>". A setting listed in the config's different_settings_to_system
    // (a value the user changed in a preset) is left as the user set it. A rule whose expression
    // cannot be evaluated (division by zero, an index past the end) is not applied and its error
    // is returned; the caller must not slice while any are returned.
    // `clashes`, when given, gets each setting that rules of more than one mod set to different
    // values (several support generators turned on), and whose value is used.
    struct RuleClash
    {
        std::string              setting;    // "<key>", or "<key>[i]" for one element
        std::string              used_mod;   // the mod whose rule applied last and stays
        std::string              used_value;
        std::vector<std::string> other_mods; // mods whose different values were overwritten
    };
    std::vector<std::string> resolve(DynamicPrintConfig &config, const std::map<std::string, ExprValue> &param_values = {},
                                     std::vector<RuleClash> *clashes = nullptr) const;

    // The parameter "<mod id>.<name>", or nullptr.
    const ModParam *find_param(const std::string &key) const;
    const Mod      *find_mod(const std::string &id) const;

    // Where a parameter's value comes from, most specific first: the region's region_mod_settings,
    // the object's object_mod_settings, then the print config's mod_settings, printer_mod_settings or
    // filament_mod_settings. Null members are skipped.
    struct ValueSources
    {
        const DynamicPrintConfig *print          = nullptr;
        const std::string        *object_values  = nullptr; // object_mod_settings JSON
        const std::string        *region_values  = nullptr; // region_mod_settings JSON
    };
    // The value of "<mod id>.<name>" (filament i for a filament parameter) from `sources`, else the
    // default. Nothing when no loaded mod has the parameter, the index does not fit its scope, or a
    // stored value has the wrong type (then `error`, when given, says which).
    std::optional<ExprValue> value_of(const std::string &key, const ValueSources &sources, std::optional<size_t> filament = {},
                                      std::string *error = nullptr) const;

    // The implementation picked for a job one of `slots` does (Orca's support is replaced by one of support.areas,
    // support.compose and support.contacts): among the slot declarations with a "when" that holds, read with the
    // parameter values `sources` give (an object's or region's own where they have one) and the print config's
    // settings, the one with the highest "priority". None picked: mod is null.
    struct SlotChoice
    {
        const Mod               *mod  = nullptr;
        const ModSlotDecl       *decl = nullptr;
        std::vector<std::string> other_mods; // other mods whose "when" holds too, at a lower priority (not used)
        std::string              error;      // a "when" that could not be read, or two that hold at one priority
    };
    SlotChoice choose_slot(const std::vector<std::string> &slots, const ValueSources &sources) const;
    // The picked implementation's setting `name` ("settings" in its slot declaration), read as its "when" is.
    // Nothing when it declares no such setting or the expression fails (then `error` says why).
    std::optional<ExprValue> slot_setting(const SlotChoice &choice, const std::string &name, const ValueSources &sources,
                                          std::string *error = nullptr) const;

    // The slicing steps to redo when a stored-values object changes from `old_json` to `new_json`
    // (mod_settings, printer_mod_settings, one filament's filament_mod_settings, object_mod_settings or
    // region_mod_settings): the union of the "invalidates" of every parameter whose value changed,
    // posSlice for one that declares none. Values of mods that are not loaded change nothing that is
    // sliced and add no step. Step names as in mod_step_names(); "none" is never returned.
    std::set<std::string> invalidated_steps(const std::string &old_json, const std::string &new_json) const;

private:
    ExprLookup slot_lookup(const ValueSources &sources) const;

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
// Every mod manifest the last load found, turned-off mods' included: LegacySettings reads their legacy_keys, so a
// file from an earlier build keeps a feature's values while its mod is off. Null before a load.
std::shared_ptr<const std::vector<Mod>> known_mods();
void set_known_mods(std::shared_ptr<const std::vector<Mod>> mods);
// Loads the mods bundled in <resources>/mods, then <data_dir>/mods, and makes them the active set;
// a mod that fails to load is logged and left out.
void load_active_mods_from_data_dir();

// The value of a process-, printer-, object- or region-scope parameter of an active mod: the one
// stored in `config` (mod_settings / printer_mod_settings), else the parameter's default. Nothing
// when no active mod declares the parameter or the stored value has the wrong type.
std::optional<ExprValue> param_value(const DynamicPrintConfig &config, const std::string &key);
// The first active mod (in load order) with support painting channels whose gui paint_when holds under `config`,
// or nullptr. A condition that cannot be evaluated does not hold.
const Mod *active_paint_channel_mod(const ModSet &mods, const DynamicPrintConfig &config);

// `override_json`'s values laid over `base_json`'s, per parameter (both objects keyed
// "<mod id>.<name>"). How a part's or modifier's region_mod_settings joins its object's. Text
// that is not a JSON object counts as empty. The result is "" for no values.
std::string merge_mod_values(const std::string &base_json, const std::string &override_json);

} // namespace ModHost
} // namespace Slic3r
