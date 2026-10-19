#include "ModParamsPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

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

wxString display_value(const json &stored, const ModHost::ModParam &p, const std::string &key)
{
    auto it = stored.find(key);
    if (p.type == "string")
        return wxString::FromUTF8(it != stored.end() && it->is_string() ? it->get<std::string>() : std::get<std::string>(p.default_value));
    double v = std::get<double>(p.default_value);
    if (it != stored.end() && it->is_number())
        v = it->get<double>();
    return p.type == "int" ? wxString::Format("%lld", (long long) std::llround(v)) : wxString::FromCDouble(v);
}

} // namespace

ModParamsPanel::ModParamsPanel(wxWindow *parent, std::shared_ptr<const ModHost::ModSet> mods, const std::string &scope)
    : wxPanel(parent), m_mods(std::move(mods)), m_scope(scope)
{
    auto *grid = new wxFlexGridSizer(3, FromDIP(4), FromDIP(8));
    grid->AddGrowableCol(1);
    for (const ModHost::Mod &mod : m_mods->mods())
        for (const ModHost::ModParam &p : mod.params) {
            if (p.scope != m_scope)
                continue;
            const std::string key   = mod.id + "." + p.name;
            const std::string language = into_u8(wxGetApp().current_language_code_safe());
            const wxString    label = wxString::FromUTF8(ModHost::localized_text(p.labels, language, p.label.empty() ? key : p.label));
            auto *text = new wxStaticText(this, wxID_ANY, label);
            text->SetToolTip(wxString::FromUTF8(key + "  (" + (mod.name.empty() ? mod.id : mod.name) + " " + mod.version + ")"));
            // Long labels wrap; otherwise the widest one pushed every value off the side of the sidebar.
            text->Wrap(FromDIP(170));
            grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);

            wxWindow *control = nullptr;
            if (p.type == "bool") {
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
            grid->Add(new wxStaticText(this, wxID_ANY, wxString::FromUTF8(p.unit)), 0, wxALIGN_CENTER_VERTICAL);
            m_rows.push_back({key, &p, control});
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
            if (p.scope == scope)
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
        if (auto *cb = dynamic_cast<wxCheckBox *>(row.control)) {
            auto it = stored.find(row.key);
            cb->SetValue(it != stored.end() && it->is_boolean() ? it->get<bool>() : std::get<bool>(row.param->default_value));
        } else if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
            tc->ChangeValue(display_value(stored, *row.param, row.key));
        }
    }
    m_loading = false;
}

void ModParamsPanel::commit()
{
    if (m_loading)
        return;
    json stored = parse_object(m_loaded_json);
    for (Row &row : m_rows) {
        const ModHost::ModParam &p = *row.param;
        json                     value;
        if (auto *cb = dynamic_cast<wxCheckBox *>(row.control)) {
            value = cb->GetValue();
        } else if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
            const std::string text = tc->GetValue().ToUTF8().data();
            if (p.type == "string") {
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
    if (text == m_loaded_json)
        return;
    m_loaded_json = text;
    if (on_change)
        on_change(text);
}

} // namespace GUI
} // namespace Slic3r
