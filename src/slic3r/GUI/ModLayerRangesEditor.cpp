#include "ModLayerRangesEditor.hpp"

#include "GUI.hpp"
#include "I18N.hpp"

#include <wx/button.h>
#include <wx/choice.h>
#include <wx/sizer.h>
#include <wx/spinctrl.h>
#include <wx/stattext.h>

#include <algorithm>
#include <climits>

namespace Slic3r {
namespace GUI {

ModLayerRangesEditor::ModLayerRangesEditor(wxWindow *parent) : wxPanel(parent)
{
    SetBackgroundColour(parent->GetBackgroundColour());
    auto *outer = new wxBoxSizer(wxVERTICAL);
    m_rows_sizer = new wxBoxSizer(wxVERTICAL);
    outer->Add(m_rows_sizer, 0, wxEXPAND);
    auto *add = new wxButton(this, wxID_ANY, _L("Add range"));
    add->SetToolTip(_L("Add a layer range. Layer numbers start at 1; where ranges overlap, the last row wins."));
    add->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        // A new row starts after the last one, on the first toolhead.
        ModHost::LayerRange r;
        if (!m_list.ranges.empty()) {
            const ModHost::LayerRange &back = m_list.ranges.back();
            r.first = r.last = std::max(back.first, back.last) + 1;
        }
        m_list.ranges.push_back(r);
        rebuild();
        changed();
    });
    outer->Add(add, 0, wxTOP, FromDIP(2));
    SetSizer(outer);
}

void ModLayerRangesEditor::set_value(const std::string &text, const std::vector<wxString> &toolhead_labels)
{
    const ModHost::LayerRangeList list = ModHost::parse_layer_ranges(text);
    if (list.ranges == m_list.ranges && list.unread == m_list.unread && toolhead_labels == m_toolhead_labels)
        return;
    m_list            = list;
    m_toolhead_labels = toolhead_labels;
    rebuild();
}

std::string ModLayerRangesEditor::value() const { return ModHost::serialize_layer_ranges(m_list); }

void ModLayerRangesEditor::rebuild()
{
    m_loading = true;
    m_rows.clear();
    // Destroys the rows' windows with their sizers.
    m_rows_sizer->Clear(true);
    for (const ModHost::LayerRange &range : m_list.ranges)
        add_row(range);
    if (!m_list.unread.empty()) {
        // TRN %1%: entries of the large nozzle override text that are not first:last:toolhead.
        auto *note = new wxStaticText(this, wxID_ANY, format_wxstr(_L("Kept as written, not read: %1%"),
                                                                    wxString::FromUTF8(ModHost::serialize_layer_ranges({ {}, m_list.unread }))));
        m_rows_sizer->Add(note, 0, wxTOP, FromDIP(2));
    }
    m_loading = false;
    Layout();
    if (wxWindow *parent = GetParent())
        parent->Layout();
}

void ModLayerRangesEditor::add_row(const ModHost::LayerRange &range)
{
    const size_t idx = m_rows.size();
    auto *line = new wxBoxSizer(wxHORIZONTAL);
    RowControls row;
    row.first = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1), wxSP_ARROW_KEYS, 1, INT_MAX,
                               int(std::min<size_t>(range.first, INT_MAX)));
    row.last  = new wxSpinCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(64), -1), wxSP_ARROW_KEYS, 1, INT_MAX,
                               int(std::min<size_t>(range.last, INT_MAX)));
    wxArrayString names;
    for (const wxString &label : m_toolhead_labels)
        names.Add(label);
    // A toolhead the printer does not have keeps its number, named as such.
    if (range.toolhead > m_toolhead_labels.size())
        for (size_t t = m_toolhead_labels.size() + 1; t <= range.toolhead; ++t)
            // TRN %1%: a toolhead number the selected printer does not have.
            names.Add(format_wxstr(_L("T%1% (not on this printer)"), t));
    row.toolhead = new wxChoice(this, wxID_ANY, wxDefaultPosition, wxSize(FromDIP(110), -1), names);
    row.toolhead->SetSelection(int(range.toolhead) - 1);
    row.remove = new wxButton(this, wxID_ANY, "x", wxDefaultPosition, wxSize(FromDIP(24), -1), wxBU_EXACTFIT);
    row.remove->SetToolTip(_L("Remove this range"));

    line->Add(new wxStaticText(this, wxID_ANY, _L("Layers")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(4));
    line->Add(row.first, 0, wxALIGN_CENTER_VERTICAL);
    line->Add(new wxStaticText(this, wxID_ANY, "-"), 0, wxALIGN_CENTER_VERTICAL | wxLEFT | wxRIGHT, FromDIP(4));
    line->Add(row.last, 0, wxALIGN_CENTER_VERTICAL);
    line->Add(row.toolhead, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(6));
    line->Add(row.remove, 0, wxALIGN_CENTER_VERTICAL | wxLEFT, FromDIP(4));
    m_rows_sizer->Add(line, 0, wxBOTTOM, FromDIP(2));

    auto read_row = [this, idx]() {
        if (m_loading || idx >= m_rows.size() || idx >= m_list.ranges.size())
            return;
        ModHost::LayerRange &r = m_list.ranges[idx];
        r.first    = size_t(std::max(1, m_rows[idx].first->GetValue()));
        r.last     = size_t(std::max(1, m_rows[idx].last->GetValue()));
        r.toolhead = size_t(std::max(0, m_rows[idx].toolhead->GetSelection()) + 1);
        changed();
    };
    row.first->Bind(wxEVT_SPINCTRL, [read_row](wxSpinEvent &) { read_row(); });
    row.last->Bind(wxEVT_SPINCTRL, [read_row](wxSpinEvent &) { read_row(); });
    row.first->Bind(wxEVT_TEXT_ENTER, [read_row](wxCommandEvent &) { read_row(); });
    row.last->Bind(wxEVT_TEXT_ENTER, [read_row](wxCommandEvent &) { read_row(); });
    row.first->Bind(wxEVT_KILL_FOCUS, [read_row](wxFocusEvent &e) { read_row(); e.Skip(); });
    row.last->Bind(wxEVT_KILL_FOCUS, [read_row](wxFocusEvent &e) { read_row(); e.Skip(); });
    row.toolhead->Bind(wxEVT_CHOICE, [read_row](wxCommandEvent &) { read_row(); });
    row.remove->Bind(wxEVT_BUTTON, [this, idx](wxCommandEvent &) {
        if (idx >= m_list.ranges.size())
            return;
        m_list.ranges.erase(m_list.ranges.begin() + idx);
        // Rebuilt after the click has been handled: the button is one of the windows rebuild() destroys.
        CallAfter([this]() {
            rebuild();
            changed();
        });
    });
    m_rows.push_back(row);
}

void ModLayerRangesEditor::changed()
{
    if (!m_loading && on_change)
        on_change();
}

} // namespace GUI
} // namespace Slic3r
