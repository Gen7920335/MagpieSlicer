#include "ModParamsPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ModHost/SupportMenu.hpp"

#include <nlohmann/json.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

std::string stored_text(const DynamicPrintConfig &config, const std::string &scope)
{
    const char *key = ModParamsPanel::config_key(scope);
    if (scope == "filament") {
        // A filament tab edits one filament preset, whose list holds a single entry.
        const auto *opt = config.option<ConfigOptionStrings>(key);
        return opt && !opt->values.empty() ? opt->values.front() : std::string();
    }
    const auto *opt = config.option<ConfigOptionString>(key);
    return opt ? opt->value : std::string();
}

json parse_object(const std::string &text)
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

// The preset tab that stores a parameter's value: object and region parameters keep their process
// value (the one objects and parts inherit) with the process preset.
const std::string &tab_scope(const ModHost::ModParam &p)
{
    static const std::string process = "process";
    return p.scope == "object" || p.scope == "region" ? process : p.scope;
}

wxString display_value(const json &stored, const ModHost::ModParam &p, const std::string &key)
{
    auto it = stored.find(key);
    if (p.type == "float_or_percent" && it != stored.end() && it->is_string())
        return wxString::FromUTF8(it->get<std::string>());
    if (p.type == "float_or_percent" && it == stored.end())
        if (const std::string *s = std::get_if<std::string>(&p.default_value))
            return wxString::FromUTF8(*s);
    if (p.type == "string")
        return wxString::FromUTF8(it != stored.end() && it->is_string() ? it->get<std::string>() : std::get<std::string>(p.default_value));
    // A float_or_percent parameter can default to a percentage and hold a number.
    double v = 0.;
    if (it != stored.end() && it->is_number())
        v = it->get<double>();
    else if (const double *d = std::get_if<double>(&p.default_value))
        v = *d;
    else if (const std::string *s = std::get_if<std::string>(&p.default_value))
        return wxString::FromUTF8(*s);
    // All the digits the value has: commit() writes every row back from its text.
    return p.type == "int" ? wxString::Format("%lld", (long long) std::llround(v)) : wxString::FromUTF8(ModHost::format_decimal(v));
}

} // namespace

ModParamsPanel::ModParamsPanel(wxWindow *parent, std::shared_ptr<const ModHost::ModSet> mods, const std::string &scope)
    : wxPanel(parent), m_mods(std::move(mods)), m_scope(scope)
{
    auto *grid = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
    grid->AddGrowableCol(1);
    for (const ModHost::Mod &mod : m_mods->mods())
        for (const ModHost::ModParam &p : mod.params) {
            if (tab_scope(p) != m_scope || ModHost::is_support_menu_param(mod, p.name))
                continue;
            const std::string key   = mod.id + "." + p.name;
            const std::string language = into_u8(wxGetApp().current_language_code_safe());
            const wxString    label = wxString::FromUTF8(ModHost::localized_text(p.labels, language, p.label.empty() ? key : p.label));
            auto *text = new wxStaticText(this, wxID_ANY, label);
            text->SetToolTip(wxString::FromUTF8(key + "  (" + ModHost::localized_text(mod.names, language, mod.name.empty() ? mod.id : mod.name) + " " + mod.version + ")"));
            // Long labels wrap; otherwise the widest one pushed every value off the side of the sidebar.
            text->Wrap(FromDIP(170));
            grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);

            wxWindow *control = nullptr;
            std::vector<std::string> choice_values;
            if (!p.options.empty() || !p.choices_from.empty()) {
                wxArrayString choices;
                for (const ModHost::ModParamOption &o : p.options) {
                    choices.Add(wxString::FromUTF8(ModHost::localized_text(o.labels, language, o.label)));
                    choice_values.push_back(o.value);
                }
                // MAGPIE: the implementations the loaded mods register for the slot, shown by their mods' names, so a
                // generator a mod installed later adds is listed (Mixed support's normal support generator).
                if (!p.choices_from.empty())
                    for (const std::string &value : ModHost::support_choices(m_mods.get(), p.choices_from)) {
                        const ModHost::Mod *provider = ModHost::slot_provider(m_mods.get(), p.choices_from, value);
                        choices.Add(provider == nullptr ? _L("Orca") :
                                    wxString::FromUTF8(ModHost::localized_text(provider->names, language, provider->name.empty() ? provider->id : provider->name)));
                        choice_values.push_back(value);
                    }
                auto *choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(100), -1), choices);
                choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { commit(); });
                control = choice;
            } else if (p.type == "bool") {
                auto *cb = new wxCheckBox(this, wxID_ANY, wxEmptyString);
                cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { commit(); });
                control = cb;
            } else {
                auto *tc = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(100), -1), wxTE_PROCESS_ENTER);
                tc->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { commit(); });
                tc->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { commit(); e.Skip(); });
                control = tc;
            }
            grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
            auto *unit = new wxStaticText(this, wxID_ANY, wxString::FromUTF8(p.unit));
            grid->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            m_rows.push_back({key, &p, control, {text, control, unit}, std::move(choice_values)});
        }
    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(outer);
}

bool ModParamsPanel::has_params(const ModHost::ModSet *mods, const std::string &scope)
{
    if (mods == nullptr)
        return false;
    for (const ModHost::Mod &mod : mods->mods())
        for (const ModHost::ModParam &p : mod.params)
            if (tab_scope(p) == scope && !ModHost::is_support_menu_param(mod, p.name))
                return true;
    return false;
}

const char *ModParamsPanel::config_key(const std::string &scope)
{
    if (scope == "printer")
        return "printer_mod_settings";
    if (scope == "filament")
        return "filament_mod_settings";
    return "mod_settings";
}

void ModParamsPanel::load_from_config(const DynamicPrintConfig &config)
{
    m_loaded_json     = stored_text(config, m_scope);
    const json stored = parse_object(m_loaded_json);
    m_loading         = true;
    for (Row &row : m_rows) {
        if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
            auto        it    = stored.find(row.key);
            std::string value = it != stored.end() && it->is_string() ? it->get<std::string>() : std::get<std::string>(row.param->default_value);
            const auto &opts  = row.choice_values;
            auto        found = std::find(opts.begin(), opts.end(), value);
            // A stored value outside the options (slicing reports it) shows as no selection.
            choice->SetSelection(found == opts.end() ? wxNOT_FOUND : int(found - opts.begin()));
        } else if (auto *cb = dynamic_cast<wxCheckBox *>(row.control)) {
            auto it = stored.find(row.key);
            cb->SetValue(it != stored.end() && it->is_boolean() ? it->get<bool>() : std::get<bool>(row.param->default_value));
        } else if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
            tc->ChangeValue(display_value(stored, *row.param, row.key));
        }
    }
    m_loading = false;
    update_visibility();
}

void ModParamsPanel::commit()
{
    if (m_loading)
        return;
    json stored = parse_object(m_loaded_json);
    for (Row &row : m_rows) {
        const ModHost::ModParam &p = *row.param;
        json                     value;
        if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
            const int sel = choice->GetSelection();
            if (sel == wxNOT_FOUND)
                continue; // nothing chosen: keep what is stored
            value = row.choice_values[size_t(sel)];
        } else if (auto *cb = dynamic_cast<wxCheckBox *>(row.control)) {
            value = cb->GetValue();
        } else if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
            const std::string text = tc->GetValue().ToUTF8().data();
            double            percent = 0.;
            if (p.type == "string") {
                value = text;
            } else if (p.type == "float_or_percent" && !text.empty() && text.back() == '%' &&
                       wxString::FromUTF8(text.substr(0, text.size() - 1)).ToCDouble(&percent)) {
                // "50%": a percentage of the setting the mod's rule writes it to.
                value = text;
            } else {
                double v = 0.;
                if (!wxString::FromUTF8(text).ToCDouble(&v) || (p.type == "int" && v != std::floor(v))) {
                    // Not a number of the parameter's type: show the stored value again.
                    tc->ChangeValue(display_value(stored, p, row.key));
                    continue;
                }
                value = p.type == "int" ? json(static_cast<long long>(v)) : json(v);
            }
        }
        // A value equal to the default is not stored, so the preset stays as small as it can be.
        const bool is_default = std::visit([&value](const auto &d) { return value == json(d); }, p.default_value);
        if (is_default)
            stored.erase(row.key);
        else
            stored[row.key] = value;
    }
    const std::string text = stored.empty() ? std::string() : stored.dump();
    update_visibility();
    if (text == m_loaded_json)
        return;
    m_loaded_json = text;
    if (on_change)
        on_change(text);
}

std::optional<ModHost::ExprValue> ModParamsPanel::row_value(const Row &row) const
{
    if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
        const int sel = choice->GetSelection();
        return sel == wxNOT_FOUND ? std::nullopt : std::optional<ModHost::ExprValue>(row.choice_values[size_t(sel)]);
    }
    if (auto *cb = dynamic_cast<wxCheckBox *>(row.control))
        return ModHost::ExprValue(cb->GetValue());
    if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
        if (row.param->type == "string" || (row.param->type == "float_or_percent" && tc->GetValue().EndsWith("%")))
            return ModHost::ExprValue(std::string(tc->GetValue().ToUTF8().data()));
        double v = 0.;
        if (tc->GetValue().ToCDouble(&v))
            return ModHost::ExprValue(v);
    }
    return std::nullopt;
}

void ModParamsPanel::update_visibility()
{
    // This tab's rows read from their controls, so a condition follows an edit not yet committed;
    // parameters of other tabs come from the presets being edited there.
    std::optional<DynamicPrintConfig> others;
    const ModHost::ExprLookup lookup = [this, &others](const std::string &name, std::optional<size_t> index) -> std::optional<ModHost::ExprValue> {
        if (index)
            return std::nullopt; // filament parameters are per filament; a condition cannot pick one
        for (const Row &row : m_rows)
            if (row.key == name)
                return row_value(row);
        if (!others) {
            if (wxGetApp().preset_bundle == nullptr)
                return std::nullopt;
            others = wxGetApp().preset_bundle->full_config();
        }
        return ModHost::param_value(*others, name);
    };
    bool changed = false;
    for (const Row &row : m_rows) {
        if (!row.param->visible)
            continue;
        const bool show = ModHost::param_visible(*row.param, lookup);
        for (wxWindow *w : row.windows)
            if (w->IsShown() != show) {
                w->Show(show);
                changed = true;
            }
    }
    if (changed) {
        Layout();
        if (wxWindow *parent = GetParent())
            parent->Layout();
    }
}

} // namespace GUI
} // namespace Slic3r
