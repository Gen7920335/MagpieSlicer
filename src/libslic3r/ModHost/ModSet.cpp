#include "ModSet.hpp"

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <mutex>
#include <cmath>
#include <functional>
#include <regex>
#include <sstream>
#include <utility>

namespace Slic3r {
namespace ModHost {

namespace {

using json = nlohmann::json;

[[noreturn]] void mod_fail(const std::string &mod_id, const std::string &what)
{
    throw ModError((mod_id.empty() ? std::string("mod") : "mod '" + mod_id + "'") + ": " + what);
}

std::string required_string(const json &j, const char *key, const std::string &mod_id)
{
    auto it = j.find(key);
    if (it == j.end() || !it->is_string() || it->get<std::string>().empty())
        mod_fail(mod_id, std::string("\"") + key + "\" must be a non-empty string");
    return it->get<std::string>();
}

std::string optional_string(const json &j, const char *key)
{
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

// Ids and parameter names are plain identifiers, so "<id>.<name>" splits back unambiguously.
bool is_identifier(const std::string &s) { return std::regex_match(s, std::regex("[a-z_][a-z0-9_]*")); }

ExprValue param_default(const json &j, const std::string &type, const std::string &where)
{
    auto it = j.find("default");
    if (it == j.end())
        throw ModError(where + ": \"default\" is missing");
    if (type == "float" && it->is_number())
        return it->get<double>();
    if (type == "int" && it->is_number_integer())
        return double(it->get<long long>());
    if (type == "bool" && it->is_boolean())
        return it->get<bool>();
    if (type == "string" && it->is_string())
        return it->get<std::string>();
    throw ModError(where + ": \"default\" does not match type \"" + type + "\"");
}

Expr parse_expr(const std::string &text, const std::string &where)
{
    try {
        return Expr::parse(text);
    } catch (const ExprError &e) {
        throw ModError(where + ": " + e.what());
    }
}

// Option types a rule can read and write. Points, vectors of enums and the like are not supported.
bool is_supported_type(ConfigOptionType t, bool writing)
{
    switch (t) {
    case coFloat: case coPercent: case coFloatOrPercent: case coInt: case coBool: case coString: case coEnum:
    case coFloats: case coPercents: case coFloatsOrPercents: case coInts: case coBools: case coStrings:
        return true;
    case coEnums:
        return !writing;
    default:
        return false;
    }
}

bool is_vector_type(ConfigOptionType t) { return (t & coVectorType) != 0; }

std::optional<ExprValue> read_option(const ConfigOption *opt, ConfigOptionType type, std::optional<size_t> index)
{
    if (opt == nullptr || !is_supported_type(type, false))
        return std::nullopt;
    if (!is_vector_type(type)) {
        if (index)
            return std::nullopt;
        switch (type) {
        case coFloat: case coPercent: case coFloatOrPercent: return static_cast<const ConfigOptionFloat *>(opt)->value;
        case coInt: return double(static_cast<const ConfigOptionInt *>(opt)->value);
        case coBool: return static_cast<const ConfigOptionBool *>(opt)->value;
        case coString: return static_cast<const ConfigOptionString *>(opt)->value;
        default: return opt->serialize(); // coEnum: the enum's name
        }
    }
    const auto *vec = static_cast<const ConfigOptionVectorBase *>(opt);
    if (!index || *index >= vec->size())
        return std::nullopt;
    const size_t i = *index;
    switch (type) {
    case coFloats: case coPercents:
        if (auto *v = dynamic_cast<const ConfigOptionFloats *>(opt)) return v->values[i];
        if (auto *v = dynamic_cast<const ConfigOptionFloatsNullable *>(opt)) return v->values[i];
        break;
    case coFloatsOrPercents:
        if (auto *v = dynamic_cast<const ConfigOptionFloatsOrPercents *>(opt)) return v->values[i].value;
        if (auto *v = dynamic_cast<const ConfigOptionFloatsOrPercentsNullable *>(opt)) return v->values[i].value;
        break;
    case coInts:
        if (auto *v = dynamic_cast<const ConfigOptionInts *>(opt)) return double(v->values[i]);
        if (auto *v = dynamic_cast<const ConfigOptionIntsNullable *>(opt)) return double(v->values[i]);
        break;
    case coBools:
        if (auto *v = dynamic_cast<const ConfigOptionBools *>(opt)) return v->values[i] != 0;
        if (auto *v = dynamic_cast<const ConfigOptionBoolsNullable *>(opt)) return v->values[i] != 0;
        break;
    case coStrings:
        if (auto *v = dynamic_cast<const ConfigOptionStrings *>(opt)) return v->values[i];
        break;
    default: // coEnums: the element's name
        return vec->vserialize()[i];
    }
    return std::nullopt;
}

double to_number(const ExprValue &v, const std::string &where)
{
    if (const double *d = std::get_if<double>(&v)) return *d;
    if (const bool *b = std::get_if<bool>(&v)) return *b ? 1. : 0.;
    throw ModError(where + ": a string cannot be stored in a numeric setting");
}

template<class Vec, class Value>
void assign_elements(Vec &vec, const Value &value, std::optional<size_t> index, const std::string &where)
{
    if (index) {
        if (*index >= vec.values.size())
            throw ModError(where + ": index " + std::to_string(*index) + " is past the end of the setting");
        vec.values[*index] = value;
    } else {
        std::fill(vec.values.begin(), vec.values.end(), value);
    }
}

void write_option(ConfigOption *opt, ConfigOptionType type, const ExprValue &value, std::optional<size_t> index, const std::string &where)
{
    if (!is_vector_type(type) && index)
        throw ModError(where + ": \"index\" is given for a setting that is not a list");
    switch (type) {
    case coFloat:
        static_cast<ConfigOptionFloat *>(opt)->value = to_number(value, where);
        return;
    case coPercent:
        // A percentage takes 35 or "35%".
        if (const std::string *s = std::get_if<std::string>(&value)) {
            if (!opt->deserialize(*s))
                throw ModError(where + ": \"" + *s + "\" is not a percentage");
        } else {
            static_cast<ConfigOptionPercent *>(opt)->value = to_number(value, where);
        }
        return;
    case coFloatOrPercent: {
        auto *o = static_cast<ConfigOptionFloatOrPercent *>(opt);
        if (const std::string *s = std::get_if<std::string>(&value)) {
            if (!o->deserialize(*s))
                throw ModError(where + ": \"" + *s + "\" is not a number or a percentage");
        } else {
            // A number is an absolute value; a rule writes "50%" as a string for a percentage.
            o->value   = to_number(value, where);
            o->percent = false;
        }
        return;
    }
    case coInt:
        static_cast<ConfigOptionInt *>(opt)->value = int(std::lround(to_number(value, where)));
        return;
    case coBool:
        static_cast<ConfigOptionBool *>(opt)->value = expr_truthy(value);
        return;
    case coString:
        static_cast<ConfigOptionString *>(opt)->value = expr_to_string(value);
        return;
    case coEnum:
        if (!opt->deserialize(expr_to_string(value)))
            throw ModError(where + ": \"" + expr_to_string(value) + "\" is not one of the setting's values");
        return;
    case coFloats: case coPercents:
        if (auto *v = dynamic_cast<ConfigOptionFloats *>(opt)) return assign_elements(*v, to_number(value, where), index, where);
        if (auto *v = dynamic_cast<ConfigOptionFloatsNullable *>(opt)) return assign_elements(*v, to_number(value, where), index, where);
        break;
    case coFloatsOrPercents: {
        FloatOrPercent fp{0., false};
        if (const std::string *s = std::get_if<std::string>(&value)) {
            ConfigOptionFloatOrPercent single;
            if (!single.deserialize(*s))
                throw ModError(where + ": \"" + *s + "\" is not a number or a percentage");
            fp = {single.value, single.percent};
        } else {
            fp = {to_number(value, where), false};
        }
        if (auto *v = dynamic_cast<ConfigOptionFloatsOrPercents *>(opt)) return assign_elements(*v, fp, index, where);
        if (auto *v = dynamic_cast<ConfigOptionFloatsOrPercentsNullable *>(opt)) return assign_elements(*v, fp, index, where);
        break;
    }
    case coInts: {
        const int i = int(std::lround(to_number(value, where)));
        if (auto *v = dynamic_cast<ConfigOptionInts *>(opt)) return assign_elements(*v, i, index, where);
        if (auto *v = dynamic_cast<ConfigOptionIntsNullable *>(opt)) return assign_elements(*v, i, index, where);
        break;
    }
    case coBools: {
        const unsigned char b = expr_truthy(value) ? 1 : 0;
        if (auto *v = dynamic_cast<ConfigOptionBools *>(opt)) return assign_elements(*v, b, index, where);
        if (auto *v = dynamic_cast<ConfigOptionBoolsNullable *>(opt)) return assign_elements(*v, b, index, where);
        break;
    }
    case coStrings:
        if (auto *v = dynamic_cast<ConfigOptionStrings *>(opt)) return assign_elements(*v, expr_to_string(value), index, where);
        break;
    default:
        break;
    }
    throw ModError(where + ": the setting's type cannot be written by a rule");
}

// Keys the user changed in the printer, filament and process presets. Each entry of
// different_settings_to_system is one preset's ';'-separated key list.
std::set<std::string> user_changed_keys(const DynamicPrintConfig &config)
{
    std::set<std::string> keys;
    if (const auto *diff = config.option<ConfigOptionStrings>("different_settings_to_system"))
        for (const std::string &entry : diff->values) {
            std::vector<std::string> parts;
            boost::split(parts, entry, [](char c) { return c == ';'; });
            for (std::string &k : parts)
                if (!k.empty())
                    keys.insert(std::move(k));
        }
    return keys;
}

std::mutex                    g_active_mutex;
std::shared_ptr<const ModSet> g_active;

} // namespace

Mod ModSet::parse_mod(const std::string &json_text)
{
    json j;
    try {
        j = json::parse(json_text, nullptr, true, /* ignore_comments */ true);
    } catch (const json::parse_error &e) {
        throw ModError(std::string("mod definition is not valid JSON: ") + e.what());
    }
    if (!j.is_object())
        throw ModError("mod definition must be a JSON object");

    Mod mod;
    mod.id = required_string(j, "id", "");
    if (!is_identifier(mod.id))
        mod_fail(mod.id, "id must use only a-z, 0-9 and _ and not start with a digit");
    mod.name    = optional_string(j, "name");
    mod.version = required_string(j, "version", mod.id);

    if (auto it = j.find("params"); it != j.end()) {
        if (!it->is_object())
            mod_fail(mod.id, "\"params\" must be an object");
        for (auto &[name, def] : it->items()) {
            const std::string where = "mod '" + mod.id + "' param '" + name + "'";
            if (!is_identifier(name))
                throw ModError(where + ": name must use only a-z, 0-9 and _");
            if (!def.is_object())
                throw ModError(where + ": must be an object");
            ModParam p;
            p.name = name;
            p.type = required_string(def, "type", mod.id);
            if (p.type != "float" && p.type != "int" && p.type != "bool" && p.type != "string")
                throw ModError(where + ": type must be float, int, bool or string");
            p.default_value = param_default(def, p.type, where);
            if (std::string scope = optional_string(def, "scope"); !scope.empty())
                p.scope = scope;
            if (p.scope != "process" && p.scope != "printer" && p.scope != "filament")
                throw ModError(where + ": scope must be process, printer or filament");
            p.label         = optional_string(def, "label");
            p.unit          = optional_string(def, "unit");
            p.page          = optional_string(def, "page");
            if (auto inv = def.find("invalidates"); inv != def.end()) {
                if (!inv->is_array())
                    throw ModError(where + ": \"invalidates\" must be a list of step names");
                for (const auto &step : *inv)
                    p.invalidates.push_back(step.get<std::string>());
            }
            mod.params.push_back(std::move(p));
        }
    }

    if (auto it = j.find("rules"); it != j.end()) {
        if (!it->is_array())
            mod_fail(mod.id, "\"rules\" must be a list");
        for (size_t i = 0; i < it->size(); ++i) {
            const json       &r     = (*it)[i];
            const std::string where = "mod '" + mod.id + "' rule " + std::to_string(i + 1);
            if (!r.is_object())
                throw ModError(where + ": must be an object");
            ModRule rule;
            rule.mod_id = mod.id;
            rule.target = required_string(r, "target", mod.id);
            rule.value  = parse_expr(required_string(r, "value", mod.id), where + " value");
            if (std::string when = optional_string(r, "when"); !when.empty())
                rule.when = parse_expr(when, where + " when");
            if (auto p = r.find("priority"); p != r.end()) {
                if (!p->is_number_integer())
                    throw ModError(where + ": \"priority\" must be a whole number");
                rule.priority = p->get<int>();
            }
            if (auto idx = r.find("index"); idx != r.end()) {
                if (!idx->is_number_unsigned())
                    throw ModError(where + ": \"index\" must be a whole number >= 0");
                rule.index = idx->get<size_t>();
            }
            mod.rules.push_back(std::move(rule));
        }
    }
    return mod;
}

ModSet ModSet::build(std::vector<Mod> mods)
{
    ModSet set;
    set.m_mods = std::move(mods);

    std::set<std::string> ids;
    for (size_t m = 0; m < set.m_mods.size(); ++m) {
        const Mod &mod = set.m_mods[m];
        if (!ids.insert(mod.id).second)
            mod_fail(mod.id, "two mods have this id");
        for (size_t p = 0; p < mod.params.size(); ++p)
            set.m_params[mod.id + "." + mod.params[p].name] = {m, p};
    }

    // Check every rule, and group the rules by the setting they write.
    std::map<std::string, std::vector<Ref>> by_target;
    for (size_t m = 0; m < set.m_mods.size(); ++m)
        for (size_t r = 0; r < set.m_mods[m].rules.size(); ++r) {
            const ModRule          &rule  = set.m_mods[m].rules[r];
            const std::string       where = "mod '" + rule.mod_id + "' rule " + std::to_string(r + 1);
            const ConfigOptionDef *def   = print_config_def.get(rule.target);
            if (def == nullptr)
                throw ModError(where + ": \"" + rule.target + "\" is not a setting");
            if (!is_supported_type(def->type, true))
                throw ModError(where + ": \"" + rule.target + "\" has a type rules cannot write");
            auto check_names = [&](const Expr &e) {
                for (const std::string &name : e.names())
                    if (set.m_params.count(name) == 0 && print_config_def.get(name) == nullptr)
                        throw ModError(where + ": \"" + name + "\" is neither a setting nor a mod parameter");
            };
            check_names(rule.value);
            if (rule.when)
                check_names(*rule.when);
            by_target[rule.target].push_back({m, r});
        }

    // Within one setting, rules apply in ascending priority; equal priorities have no defined order.
    for (auto &[target, refs] : by_target) {
        std::stable_sort(refs.begin(), refs.end(), [&set](const Ref &a, const Ref &b) {
            return set.m_mods[a.first].rules[a.second].priority < set.m_mods[b.first].rules[b.second].priority;
        });
        for (size_t i = 1; i < refs.size(); ++i) {
            const ModRule &a = set.m_mods[refs[i - 1].first].rules[refs[i - 1].second];
            const ModRule &b = set.m_mods[refs[i].first].rules[refs[i].second];
            if (a.priority == b.priority)
                throw ModError("rules of '" + a.mod_id + "' and '" + b.mod_id + "' both set \"" + target +
                               "\" with priority " + std::to_string(a.priority) + "; give them different priorities");
        }
    }

    // Order the settings so a rule runs after every rule that writes a setting it reads. A rule
    // reading its own setting reads the value from below it, which is not a dependency.
    std::map<std::string, std::set<std::string>> reads; // target -> other targets its rules read
    for (const auto &[target, refs] : by_target)
        for (const Ref &ref : refs) {
            const ModRule &rule = set.m_mods[ref.first].rules[ref.second];
            auto           add  = [&](const Expr &e) {
                for (const std::string &name : e.names())
                    if (name != target && by_target.count(name))
                        reads[target].insert(name);
            };
            add(rule.value);
            if (rule.when)
                add(*rule.when);
        }
    std::map<std::string, int> state; // 0 unvisited, 1 in progress, 2 done
    std::function<void(const std::string &, std::vector<std::string> &)> visit = [&](const std::string &t, std::vector<std::string> &path) {
        if (state[t] == 2)
            return;
        path.push_back(t);
        if (state[t] == 1) {
            std::string cycle;
            for (auto it = std::find(path.begin(), path.end(), t); it != path.end(); ++it)
                cycle += (cycle.empty() ? "" : " -> ") + *it;
            throw ModError("rules depend on each other in a cycle: " + cycle);
        }
        state[t] = 1;
        for (const std::string &dep : reads[t])
            visit(dep, path);
        state[t] = 2;
        path.pop_back();
        for (const Ref &ref : by_target[t])
            set.m_order.push_back(ref);
    };
    for (const auto &[target, refs] : by_target) {
        std::vector<std::string> path;
        visit(target, path);
    }
    return set;
}

ModSet ModSet::load_dir(const boost::filesystem::path &dir, std::vector<std::string> &errors)
{
    namespace fs = boost::filesystem;
    std::vector<fs::path> files;
    boost::system::error_code ec;
    if (!fs::is_directory(dir, ec))
        return {};
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (fs::is_regular_file(it->status()) && boost::iequals(it->path().extension().string(), ".json"))
            files.push_back(it->path());
    std::sort(files.begin(), files.end());

    std::vector<Mod> mods;
    for (const fs::path &file : files) {
        try {
            boost::nowide::ifstream in(file.string());
            std::stringstream       ss;
            ss << in.rdbuf();
            Mod mod = parse_mod(ss.str());
            // Check each mod against those already accepted, so one bad mod does not drop the rest.
            std::vector<Mod> trial = mods;
            trial.push_back(mod);
            build(std::move(trial));
            mods.push_back(std::move(mod));
        } catch (const std::exception &e) {
            errors.push_back(file.filename().string() + ": " + e.what());
        }
    }
    return build(std::move(mods));
}

std::vector<std::string> ModSet::resolve(DynamicPrintConfig &config, const std::map<std::string, ExprValue> &param_values) const
{
    std::vector<std::string> errors;
    if (m_order.empty())
        return errors;
    const std::set<std::string> user_changed = user_changed_keys(config);

    // Stored parameter values: one object for the process, one for the printer, one per filament.
    auto parse_values = [&errors](const std::string &key, const std::string &text) {
        json values = json::object();
        if (text.empty())
            return values;
        try {
            values = json::parse(text);
        } catch (const json::parse_error &e) {
            errors.push_back(key + " is not valid JSON: " + e.what());
            return json(json::object());
        }
        if (!values.is_object()) {
            errors.push_back(key + " must be a JSON object");
            return json(json::object());
        }
        return values;
    };
    auto string_of = [&config](const char *key) {
        const auto *opt = config.option<ConfigOptionString>(key);
        return opt ? opt->value : std::string();
    };
    const json process_values = parse_values("mod_settings", string_of("mod_settings"));
    const json printer_values = parse_values("printer_mod_settings", string_of("printer_mod_settings"));
    std::vector<json> filament_values;
    if (const auto *opt = config.option<ConfigOptionStrings>("filament_mod_settings"))
        for (const std::string &text : opt->values)
            filament_values.push_back(parse_values("filament_mod_settings", text));

    // A stored value of the wrong type is an error, never silently replaced by the default.
    auto stored_value = [&errors](const ModParam &p, const std::string &name, const json &values) -> std::optional<ExprValue> {
        auto it = values.find(name);
        if (it == values.end())
            return p.default_value;
        if ((p.type == "float" && it->is_number()) || (p.type == "int" && it->is_number_integer()))
            return it->get<double>();
        if (p.type == "bool" && it->is_boolean())
            return it->get<bool>();
        if (p.type == "string" && it->is_string())
            return it->get<std::string>();
        errors.push_back("stored value of mod parameter " + name + " is not a " + p.type);
        return std::nullopt;
    };

    const ExprLookup lookup = [&](const std::string &name, std::optional<size_t> index) -> std::optional<ExprValue> {
        if (auto p = m_params.find(name); p != m_params.end()) {
            const ModParam &param = m_mods[p->second.first].params[p->second.second];
            // A filament parameter is read per filament, with an index; the others without.
            if ((param.scope == "filament") != index.has_value())
                return std::nullopt;
            if (auto v = param_values.find(name); v != param_values.end())
                return v->second;
            if (param.scope == "filament")
                return *index < filament_values.size() ? stored_value(param, name, filament_values[*index]) : std::nullopt;
            return stored_value(param, name, param.scope == "printer" ? printer_values : process_values);
        }
        const ConfigOptionDef *def = print_config_def.get(name);
        return def ? read_option(config.option(name), def->type, index) : std::nullopt;
    };

    for (const Ref &ref : m_order) {
        const ModRule &rule  = m_mods[ref.first].rules[ref.second];
        const std::string where = "mod '" + rule.mod_id + "' rule " + std::to_string(ref.second + 1) + " (" + rule.target + ")";
        if (user_changed.count(rule.target))
            continue;
        ConfigOption *opt = config.option(rule.target);
        if (opt == nullptr)
            continue;
        try {
            if (rule.when && !expr_truthy(rule.when->evaluate(lookup)))
                continue;
            write_option(opt, print_config_def.get(rule.target)->type, rule.value.evaluate(lookup), rule.index, where);
        } catch (const ExprError &e) {
            errors.push_back(where + ": " + e.what());
        } catch (const ModError &e) {
            errors.push_back(e.what());
        }
    }
    return errors;
}

std::shared_ptr<const ModSet> active_mods()
{
    std::lock_guard<std::mutex> lock(g_active_mutex);
    return g_active;
}

void set_active_mods(std::shared_ptr<const ModSet> mods)
{
    std::lock_guard<std::mutex> lock(g_active_mutex);
    g_active = std::move(mods);
}

void load_active_mods_from_data_dir()
{
    if (data_dir().empty())
        return;
    const boost::filesystem::path dir = boost::filesystem::path(data_dir()) / "mods";
    std::vector<std::string>      errors;
    auto                          mods = std::make_shared<const ModSet>(ModSet::load_dir(dir, errors));
    for (const std::string &e : errors)
        BOOST_LOG_TRIVIAL(error) << "Mod not loaded: " << e;
    if (!mods->mods().empty())
        BOOST_LOG_TRIVIAL(info) << "Loaded " << mods->mods().size() << " mod(s) from " << dir.string();
    set_active_mods(std::move(mods));
}

} // namespace ModHost
} // namespace Slic3r
