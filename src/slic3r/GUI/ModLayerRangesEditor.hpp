#pragma once

#include <libslic3r/ModHost/LayerRanges.hpp>

#include <wx/panel.h>

#include <functional>
#include <string>
#include <vector>

class wxBoxSizer;
class wxChoice;
class wxSpinCtrl;

namespace Slic3r {
namespace GUI {

// MAGPIE: a table editor for a mod parameter that lists layer ranges with a toolhead each ("first:last:toolhead;...",
// ModHost::parse_layer_ranges), the "layer_ranges" widget of a mod parameter: one row per range, a toolhead dropdown
// named by the printer's nozzles, and Add / remove buttons. Entries it cannot read stay in the text it writes.
class ModLayerRangesEditor : public wxPanel
{
public:
    explicit ModLayerRangesEditor(wxWindow *parent);

    // Shows the ranges of `text`; `toolhead_labels` names toolheads 1..n ("T1 (0.4 mm)").
    void set_value(const std::string &text, const std::vector<wxString> &toolhead_labels);
    // The parameter text the rows give.
    std::string value() const;
    // Called after the user changed, added or removed a row.
    std::function<void()> on_change;

private:
    struct RowControls
    {
        wxSpinCtrl *first    = nullptr;
        wxSpinCtrl *last     = nullptr;
        wxChoice   *toolhead = nullptr;
        wxWindow   *remove   = nullptr;
    };

    void rebuild();
    void add_row(const ModHost::LayerRange &range);
    void changed();

    ModHost::LayerRangeList   m_list;
    std::vector<wxString>     m_toolhead_labels;
    std::vector<RowControls>  m_rows;
    wxBoxSizer               *m_rows_sizer = nullptr;
    bool                      m_loading    = false;
};

} // namespace GUI
} // namespace Slic3r
