#include "ModSet.hpp"

#include "libslic3r/I18N.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/log/trivial.hpp>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <mutex>
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

// "50%", "12.5%", "-3%": a number followed by a percent sign, nothing else.
bool is_percent_text(const std::string &s)
{
    if (s.size() < 2 || s.back() != '%')
        return false;
    const std::string number = s.substr(0, s.size() - 1);
    const char       *end    = nullptr;
    parse_decimal(number.c_str(), &end);
    return end != number.c_str() && *end == '\0' && number.find_first_of(" \t") == std::string::npos;
}

// A stored value of `p` read from JSON: the value, or nothing when it has the wrong type or is not
// one of a dropdown's options (`error` then says which).
std::optional<ExprValue> json_param_value(const ModParam &p, const std::string &key, const json &v, std::string *error)
{
    if ((p.type == "float" && v.is_number()) || (p.type == "int" && v.is_number_integer()))
        return v.get<double>();
    if (p.type == "bool" && v.is_boolean())
        return v.get<bool>();
    if (p.type == "float_or_percent") {
        if (v.is_number())
            return v.get<double>();
        if (v.is_string() && is_percent_text(v.get<std::string>()))
            return v.get<std::string>();
    }
    if (p.type == "string" && v.is_string()) {
        const std::string value = v.get<std::string>();
        if (param_allows(p, value))
            return value;
        if (error)
            *error = "stored value \"" + value + "\" of mod parameter " + key + " is not one of its options";
        return std::nullopt;
    }
    if (error)
        *error = "stored value of mod parameter " + key + " is not a " + p.type;
    return std::nullopt;
}

// A stored-values object ({"<id>.<name>": value}); text that is not a JSON object is empty.
json parse_values_object(const std::string &text)
{
    if (text.empty())
        return json::object();
    try {
        json j = json::parse(text);
        if (j.is_object())
            return j;
    } catch (const json::parse_error &) {
    }
    return json::object();
}

std::vector<ModGuiItem> parse_gui_items(const json &gui, const char *list, const std::string &where, bool needs_color)
{
    std::vector<ModGuiItem> items;
    auto it = gui.find(list);
    if (it == gui.end())
        return items;
    if (!it->is_array())
        throw ModError(where + ": \"gui\"." + list + " must be a list");
    std::set<std::string> ids;
    for (const json &j : *it) {
        if (!j.is_object())
            throw ModError(where + ": each \"gui\"." + list + " entry must be an object");
        ModGuiItem item;
        item.id     = optional_string(j, "id");
        item.label  = optional_string(j, "label");
        item.labels = {};
        for (auto &[key, value] : j.items())
            if (key.size() > 6 && key.compare(0, 6, "label_") == 0 && value.is_string())
                item.labels[key.substr(6)] = value.get<std::string>();
        item.description = optional_string(j, "description");
        for (auto &[key, value] : j.items())
            if (key.size() > 12 && key.compare(0, 12, "description_") == 0 && value.is_string())
                item.descriptions[key.substr(12)] = value.get<std::string>();
        item.color = optional_string(j, "color");
        item.unit  = optional_string(j, "unit");
        if (!is_identifier(item.id))
            throw ModError(where + ": \"gui\"." + list + " entry needs an \"id\" of a-z, 0-9 and _");
        if (!ids.insert(item.id).second)
            throw ModError(where + ": \"gui\"." + list + " lists \"" + item.id + "\" twice");
        if (needs_color && !std::regex_match(item.color, std::regex("#[0-9A-Fa-f]{6}")))
            throw ModError(where + ": \"gui\"." + list + " \"" + item.id + "\" needs a \"color\" like \"#22AA44\"");
        items.push_back(std::move(item));
    }
    return items;
}

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
    if (type == "float_or_percent") {
        if (it->is_number())
            return it->get<double>();
        if (it->is_string() && is_percent_text(it->get<std::string>()))
            return it->get<std::string>();
    }
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

// "<prefix><language>": "<text>" entries of a JSON object, by language.
static std::map<std::string, std::string> translations(const nlohmann::json &j, const std::string &prefix)
{
    std::map<std::string, std::string> out;
    for (auto &[key, value] : j.items())
        if (key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0 && value.is_string())
            out[key.substr(prefix.size())] = value.get<std::string>();
    return out;
}

namespace {

// An optional "visible" condition of a printer panel item.
std::optional<Expr> printer_item_condition(const json &j, const std::string &where)
{
    if (std::string text = optional_string(j, "visible"); !text.empty())
        return parse_expr(text, where + " visible");
    return std::nullopt;
}

// Each entry of the list `key` of the printer panel `printer`, with a unique identifier "id".
template<class Fn> void for_each_printer_item(const json &printer, const char *key, const std::string &where, Fn &&fn)
{
    auto it = printer.find(key);
    if (it == printer.end())
        return;
    if (!it->is_array())
        throw ModError(where + ": \"gui\".printer." + key + " must be a list");
    std::set<std::string> ids;
    for (const json &j : *it) {
        if (!j.is_object())
            throw ModError(where + ": each \"gui\".printer." + key + " entry must be an object");
        const std::string id = optional_string(j, "id");
        if (!is_identifier(id))
            throw ModError(where + ": \"gui\".printer." + key + " entry needs an \"id\" of a-z, 0-9 and _");
        if (!ids.insert(id).second)
            throw ModError(where + ": \"gui\".printer." + key + " lists \"" + id + "\" twice");
        fn(j, id, where + " printer " + key + " \"" + id + "\"");
    }
}

// A path inside the mod's folder: relative, forward slashes, no "..".
bool is_package_relative_path(const std::string &path)
{
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string::npos || path.find(':') != std::string::npos)
        return false;
    std::vector<std::string> parts;
    boost::split(parts, path, [](char c) { return c == '/'; });
    return std::none_of(parts.begin(), parts.end(), [](const std::string &p) { return p.empty() || p == "." || p == ".."; });
}

ModGuiPrinter parse_gui_printer(const json &printer, const std::string &where)
{
    if (!printer.is_object())
        throw ModError(where + ": \"gui\".printer must be an object");
    ModGuiPrinter out;
    for_each_printer_item(printer, "fields", where, [&](const json &j, const std::string &id, const std::string &w) {
        ModGuiFormField f;
        f.id     = id;
        f.kind   = j.contains("type") ? optional_string(j, "type") : std::string("text");
        f.label  = optional_string(j, "label");
        f.labels = translations(j, "label_");
        f.help   = optional_string(j, "help");
        f.helps  = translations(j, "help_");
        f.default_value = optional_string(j, "default");
        f.visible       = printer_item_condition(j, w);
        if (f.kind != "text" && f.kind != "secret" && f.kind != "choice" && f.kind != "path")
            throw ModError(w + ": \"type\" must be text, secret, choice or path");
        if (auto opts = j.find("options"); opts != j.end()) {
            if (f.kind != "choice" || !opts->is_array())
                throw ModError(w + ": \"options\" is a list, for a choice field only");
            for (const json &o : *opts) {
                ModParamOption option;
                option.value  = o.is_string() ? o.get<std::string>() : o.is_object() ? optional_string(o, "value") : std::string();
                option.label  = o.is_object() ? optional_string(o, "label") : std::string();
                option.labels = o.is_object() ? translations(o, "label_") : std::map<std::string, std::string>();
                if (option.value.empty())
                    throw ModError(w + ": each option must be a non-empty string or an object with a \"value\"");
                if (option.label.empty())
                    option.label = option.value;
                f.options.push_back(std::move(option));
            }
        }
        if (f.kind == "choice" && f.options.empty())
            throw ModError(w + ": a choice field needs \"options\"");
        // A secret never has a default: it would be a secret written into the manifest.
        if (f.kind == "secret" && !f.default_value.empty())
            throw ModError(w + ": a secret field cannot have a \"default\"");
        out.fields.push_back(std::move(f));
    });
    for_each_printer_item(printer, "actions", where, [&](const json &j, const std::string &id, const std::string &w) {
        ModGuiAction a;
        a.id       = id;
        a.label    = optional_string(j, "label");
        a.labels   = translations(j, "label_");
        a.row      = optional_string(j, "row");
        a.command  = optional_string(j, "command");
        a.confirm  = optional_string(j, "confirm");
        a.confirms = translations(j, "confirm_");
        a.visible  = printer_item_condition(j, w);
        if (!is_identifier(a.command))
            throw ModError(w + ": \"command\" must be a command name of a-z, 0-9 and _");
        if (auto args = j.find("args"); args != j.end()) {
            if (!args->is_object())
                throw ModError(w + ": \"args\" must be an object");
            a.args_json = args->dump();
        }
        if (auto idle = j.find("idle_only"); idle != j.end()) {
            if (!idle->is_boolean())
                throw ModError(w + ": \"idle_only\" must be true or false");
            a.idle_only = idle->get<bool>();
        }
        out.actions.push_back(std::move(a));
    });
    for_each_printer_item(printer, "panels", where, [&](const json &j, const std::string &id, const std::string &w) {
        ModGuiWebPanel p;
        p.id      = id;
        p.label   = optional_string(j, "label");
        p.labels  = translations(j, "label_");
        p.page    = optional_string(j, "page");
        p.visible = printer_item_condition(j, w);
        if (!is_package_relative_path(p.page))
            throw ModError(w + ": \"page\" must be a relative path inside the mod folder");
        out.panels.push_back(std::move(p));
    });
    for_each_printer_item(printer, "notices", where, [&](const json &j, const std::string &id, const std::string &w) {
        ModGuiNotice n;
        n.id      = id;
        n.text    = optional_string(j, "text");
        n.texts   = translations(j, "text_");
        n.visible = printer_item_condition(j, w);
        if (n.text.empty())
            throw ModError(w + ": a notice needs a \"text\"");
        out.notices.push_back(std::move(n));
    });
    return out;
}

ModNetwork parse_network(const json &network, const std::string &where)
{
    if (!network.is_object())
        throw ModError(where + ": \"network\" must be an object");
    ModNetwork out;
    out.reason  = optional_string(network, "reason");
    out.reasons = translations(network, "reason_");
    if (out.reason.empty())
        throw ModError(where + ": \"network\" needs a \"reason\" the user reads before agreeing");
    auto it = network.find("endpoints");
    if (it == network.end() || !it->is_array() || it->empty())
        throw ModError(where + ": \"network\".endpoints must be a non-empty list");
    for (const json &e : *it) {
        if (!e.is_object())
            throw ModError(where + ": each \"network\" endpoint must be {\"host\", \"ports\"}");
        ModNetworkEndpoint ep;
        ep.host    = optional_string(e, "host");
        ep.purpose = optional_string(e, "purpose");
        // "lan", a host name, or "*." and a domain of at least two labels: never a bare "*".
        if (ep.host != "lan" && !std::regex_match(ep.host, std::regex("(\\*\\.)?[A-Za-z0-9-]+(\\.[A-Za-z0-9-]+)+")))
            throw ModError(where + ": network host \"" + ep.host + "\" must be \"lan\", a host name or \"*.<domain>\"");
        auto ports = e.find("ports");
        if (ports == e.end() || !ports->is_array() || ports->empty())
            throw ModError(where + ": network host \"" + ep.host + "\" needs a non-empty \"ports\" list");
        for (const json &p : *ports) {
            if (!p.is_number_unsigned() || p.get<uint64_t>() == 0 || p.get<uint64_t>() > 65535)
                throw ModError(where + ": network host \"" + ep.host + "\": ports are 1 to 65535");
            ep.ports.push_back(uint16_t(p.get<uint64_t>()));
        }
        out.endpoints.push_back(std::move(ep));
    }
    return out;
}

} // namespace

const std::string &localized_text(const std::map<std::string, std::string> &translations, const std::string &language,
                                  const std::string &fallback)
{
    if (auto it = translations.find(language); it != translations.end())
        return it->second;
    if (language.size() > 2)
        if (auto it = translations.find(language.substr(0, 2)); it != translations.end())
            return it->second;
    return fallback;
}

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
    mod.names   = translations(j, "name_");
    mod.version = required_string(j, "version", mod.id);
    mod.author  = optional_string(j, "author");

    if (auto it = j.find("params"); it != j.end()) {
        if (!it->is_object())
            mod_fail(mod.id, "\"params\" must be an object");
        // Parameters keep the order the file gives them, which is the order of their rows in the settings tab.
        std::vector<std::string>   names;
        const nlohmann::ordered_json ordered = nlohmann::ordered_json::parse(json_text, nullptr, true, true);
        for (auto &[name, def] : ordered.at("params").items())
            names.push_back(name);
        for (const std::string &name : names) {
            const json &def = (*it)[name];
            const std::string where = "mod '" + mod.id + "' param '" + name + "'";
            if (!is_identifier(name))
                throw ModError(where + ": name must use only a-z, 0-9 and _");
            if (!def.is_object())
                throw ModError(where + ": must be an object");
            ModParam p;
            p.name = name;
            p.type = required_string(def, "type", mod.id);
            if (p.type != "float" && p.type != "int" && p.type != "bool" && p.type != "string" && p.type != "float_or_percent")
                throw ModError(where + ": type must be float, int, bool, string or float_or_percent");
            p.default_value = param_default(def, p.type, where);
            if (std::string scope = optional_string(def, "scope"); !scope.empty())
                p.scope = scope;
            if (p.scope != "process" && p.scope != "printer" && p.scope != "filament" && p.scope != "object" && p.scope != "region")
                throw ModError(where + ": scope must be process, printer, filament, object or region");
            p.label         = optional_string(def, "label");
            p.labels        = translations(def, "label_");
            p.unit          = optional_string(def, "unit");
            if (auto ui = def.find("ui"); ui != def.end()) {
                if (!ui->is_object())
                    throw ModError(where + ": \"ui\" must be an object");
                p.ui.after       = optional_string(*ui, "after");
                p.ui.page        = optional_string(*ui, "page");
                p.ui.group       = optional_string(*ui, "group");
                p.ui.group_after = optional_string(*ui, "group_after");
                p.ui.widget      = optional_string(*ui, "widget");
                if (!p.ui.after.empty() && !p.ui.group.empty())
                    throw ModError(where + ": \"ui\" names a row \"after\" an option or a \"group\", not both");
                if (!p.ui.group.empty() && p.ui.page.empty())
                    throw ModError(where + ": a \"ui\" \"group\" needs the \"page\" it is on");
                if (!p.ui.widget.empty() && p.ui.widget != "toolhead" && p.ui.widget != "layer_ranges")
                    throw ModError(where + ": \"ui\" \"widget\" must be toolhead or layer_ranges");
                if (p.ui.widget == "toolhead" && p.type != "int")
                    throw ModError(where + ": the toolhead widget is for an int parameter");
                if (p.ui.widget == "layer_ranges" && p.type != "string")
                    throw ModError(where + ": the layer_ranges widget is for a string parameter");
            }
            if (auto inv = def.find("invalidates"); inv != def.end()) {
                if (!inv->is_array())
                    throw ModError(where + ": \"invalidates\" must be a list of step names");
                const std::vector<std::string> &known = mod_step_names();
                for (const auto &step : *inv) {
                    if (!step.is_string() || std::find(known.begin(), known.end(), step.get<std::string>()) == known.end())
                        throw ModError(where + ": \"invalidates\" names an unknown step " + step.dump() +
                                       " (one of posSlice, posPerimeters, posPrepareInfill, posInfill, posSupportMaterial, "
                                       "psWipeTower, psSkirtBrim, psGCodeExport, ... or none)");
                    p.invalidates.push_back(step.get<std::string>());
                }
                if (std::find(p.invalidates.begin(), p.invalidates.end(), "none") != p.invalidates.end()) {
                    if (p.invalidates.size() > 1)
                        throw ModError(where + ": \"none\" cannot be listed with other steps");
                    // Rules read the process value only, so an object or region value would change nothing
                    // through them: the mod's own code reads it and the steps it feeds must be named.
                    if (p.scope == "object" || p.scope == "region")
                        throw ModError(where + ": an " + p.scope + " parameter is read by the mod's code; name the steps it changes");
                }
            }
            if (auto opts = def.find("options"); opts != def.end()) {
                if (p.type != "string")
                    throw ModError(where + ": \"options\" is only for a string parameter");
                if (!opts->is_array() || opts->empty())
                    throw ModError(where + ": \"options\" must be a non-empty list");
                for (const auto &o : *opts) {
                    ModParamOption option;
                    if (o.is_string()) {
                        option.value = o.get<std::string>();
                    } else if (o.is_object()) {
                        option.value  = optional_string(o, "value");
                        option.label  = optional_string(o, "label");
                        option.labels = translations(o, "label_");
                    }
                    if (option.value.empty())
                        throw ModError(where + ": each option must be a non-empty string or an object with a \"value\"");
                    for (const ModParamOption &seen : p.options)
                        if (seen.value == option.value)
                            throw ModError(where + ": option \"" + option.value + "\" is listed twice");
                    if (option.label.empty())
                        option.label = option.value;
                    p.options.push_back(std::move(option));
                }
                if (!param_allows(p, p.default_value))
                    throw ModError(where + ": \"default\" is not one of the options");
            }
            if (std::string from = optional_string(def, "choices_from"); !from.empty()) {
                if (p.type != "string" || !p.options.empty())
                    throw ModError(where + ": \"choices_from\" is only for a string parameter without \"options\"");
                p.choices_from = from;
            }
            if (std::string visible = optional_string(def, "visible"); !visible.empty())
                p.visible = parse_expr(visible, where + " visible");
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

    // Package manifest fields.
    const std::string where = "mod '" + mod.id + "'";
    if (auto it = j.find("api"); it != j.end()) {
        if (!it->is_object() || !it->contains("major") || !(*it)["major"].is_number_unsigned())
            mod_fail(mod.id, "\"api\" must be {\"major\": <n>, \"min_minor\": <n>}");
        mod.api_major = (*it)["major"].get<int>();
        if (auto minor = it->find("min_minor"); minor != it->end()) {
            if (!minor->is_number_unsigned())
                mod_fail(mod.id, "\"api\".min_minor must be a whole number >= 0");
            mod.api_min_minor = minor->get<int>();
        }
    }
    mod.binary = optional_string(j, "binary");
    if (!mod.binary.empty() && !std::regex_match(mod.binary, std::regex("[A-Za-z0-9_][A-Za-z0-9_.-]*")))
        mod_fail(mod.id, "\"binary\" must be a plain file name without extension");
    if (!mod.binary.empty() && mod.api_major == 0)
        mod_fail(mod.id, "a mod with a \"binary\" must give the \"api\" it was built for");
    if (auto it = j.find("slots"); it != j.end()) {
        if (!it->is_array())
            mod_fail(mod.id, "\"slots\" must be a list");
        for (const json &s : *it) {
            ModSlotDecl decl;
            if (!s.is_object() || !s.contains("version") || !s["version"].is_number_unsigned())
                mod_fail(mod.id, "each slot must be {\"slot\", \"version\", \"impl\"}");
            decl.slot    = required_string(s, "slot", mod.id);
            decl.version = s["version"].get<uint32_t>();
            decl.impl    = required_string(s, "impl", mod.id);
            const std::string where_slot = where + " slot " + decl.slot;
            if (std::string when = optional_string(s, "when"); !when.empty())
                decl.when = parse_expr(when, where_slot + " when");
            if (auto p = s.find("priority"); p != s.end()) {
                if (!p->is_number_integer())
                    mod_fail(mod.id, "slot " + decl.slot + ": \"priority\" must be a whole number");
                decl.priority = p->get<int>();
            }
            if (auto st = s.find("settings"); st != s.end()) {
                if (!st->is_object())
                    mod_fail(mod.id, "slot " + decl.slot + ": \"settings\" must map names to expressions");
                for (auto &[name, text] : st->items()) {
                    if (!text.is_string())
                        mod_fail(mod.id, "slot " + decl.slot + ": setting \"" + name + "\" must be an expression");
                    decl.settings.emplace(name, parse_expr(text.get<std::string>(), where_slot + " setting " + name));
                }
            }
            if (auto m = s.find("menu"); m != s.end()) {
                // {"param": "<bool parameter of this mod>", "entries": [{"label", "label_<lang>", "support_type"}]}
                if (!boost::starts_with(decl.slot, "support."))
                    mod_fail(mod.id, "slot " + decl.slot + ": a \"menu\" is for the support slots");
                if (!m->is_object() || !m->contains("entries") || !(*m)["entries"].is_array() || (*m)["entries"].empty())
                    mod_fail(mod.id, "slot " + decl.slot + ": \"menu\" must be {\"param\", \"entries\": [...]}");
                decl.menu_param = required_string(*m, "param", mod.id);
                const auto param = std::find_if(mod.params.begin(), mod.params.end(), [&decl](const ModParam &p) { return p.name == decl.menu_param; });
                if (param == mod.params.end() || param->type != "bool" || param->scope != "process")
                    mod_fail(mod.id, "slot " + decl.slot + ": \"menu\" param \"" + decl.menu_param + "\" must be a process-scope bool parameter of the mod");
                const auto &types = ConfigOptionEnum<SupportType>::get_enum_values();
                for (const json &e : (*m)["entries"]) {
                    ModSupportMenuEntry entry;
                    if (!e.is_object())
                        mod_fail(mod.id, "slot " + decl.slot + ": each menu entry must be {\"label\", \"support_type\"}");
                    entry.label        = required_string(e, "label", mod.id);
                    entry.labels       = translations(e, "label_");
                    entry.support_type = required_string(e, "support_type", mod.id);
                    if (types.find(entry.support_type) == types.end())
                        mod_fail(mod.id, "slot " + decl.slot + ": menu entry \"" + entry.label + "\": unknown support_type \"" + entry.support_type + "\"");
                    decl.menu.push_back(std::move(entry));
                }
            }
            mod.slots.push_back(std::move(decl));
        }
        if (!mod.slots.empty() && mod.binary.empty())
            mod_fail(mod.id, "\"slots\" are filled by native code; the mod needs a \"binary\"");
    }
    if (auto it = j.find("depends"); it != j.end()) {
        if (!it->is_array())
            mod_fail(mod.id, "\"depends\" must be a list");
        for (const json &d : *it) {
            if (!d.is_object())
                mod_fail(mod.id, "each dependency must be {\"id\", \"min\"}");
            ModDependency dep{required_string(d, "id", mod.id), optional_string(d, "min")};
            if (dep.id == mod.id)
                mod_fail(mod.id, "a mod cannot depend on itself");
            mod.depends.push_back(std::move(dep));
        }
    }
    if (auto it = j.find("conflicts"); it != j.end()) {
        if (!it->is_array())
            mod_fail(mod.id, "\"conflicts\" must be a list of mod ids");
        for (const json &c : *it) {
            if (!c.is_string())
                mod_fail(mod.id, "\"conflicts\" must be a list of mod ids");
            mod.conflicts.push_back(c.get<std::string>());
        }
    }
    if (auto it = j.find("thread_safe"); it != j.end()) {
        if (!it->is_boolean())
            mod_fail(mod.id, "\"thread_safe\" must be true or false");
        mod.thread_safe = it->get<bool>();
    }
    if (auto it = j.find("legacy_keys"); it != j.end()) {
        if (!it->is_object())
            mod_fail(mod.id, "\"legacy_keys\" must map old setting keys to parameter names");
        auto has_param = [&mod](const std::string &name) {
            return std::any_of(mod.params.begin(), mod.params.end(), [&name](const ModParam &p) { return p.name == name; });
        };
        for (auto &[old_key, entry] : it->items()) {
            // "old_key": "param", or {"param": "param", "saved_default": "0"}, or {"point": "position"}.
            ModLegacyKey key;
            if (entry.is_string()) {
                key.param = entry.get<std::string>();
            } else if (entry.is_object()) {
                if (auto p = entry.find("point"); p != entry.end() && p->is_string()) {
                    key.param = p->get<std::string>();
                    key.point = true;
                } else
                    key.param = optional_string(entry, "param");
                if (auto d = entry.find("saved_default"); d != entry.end()) {
                    if (!d->is_string())
                        mod_fail(mod.id, "\"legacy_keys\": \"" + old_key + "\" saved_default must be the stored text");
                    key.saved_default = d->get<std::string>();
                }
            }
            const bool known = key.point ? has_param(key.param + "_x") && has_param(key.param + "_y") : has_param(key.param);
            if (key.param.empty() || !known)
                mod_fail(mod.id, "\"legacy_keys\": \"" + old_key + "\" must name a parameter of this mod" +
                                     (key.point ? std::string(" (a point: <name>_x and <name>_y)") : std::string()));
            mod.legacy_keys[old_key] = std::move(key);
        }
    }
    if (auto it = j.find("gui"); it != j.end()) {
        if (!it->is_object())
            mod_fail(mod.id, "\"gui\" must be an object");
        mod.gui.paint_channels = parse_gui_items(*it, "paint_channels", where, true);
        if (mod.gui.paint_channels.size() > 2)
            mod_fail(mod.id, "\"gui\".paint_channels: at most two channels");
        if (std::string when = optional_string(*it, "paint_when"); !when.empty())
            mod.gui.paint_when = parse_expr(when, where + " gui paint_when");
        mod.gui.aux_towers     = parse_gui_items(*it, "aux_towers", where, false);
        mod.gui.legends        = parse_gui_items(*it, "legends", where, false);
        mod.gui.calibrations   = parse_gui_items(*it, "calibrations", where, false);
        if (auto op = it->find("object_params"); op != it->end()) {
            if (!op->is_array())
                mod_fail(mod.id, "\"gui\".object_params must be a list of parameter names");
            for (const json &name : *op) {
                auto p = name.is_string() ? std::find_if(mod.params.begin(), mod.params.end(),
                                                         [&name](const ModParam &q) { return q.name == name.get<std::string>(); })
                                          : mod.params.end();
                if (p == mod.params.end() || (p->scope != "object" && p->scope != "region"))
                    mod_fail(mod.id, "\"gui\".object_params: " + name.dump() + " is not an object or region parameter of this mod");
                mod.gui.object_params.push_back(name.get<std::string>());
            }
        }
        if (auto printer = it->find("printer"); printer != it->end()) {
            mod.gui.printer = parse_gui_printer(*printer, where);
            const bool fills_printer_agent = std::any_of(mod.slots.begin(), mod.slots.end(),
                                                         [](const ModSlotDecl &d) { return d.slot == "printer.agent"; });
            if (!fills_printer_agent)
                mod_fail(mod.id, "\"gui\".printer is the panel of a printer.agent slot, which the mod does not declare");
        }
    }
    if (auto it = j.find("network"); it != j.end()) {
        mod.network = parse_network(*it, where);
        if (mod.binary.empty())
            mod_fail(mod.id, "\"network\" is reached by native code; the mod needs a \"binary\"");
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
    for (const Mod &mod : set.m_mods)
        for (const std::string &other : mod.conflicts)
            if (ids.count(other))
                mod_fail(mod.id, "conflicts with mod '" + other + "'; the two do not load together");

    // A visibility condition reads parameters only: the settings tab has no full print config.
    for (const Mod &mod : set.m_mods)
        for (const ModParam &p : mod.params)
            if (p.visible)
                for (const std::string &name : p.visible->names())
                    if (set.m_params.count(name) == 0)
                        mod_fail(mod.id, "param '" + p.name + "' visible: \"" + name + "\" is not a mod parameter");

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
            // G-code settings are open to every mod (Gen, 2026-10-22, replacing audit decision B): an unverified
            // native mod warns that it can change the G-code before it loads (ModPackage.hpp).
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

    // A slot declaration's condition and settings read settings and parameters. The core reads them for the picked
    // implementation only, so a parameter they read must name the steps it changes: "none" would leave the print
    // stale when it changes.
    for (const Mod &mod : set.m_mods)
        for (const ModSlotDecl &decl : mod.slots) {
            const std::string where = "mod '" + mod.id + "' slot " + decl.slot;
            auto check_names = [&](const Expr &e) {
                for (const std::string &name : e.names()) {
                    if (auto p = set.m_params.find(name); p != set.m_params.end()) {
                        const ModParam &param = set.m_mods[p->second.first].params[p->second.second];
                        if (param.invalidates.size() == 1 && param.invalidates.front() == "none")
                            throw ModError(where + ": it reads \"" + name + "\", whose \"invalidates\" is \"none\"; name the steps it changes");
                        if (param.scope == "filament")
                            throw ModError(where + ": \"" + name + "\" is a filament parameter, which has one value per filament");
                    } else if (print_config_def.get(name) == nullptr)
                        throw ModError(where + ": \"" + name + "\" is neither a setting nor a mod parameter");
                }
            };
            if (decl.when)
                check_names(*decl.when);
            for (const auto &[name, e] : decl.settings)
                check_names(e);
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
    return load_dirs({dir}, errors);
}

ModSet ModSet::load_dirs(const std::vector<boost::filesystem::path> &dirs, std::vector<std::string> &errors)
{
    namespace fs = boost::filesystem;
    std::vector<Mod> mods;
    for (const fs::path &dir : dirs) {
        std::vector<fs::path>     files;
        boost::system::error_code ec;
        if (!fs::is_directory(dir, ec))
            continue;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (fs::is_regular_file(it->status()) && boost::iequals(it->path().extension().string(), ".json"))
                files.push_back(it->path());
            // A package folder <id>/mod.json; folders being installed or replaced (<id>.pending) are not mods yet.
            else if (fs::is_directory(it->status()) && it->path().extension().empty() && fs::is_regular_file(it->path() / "mod.json"))
                files.push_back(it->path() / "mod.json");
        }
        std::sort(files.begin(), files.end());
        for (const fs::path &file : files) {
            try {
                boost::nowide::ifstream in(file.string());
                std::stringstream       ss;
                ss << in.rdbuf();
                Mod mod = parse_mod(ss.str());
                // These directories ship with the application (and tests load the bundled mods this way).
                mod.origin   = ModOrigin::Bundled;
                mod.official = mod.verified = true;
                if (file.filename() == "mod.json")
                    mod.dir = file.parent_path().string();
                read_message_catalogs(mod);
                // Check each mod against those already accepted, so one bad mod does not drop the rest.
                std::vector<Mod> trial = mods;
                trial.push_back(mod);
                build(std::move(trial));
                mods.push_back(std::move(mod));
            } catch (const std::exception &e) {
                errors.push_back(file.filename().string() + ": " + e.what());
            }
        }
    }
    return build(std::move(mods));
}

std::vector<std::string> ModSet::resolve(DynamicPrintConfig &config, const std::map<std::string, ExprValue> &param_values,
                                         std::vector<RuleClash> *clashes) const
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
        std::string error;
        std::optional<ExprValue> v = json_param_value(p, name, *it, &error);
        if (!v)
            errors.push_back(error);
        return v;
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
            // Object and region parameters read their process value here: rules apply to the whole print.
            return stored_value(param, name, param.scope == "printer" ? printer_values : process_values);
        }
        const ConfigOptionDef *def = print_config_def.get(name);
        return def ? read_option(config.option(name), def->type, index) : std::nullopt;
    };

    // Audit decision D: which mods set each setting, and to what, so a setting two mods want differently
    // (two support generators turned on) is reported instead of the higher priority winning in silence.
    struct Write { std::string mod_id; std::string value; };
    std::map<std::string, std::vector<Write>> writes; // "<target>" or "<target>[i]"
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
            const ExprValue value = rule.value.evaluate(lookup);
            write_option(opt, print_config_def.get(rule.target)->type, value, rule.index, where);
            writes[rule.index ? rule.target + "[" + std::to_string(*rule.index) + "]" : rule.target].push_back({rule.mod_id, expr_to_string(value)});
        } catch (const ExprError &e) {
            errors.push_back(where + ": " + e.what());
        } catch (const ModError &e) {
            errors.push_back(e.what());
        }
    }
    if (clashes != nullptr)
        for (const auto &[target, list] : writes) {
            // Rules apply in ascending priority, so the last write is the one that stays.
            const Write &used = list.back();
            std::vector<std::string> others;
            for (const Write &w : list)
                if (w.mod_id != used.mod_id && w.value != used.value &&
                    std::find(others.begin(), others.end(), w.mod_id) == others.end())
                    others.push_back(w.mod_id);
            if (!others.empty())
                clashes->push_back({target, used.mod_id, used.value, std::move(others)});
        }
    return errors;
}

const ModParam *ModSet::find_param(const std::string &key) const
{
    auto it = m_params.find(key);
    return it == m_params.end() ? nullptr : &m_mods[it->second.first].params[it->second.second];
}

const Mod *ModSet::find_mod(const std::string &id) const
{
    for (const Mod &mod : m_mods)
        if (mod.id == id)
            return &mod;
    return nullptr;
}

std::optional<ExprValue> ModSet::value_of(const std::string &key, const ValueSources &sources, std::optional<size_t> filament,
                                          std::string *error) const
{
    const ModParam *p = find_param(key);
    if (p == nullptr || (p->scope == "filament") != filament.has_value())
        return std::nullopt;
    // Looks `key` up in one stored-values text. Outer nothing: not stored there, look further. Inner
    // nothing: stored but unusable (the text is not a JSON object, or the value has the wrong type),
    // which never falls back to a less specific value or the default.
    auto from_text = [&](const std::string &text, const char *what) -> std::optional<std::optional<ExprValue>> {
        if (text.empty())
            return std::nullopt;
        json values;
        try {
            values = json::parse(text);
        } catch (const json::parse_error &) {
        }
        if (!values.is_object()) {
            if (error)
                *error = std::string(what) + " is not a JSON object";
            return std::optional<ExprValue>();
        }
        auto it = values.find(key);
        if (it == values.end())
            return std::nullopt;
        return json_param_value(*p, key, *it, error);
    };
    auto string_of = [&sources](const char *k) {
        const auto *opt = sources.print ? sources.print->option<ConfigOptionString>(k) : nullptr;
        return opt ? opt->value : std::string();
    };
    if (p->scope == "region" && sources.region_values)
        if (auto v = from_text(*sources.region_values, "region_mod_settings"))
            return *v;
    if ((p->scope == "region" || p->scope == "object") && sources.object_values)
        if (auto v = from_text(*sources.object_values, "object_mod_settings"))
            return *v;
    if (p->scope == "filament") {
        const auto *opt = sources.print ? sources.print->option<ConfigOptionStrings>("filament_mod_settings") : nullptr;
        if (opt == nullptr)
            return p->default_value;
        if (*filament >= opt->values.size())
            return std::nullopt;
        if (auto v = from_text(opt->values[*filament], "filament_mod_settings"))
            return *v;
        return p->default_value;
    }
    const char *store = p->scope == "printer" ? "printer_mod_settings" : "mod_settings";
    if (auto v = from_text(string_of(store), store))
        return *v;
    return p->default_value;
}

ExprLookup ModSet::slot_lookup(const ValueSources &sources) const
{
    return [this, &sources](const std::string &name, std::optional<size_t> index) -> std::optional<ExprValue> {
        if (m_params.count(name))
            return index ? std::nullopt : value_of(name, sources);
        const ConfigOptionDef *def = print_config_def.get(name);
        return def && sources.print ? read_option(sources.print->option(name), def->type, index) : std::nullopt;
    };
}

ModSet::SlotChoice ModSet::choose_slot(const std::vector<std::string> &slots, const ValueSources &sources) const
{
    SlotChoice choice;
    const ExprLookup lookup = slot_lookup(sources);
    // Every declaration whose condition holds, as (priority, mod, declaration).
    struct Held { int priority; const Mod *mod; const ModSlotDecl *decl; };
    std::vector<Held> held;
    for (const Mod &mod : m_mods)
        for (const ModSlotDecl &decl : mod.slots) {
            if (!decl.when || std::find(slots.begin(), slots.end(), decl.slot) == slots.end())
                continue;
            try {
                if (expr_truthy(decl.when->evaluate(lookup)))
                    held.push_back({decl.priority, &mod, &decl});
            } catch (const ExprError &e) {
                choice.error = "mod '" + mod.id + "' slot " + decl.slot + " when: " + e.what();
                return choice;
            }
        }
    if (held.empty())
        return choice;
    std::stable_sort(held.begin(), held.end(), [](const Held &a, const Held &b) { return a.priority > b.priority; });
    if (held.size() > 1 && held[1].priority == held[0].priority && held[1].mod != held[0].mod) {
        choice.error = "mods '" + held[0].mod->id + "' and '" + held[1].mod->id + "' are both turned on for " + held[0].decl->slot +
                       " with priority " + std::to_string(held[0].priority) + "; turn one off";
        return choice;
    }
    choice.mod  = held[0].mod;
    choice.decl = held[0].decl;
    for (size_t i = 1; i < held.size(); ++i)
        if (held[i].mod != choice.mod &&
            std::find(choice.other_mods.begin(), choice.other_mods.end(), held[i].mod->id) == choice.other_mods.end())
            choice.other_mods.push_back(held[i].mod->id);
    return choice;
}

std::optional<ExprValue> ModSet::slot_setting(const SlotChoice &choice, const std::string &name, const ValueSources &sources,
                                              std::string *error) const
{
    if (choice.decl == nullptr)
        return std::nullopt;
    auto it = choice.decl->settings.find(name);
    if (it == choice.decl->settings.end())
        return std::nullopt;
    try {
        return it->second.evaluate(slot_lookup(sources));
    } catch (const ExprError &e) {
        if (error)
            *error = "mod '" + choice.mod->id + "' slot " + choice.decl->slot + " setting " + name + ": " + e.what();
        return std::nullopt;
    }
}

std::set<std::string> ModSet::invalidated_steps(const std::string &old_json, const std::string &new_json) const
{
    std::set<std::string> steps;
    if (old_json == new_json)
        return steps;
    const json before = parse_values_object(old_json);
    const json after  = parse_values_object(new_json);
    std::set<std::string> keys;
    for (auto &[k, v] : before.items())
        keys.insert(k);
    for (auto &[k, v] : after.items())
        keys.insert(k);
    for (const std::string &key : keys) {
        auto b = before.find(key);
        auto a = after.find(key);
        if (b != before.end() && a != after.end() && *b == *a)
            continue;
        const ModParam *p = find_param(key);
        if (p == nullptr)
            continue; // no loaded mod reads it
        if (p->invalidates.empty())
            steps.insert("posSlice");
        for (const std::string &step : p->invalidates)
            if (step != "none")
                steps.insert(step);
    }
    return steps;
}

const std::vector<std::string> &mod_step_names()
{
    static const std::vector<std::string> names = {
        "posSlice", "posPerimeters", "posEstimateCurledExtrusions", "posPrepareInfill", "posInfill", "posIroning",
        "posContouring", "posSupportMaterial", "posSimplifyPath", "posSimplifySupportPath", "posDetectOverhangsForLift",
        "posSimplifyWall", "posSimplifyInfill", "psWipeTower", "psToolOrdering", "psSkirtBrim", "psGCodeExport", "none",
    };
    return names;
}

bool is_gcode_setting(const std::string &key)
{
    const ConfigOptionDef *def = print_config_def.get(key);
    if (def == nullptr || (def->type != coString && def->type != coStrings))
        return false;
    // post_process names programs run on the exported file: worse than G-code, the same rule.
    return boost::ends_with(key, "_gcode") || boost::ends_with(key, "_gcodes") || key == "post_process";
}

int compare_versions(const std::string &a, const std::string &b)
{
    auto parts = [](const std::string &v) {
        std::vector<long> out;
        std::vector<std::string> pieces;
        boost::split(pieces, v, [](char c) { return c == '.'; });
        for (const std::string &p : pieces)
            out.push_back(std::strtol(p.c_str(), nullptr, 10));
        return out;
    };
    std::vector<long> x = parts(a), y = parts(b);
    x.resize(std::max(x.size(), y.size()), 0);
    y.resize(x.size(), 0);
    return x < y ? -1 : (y < x ? 1 : 0);
}

std::string merge_mod_values(const std::string &base_json, const std::string &override_json)
{
    if (override_json.empty())
        return base_json;
    json merged = parse_values_object(base_json);
    // A named object: items() of a temporary would outlive it in the loop.
    const json overrides = parse_values_object(override_json);
    for (auto &[k, v] : overrides.items())
        merged[k] = v;
    return merged.empty() ? std::string() : merged.dump();
}

void read_message_catalogs(Mod &mod)
{
    mod.messages.clear();
    if (mod.dir.empty())
        return;
    const boost::filesystem::path   folder = boost::filesystem::path(mod.dir) / "i18n";
    boost::system::error_code       ec;
    if (!boost::filesystem::is_directory(folder, ec))
        return;
    for (boost::filesystem::directory_iterator it(folder, ec), end; !ec && it != end; it.increment(ec)) {
        const boost::filesystem::path &file = it->path();
        if (!boost::filesystem::is_regular_file(it->status()) || !boost::iequals(file.extension().string(), ".json"))
            continue;
        const std::string language = file.stem().string();
        boost::nowide::ifstream in(file.string());
        std::stringstream       ss;
        ss << in.rdbuf();
        json j;
        try {
            j = json::parse(ss.str(), nullptr, true, /* ignore_comments */ true);
        } catch (const json::parse_error &e) {
            mod_fail(mod.id, "i18n/" + file.filename().string() + ": " + e.what());
        }
        if (!j.is_object())
            mod_fail(mod.id, "i18n/" + file.filename().string() + " must be an object of English text -> translation");
        std::map<std::string, std::string> &catalog = mod.messages[language];
        for (auto &[text, translation] : j.items()) {
            if (!translation.is_string())
                mod_fail(mod.id, "i18n/" + file.filename().string() + ": the translation of \"" + text + "\" is not a string");
            catalog[text] = translation.get<std::string>();
        }
    }
}

static std::mutex  g_language_mutex;
static std::string g_message_language;

void set_message_language(const std::string &language)
{
    std::lock_guard<std::mutex> lock(g_language_mutex);
    g_message_language = language;
}

std::string message_language()
{
    std::lock_guard<std::mutex> lock(g_language_mutex);
    return g_message_language;
}

std::string mod_text(const std::string &mod_id, const std::string &text)
{
    if (const std::string language = message_language(); !language.empty())
        if (std::shared_ptr<const ModSet> mods = active_mods())
            if (const Mod *mod = mods->find_mod(mod_id)) {
                auto catalog = mod->messages.find(language);
                if (catalog == mod->messages.end() && language.size() > 2)
                    catalog = mod->messages.find(language.substr(0, 2));
                if (catalog != mod->messages.end())
                    if (auto it = catalog->second.find(text); it != catalog->second.end())
                        return it->second;
            }
    return I18N::translate(text);
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

static std::shared_ptr<const std::vector<Mod>> g_known;

std::shared_ptr<const std::vector<Mod>> known_mods()
{
    std::lock_guard<std::mutex> lock(g_active_mutex);
    return g_known;
}

void set_known_mods(std::shared_ptr<const std::vector<Mod>> mods)
{
    std::lock_guard<std::mutex> lock(g_active_mutex);
    g_known = std::move(mods);
}

std::optional<ExprValue> param_value(const DynamicPrintConfig &config, const std::string &key)
{
    std::shared_ptr<const ModSet> mods = active_mods();
    const ModParam *p = mods ? mods->find_param(key) : nullptr;
    if (p == nullptr || p->scope == "filament")
        return std::nullopt;
    ModSet::ValueSources sources;
    sources.print = &config;
    return mods->value_of(key, sources);
}

const Mod *active_paint_channel_mod(const ModSet &mods, const DynamicPrintConfig &config)
{
    const ExprLookup lookup = [&config](const std::string &name, std::optional<size_t>) { return param_value(config, name); };
    for (const Mod &mod : mods.mods()) {
        if (mod.gui.paint_channels.empty())
            continue;
        if (!mod.gui.paint_when)
            return &mod;
        try {
            if (expr_truthy(mod.gui.paint_when->evaluate(lookup)))
                return &mod;
        } catch (const ExprError &) {
        }
    }
    return nullptr;
}

bool param_visible(const ModParam &param, const ExprLookup &lookup)
{
    if (!param.visible)
        return true;
    try {
        return expr_truthy(param.visible->evaluate(lookup));
    } catch (const ExprError &) {
        return true;
    }
}

bool param_allows(const ModParam &param, const ExprValue &value)
{
    if (param.options.empty())
        return true;
    const std::string *s = std::get_if<std::string>(&value);
    return s != nullptr && std::any_of(param.options.begin(), param.options.end(),
                                       [s](const ModParamOption &o) { return o.value == *s; });
}

} // namespace ModHost
} // namespace Slic3r
