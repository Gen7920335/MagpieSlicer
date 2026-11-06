#include "CategoryHeader.hpp"
#include "Label.hpp"
#include "StateColor.hpp"

#include <wx/dc.h>
#include <wx/dcclient.h>
#include <wx/dcbuffer.h>
#include <wx/settings.h>

BEGIN_EVENT_TABLE(CategoryHeader, wxWindow)
EVT_PAINT(CategoryHeader::paintEvent)
END_EVENT_TABLE()

// The band is Magpie's blue ("#398FF4", the tabs' underline and the toggle buttons) tinted while the category is
// open, grey while it is folded; the count sits in a pill at the right.
static const wxColour ACCENT("#398FF4");
static const wxColour ACCENT_SOFT("#E8F2FE");
static const wxColour ACCENT_SOFT_HOVER("#D6E8FD");
static const wxColour FOLDED_BAR("#A9B0B8");
static const wxColour FOLDED_SOFT("#F1F3F5");
static const wxColour FOLDED_SOFT_HOVER("#E6E9ED");
static const wxColour TEXT_OPEN("#1F2A33");
static const wxColour TEXT_FOLDED("#454D55");
static const wxColour PILL_FOLDED_TEXT("#FFFFFF");
static const wxColour PILL_OPEN("#C9D3DE");

CategoryHeader::CategoryHeader(wxWindow* parent, const wxString& label, bool collapsed)
    : wxWindow(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxBORDER_NONE)
    , m_collapsed(collapsed)
    , m_lineColor(ACCENT)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT);
    wxWindow::SetBackgroundColour(parent->GetBackgroundColour());
    DisableFocusFromKeyboard();
    SetFont(Label::Head_14);
    SetCursor(wxCursor(wxCURSOR_HAND));
    wxWindow::SetLabel(label);
    measureSize();
    Bind(wxEVT_LEFT_DOWN, [this](wxMouseEvent&) {
        SetCollapsed(!m_collapsed);
        if (on_toggle)
            on_toggle(m_collapsed);
    });
    Bind(wxEVT_ENTER_WINDOW, [this](wxMouseEvent& e) { m_hover = true; Refresh(); e.Skip(); });
    Bind(wxEVT_LEAVE_WINDOW, [this](wxMouseEvent& e) { m_hover = false; Refresh(); e.Skip(); });
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
    Refresh();
}

void CategoryHeader::SetCount(size_t count)
{
    if (m_count == count)
        return;
    m_count = count;
    Refresh();
}

void CategoryHeader::Rescale()
{
    measureSize();
    Refresh();
}

wxString CategoryHeader::text() const
{
    return GetLabel();
}

void CategoryHeader::measureSize()
{
    wxClientDC dc(this);
    dc.SetFont(GetFont());
    wxSize sz = dc.GetTextExtent(text());
    sz.x += FromDIP(90); // the chevron, the bar and room for the pill
    sz.y  = std::max(sz.y + FromDIP(14), FromDIP(30));
    SetMinSize(sz);
}

void CategoryHeader::paintEvent(wxPaintEvent&)
{
    wxAutoBufferedPaintDC dc(this);
    render(dc);
}

void CategoryHeader::render(wxDC& dc)
{
    const wxSize size = GetSize();
    dc.SetBackground(wxBrush(StateColor::darkModeColorFor(GetBackgroundColour())));
    dc.Clear();

    const wxColour band = StateColor::darkModeColorFor(m_collapsed ? (m_hover ? FOLDED_SOFT_HOVER : FOLDED_SOFT)
                                                                    : (m_hover ? ACCENT_SOFT_HOVER : ACCENT_SOFT));
    const wxColour bar  = StateColor::darkModeColorFor(m_collapsed ? FOLDED_BAR : ACCENT);
    const wxColour textColor = StateColor::darkModeColorFor(m_collapsed ? TEXT_FOLDED : TEXT_OPEN);
    const int      r    = FromDIP(6);

    // The band, with the accent bar flush on its left edge.
    dc.SetPen(*wxTRANSPARENT_PEN);
    dc.SetBrush(wxBrush(band));
    dc.DrawRoundedRectangle(0, 0, size.x, size.y, r);
    dc.SetBrush(wxBrush(bar));
    dc.DrawRoundedRectangle(0, 0, FromDIP(8), size.y, r);
    dc.SetBrush(wxBrush(band));
    dc.DrawRectangle(FromDIP(4), 0, FromDIP(6), size.y);

    // The chevron: a "v" while open, a ">" while folded.
    const int cy = size.y / 2;
    const int cx = FromDIP(18);
    const int a  = FromDIP(4);
    dc.SetPen(wxPen(bar, FromDIP(2)));
    if (m_collapsed) {
        dc.DrawLine(cx - a / 2, cy - a, cx + a / 2, cy);
        dc.DrawLine(cx + a / 2, cy, cx - a / 2, cy + a);
    } else {
        dc.DrawLine(cx - a, cy - a / 2, cx, cy + a / 2);
        dc.DrawLine(cx, cy + a / 2, cx + a, cy - a / 2);
    }

    // The title.
    dc.SetFont(GetFont());
    dc.SetTextForeground(textColor);
    const wxString label    = text();
    const wxSize   textSize = dc.GetTextExtent(label);
    dc.DrawText(label, FromDIP(30), (size.y - textSize.y) / 2);

    // The count pill at the right: filled while folded (what the fold hides), outlined while open.
    if (m_count > 0) {
        wxFont smallFont = Label::Body_12;
        dc.SetFont(smallFont);
        const wxString count     = wxString::Format("%d", int(m_count));
        const wxSize   countSize = dc.GetTextExtent(count);
        const int      pw        = countSize.x + FromDIP(14);
        const int      ph        = countSize.y + FromDIP(4);
        const int      px        = size.x - pw - FromDIP(8);
        const int      py        = (size.y - ph) / 2;
        if (m_collapsed) {
            dc.SetPen(*wxTRANSPARENT_PEN);
            dc.SetBrush(wxBrush(StateColor::darkModeColorFor(ACCENT)));
            dc.DrawRoundedRectangle(px, py, pw, ph, ph / 2);
            dc.SetTextForeground(PILL_FOLDED_TEXT);
        } else {
            dc.SetPen(wxPen(StateColor::darkModeColorFor(PILL_OPEN)));
            dc.SetBrush(*wxTRANSPARENT_BRUSH);
            dc.DrawRoundedRectangle(px, py, pw, ph, ph / 2);
            dc.SetTextForeground(StateColor::darkModeColorFor(TEXT_FOLDED));
        }
        dc.DrawText(count, px + (pw - countSize.x) / 2, py + (ph - countSize.y) / 2);
    }
}
