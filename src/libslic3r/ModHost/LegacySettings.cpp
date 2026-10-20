#include "LegacySettings.hpp"
#include "ModSet.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>

namespace Slic3r {
namespace ModHost {

using json = nlohmann::json;

namespace {

enum class Kind { Bool, Int, Float, Percent, FloatOrPercent, String };

// One line 1 setting and the mod parameter that now carries it. `owned` marks keys this build
// also defines (a mod rule writes them); the others are dropped from the loaded config.
struct LegacyKey
{
    const char *key;
    const char *param; // "<mod>.<name>"
    Kind        kind;
    bool        owned;
    // For a key earlier builds of this line defined and saved in every file: its default there, which
    // carries nothing to migrate.
    const char *saved_default = nullptr;
};

const LegacyKey legacy_keys[] = {
    // H/2 (a native mod since M7). Line 2 saved these keys as its rule wrote them, from the parameters mod_settings
    // keeps; their defaults carry nothing.
    { "outer_wall_half_layer_height", "half_layer.outer_walls", Kind::Bool, false, "0" },
    { "support_half_layer_height", "half_layer.support", Kind::Bool, false, "0" },
    // The low temperature interface (a native mod since M6). Line 2 saved these keys as its rule wrote them, from
    // the parameters mod_settings keeps; their defaults carry nothing.
    { "single_nozzle_low_temperature_interface", "low_temp_interface.enable", Kind::Bool, false, "0" },
    { "support_interface_temperature", "low_temp_interface.temperature", Kind::Int, false, "170" },
    { "support_interface_heating_time", "low_temp_interface.heating_time", Kind::Float, false, "5" },
    { "support_interface_auxiliary_fan_cooling_on_temperature_change", "low_temp_interface.aux_fan", Kind::Bool, false, "0" },
    { "support_interface_auxiliary_fan_speed", "low_temp_interface.aux_fan_speed", Kind::Int, false, "100" },
    { "support_interface_nozzle_wiping_on_temperature_change", "low_temp_interface.brush", Kind::Bool, false, "0" },
    { "support_interface_temperature_drop_tower", "low_temp_interface.tower", Kind::Bool, false, "0" },
    // Its printer settings (line 1's, never written by a rule): the brush's round trips and speed.
    { "support_interface_brush_repetitions", "low_temp_interface.brush_repetitions", Kind::Int, false, "0" },
    { "support_interface_brush_speed", "low_temp_interface.brush_speed", Kind::Float, false, "80" },
    { "use_smaller_nozzles_in_crisp_corners", "multi_nozzle_walls.enable", Kind::Bool, false, "0" },
    { "crisp_corner_small_nozzle_wall_count", "multi_nozzle_walls.small_wall_count", Kind::Int, false, "0" },
    { "crisp_corner_nozzle_wall_overlap", "multi_nozzle_walls.wall_overlap", Kind::Percent, false, "15%" },
    { "crisp_corner_interlace_small_nozzle_walls", "multi_nozzle_walls.interlock", Kind::Bool, false, "0" },
    { "crisp_corner_detail_toolhead", "multi_nozzle_walls.detail_toolhead", Kind::Int, false, "0" },
    { "crisp_corner_small_nozzle_wall_speed", "multi_nozzle_walls.small_wall_speed", Kind::FloatOrPercent, true },
    { "crisp_corner_large_nozzle_override_regions", "multi_nozzle_walls.override_regions", Kind::String, false, "" },
    { "support_interface_sublayer_angle", "support_extras.sublayer_angle", Kind::Float, false, "0" },
    { "support_interface_sublayer_temperature", "support_extras.sublayer_temperature", Kind::Int, false, "0" },
    { "cura_support_join_distance", "cura_support.join_distance", Kind::Float, false },
    { "cura_solid_support_raft", "cura_support.solid_first_layer", Kind::Bool, false },
    { "mixed_normal_coverage_threshold", "mixed_support.coverage_threshold", Kind::Percent, false },
    { "mixed_selective_merge", "mixed_support.selective_merge", Kind::Bool, false },
    { "mixed_tree_support_style", "mixed_support.tree_style", Kind::String, false },
    { "resin_support_tree_type", "resin_support.tree_type", Kind::String, false },
    { "resin_support_points_density_relative", "resin_support.points_density", Kind::Int, false },
    { "resin_support_enforcers_only", "resin_support.enforcers_only", Kind::Bool, false },
};

// Settings earlier builds of this line defined only for a mod's rules to write (support_extras before it became a
// native mod, and the resin support's object_elevation, now the object.z_offset slot). The mod's parameters, which mod_settings carries, are what the user set, so these are dropped
// without migrating: a vendor profile that happens to carry one of the names (SeeMeCNC's support_wall_loops, a key
// Orca does not define) must not turn a mod parameter on.
const char *const removed_rule_keys[] = {
    "support_interface_pattern_override", "support_interface_sublayer_start", "support_interface_sublayer_end",
    "support_wall_loops", "tree_support_wall_loops", "object_elevation",
    // The Snapmaker U1 start guard's switch, which its rule wrote from the printer's mod parameter.
    "snapmaker_u1_start_guard",
};

// Line 1 printer settings holding an X/Y position ("XxY" or "X,Y", mm), each the two parameters <param>_x, <param>_y.
// A negative coordinate is their automatic position, the default, and carries nothing.
struct LegacyPoint { const char *key; const char *param; };
const LegacyPoint legacy_points[] = {
    { "support_interface_cooling_position", "low_temp_interface.cooling_position" },
    { "support_interface_brush_start", "low_temp_interface.brush_start" },
    { "support_interface_brush_end", "low_temp_interface.brush_end" },
};

const LegacyPoint *find_point(const std::string &key)
{
    for (const LegacyPoint &p : legacy_points)
        if (key == p.key)
            return &p;
    return nullptr;
}

// "100x200" or "100,200" as two numbers; nothing for anything else.
std::optional<std::pair<double, double>> parse_point(const std::string &value)
{
    const size_t sep = value.find_first_of("x,");
    if (sep == std::string::npos)
        return std::nullopt;
    const std::string a = value.substr(0, sep), b = value.substr(sep + 1);
    char *end_a = nullptr, *end_b = nullptr;
    const double x = std::strtod(a.c_str(), &end_a), y = std::strtod(b.c_str(), &end_b);
    if (end_a == a.c_str() || end_b == b.c_str())
        return std::nullopt;
    return std::make_pair(x, y);
}

// Line 1 settings handled below by hand (value maps, conditions), or not reproducible here.
const char *const special_keys[] = {
    "mixed_normal_support_generator", "support_interface_sublayer_pattern", "support_interface_sublayer_pattern_type",
    "support_interface_sublayer_start_layer", "support_interface_sublayer_end_layer", "support_wall_count",
};
// Line 1's per-toolhead line widths (printer settings, mm or % of that toolhead's nozzle, 0 = the process width).
// They become this build's per-nozzle line width fields (process settings, mm): "toolhead_<width key>".
const char *const toolhead_width_keys[] = {
    "toolhead_bridge_line_width", "toolhead_initial_layer_line_width",
    "toolhead_inner_wall_line_width", "toolhead_internal_solid_infill_line_width", "toolhead_line_width",
    "toolhead_outer_wall_line_width", "toolhead_sparse_infill_line_width", "toolhead_support_line_width",
    "toolhead_top_surface_line_width",
};

// Line 1 writes these with neutral defaults ("0", "" or "nil" per toolhead); only a set value carries anything.
bool toolhead_width_set(const std::string &value)
{
    return value != "0" && !value.empty() && value.find_first_not_of("nil,0% ") != std::string::npos;
}

// A line 1 toolhead width list ("0.17,0,50%,nil") as mm per nozzle: a percentage of that nozzle's diameter, and
// 0 for an unset, unreadable or nil entry or a nozzle the printer does not have.
std::vector<double> toolhead_widths_mm(const std::string &value, const std::vector<double> &nozzles_mm)
{
    std::vector<double> out(nozzles_mm.size(), 0.);
    size_t begin = 0;
    for (size_t idx = 0; idx < out.size() && begin <= value.size(); ++idx) {
        const size_t      end   = std::min(value.find(',', begin), value.size());
        const std::string entry = value.substr(begin, end - begin);
        begin                   = end + 1;
        char        *stop = nullptr;
        const double v    = std::strtod(entry.c_str(), &stop);
        if (stop == entry.c_str() || !(v > 0.))
            continue;
        out[idx] = *stop == '%' ? 0.01 * v * nozzles_mm[idx] : v;
    }
    return out;
}

// Line 1's resin support parameters: resin_support_<x> (default tree) and resin_branching_support_<x>.
struct ResinKey { const char *suffix; Kind kind; };
const ResinKey resin_keys[] = {
    { "head_front_diameter", Kind::Float }, { "head_width", Kind::Float }, { "pillar_diameter", Kind::Float },
    { "small_pillar_diameter_percent", Kind::Percent }, { "max_bridges_on_pillar", Kind::Int },
    { "max_weight_on_model", Kind::Float }, { "pillar_connection_mode", Kind::String }, { "buildplate_only", Kind::Bool },
    { "pillar_widening_factor", Kind::Float }, { "base_diameter", Kind::Float }, { "base_height", Kind::Float },
    { "base_safety_distance", Kind::Float }, { "critical_angle", Kind::Float }, { "max_bridge_length", Kind::Float },
    { "max_pillar_link_distance", Kind::Float }, { "object_elevation", Kind::Float },
};

// Maps a line 1 resin key to its mod parameter, or nothing.
std::optional<std::pair<std::string, Kind>> resin_param(const std::string &key)
{
    for (const char *prefix : { "resin_support_", "resin_branching_support_" }) {
        const std::string p(prefix);
        if (key.compare(0, p.size(), p) != 0)
            continue;
        const std::string suffix = key.substr(p.size());
        for (const ResinKey &r : resin_keys)
            if (suffix == r.suffix)
                return std::make_pair(std::string("resin_support.") + (p == "resin_support_" ? "default_" : "branching_") + suffix, r.kind);
    }
    return std::nullopt;
}

const LegacyKey *find_key(const std::string &key)
{
    for (const LegacyKey &k : legacy_keys)
        if (key == k.key)
            return &k;
    return nullptr;
}

template<size_t N> bool in(const char *const (&list)[N], const std::string &key)
{
    return std::any_of(std::begin(list), std::end(list), [&key](const char *k) { return key == k; });
}

std::optional<json> convert(const std::string &value, Kind kind)
{
    char *end = nullptr;
    switch (kind) {
    case Kind::Bool:
        if (value == "1" || value == "true") return json(true);
        if (value == "0" || value == "false") return json(false);
        return std::nullopt;
    case Kind::Int: {
        const long v = std::strtol(value.c_str(), &end, 10);
        return end != value.c_str() ? std::optional<json>(json(v)) : std::nullopt;
    }
    case Kind::Float:
    case Kind::Percent: {
        // Percentages are stored as "80%" and become 80.
        const double v = std::strtod(value.c_str(), &end);
        return end != value.c_str() ? std::optional<json>(json(v)) : std::nullopt;
    }
    case Kind::FloatOrPercent: {
        // "50%" (of the wall speed) stays a percentage; a number is mm/s.
        const double v = std::strtod(value.c_str(), &end);
        if (end == value.c_str())
            return std::nullopt;
        return *end == '%' ? json(value.substr(0, size_t(end - value.c_str())) + "%") : json(v);
    }
    case Kind::String:
        if (!value.empty() && value.front() == '"') {
            // A list of strings (line 2's large nozzle override ranges) is stored quoted and comma-separated; its
            // entries are ranges, which the parameter separates with ';'.
            ConfigOptionStrings list;
            if (!list.deserialize(value))
                return std::nullopt;
            std::string joined;
            for (const std::string &entry : list.values)
                if (!entry.empty())
                    joined += (joined.empty() ? "" : ";") + entry;
            return json(joined);
        }
        return json(value);
    }
    return std::nullopt;
}

// Whether a stored value is a key's old default. A list of strings is stored quoted ("" for an empty one).
bool saved_default(const std::string &value, const char *default_value)
{
    return value == default_value || value == std::string("\"") + default_value + "\"";
}

} // namespace

// The parameter ("<mod>.<name>") and its type that a loaded mod's "legacy_keys" give for `key`.
static std::optional<std::pair<std::string, std::string>> manifest_legacy_param(const std::string &key)
{
    if (const auto mods = active_mods())
        for (const Mod &mod : mods->mods())
            if (auto it = mod.legacy_keys.find(key); it != mod.legacy_keys.end())
                if (const ModParam *p = mods->find_param(mod.id + "." + it->second))
                    return std::make_pair(mod.id + "." + it->second, p->type);
    return std::nullopt;
}

bool stash_legacy_setting(std::string &opt_key, std::string &value, std::map<std::string, std::string> &stash)
{
    if (opt_key == "support_type") {
        // Line 1's own support types are a generator chosen by a mod over Orca's normal support.
        static const std::pair<const char *, const char *> types[] = {
            { "normal_cura(auto)", "normal(auto)" }, { "normal_cura(manual)", "normal(manual)" },
            { "mixed(auto)", "normal(auto)" }, { "resin(auto)", "normal(auto)" } };
        for (const auto &[old_value, new_value] : types)
            if (value == old_value) {
                stash[opt_key] = value;
                value = new_value;
                return true;
            }
        return false;
    }
    if (opt_key == "support_interface_pattern") {
        if (value != "triangles")
            return false;
        stash[opt_key] = value;
        value = "auto";
        return true;
    }
    if (opt_key == "support_interface_sublayer_pattern") {
        // Line 1 stored a switch ("0"/"1"); earlier builds of this line the pattern name a rule wrote, which is
        // dropped like the other removed_rule_keys.
        if (value == "0" || value == "1")
            stash[opt_key] = value;
        opt_key.clear();
        return true;
    }
    if (in(removed_rule_keys, opt_key)) {
        opt_key.clear();
        return true;
    }
    if (const LegacyKey *k = find_key(opt_key)) {
        stash[opt_key] = value;
        if (!k->owned)
            opt_key.clear();
        return true;
    }
    if (in(special_keys, opt_key) || in(toolhead_width_keys, opt_key) || resin_param(opt_key) || find_point(opt_key)) {
        stash[opt_key] = value;
        opt_key.clear();
        return true;
    }
    // A key a loaded mod's manifest names in "legacy_keys". Kept when this build still defines it.
    if (manifest_legacy_param(opt_key)) {
        stash[opt_key] = value;
        if (print_config_def.get(opt_key) == nullptr)
            opt_key.clear();
        return true;
    }
    return false;
}

// Whether the active mods define `param` ("<mod>.<name>").
static bool mod_param_loaded(const std::string &param)
{
    const auto   mods = active_mods();
    const size_t dot  = param.find('.');
    if (!mods || dot == std::string::npos)
        return false;
    for (const Mod &mod : mods->mods())
        if (mod.id == param.substr(0, dot))
            for (const ModParam &p : mod.params)
                if (p.name == param.substr(dot + 1))
                    return true;
    return false;
}

LegacyMigration migrate_legacy_settings(DynamicPrintConfig &config, std::map<std::string, std::string> &stash)
{
    LegacyMigration result;
    if (stash.empty())
        return result;

    std::map<std::string, json> params;
    auto set = [&params](const std::string &param, std::optional<json> v) {
        if (v)
            params[param] = *v;
    };
    for (const auto &[key, value] : stash) {
        if (const LegacyKey *k = find_key(key)) {
            if (k->owned && mod_param_loaded(k->param)) {
                // A key this build defines is written by the mod's rule from now on. Back to its
                // default, it no longer counts as a user change, which would make the rule skip it
                // and keep the feature on after the mod parameter is turned off. Its default value
                // carries nothing to migrate (line 2 files save these keys too). Without the mod
                // loaded (a test, a CLI without resources) the key keeps driving the feature.
                const ConfigOptionDef *def = print_config_def.get(key);
                if (def != nullptr && def->default_value) {
                    const std::string default_value = def->default_value->serialize();
                    if (ConfigOption *opt = config.option(key))
                        opt->set(def->default_value.get());
                    if (value == default_value)
                        continue;
                }
            }
            if (!k->owned && k->saved_default != nullptr && saved_default(value, k->saved_default))
                continue;
            set(k->param, convert(value, k->kind));
        }
        else if (auto r = resin_param(key))
            set(r->first, convert(value, r->second));
        else if (const LegacyPoint *point = find_point(key)) {
            if (auto xy = parse_point(value); xy && (xy->first >= 0. || xy->second >= 0.)) {
                params[std::string(point->param) + "_x"] = xy->first;
                params[std::string(point->param) + "_y"] = xy->second;
            }
        }
        else if (auto m = manifest_legacy_param(key)) {
            const Kind kind = m->second == "bool" ? Kind::Bool : m->second == "int" ? Kind::Int : m->second == "float" ? Kind::Float :
                              m->second == "float_or_percent" ? Kind::FloatOrPercent : Kind::String;
            set(m->first, convert(value, kind));
        }
        else if (in(toolhead_width_keys, key)) {
            if (!toolhead_width_set(value))
                continue;
            // MAGPIE: into the per-nozzle line width field of the same width, in mm per nozzle. Only where the
            // config holds process settings (a project, a full config); a printer preset alone cannot carry them.
            const std::string width_key      = key.substr(std::string("toolhead_").size());
            const std::string per_nozzle_key = per_nozzle_line_width_key(width_key);
            const auto       *nozzles        = config.option<ConfigOptionFloats>("nozzle_diameter");
            auto             *field          = config.option<ConfigOptionFloats>(per_nozzle_key);
            if (nozzles == nullptr || nozzles->values.empty() || config.option(width_key) == nullptr) {
                result.dropped.push_back(key);
                continue;
            }
            // A field the file already sets keeps its values.
            if (field != nullptr && std::any_of(field->values.begin(), field->values.end(), [](double v) { return v > 0.; }))
                continue;
            config.set_key_value(per_nozzle_key, new ConfigOptionFloats(toolhead_widths_mm(value, nozzles->values)));
            result.migrated.push_back(per_nozzle_key);
        }
    }
    if (auto it = stash.find("support_type"); it != stash.end()) {
        if (it->second.rfind("normal_cura", 0) == 0)
            params["cura_support.enable"] = true;
        else if (it->second == "mixed(auto)")
            params["mixed_support.enable"] = true;
        else if (it->second == "resin(auto)")
            params["resin_support.enable"] = true;
    }
    if (stash.count("support_interface_pattern"))
        params["support_extras.interface_pattern"] = "triangles";
    if (auto it = stash.find("mixed_normal_support_generator"); it != stash.end())
        params["mixed_support.normal_generator"] = it->second == "cura" ? "cura" : "orca";
    if (auto it = stash.find("support_interface_sublayer_pattern"); it != stash.end() && it->second == "1") {
        // Line 1's sublayer switch with its start/end layers and pattern (default rectilinear).
        if (auto s = stash.find("support_interface_sublayer_start_layer"); s != stash.end())
            set("support_extras.sublayer_start", convert(s->second, Kind::Int));
        if (auto e = stash.find("support_interface_sublayer_end_layer"); e != stash.end())
            set("support_extras.sublayer_end", convert(e->second, Kind::Int));
        auto p = stash.find("support_interface_sublayer_pattern_type");
        params["support_extras.sublayer_pattern"] = p == stash.end() || p->second == "auto" ? "rectilinear" : p->second;
    }
    if (auto it = stash.find("support_wall_count"); it != stash.end()) {
        // Line 1's 0 means Orca's own walls, this build's -1.
        if (auto v = convert(it->second, Kind::Int); v && v->get<long>() > 0)
            params["support_extras.normal_wall_loops"] = *v;
    }
    stash.clear();

    // Keep values the file's mod_settings (printer_mod_settings for a printer parameter) already holds; leave out values
    // equal to the default.
    const auto mods = active_mods();
    auto default_of = [&mods](const std::string &param) -> std::optional<json> {
        if (!mods)
            return std::nullopt;
        const size_t dot = param.find('.');
        for (const Mod &mod : mods->mods())
            if (mod.id == param.substr(0, dot))
                for (const ModParam &p : mod.params)
                    if (p.name == param.substr(dot + 1)) {
                        if (const bool *b = std::get_if<bool>(&p.default_value)) return json(*b);
                        if (const double *d = std::get_if<double>(&p.default_value)) return json(*d);
                        if (const std::string *s = std::get_if<std::string>(&p.default_value)) return json(*s);
                    }
        return std::nullopt;
    };
    auto store_of = [&mods](const std::string &param) -> const char * {
        // The printer parameters this table migrates, for when their mod is not loaded.
        static const std::set<std::string> printer_params = {
            "low_temp_interface.cooling_position_x", "low_temp_interface.cooling_position_y", "low_temp_interface.brush_start_x",
            "low_temp_interface.brush_start_y",      "low_temp_interface.brush_end_x",        "low_temp_interface.brush_end_y",
            "low_temp_interface.brush_repetitions",  "low_temp_interface.brush_speed" };
        const ModParam *p = mods ? mods->find_param(param) : nullptr;
        const bool printer = p != nullptr ? p->scope == "printer" : printer_params.count(param) > 0;
        return printer ? "printer_mod_settings" : "mod_settings";
    };
    std::map<std::string, json> stored;
    auto stored_values = [&config, &stored](const char *store) -> json & {
        auto it = stored.find(store);
        if (it != stored.end())
            return it->second;
        json values = json::object();
        if (const auto *opt = config.option<ConfigOptionString>(store); opt && !opt->value.empty()) {
            try {
                values = json::parse(opt->value);
            } catch (const json::parse_error &) {
                values = json::object();
            }
            if (!values.is_object())
                values = json::object();
        }
        return stored.emplace(store, std::move(values)).first->second;
    };
    std::set<std::string> changed;
    for (const auto &[param, v] : params) {
        const char *store  = store_of(param);
        json       &values = stored_values(store);
        if (values.contains(param))
            continue;
        if (auto d = default_of(param); d && (*d == v || (d->is_number() && v.is_number() && d->get<double>() == v.get<double>())))
            continue;
        values[param] = v;
        result.migrated.push_back(param);
        changed.insert(store);
    }
    for (const std::string &store : changed)
        config.set_key_value(store, new ConfigOptionString(stored[store].dump()));
    for (const std::string &key : result.dropped)
        BOOST_LOG_TRIVIAL(warning) << "Magpie line 1 setting not supported by this build, ignored: " << key;
    return result;
}

} // namespace ModHost
} // namespace Slic3r
