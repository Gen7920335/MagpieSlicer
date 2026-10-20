#include "ModObjectSettingsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <nlohmann/json.hpp>

#include <wx/button.h>
#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/scrolwin.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

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

std::string stored(const ModelConfigObject &config, const char *key)
{
    const auto *opt = config.get().option<ConfigOptionString>(key);
    return opt ? opt->value : std::string();
}

json to_json(const ModHost::ExprValue &v)
{
    if (const double *d = std::get_if<double>(&v))
        return *d;
    if (const bool *b = std::get_if<bool>(&v))
        return *b;
    return std::get<std::string>(v);
}

// The parameters a target shows, mod by mod, each mod's "gui"."object_params" order first.
std::vector<std::pair<const ModHost::Mod *, const ModHost::ModParam *>> shown_params(const ModHost::ModSet &mods, bool volume)
{
    std::vector<std::pair<const ModHost::Mod *, const ModHost::ModParam *>> out;
    for (const ModHost::Mod &mod : mods.mods()) {
        auto shows = [volume](const ModHost::ModParam &p) { return p.scope == "region" || (!volume && p.scope == "object"); };
        std::vector<const ModHost::ModParam *> params;
        for (const std::string &name : mod.gui.object_params)
            for (const ModHost::ModParam &p : mod.params)
                if (p.name == name && shows(p))
                    params.push_back(&p);
        if (mod.gui.object_params.empty())
            for (const ModHost::ModParam &p : mod.params)
                if (shows(p))
                    params.push_back(&p);
        for (const ModHost::ModParam *p : params)
            out.emplace_back(&mod, p);
    }
    return out;
}

} // namespace

bool ModObjectSettingsDialog::has_params(bool volume)
{
    std::shared_ptr<const ModHost::ModSet> mods = ModHost::active_mods();
    return mods && !shown_params(*mods, volume).empty();
}

ModObjectSettingsDialog::ModObjectSettingsDialog(wxWindow *parent, ModelObject &object, ModelVolume *volume)
    : wxDialog(parent, wxID_ANY, _L("Mod settings"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_mods(ModHost::active_mods())
    , m_object(object)
    , m_volume(volume)
{
    const int         em       = wxGetApp().em_unit();
    const std::string language = into_u8(wxGetApp().current_language_code_safe());
    auto             *top      = new wxBoxSizer(wxVERTICAL);
    const wxString    target   = from_u8(volume ? volume->name : object.name);
    // TRN %s: the name of an object, part or modifier.
    top->Add(new wxStaticText(this, wxID_ANY, wxString::Format(_L("Values for \"%s\". A value not set here is inherited."), target)), 0,
             wxALL, em);

    auto *scroll = new wxScrolledWindow(this, wxID_ANY, wxDefaultPosition, wxSize(60 * em, 30 * em));
    scroll->SetScrollRate(0, em);
    auto *grid = new wxFlexGridSizer(4, em / 2, em);
    grid->AddGrowableCol(1);
    if (m_mods)
        for (const auto &[mod, p] : shown_params(*m_mods, volume != nullptr)) {
            Row row;
            row.key      = mod->id + "." + p->name;
            row.param    = p;
            row.store    = p->scope == "object" ? "object_mod_settings" : "region_mod_settings";
            row.set_here = new wxCheckBox(scroll, wxID_ANY, _L("Set here"));
            auto *label  = new wxStaticText(scroll, wxID_ANY, from_u8(ModHost::localized_text(p->labels, language, p->label.empty() ? row.key : p->label)));
            label->SetToolTip(from_u8(row.key + "  (" + (mod->name.empty() ? mod->id : mod->name) + " " + mod->version + ")"));
            if (!p->options.empty()) {
                wxArrayString choices;
                for (const ModHost::ModParamOption &o : p->options)
                    choices.Add(from_u8(ModHost::localized_text(o.labels, language, o.label)));
                row.control = new wxChoice(scroll, wxID_ANY, wxDefaultPosition, wxSize(12 * em, -1), choices);
            } else if (p->type == "bool") {
                row.control = new wxCheckBox(scroll, wxID_ANY, wxEmptyString);
            } else {
                row.control = new wxTextCtrl(scroll, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(12 * em, -1));
            }
            grid->Add(row.set_here, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(label, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(row.control, 0, wxALIGN_CENTER_VERTICAL);
            grid->Add(new wxStaticText(scroll, wxID_ANY, from_u8(p->unit)), 0, wxALIGN_CENTER_VERTICAL);

            const ModelConfigObject &level = volume ? volume->config : object.config;
            const json               here  = parse_object(stored(level, row.store.c_str()));
            const auto               it    = here.find(row.key);
            const bool               set   = it != here.end();
            row.set_here->SetValue(set);
            row.control->Enable(set);
            std::string error;
            ModHost::ExprValue value = inherited_value(row);
            if (set) {
                // A stored value of the wrong type shows the inherited one; OK then writes a valid value.
                ModHost::ModSet::ValueSources sources;
                const std::string             text = it->dump();
                const std::string             one  = "{\"" + row.key + "\":" + text + "}";
                sources.object_values = &one;
                sources.region_values = &one;
                if (auto v = m_mods->value_of(row.key, sources, {}, &error))
                    value = *v;
            }
            show_value(row, value);
            row.set_here->Bind(wxEVT_CHECKBOX, [this, idx = m_rows.size()](wxCommandEvent &) {
                Row &r = m_rows[idx];
                r.control->Enable(r.set_here->GetValue());
                if (!r.set_here->GetValue())
                    show_value(r, inherited_value(r));
            });
            m_rows.push_back(row);
        }
    scroll->SetSizer(grid);
    top->Add(scroll, 1, wxEXPAND | wxLEFT | wxRIGHT, em);

    auto *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        if (e.GetId() == wxID_OK)
            apply();
        else
            e.Skip();
    });
    SetSizerAndFit(top);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
}

ModHost::ExprValue ModObjectSettingsDialog::inherited_value(const Row &row) const
{
    DynamicPrintConfig print;
    if (wxGetApp().preset_bundle != nullptr)
        print = wxGetApp().preset_bundle->full_config();
    ModHost::ModSet::ValueSources sources;
    sources.print = &print;
    // A part inherits the object's region value; the object inherits the process value.
    const std::string object_region = stored(m_object.config, "region_mod_settings");
    if (m_volume != nullptr && row.param->scope == "region")
        sources.object_values = &object_region;
    std::optional<ModHost::ExprValue> v = m_mods->value_of(row.key, sources);
    return v ? *v : row.param->default_value;
}

void ModObjectSettingsDialog::show_value(const Row &row, const ModHost::ExprValue &value)
{
    if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
        const std::string *s     = std::get_if<std::string>(&value);
        const auto        &opts  = row.param->options;
        auto               found = std::find_if(opts.begin(), opts.end(), [s](const ModHost::ModParamOption &o) { return s && o.value == *s; });
        choice->SetSelection(found == opts.end() ? wxNOT_FOUND : int(found - opts.begin()));
    } else if (auto *cb = dynamic_cast<wxCheckBox *>(row.control)) {
        cb->SetValue(ModHost::expr_truthy(value));
    } else if (auto *tc = dynamic_cast<wxTextCtrl *>(row.control)) {
        if (const double *d = std::get_if<double>(&value))
            tc->ChangeValue(row.param->type == "int" ? wxString::Format("%lld", (long long) std::llround(*d)) : wxString::FromCDouble(*d));
        else
            tc->ChangeValue(from_u8(ModHost::expr_to_string(value)));
    }
}

std::optional<ModHost::ExprValue> ModObjectSettingsDialog::control_value(const Row &row) const
{
    if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
        const int sel = choice->GetSelection();
        return sel == wxNOT_FOUND ? std::nullopt : std::optional<ModHost::ExprValue>(row.param->options[size_t(sel)].value);
    }
    if (auto *cb = dynamic_cast<wxCheckBox *>(row.control))
        return ModHost::ExprValue(cb->GetValue());
    auto *tc = dynamic_cast<wxTextCtrl *>(row.control);
    if (tc == nullptr)
        return std::nullopt;
    const std::string text = into_u8(tc->GetValue());
    if (row.param->type == "string")
        return ModHost::ExprValue(text);
    if (row.param->type == "float_or_percent" && !text.empty() && text.back() == '%') {
        double v = 0.;
        if (wxString::FromUTF8(text.substr(0, text.size() - 1)).ToCDouble(&v))
            return ModHost::ExprValue(text);
        return std::nullopt;
    }
    double v = 0.;
    if (!tc->GetValue().ToCDouble(&v) || (row.param->type == "int" && v != std::floor(v)))
        return std::nullopt;
    return ModHost::ExprValue(v);
}

void ModObjectSettingsDialog::apply()
{
    ModelConfigObject &level = m_volume ? m_volume->config : m_object.config;
    json object_values = parse_object(stored(level, "object_mod_settings"));
    json region_values = parse_object(stored(level, "region_mod_settings"));
    for (const Row &row : m_rows) {
        json &values = row.store == "object_mod_settings" ? object_values : region_values;
        if (!row.set_here->GetValue()) {
            values.erase(row.key);
            continue;
        }
        std::optional<ModHost::ExprValue> v = control_value(row);
        if (!v) {
            // TRN %s: a mod parameter such as "cura_support.join_distance".
            MessageDialog(this, wxString::Format(_L("The value of %s is not a valid %s."), from_u8(row.key), from_u8(row.param->type)),
                          _L("Mod settings"), wxOK | wxICON_WARNING).ShowModal();
            return;
        }
        values[row.key] = to_json(*v);
    }
    auto text = [](const json &j) { return j.empty() ? std::string() : j.dump(); };
    const bool object_changed = text(object_values) != stored(level, "object_mod_settings");
    const bool region_changed = text(region_values) != stored(level, "region_mod_settings");
    if (object_changed || region_changed) {
        Plater::TakeSnapshot snapshot(wxGetApp().plater(), _u8L("Mod settings"));
        auto put = [&level](const char *key, const std::string &value) {
            if (value.empty())
                level.erase(key);
            else
                level.set_key_value(key, new ConfigOptionString(value));
        };
        if (object_changed)
            put("object_mod_settings", text(object_values));
        if (region_changed)
            put("region_mod_settings", text(region_values));
        const auto &objects = wxGetApp().plater()->model().objects;
        const auto  it      = std::find(objects.begin(), objects.end(), &m_object);
        if (it != objects.end())
            wxGetApp().obj_list()->changed_object(int(it - objects.begin()));
    }
    EndModal(wxID_OK);
}

} // namespace GUI
} // namespace Slic3r
