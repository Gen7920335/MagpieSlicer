#include "ModParamsPanel.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ModLayerRangesEditor.hpp"
#include "wxExtensions.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/ModHost/SupportMenu.hpp"

#include <nlohmann/json.hpp>

#include <wx/checkbox.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#ifdef __WXMSW__
#include <wx/msw/wrapwin.h>
#endif

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;

namespace {

// A nozzle diameter in mm as the sidebar spells it: "0.4", "0.25", "0.15".
wxString nozzle_text(double mm)
{
    wxString text = wxString::FromCDouble(mm, 2);
    while (text.EndsWith("0") && !text.EndsWith(".0"))
        text.RemoveLast();
    return text;
}

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

// A dropdown as wide as its longest entry within [min_dip, max_dip] DIP, so it fits the sidebar; on Windows its open
// list is as wide as the longest entry, so every entry reads whole there.
void fit_choice_width(wxChoice *choice, int min_dip, int max_dip)
{
    choice->InvalidateBestSize();
    const int best = choice->GetBestSize().x;
    choice->SetMinSize(wxSize(std::clamp(best, choice->FromDIP(min_dip), choice->FromDIP(max_dip)), -1));
#ifdef __WXMSW__
    ::SendMessage(static_cast<HWND>(choice->GetHWND()), CB_SETDROPPEDWIDTH, WPARAM(best), 0);
#endif
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

ModParamsPanel::ModParamsPanel(wxWindow *parent, std::shared_ptr<const ModHost::ModSet> mods, const std::string &scope,
                               const std::vector<std::string> &keys, bool labels)
    : wxPanel(parent), m_mods(std::move(mods)), m_scope(scope)
{
    // The page's background, which the revert button's face shows.
    SetBackgroundColour(parent->GetBackgroundColour());
    // Label (with labels), revert button, control, unit. A row without labels sits in the value column beside Orca's
    // settings, so it packs tighter to keep its unit inside the sidebar.
    auto *grid = new wxFlexGridSizer(labels ? 4 : 3, FromDIP(4), FromDIP(labels ? 8 : 4));
    grid->AddGrowableCol(labels ? 2 : 1);
    for (const ModHost::Mod &mod : m_mods->mods())
        for (const ModHost::ModParam &p : mod.params) {
            const std::string key = mod.id + "." + p.name;
            if (std::find(keys.begin(), keys.end(), key) == keys.end())
                continue;
            const std::string language = into_u8(wxGetApp().current_language_code_safe());
            const wxString    label = wxString::FromUTF8(ModHost::localized_text(p.labels, language, p.label.empty() ? key : p.label));
            const wxString    tooltip = wxString::FromUTF8(key + "  (" + ModHost::localized_text(mod.names, language, mod.name.empty() ? mod.id : mod.name) + " " + mod.version + ")");
            wxStaticText     *text = nullptr;
            if (labels) {
                text = new wxStaticText(this, wxID_ANY, label);
                text->SetToolTip(tooltip);
                // Long labels wrap; otherwise the widest one pushed every value off the side of the sidebar.
                text->Wrap(FromDIP(150));
                grid->Add(text, 0, wxALIGN_CENTER_VERTICAL);
            }
            // Orca's white bullet, the revert arrow while the value differs from the saved preset (update_modified).
            auto *revert = new ScalableButton(this, wxID_ANY, "dot");
            revert->SetBackgroundColour(GetBackgroundColour());
            revert->Bind(wxEVT_BUTTON, [this, key](wxCommandEvent &) { this->revert(key); });
            grid->Add(revert, 0, wxALIGN_CENTER_VERTICAL);

            wxWindow *control = nullptr;
            std::vector<std::string> choice_values;
            if (p.ui.widget == "toolhead") {
                // MAGPIE: filled for the printer being edited when the values load (fill_toolhead_choice).
                auto *choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(140), -1));
                choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { commit(); });
                control = choice;
            } else if (p.ui.widget == "layer_ranges") {
                auto *editor = new ModLayerRangesEditor(this);
                editor->on_change = [this]() { commit(); };
                control = editor;
            } else if (!p.options.empty() || !p.choices_from.empty()) {
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
                auto *choice = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, choices);
                fit_choice_width(choice, 100, 140);
                choice->Bind(wxEVT_CHOICE, [this](wxCommandEvent &) { commit(); });
                control = choice;
            } else if (p.type == "bool") {
                auto *cb = new wxCheckBox(this, wxID_ANY, wxEmptyString);
                cb->Bind(wxEVT_CHECKBOX, [this](wxCommandEvent &) { commit(); });
                control = cb;
            } else {
                // Narrow enough that a row beside Orca's settings keeps its unit inside the sidebar.
                auto *tc = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(labels ? 72 : 60), -1), wxTE_PROCESS_ENTER);
                tc->Bind(wxEVT_TEXT_ENTER, [this](wxCommandEvent &) { commit(); });
                tc->Bind(wxEVT_KILL_FOCUS, [this](wxFocusEvent &e) { commit(); e.Skip(); });
                control = tc;
            }
            control->SetToolTip(tooltip);
            if (dynamic_cast<wxChoice *>(control) != nullptr || dynamic_cast<wxTextCtrl *>(control) != nullptr)
                scroll_page_on_wheel(control);
            grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
            // Orca's translation where it has the unit ("mm/s or %").
            auto *unit = new wxStaticText(this, wxID_ANY, p.unit.empty() ? wxString() : _(wxString::FromUTF8(p.unit)));
            grid->Add(unit, 0, wxALIGN_CENTER_VERTICAL);
            std::vector<wxWindow *> windows{revert, control, unit};
            if (text != nullptr)
                windows.push_back(text);
            Row row;
            row.key           = key;
            row.param         = &p;
            row.control       = control;
            row.windows       = std::move(windows);
            row.label         = text;
            row.revert        = revert;
            row.choice_values = std::move(choice_values);
            m_rows.push_back(std::move(row));
        }
    auto *outer = new wxBoxSizer(wxVERTICAL);
    outer->Add(grid, 0, wxEXPAND | wxALL, FromDIP(4));
    SetSizer(outer);
}

std::vector<std::string> ModParamsPanel::param_keys(const ModHost::ModSet *mods, const std::string &scope)
{
    std::vector<std::string> keys;
    if (mods == nullptr)
        return keys;
    for (const ModHost::Mod &mod : mods->mods())
        for (const ModHost::ModParam &p : mod.params)
            if (tab_scope(p) == scope && !ModHost::is_support_menu_param(mod, p.name))
                keys.push_back(mod.id + "." + p.name);
    return keys;
}

const ModHost::ModParam *ModParamsPanel::find_param(const ModHost::ModSet *mods, const std::string &key)
{
    if (mods == nullptr)
        return nullptr;
    for (const ModHost::Mod &mod : mods->mods())
        for (const ModHost::ModParam &p : mod.params)
            if (key == mod.id + "." + p.name)
                return &p;
    return nullptr;
}

wxString ModParamsPanel::param_label(const ModHost::ModSet *mods, const std::string &key)
{
    const ModHost::ModParam *p = find_param(mods, key);
    if (p == nullptr)
        return wxString::FromUTF8(key);
    const std::string language = into_u8(wxGetApp().current_language_code_safe());
    return wxString::FromUTF8(ModHost::localized_text(p->labels, language, p->label.empty() ? key : p->label));
}

void ModParamsPanel::fill_toolhead_choice(Row &row)
{
    auto *choice = dynamic_cast<wxChoice *>(row.control);
    if (choice == nullptr || wxGetApp().preset_bundle == nullptr)
        return;
    PresetBundle             &bundle   = *wxGetApp().preset_bundle;
    const DynamicPrintConfig &printer  = bundle.printers.get_edited_preset().config;
    const auto               *nozzles  = printer.option<ConfigOptionFloats>("nozzle_diameter");
    const size_t              count    = nozzles != nullptr ? nozzles->values.size() : 0;
    // The toolheads a filament of the project is printed with: on a Bambu printer the filament map's, else
    // filament n on toolhead n (the way the core maps them, printing_nozzle_index).
    std::vector<bool>         has_filament(count, false);
    const size_t              filaments = bundle.filament_presets.size();
    const auto               *map       = bundle.project_config.option<ConfigOptionInts>("filament_map");
    for (size_t f = 0; f < filaments; ++f) {
        int toolhead = int(f) + 1;
        if (bundle.is_bbl_vendor() && count > 1 && map != nullptr && f < map->values.size())
            toolhead = map->values[f];
        if (toolhead >= 1 && size_t(toolhead) <= count)
            has_filament[size_t(toolhead - 1)] = true;
    }
    choice->Clear();
    row.choice_values.clear();
    choice->Append(_L("Auto"));
    row.choice_values.push_back("0");
    for (size_t t = 0; t < count; ++t) {
        const wxString nozzle = nozzle_text(nozzles->values[t]);
        // TRN %1%: a toolhead number, %2%: its nozzle diameter in mm.
        wxString label = format_wxstr(_L("T%1% (%2% mm)"), t + 1, nozzle);
        if (!has_filament[t])
            // TRN Appended to a toolhead in the crisp corner toolhead list: no filament of the project is printed with it.
            label += " " + _L("- no filament");
        choice->Append(label);
        row.choice_values.push_back(std::to_string(t + 1));
    }
    fit_choice_width(choice, 140, 160);
}

std::string ModParamsPanel::stored_json(const DynamicPrintConfig &config, const std::string &scope)
{
    return stored_text(config, scope);
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
    load_text(stored_text(config, m_scope));
}

void ModParamsPanel::load_text(const std::string &text)
{
    m_loaded_json     = text;
    const json stored = parse_object(m_loaded_json);
    m_loading         = true;
    for (Row &row : m_rows) {
        if (row.param->ui.widget == "toolhead") {
            fill_toolhead_choice(row);
            auto        *choice = static_cast<wxChoice *>(row.control);
            auto         it     = stored.find(row.key);
            const double v      = it != stored.end() && it->is_number() ? it->get<double>() : std::get<double>(row.param->default_value);
            const auto   found  = std::find(row.choice_values.begin(), row.choice_values.end(), std::to_string(std::llround(v)));
            // A toolhead this printer does not have shows as no selection and keeps its stored value (commit skips it).
            choice->SetSelection(found == row.choice_values.end() ? wxNOT_FOUND : int(found - row.choice_values.begin()));
        } else if (row.param->ui.widget == "layer_ranges") {
            auto it = stored.find(row.key);
            std::vector<wxString> toolheads;
            if (wxGetApp().preset_bundle != nullptr)
                if (const auto *nozzles = wxGetApp().preset_bundle->printers.get_edited_preset().config.option<ConfigOptionFloats>("nozzle_diameter"))
                    for (size_t t = 0; t < nozzles->values.size(); ++t)
                        toolheads.push_back(format_wxstr(_L("T%1% (%2% mm)"), t + 1, nozzle_text(nozzles->values[t])));
            static_cast<ModLayerRangesEditor *>(row.control)->set_value(
                it != stored.end() && it->is_string() ? it->get<std::string>() : std::get<std::string>(row.param->default_value), toolheads);
        } else if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
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
    update_modified();
}

void ModParamsPanel::commit()
{
    if (m_loading)
        return;
    // From what the preset holds now when the tab has other panels on the same key (current_text).
    json stored = parse_object(current_text ? current_text() : m_loaded_json);
    for (Row &row : m_rows) {
        const ModHost::ModParam &p = *row.param;
        json                     value;
        if (p.ui.widget == "toolhead") {
            const int sel = static_cast<wxChoice *>(row.control)->GetSelection();
            if (sel == wxNOT_FOUND || size_t(sel) >= row.choice_values.size())
                continue; // nothing chosen: keep what is stored
            value = json(std::stoll(row.choice_values[size_t(sel)]));
        } else if (p.ui.widget == "layer_ranges") {
            value = static_cast<ModLayerRangesEditor *>(row.control)->value();
        } else if (auto *choice = dynamic_cast<wxChoice *>(row.control)) {
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
    const std::string text = kept_saved_text(stored.empty() ? std::string() : stored.dump());
    update_visibility();
    if (text == (current_text ? current_text() : m_loaded_json)) {
        m_loaded_json = text;
        return;
    }
    m_loaded_json = text;
    if (on_change)
        on_change(text);
    update_modified();
}

void ModParamsPanel::revert(const std::string &key)
{
    if (!saved_text)
        return;
    json       stored = parse_object(current_text ? current_text() : m_loaded_json);
    const json saved  = parse_object(saved_text());
    if (auto it = saved.find(key); it != saved.end())
        stored[key] = *it;
    else
        stored.erase(key); // the saved preset has the default
    const std::string text = kept_saved_text(stored.empty() ? std::string() : stored.dump());
    if (on_change)
        on_change(text);
    load_text(text);
}

std::string ModParamsPanel::kept_saved_text(const std::string &text) const
{
    // MAGPIE: a saved preset may store defaults and order its keys otherwise; a rewrite of the same values left the
    // preset marked modified after its only change was reverted (GUI test, resin tree type).
    if (!saved_text)
        return text;
    const std::string saved_raw = saved_text();
    const json        saved     = parse_object(saved_raw);
    const json        stored    = parse_object(text);
    bool              same      = true;
    for (auto it = stored.begin(); same && it != stored.end(); ++it)
        same = saved.contains(it.key()) && saved.at(it.key()) == it.value();
    for (auto it = saved.begin(); same && it != saved.end(); ++it)
        if (!stored.contains(it.key())) {
            // Dropped from the text: equal only if it is one of this panel's rows at its default.
            const auto row = std::find_if(m_rows.begin(), m_rows.end(), [&it](const Row &r) { return r.key == it.key(); });
            same = row != m_rows.end() && std::visit([&it](const auto &d) { return it.value() == json(d); }, row->param->default_value);
        }
    return same ? saved_raw : text;
}

int ModParamsPanel::value_column_offset() const
{
    if (m_rows.size() != 1 || m_rows.front().label != nullptr)
        return 0;
    return m_rows.front().control->GetPosition().x;
}

void ModParamsPanel::update_modified()
{
    const json current = parse_object(current_text ? current_text() : m_loaded_json);
    const json saved   = saved_text ? parse_object(saved_text()) : current;
    bool       any     = false;
    for (Row &row : m_rows) {
        // A missing key is the default on both sides; numbers compare by value (3 == 3.0). MAGPIE: a saved preset may
        // store a value equal to the default (a project written by the CLI), which a commit drops again: comparing
        // the values with the default filled in keeps such a row unmodified (GUI test: changing the resin tree type
        // marked the points density and head width rows modified too).
        const auto c          = current.find(row.key);
        const auto v          = saved.find(row.key);
        const json fallback   = std::visit([](const auto &d) { return json(d); }, row.param->default_value);
        const json c_value    = c != current.end() ? *c : fallback;
        const json v_value    = v != saved.end() ? *v : fallback;
        const bool modified   = c_value != v_value;
        any                 = any || modified;
        if (modified == row.modified)
            continue;
        row.modified = modified;
        row.revert->SetBitmap_(modified ? "undo" : "dot");
        row.revert->SetToolTip(modified ? _L("Click to drop current modifications and reset to saved value.") : wxString());
        if (row.label != nullptr) {
            row.label->SetForegroundColour(modified ? wxGetApp().get_label_clr_modified() : wxGetApp().get_label_clr_default());
            row.label->Refresh();
        }
    }
    if (on_modified)
        on_modified(any);
}

std::optional<ModHost::ExprValue> ModParamsPanel::row_value(const Row &row) const
{
    if (row.param->ui.widget == "toolhead") {
        const int sel = static_cast<wxChoice *>(row.control)->GetSelection();
        return sel == wxNOT_FOUND || size_t(sel) >= row.choice_values.size() ? std::nullopt :
                                                                               std::optional<ModHost::ExprValue>(std::stod(row.choice_values[size_t(sel)]));
    }
    if (row.param->ui.widget == "layer_ranges")
        return ModHost::ExprValue(static_cast<ModLayerRangesEditor *>(row.control)->value());
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
    bool any_shown = false;
    for (const Row &row : m_rows)
        any_shown = any_shown || row.control->IsShown();
    if (on_rows_shown && m_any_shown != any_shown) {
        m_any_shown = any_shown;
        on_rows_shown(any_shown);
    }
}

} // namespace GUI
} // namespace Slic3r
