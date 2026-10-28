#include "ModCalibrationDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "ModParamsPanel.hpp"
#include "Plater.hpp"
#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/LabeledStaticBox.hpp"
#include "Widgets/TextInput.hpp"

#include "libslic3r/ModHost/Expr.hpp"
#include "libslic3r/ModHost/MergedSettings.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/tglbtn.h>
#include <wx/valtext.h>

#include <optional>

namespace Slic3r {
namespace GUI {

namespace {

// The mod's text for the UI language: its own translation, else the English one through the mod's catalog and the
// application's (mod_text), so texts the core also uses keep their translation.
wxString mod_label(const std::string &mod_id, const std::map<std::string, std::string> &translations, const std::string &fallback)
{
    const std::string language = ModHost::message_language();
    auto              own      = translations.find(language);
    if (own == translations.end() && language.size() >= 2)
        own = translations.find(language.substr(0, 2));
    if (own != translations.end())
        return wxString::FromUTF8(own->second.c_str());
    return wxString::FromUTF8(ModHost::mod_text(mod_id, fallback).c_str());
}

} // namespace

struct ModCalibrationDialog::Group
{
    std::string   id;
    wxStaticText *header = nullptr;
    wxString      label;
    bool          open   = true;
    wxSizer      *sizer  = nullptr;
};

// One form field: its manifest entry and its control.
struct ModCalibrationDialog::Field
{
    const ModHost::ModCalibrationField *def   = nullptr;
    wxStaticText                       *label = nullptr;
    wxWindow                           *ctrl  = nullptr;
    wxStaticText                       *unit  = nullptr;
    Group                              *group = nullptr;
    // Its "visible" condition holds: a setting field writes its value, also while its group is folded.
    bool                                active = true;
    // A setting field: what it binds to.
    const ConfigOptionDef  *orca  = nullptr; // an Orca option
    const ModHost::ModParam *param = nullptr; // or a mod parameter
    ModHost::MergedChoices   merged;          // an Orca dropdown with the choices merged into it
    std::vector<std::string> choice_values;   // a dropdown's values by index (choice fields, a mod parameter's options)

    bool is_merged() const { return orca != nullptr && orca->type == coEnum; }

    // ::ComboBox is a ::TextInput: the choice comes first, or its shown label would be read as the value.
    std::string value() const
    {
        if (auto *choice = dynamic_cast<::ComboBox *>(ctrl)) {
            const int i = choice->GetSelection();
            if (is_merged())
                return i >= 0 && size_t(i) < merged.entries.size() ? merged.entries[size_t(i)].value : std::string();
            return i >= 0 && size_t(i) < choice_values.size() ? choice_values[size_t(i)] : std::string();
        }
        if (auto *text = dynamic_cast<::TextInput *>(ctrl))
            return into_u8(text->GetTextCtrl()->GetValue());
        if (auto *check = dynamic_cast<::CheckBox *>(ctrl))
            return check->GetValue() ? "1" : "0";
        return {};
    }

    void set_value(const std::string &v)
    {
        if (auto *choice = dynamic_cast<::ComboBox *>(ctrl)) {
            for (size_t i = 0; i < choice_values.size(); ++i)
                if (choice_values[i] == v)
                    choice->SetSelection(int(i));
        } else if (auto *text = dynamic_cast<::TextInput *>(ctrl))
            text->GetTextCtrl()->ChangeValue(from_u8(v));
        else if (auto *check = dynamic_cast<::CheckBox *>(ctrl))
            check->SetValue(v == "1" || v == "true");
    }

    void show(bool on)
    {
        label->Show(on);
        ctrl->Show(on);
        if (unit)
            unit->Show(on);
    }
};

ModCalibrationDialog::ModCalibrationDialog(wxWindow *parent, Plater *plater, const ModHost::Mod &mod, const ModHost::ModGuiItem &calibration)
    : DPIDialog(parent, wxID_ANY, mod_label(mod.id, calibration.labels, calibration.label.empty() ? calibration.id : calibration.label),
                wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE)
    , m_plater(plater)
    , m_mod_id(mod.id)
    , m_calibration(calibration)
{
    // As Orca's calibration dialogs (calib_dlg.cpp): the light palette, which UpdateDlgDarkUI maps in dark mode.
    SetBackgroundColour(*wxWHITE);
    SetForegroundColour(wxColour("#363636"));
    SetFont(Label::Body_14);

    std::shared_ptr<const ModHost::ModSet> mods = ModHost::active_mods();
    const DynamicPrintConfig full = wxGetApp().preset_bundle->full_config();

    auto *v_sizer = new wxBoxSizer(wxVERTICAL);
    SetSizer(v_sizer);
    LabeledStaticBox *stb            = new LabeledStaticBox(this, _L("Settings"));
    auto             *settings_sizer = new wxStaticBoxSizer(stb, wxVERTICAL);
    settings_sizer->AddSpacer(FromDIP(5));
    const wxSize ctrl_size = FromDIP(wxSize(220, -1));

    // The main fields, then each group under its header.
    auto *main_grid = new wxFlexGridSizer(3, FromDIP(2), FromDIP(4));
    settings_sizer->Add(main_grid, 0, wxLEFT, FromDIP(3));
    std::map<std::string, wxFlexGridSizer *> group_grids;
    for (const ModHost::ModCalibrationGroup &g : m_calibration.groups) {
        auto group    = std::make_unique<Group>();
        group->id     = g.id;
        group->label  = mod_label(m_mod_id, g.labels, g.label.empty() ? g.id : g.label);
        group->open   = !g.collapsed;
        group->header = new wxStaticText(this, wxID_ANY, wxEmptyString);
        group->header->SetFont(Label::Head_14);
        group->header->SetCursor(wxCursor(wxCURSOR_HAND));
        Group *raw = group.get();
        group->header->Bind(wxEVT_LEFT_DOWN, [this, raw](wxMouseEvent &) {
            raw->open = !raw->open;
            on_changed();
        });
        auto *grid    = new wxFlexGridSizer(3, FromDIP(2), FromDIP(4));
        group->sizer  = grid;
        group_grids[g.id] = grid;
        settings_sizer->Add(group->header, 0, wxLEFT | wxTOP, FromDIP(6));
        settings_sizer->Add(grid, 0, wxLEFT, FromDIP(15));
        m_groups.push_back(std::move(group));
    }

    for (const ModHost::ModCalibrationField &f : m_calibration.form) {
        auto field = std::make_unique<Field>();
        field->def = &f;
        for (auto &g : m_groups)
            if (g->id == f.group)
                field->group = g.get();
        wxString    label;
        wxString    unit = from_u8(f.unit.empty() ? std::string() : ModHost::mod_text(m_mod_id, f.unit));
        std::string initial = f.default_value;
        if (f.kind == "setting") {
            if (f.bind.find('.') == std::string::npos) {
                field->orca = print_config_def.get(f.bind);
                if (field->orca != nullptr) {
                    label = _(field->orca->label);
                    unit  = _(field->orca->sidetext);
                    // A setting field shows the preset's value, unless the calibration gives a value of its own.
                    if (full.has(f.bind) && f.default_value.empty())
                        initial = full.opt_serialize(f.bind);
                }
            } else if (mods) {
                field->param = mods->find_param(f.bind);
                label        = ModParamsPanel::param_label(mods.get(), f.bind);
                if (field->param != nullptr) {
                    unit = from_u8(field->param->unit);
                    if (std::optional<ModHost::ExprValue> v = ModHost::param_value(full, f.bind); v && f.default_value.empty()) {
                        if (const bool *b = std::get_if<bool>(&*v))
                            initial = *b ? "1" : "0";
                        else
                            initial = ModHost::expr_to_string(*v);
                    }
                }
            }
            if (field->orca == nullptr && field->param == nullptr)
                continue; // a setting this application does not have (a mod that is not loaded): no field
        } else
            label = mod_label(m_mod_id, f.labels, f.label.empty() ? f.id : f.label);

        if (field->is_merged()) {
            field->merged = ModHost::merged_choices(*mods, *field->orca, ModHost::merged_params(*mods, f.bind));
            auto *choice  = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ctrl_size, 0, nullptr, wxCB_READONLY);
            for (const ModHost::MergedChoice &e : field->merged.entries)
                choice->Append(e.param.empty() ? _(from_u8(e.label)) : mod_label(e.param.substr(0, e.param.find('.')), e.labels, e.label));
            const std::string stored = full.has("mod_settings") ? full.opt_string("mod_settings") : std::string();
            choice->SetSelection(ModHost::merged_selection(field->merged, stored, full, f.bind));
            field->ctrl = choice;
            unit.clear();
        } else if (f.kind == "choice" || (field->param != nullptr && !field->param->options.empty())) {
            auto *choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, ctrl_size, 0, nullptr, wxCB_READONLY);
            if (f.kind == "choice")
                for (const ModHost::ModCalibrationOption &o : f.options) {
                    choice->Append(mod_label(m_mod_id, o.labels, o.label.empty() ? o.value : o.label));
                    field->choice_values.push_back(o.value);
                }
            else
                for (const ModHost::ModParamOption &o : field->param->options) {
                    choice->Append(mod_label(f.bind.substr(0, f.bind.find('.')), o.labels, o.label.empty() ? o.value : o.label));
                    field->choice_values.push_back(o.value);
                }
            field->ctrl = choice;
        } else if ((field->orca != nullptr && field->orca->type == coBool) || (field->param != nullptr && field->param->type == "bool")) {
            field->ctrl = new ::CheckBox(this);
            unit.clear();
        } else {
            auto *text = new ::TextInput(this, wxEmptyString, "", "", wxDefaultPosition, ctrl_size);
            text->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
            field->ctrl = text;
        }
        if (!field->is_merged())
            field->set_value(initial);
        field->label = new wxStaticText(this, wxID_ANY, label);
        field->unit  = new wxStaticText(this, wxID_ANY, unit);
        if (field->orca != nullptr && !field->orca->tooltip.empty())
            field->ctrl->SetToolTip(_(field->orca->tooltip));

        wxFlexGridSizer *grid = field->group != nullptr ? group_grids[field->group->id] : main_grid;
        grid->Add(field->label, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
        grid->Add(field->ctrl, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));
        grid->Add(field->unit, 0, wxALL | wxALIGN_CENTER_VERTICAL, FromDIP(2));

        const ModHost::ModCalibrationField *def = &f;
        if (auto *choice = dynamic_cast<::ComboBox *>(field->ctrl))
            choice->Bind(wxEVT_COMBOBOX, [this, def](wxCommandEvent &e) {
                // An option's "sets": the other fields it fills in (a combination of the two axes).
                for (const auto &field : m_fields)
                    if (field->def == def && def->kind == "choice")
                        for (const ModHost::ModCalibrationOption &o : def->options)
                            if (o.value == field->value())
                                for (const auto &[id, value] : o.sets)
                                    for (const auto &other : m_fields)
                                        if (other->def->id == id)
                                            other->set_value(value);
                on_changed();
                e.Skip();
            });
        else if (auto *text = dynamic_cast<::TextInput *>(field->ctrl))
            text->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
                on_changed();
                e.Skip();
            });
        else if (auto *check = dynamic_cast<::CheckBox *>(field->ctrl))
            check->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
                on_changed();
                e.Skip();
            });
        m_fields.push_back(std::move(field));
    }

    m_summary  = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_warnings = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_errors   = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_warnings->SetForegroundColour(wxColour(0xE0, 0x8A, 0x00));
    m_errors->SetForegroundColour(wxColour(0xD0, 0x30, 0x30));
    settings_sizer->Add(m_summary, 0, wxLEFT | wxTOP, FromDIP(5));
    settings_sizer->Add(m_warnings, 0, wxLEFT | wxTOP, FromDIP(5));
    settings_sizer->Add(m_errors, 0, wxLEFT | wxTOP | wxBOTTOM, FromDIP(5));

    v_sizer->Add(settings_sizer, 0, wxTOP | wxRIGHT | wxLEFT | wxEXPAND, FromDIP(10));
    v_sizer->AddSpacer(FromDIP(5));
    auto *dlg_btns     = new DialogButtons(this, { "OK", "Cancel" });
    auto *bottom_sizer = new wxBoxSizer(wxHORIZONTAL);
    bottom_sizer->AddStretchSpacer();
    bottom_sizer->Add(dlg_btns, 0, wxEXPAND);
    v_sizer->Add(bottom_sizer, 0, wxEXPAND);
    m_ok = dlg_btns->GetOK();
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { on_ok(); });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });

    wxGetApp().UpdateDlgDarkUI(this);
    on_changed();
}

ModCalibrationDialog::~ModCalibrationDialog() = default;

ModHost::CalibrationFields ModCalibrationDialog::fields() const
{
    ModHost::CalibrationFields out;
    for (const auto &field : m_fields)
        out.emplace_back(field->def->id, field->value());
    return out;
}

std::vector<std::pair<std::string, std::string>> ModCalibrationDialog::setting_writes() const
{
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto &field : m_fields) {
        if (field->def->kind != "setting" || !field->active)
            continue;
        if (field->is_merged()) {
            auto *choice = dynamic_cast<::ComboBox *>(field->ctrl);
            const int sel = choice != nullptr ? choice->GetSelection() : -1;
            if (sel < 0 || size_t(sel) >= field->merged.entries.size())
                continue;
            const ModHost::MergedChoice &e = field->merged.entries[size_t(sel)];
            // As the settings tab writes it (merged_write): Orca's value with the parameters at their default, or the
            // parameter's value. A parameter's text value, as set_calibration_option reads it.
            if (e.param.empty()) {
                out.emplace_back(field->def->bind, e.value);
                for (const std::string &p : field->merged.params)
                    out.emplace_back(p, std::string());
            } else
                out.emplace_back(e.param, e.value);
        } else
            out.emplace_back(field->def->bind, field->value());
    }
    return out;
}

void ModCalibrationDialog::on_changed()
{
    if (m_updating)
        return;
    m_updating = true;
    // Conditions read the form's values by field id: a number when the text is one, else the text.
    const ModHost::ExprLookup lookup = [this](const std::string &name, std::optional<size_t>) -> std::optional<ModHost::ExprValue> {
        for (const auto &field : m_fields)
            if (field->def->id == name) {
                const std::string v   = field->value();
                const char       *end = nullptr;
                const double      d   = ModHost::parse_decimal(v.c_str(), &end);
                if (!v.empty() && end != v.c_str() && *end == '\0')
                    return ModHost::ExprValue(d);
                return ModHost::ExprValue(v);
            }
        return std::nullopt;
    };
    // A condition that cannot be evaluated shows the field, so a broken condition never hides one.
    auto visible = [&lookup](const std::optional<ModHost::Expr> &cond) {
        if (!cond)
            return true;
        try {
            return ModHost::expr_truthy(cond->evaluate(lookup));
        } catch (const std::exception &) {
            return true;
        }
    };
    for (auto &group : m_groups)
        group->header->SetLabel((group->open ? wxString::FromUTF8("\xE2\x96\xBE ") : wxString::FromUTF8("\xE2\x96\xB8 ")) + group->label);
    for (auto &field : m_fields) {
        field->active = visible(field->def->visible);
        field->show(field->active && (field->group == nullptr || field->group->open));
        // A unit that follows another field's value (an axis' start and end).
        if (!field->def->units_of.empty())
            for (const auto &other : m_fields)
                if (other->def->id == field->def->units_of) {
                    auto it = field->def->units.find(other->value());
                    field->unit->SetLabel(it != field->def->units.end() ? from_u8(ModHost::mod_text(m_mod_id, it->second)) : wxString());
                }
    }
    // The mod's check, with the settings the form writes.
    DynamicPrintConfig config = wxGetApp().preset_bundle->full_config();
    wxString           errors, warnings, summary;
    try {
        for (const auto &[key, value] : setting_writes())
            ModHost::set_calibration_option(config, key, value);
        const ModHost::CalibrationCheck check = ModHost::check_calibration(m_mod_id, m_calibration.id, config, fields());
        if (!check.failure.empty())
            errors = from_u8(check.failure);
        for (const std::string &e : check.errors)
            errors += (errors.empty() ? "" : "\n") + from_u8(e);
        for (const std::string &w : check.warnings)
            warnings += (warnings.empty() ? "" : "\n") + from_u8(w);
        summary = from_u8(check.summary);
    } catch (const std::exception &e) {
        errors = from_u8(e.what());
    }
    m_summary->SetLabel(summary);
    m_warnings->SetLabel(warnings);
    m_errors->SetLabel(errors);
    m_summary->Show(!summary.empty());
    m_warnings->Show(!warnings.empty());
    m_errors->Show(!errors.empty());
    m_ok->Enable(errors.empty());
    m_updating = false;
    Layout();
    GetSizer()->SetSizeHints(this);
    Fit();
}

void ModCalibrationDialog::on_ok()
{
    const wxString name = mod_label(m_mod_id, m_calibration.labels, m_calibration.label.empty() ? m_calibration.id : m_calibration.label);
    if (m_plater->calib_mod(m_mod_id, m_calibration.id, name, fields(), setting_writes()))
        EndModal(wxID_OK);
}

void ModCalibrationDialog::on_dpi_changed(const wxRect &)
{
    Refresh();
    Fit();
}

} // namespace GUI
} // namespace Slic3r
