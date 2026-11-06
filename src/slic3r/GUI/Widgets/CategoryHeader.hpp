#ifndef slic3r_GUI_CategoryHeader_hpp_
#define slic3r_GUI_CategoryHeader_hpp_

#include <functional>
#include <wx/window.h>

// MAGPIE: a clickable header above a run of option groups on a settings page; a click folds or unfolds the
// groups under it (Page::set_category_collapsed). Drawn as a triangle, the title and a rule, like StaticLine.
class CategoryHeader : public wxWindow
{
public:
    CategoryHeader(wxWindow* parent, const wxString& label, bool collapsed);

    void SetLabel(const wxString& label) override;
    void SetCollapsed(bool collapsed);
    bool IsCollapsed() const { return m_collapsed; }
    // How many settings the header stands for while folded; 0 shows none.
    void SetCount(size_t count);
    void Rescale();

    std::function<void(bool collapsed)> on_toggle;

private:
    bool     m_collapsed;
    bool     m_hover = false;
    size_t   m_count = 0;
    wxColour m_lineColor;

    void paintEvent(wxPaintEvent& evt);
    void measureSize();
    void render(wxDC& dc);
    wxString text() const;

    DECLARE_EVENT_TABLE()
};

#endif // slic3r_GUI_CategoryHeader_hpp_
