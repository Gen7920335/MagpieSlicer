#include "CategoryHeader.hpp"
#include "Label.hpp"
#include "StateColor.hpp"

#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/settings.h>

BEGIN_EVENT_TABLE(CategoryHeader, wxWindow)
EVT_PAINT(CategoryHeader::paintEvent)
END_EVENT_TABLE()

CategoryHeader::CategoryHeader(wxWindow* parent, const wxString& label, bool collapsed)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_collapsed(collapsed)
    , m_lineColor(wxColour("#D0D0D0"))
{
    wxWindow::SetBackgroundColour(parent->GetBackgroundColour());
    DisableFocusFromKeyboard();
    SetFont(Label::Head_16);
    SetForegroundColour(wxColour("#363636"));
    SetCursor(wxCursor(wxCURSOR_HAND));
    wxWindow::SetLabel(label);
    measureSize();
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        SetCollapsed(!m_collapsed);
        if (on_toggle)
            on_toggle(m_collapsed);
    });
}

void CategoryHeader::SetLabel(const wxString& label)
{
    wxWindow::SetLabel(label);
    measureSize();
    Refresh();
}

void CategoryHeader::SetCollapsed(bool collapsed)
{
    if (m_collapsed == collapsed)
        return;
    m_collapsed = collapsed;
    measureSize();
    Refresh();
}

void CategoryHeader::SetCount(size_t count)
{
    if (m_count == count)
        return;
    m_count = count;
    measureSize();
    Refresh();
}

void CategoryHeader::Rescale()
{
    measureSize();
    Refresh();
}

wxString CategoryHeader::text() const
{
    wxString label = GetLabel();
    if (m_collapsed && m_count > 0)
        label += wxString::Format(" (%d)", int(m_count));
    return label;
}

void CategoryHeader::measureSize()
{
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    wxSize sz = dc.GetTextExtent(text());
    sz.x += FromDIP(28);
    sz.y += FromDIP(10);
    SetMinSize(sz);
}

void CategoryHeader::paintEvent(wxPaintEvent&)
{
    wxPaintDC dc(this);
    render(dc);
}

void CategoryHeader::render(wxDC& dc)
{
    const wxSize   size  = GetSize();
    const wxString label = text();
    dc.SetFont(GetFont());
    const wxSize textSize = dc.GetTextExtent(label);
    const int    y        = size.y / 2;
    const int    arrow    = FromDIP(5);
    const wxColour fg     = StateColor::darkModeColorFor(GetForegroundColour());

    // The triangle: pointing right while folded, down while open.
    dc.SetPen(wxPen(fg));
    dc.SetBrush(wxBrush(fg));
    const int ax = FromDIP(4);
    if (m_collapsed) {
        wxPoint pts[3] = { { ax, y - arrow }, { ax + arrow, y }, { ax, y + arrow } };
        dc.DrawPolygon(3, pts);
    } else {
        wxPoint pts[3] = { { ax - 1, y - arrow / 2 }, { ax + 2 * arrow - 1, y - arrow / 2 }, { ax + arrow - 1, y + arrow / 2 } };
        dc.DrawPolygon(3, pts);
    }

    int x = FromDIP(20);
    dc.SetTextForeground(fg);
    dc.DrawText(label, x, (size.y - textSize.y) / 2);
    x += textSize.x + FromDIP(8);

    dc.SetPen(wxPen(StateColor::darkModeColorFor(m_lineColor)));
    dc.DrawLine(x, y, size.x, y);
}
