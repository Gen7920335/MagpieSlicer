#include "PerNozzleLineWidthsPanel.hpp"

#include "GUI.hpp"
#include "I18N.hpp"
#include "libslic3r/LineWidths.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "Widgets/TextInput.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace GUI {

namespace {

// The rows, in the order of the line width settings on the Quality page.
const char *const row_keys[] = {
    "line_width", "initial_layer_line_width", "outer_wall_line_width", "inner_wall_line_width", "top_surface_line_width",
    "sparse_infill_line_width", "internal_solid_infill_line_width", "support_line_width", "bridge_line_width",
    "skin_infill_line_width", "skeleton_infill_line_width",
};

// A width in mm for a cell, without trailing zeros (0.1575 -> "0.158").
wxString format_mm(double mm)
{
    wxString text = wxString::Format("%.3f", mm);
    while (text.EndsWith("0"))
        text.RemoveLast();
    if (text.EndsWith("."))
        text.RemoveLast();
    return text;
}

bool one_nozzle_size(const std::vector<double> &nozzles_mm)
{
    if (nozzles_mm.empty())
        return true;
    const auto [lo, hi] = std::minmax_element(nozzles_mm.begin(), nozzles_mm.end());
    return *hi - *lo <= EPSILON; // mm, as absolute_line_width_reference_nozzle()
}

} // namespace

PerNozzleLineWidthsPanel::PerNozzleLineWidthsPanel(wxWindow *parent) : wxPanel(parent), m_config(std::make_unique<DynamicPrintConfig>())
{
    // The settings page's palette colour, dark mode included.
    SetBackgroundColour(parent->GetBackgroundColour());
    SetSizer(new wxBoxSizer(wxVERTICAL));
}

PerNozzleLineWidthsPanel::~PerNozzleLineWidthsPanel() = default;

void PerNozzleLineWidthsPanel::rebuild(const std::vector<double> &nozzles_mm)
{
    m_nozzles_mm = nozzles_mm;
    m_cells.clear();
    DestroyChildren();
    wxSizer *outer = GetSizer();
    outer->Clear();

    auto *title = new wxStaticText(this, wxID_ANY, _L("Line width per nozzle"));
    title->SetToolTip(_L("Line width for each nozzle, in mm, on a printer whose nozzles differ in size. An empty cell uses the line width setting, scaled to that nozzle; the greyed value is the width that gives."));
    outer->Add(title, 0, wxLEFT | wxTOP, FromDIP(4));

    auto *grid = new wxFlexGridSizer(int(nozzles_mm.size()) + 1, FromDIP(3), FromDIP(6));
    grid->Add(new wxStaticText(this, wxID_ANY, wxEmptyString));
    for (size_t nozzle = 0; nozzle < nozzles_mm.size(); ++nozzle)
        // TRN %1%: the nozzle number, %2%: its diameter in mm. A column heading of the per-nozzle line width table.
        grid->Add(new wxStaticText(this, wxID_ANY, format_wxstr(_L("Nozzle %1% (%2% mm)"), nozzle + 1, format_mm(nozzles_mm[nozzle]))), 0,
                  wxALIGN_CENTER_HORIZONTAL);
    for (const char *width_key : row_keys) {
        const ConfigOptionDef *def = print_config_def.get(width_key);
        grid->Add(new wxStaticText(this, wxID_ANY, def != nullptr ? _(def->label) : wxString::FromUTF8(width_key)), 0, wxALIGN_CENTER_VERTICAL);
        const std::string key = per_nozzle_line_width_key(width_key);
        for (size_t nozzle = 0; nozzle < nozzles_mm.size(); ++nozzle) {
            auto *control = new ::TextInput(this, wxEmptyString, wxEmptyString, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(70), -1), wxTE_PROCESS_ENTER);
            control->SetToolTip(format_wxstr(_L("%1% on nozzle %2% (%3% mm), in mm. Leave empty to use the line width setting."),
                                             def != nullptr ? _(def->label) : wxString::FromUTF8(width_key), nozzle + 1,
                                             format_mm(nozzles_mm[nozzle])));
            m_cells.push_back({ key, nozzle, control });
            const size_t idx = m_cells.size() - 1;
            control->GetTextCtrl()->Bind(wxEVT_TEXT_ENTER, [this, idx](wxCommandEvent &e) { commit(m_cells[idx]); e.Skip(); });
            control->GetTextCtrl()->Bind(wxEVT_KILL_FOCUS, [this, idx](wxFocusEvent &e) { commit(m_cells[idx]); e.Skip(); });
            grid->Add(control, 0, wxALIGN_CENTER_VERTICAL);
        }
    }
    outer->Add(grid, 0, wxALL, FromDIP(4));
    Layout();
}

std::vector<double> PerNozzleLineWidthsPanel::stored_values(const std::string &key) const
{
    std::vector<double> values(m_nozzles_mm.size(), 0.);
    if (const auto *opt = m_config->option<ConfigOptionFloats>(key))
        for (size_t idx = 0; idx < values.size() && idx < opt->values.size(); ++idx)
            values[idx] = std::max(0., opt->values[idx]);
    return values;
}

void PerNozzleLineWidthsPanel::load(const DynamicPrintConfig &process, const DynamicPrintConfig &printer)
{
    *m_config = process;
    for (const char *key : { "nozzle_diameter", "printer_variant" })
        if (const ConfigOption *opt = printer.option(key))
            m_config->set_key_value(key, opt->clone());
    const auto               *nozzles    = m_config->option<ConfigOptionFloats>("nozzle_diameter");
    const std::vector<double> nozzles_mm = nozzles != nullptr ? nozzles->values : std::vector<double>();
    const bool                shown      = !one_nozzle_size(nozzles_mm);
    if (shown && nozzles_mm != m_nozzles_mm)
        rebuild(nozzles_mm);
    m_loading = true;
    for (const Cell &cell : m_cells) {
        const double value = stored_values(cell.key)[cell.nozzle];
        cell.control->GetTextCtrl()->ChangeValue(value > 0. ? format_mm(value) : wxString());
        // The width the empty cell gives, greyed.
        const std::string width_key = cell.key.substr(0, cell.key.size() - std::string("_per_nozzle").size());
        cell.control->GetTextCtrl()->SetHint(format_mm(per_nozzle_line_width_fallback_mm(*m_config, width_key.c_str(), cell.nozzle)));
    }
    m_loading = false;
    if (IsShown() != shown) {
        Show(shown);
        if (GetParent() != nullptr)
            GetParent()->Layout();
    }
}

void PerNozzleLineWidthsPanel::commit(const Cell &cell)
{
    if (m_loading)
        return;
    std::vector<double> values = stored_values(cell.key);
    const wxString      text   = cell.control->GetTextCtrl()->GetValue().Strip(wxString::both);
    double              mm     = 0.;
    // Empty is 0 (the setting); a number that is not a width shows the stored value again.
    if (!text.empty() && (!text.ToCDouble(&mm) || !std::isfinite(mm) || mm < 0.)) {
        cell.control->GetTextCtrl()->ChangeValue(values[cell.nozzle] > 0. ? format_mm(values[cell.nozzle]) : wxString());
        return;
    }
    if (mm == values[cell.nozzle])
        return;
    values[cell.nozzle] = mm;
    m_config->set_key_value(cell.key, new ConfigOptionFloats(values));
    if (on_change)
        on_change(cell.key, values);
}

} // namespace GUI
} // namespace Slic3r
