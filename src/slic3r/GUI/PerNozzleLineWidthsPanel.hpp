#pragma once

#include <wx/panel.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

class wxTextCtrl;
class wxWindow;

namespace Slic3r {

class DynamicPrintConfig;

namespace GUI {

// MAGPIE: the per-nozzle line widths of a process preset (the <key>_per_nozzle settings, LineWidths.hpp) as a table:
// one row per line width setting, one column per nozzle of the selected printer, labelled with its diameter. An empty
// cell takes the setting itself and shows, greyed, the width slicing then uses on that nozzle. Hidden on a printer
// whose nozzles all have one size, where slicing ignores these settings.
class PerNozzleLineWidthsPanel : public wxPanel
{
public:
    explicit PerNozzleLineWidthsPanel(wxWindow *parent);
    ~PerNozzleLineWidthsPanel() override;

    // Shows the values of `process` for the nozzles of `printer` (nozzle_diameter, printer_variant).
    void load(const DynamicPrintConfig &process, const DynamicPrintConfig &printer);
    // Called with a setting and its new values (mm per nozzle, 0 = the setting) after the user changes a cell.
    std::function<void(const std::string &key, const std::vector<double> &values)> on_change;

private:
    struct Cell
    {
        std::string key; // the per-nozzle setting
        size_t      nozzle;
        wxTextCtrl *control;
    };

    void rebuild(const std::vector<double> &nozzles_mm);
    void commit(const Cell &cell);
    // The values of a row as last loaded.
    std::vector<double> stored_values(const std::string &key) const;

    std::vector<double> m_nozzles_mm;
    std::vector<Cell>   m_cells;
    // process + the printer's nozzle settings, as last loaded.
    std::unique_ptr<DynamicPrintConfig> m_config;
    bool                m_loading = false;
};

} // namespace GUI
} // namespace Slic3r
