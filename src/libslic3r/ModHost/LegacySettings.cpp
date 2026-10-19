#include "LegacySettings.hpp"
#include "ModSet.hpp"

#include "libslic3r/PrintConfig.hpp"

#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>

namespace Slic3r {
namespace ModHost {

using json = nlohmann::json;

namespace {

enum class Kind { Bool, Int, Float, Percent, String };

// One line 1 setting and the mod parameter that now carries it. `owned` marks keys this build
// also defines (a mod rule writes them); the others are dropped from the loaded config.
struct LegacyKey
{
    const char *key;
    const char *param; // "<mod>.<name>"
    Kind        kind;
    bool        owned;
};

const LegacyKey legacy_keys[] = {
    { "outer_wall_half_layer_height", "half_layer.outer_walls", Kind::Bool, true },
    { "support_half_layer_height", "half_layer.support", Kind::Bool, true },
    { "single_nozzle_low_temperature_interface", "low_temp_interface.enable", Kind::Bool, true },
    { "support_interface_temperature", "low_temp_interface.temperature", Kind::Int, true },
    { "support_interface_heating_time", "low_temp_interface.heating_time", Kind::Float, true },
    { "support_interface_auxiliary_fan_cooling_on_temperature_change", "low_temp_interface.aux_fan", Kind::Bool, true },
    { "support_interface_auxiliary_fan_speed", "low_temp_interface.aux_fan_speed", Kind::Int, true },
    { "support_interface_nozzle_wiping_on_temperature_change", "low_temp_interface.brush", Kind::Bool, true },
    { "support_interface_temperature_drop_tower", "low_temp_interface.tower", Kind::Bool, true },
    { "use_smaller_nozzles_in_crisp_corners", "multi_nozzle_walls.enable", Kind::Bool, true },
    { "crisp_corner_small_nozzle_wall_count", "multi_nozzle_walls.small_wall_count", Kind::Int, true },
    { "crisp_corner_nozzle_wall_overlap", "multi_nozzle_walls.wall_overlap", Kind::Percent, true },
    { "crisp_corner_interlace_small_nozzle_walls", "multi_nozzle_walls.interlock", Kind::Bool, true },
    { "crisp_corner_detail_toolhead", "multi_nozzle_walls.detail_toolhead", Kind::Int, true },
    { "crisp_corner_small_nozzle_wall_speed", "multi_nozzle_walls.small_wall_speed", Kind::Float, true },
    { "crisp_corner_large_nozzle_override_regions", "multi_nozzle_walls.override_regions", Kind::String, true },
    { "support_interface_sublayer_angle", "support_extras.sublayer_angle", Kind::Float, true },
    { "support_interface_sublayer_temperature", "support_extras.sublayer_temperature", Kind::Int, true },
    { "cura_support_join_distance", "cura_support.join_distance", Kind::Float, false },
    { "cura_solid_support_raft", "cura_support.solid_first_layer", Kind::Bool, false },
    { "mixed_normal_coverage_threshold", "mixed_support.coverage_threshold", Kind::Percent, false },
    { "mixed_selective_merge", "mixed_support.selective_merge", Kind::Bool, false },
    { "mixed_tree_support_style", "mixed_support.tree_style", Kind::String, false },
    { "resin_support_tree_type", "resin_support.tree_type", Kind::String, false },
    { "resin_support_points_density_relative", "resin_support.points_density", Kind::Int, false },
    { "resin_support_enforcers_only", "resin_support.enforcers_only", Kind::Bool, false },
};

// Line 1 settings handled below by hand (value maps, conditions), or not reproducible here.
const char *const special_keys[] = {
    "mixed_normal_support_generator", "support_interface_sublayer_pattern", "support_interface_sublayer_pattern_type",
    "support_interface_sublayer_start_layer", "support_interface_sublayer_end_layer", "support_wall_count",
};
const char *const unsupported_keys[] = {
    "toolhead_bridge_line_width", "toolhead_initial_layer_line_width",
    "toolhead_inner_wall_line_width", "toolhead_internal_solid_infill_line_width", "toolhead_line_width",
    "toolhead_outer_wall_line_width", "toolhead_sparse_infill_line_width", "toolhead_support_line_width",
    "toolhead_top_surface_line_width",
};

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
    case Kind::String: return json(value);
    }
    return std::nullopt;
}

} // namespace

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
        // Line 1 stored a switch ("0"/"1"); this build stores the pattern name.
        if (value != "0" && value != "1")
            return false;
        stash[opt_key] = value;
        opt_key.clear();
        return true;
    }
    if (const LegacyKey *k = find_key(opt_key)) {
        stash[opt_key] = value;
        if (!k->owned)
            opt_key.clear();
        return true;
    }
    if (in(special_keys, opt_key) || in(unsupported_keys, opt_key) || resin_param(opt_key)) {
        stash[opt_key] = value;
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
            if (k->kind == Kind::Float && value.find('%') != std::string::npos) {
                // Line 1 allowed a percentage here (of the wall speed); the parameter is mm/s, so
                // reading "50%" as 50 mm/s would change the print. Report it instead.
                result.dropped.push_back(key);
                continue;
            }
            set(k->param, convert(value, k->kind));
        }
        else if (auto r = resin_param(key))
            set(r->first, convert(value, r->second));
        else if (in(unsupported_keys, key)) {
            // Line 1 writes these with neutral defaults ("0", "" or "nil" per toolhead); only a set value is lost.
            if (value != "0" && !value.empty() && value.find_first_not_of("nil,0% ") != std::string::npos)
                result.dropped.push_back(key);
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

    // Keep values the file's mod_settings already holds; leave out values equal to the default.
    json stored = json::object();
    if (const auto *opt = config.option<ConfigOptionString>("mod_settings"); opt && !opt->value.empty()) {
        try {
            stored = json::parse(opt->value);
        } catch (const json::parse_error &) {
            stored = json::object();
        }
        if (!stored.is_object())
            stored = json::object();
    }
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
    bool changed = false;
    for (const auto &[param, v] : params) {
        if (stored.contains(param))
            continue;
        if (auto d = default_of(param); d && (*d == v || (d->is_number() && v.is_number() && d->get<double>() == v.get<double>())))
            continue;
        stored[param] = v;
        result.migrated.push_back(param);
        changed = true;
    }
    if (changed)
        config.set_key_value("mod_settings", new ConfigOptionString(stored.dump()));
    for (const std::string &key : result.dropped)
        BOOST_LOG_TRIVIAL(warning) << "Magpie line 1 setting not supported by this build, ignored: " << key;
    return result;
}

} // namespace ModHost
} // namespace Slic3r
