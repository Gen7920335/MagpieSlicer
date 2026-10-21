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

// Settings earlier builds of this line defined only for a mod's rules to write (support_extras before it became a
// native mod, and the resin support's object_elevation, now the object.z_offset slot). The mod's parameters, which mod_settings carries, are what the user set, so these are dropped
// without migrating: a vendor profile that happens to carry one of the names (SeeMeCNC's support_wall_loops, a key
// Orca does not define) must not turn a mod parameter on.
const char *const removed_rule_keys[] = {
    "support_interface_pattern_override", "support_interface_sublayer_start", "support_interface_sublayer_end",
    "support_wall_loops", "tree_support_wall_loops", "object_elevation",
    // The Snapmaker U1 start guard's switch, which its rule wrote from the printer's mod parameter.
    "snapmaker_u1_start_guard",
    // The settings mod rules wrote to pick a mod's implementation and to pass its values to the core's export: the
    // active mods' slot declarations do both now ("when", "settings"; ModHost/SlotChoice.hpp).
    "support_generator", "wall_planner", "layer_time_planner", "layer_time_blend_log", "layer_time_blend_max_diff",
    "layer_time_blend_max_ratio", "layer_time_blend_ease_layers", "layer_time_blend_start_layer", "layer_time_blend_wait_cap",
    "layer_time_blend_warn_total", "layer_time_blend_warn_single", "layer_time_blend_wait_at", "layer_time_blend_in_place_max",
    "layer_time_blend_tower_size", "layer_time_blend_park_x", "layer_time_blend_park_y", "layer_time_blend_heat_rate",
    "layer_time_blend_cool_rate",
};

// "100x200" or "100,200" as two numbers; nothing for anything else.
std::optional<std::pair<double, double>> parse_point(const std::string &value)
{
    const size_t sep = value.find_first_of("x,");
    if (sep == std::string::npos)
        return std::nullopt;
    const std::string a = value.substr(0, sep), b = value.substr(sep + 1);
    const char *end_a = nullptr, *end_b = nullptr;
    const double x = parse_decimal(a.c_str(), &end_a), y = parse_decimal(b.c_str(), &end_b);
    if (end_a == a.c_str() || end_b == b.c_str())
        return std::nullopt;
    return std::make_pair(x, y);
}

// Line 1 settings handled below by hand: value maps and settings read together, which a mod's "legacy_keys" (one key,
// one parameter) cannot say. The plain ones are in the mods' manifests.
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
        const char  *stop = nullptr;
        const double v    = parse_decimal(entry.c_str(), &stop);
        if (stop == entry.c_str() || !(v > 0.))
            continue;
        out[idx] = *stop == '%' ? 0.01 * v * nozzles_mm[idx] : v;
    }
    return out;
}

// A legacy key a mod's manifest declares: the mod (active, else known and turned off) and how the key loads.
struct LegacyTarget
{
    const Mod          *mod = nullptr;
    const ModLegacyKey *key = nullptr;
};

// The mods to read legacy keys and parameters from: the active ones first, then every known one (turned off).
struct LegacyMods
{
    std::shared_ptr<const ModSet>           active = active_mods();
    std::shared_ptr<const std::vector<Mod>> known  = known_mods();

    template<class F> const Mod *find_mod(F &&pred) const
    {
        if (active)
            for (const Mod &mod : active->mods())
                if (pred(mod))
                    return &mod;
        if (known)
            for (const Mod &mod : *known)
                if (pred(mod))
                    return &mod;
        return nullptr;
    }
    LegacyTarget find_key(const std::string &key) const
    {
        const Mod *mod = find_mod([&key](const Mod &m) { return m.legacy_keys.count(key) > 0; });
        return mod ? LegacyTarget{ mod, &mod->legacy_keys.at(key) } : LegacyTarget{};
    }
    // The parameter "<mod>.<name>".
    const ModParam *find_param(const std::string &param) const
    {
        const size_t dot = param.find('.');
        if (dot == std::string::npos)
            return nullptr;
        const std::string id = param.substr(0, dot), name = param.substr(dot + 1);
        const Mod *mod = find_mod([&id](const Mod &m) { return m.id == id; });
        if (mod == nullptr)
            return nullptr;
        for (const ModParam &p : mod->params)
            if (p.name == name)
                return &p;
        return nullptr;
    }
};

Kind kind_of(const ModParam &p)
{
    return p.type == "bool" ? Kind::Bool : p.type == "int" ? Kind::Int : p.type == "float" ? Kind::Float :
           p.type == "float_or_percent" ? Kind::FloatOrPercent : Kind::String;
}

template<size_t N> bool in(const char *const (&list)[N], const std::string &key)
{
    return std::any_of(std::begin(list), std::end(list), [&key](const char *k) { return key == k; });
}

std::optional<json> convert(const std::string &value, Kind kind)
{
    const char *end = nullptr;
    switch (kind) {
    case Kind::Bool:
        if (value == "1" || value == "true") return json(true);
        if (value == "0" || value == "false") return json(false);
        return std::nullopt;
    case Kind::Int: {
        char      *int_end = nullptr;
        const long v       = std::strtol(value.c_str(), &int_end, 10);
        return int_end != value.c_str() ? std::optional<json>(json(v)) : std::nullopt;
    }
    case Kind::Float:
    case Kind::Percent: {
        // Percentages are stored as "80%" and become 80.
        const double v = parse_decimal(value.c_str(), &end);
        return end != value.c_str() ? std::optional<json>(json(v)) : std::nullopt;
    }
    case Kind::FloatOrPercent: {
        // "50%" (of the wall speed) stays a percentage; a number is mm/s.
        const double v = parse_decimal(value.c_str(), &end);
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
    if (in(special_keys, opt_key) || in(toolhead_width_keys, opt_key)) {
        stash[opt_key] = value;
        opt_key.clear();
        return true;
    }
    // A key a mod's manifest names in "legacy_keys" (an active mod's, else a turned-off one's). Kept when this build
    // still defines it.
    if (LegacyMods().find_key(opt_key).key != nullptr) {
        stash[opt_key] = value;
        if (print_config_def.get(opt_key) == nullptr)
            opt_key.clear();
        return true;
    }
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
    const LegacyMods mods;
    for (const auto &[key, value] : stash) {
        if (const LegacyTarget t = mods.find_key(key); t.key != nullptr) {
            const ModLegacyKey &k = *t.key;
            if (k.saved_default && saved_default(value, k.saved_default->c_str()))
                continue;
            if (k.point) {
                if (auto xy = parse_point(value); xy && (xy->first >= 0. || xy->second >= 0.)) {
                    params[t.mod->id + "." + k.param + "_x"] = xy->first;
                    params[t.mod->id + "." + k.param + "_y"] = xy->second;
                }
                continue;
            }
            const std::string param = t.mod->id + "." + k.param;
            if (const ModParam *p = mods.find_param(param))
                set(param, convert(value, kind_of(*p)));
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
    auto default_of = [&mods](const std::string &param) -> std::optional<json> {
        if (const ModParam *p = mods.find_param(param)) {
            if (const bool *b = std::get_if<bool>(&p->default_value)) return json(*b);
            if (const double *d = std::get_if<double>(&p->default_value)) return json(*d);
            if (const std::string *s = std::get_if<std::string>(&p->default_value)) return json(*s);
        }
        return std::nullopt;
    };
    auto store_of = [&mods](const std::string &param) -> const char * {
        const ModParam *p = mods.find_param(param);
        return p != nullptr && p->scope == "printer" ? "printer_mod_settings" : "mod_settings";
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
