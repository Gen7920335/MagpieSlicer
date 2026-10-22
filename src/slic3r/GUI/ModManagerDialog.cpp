#include "ModManagerDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>

#include <algorithm>

#include <wx/button.h>
#include <wx/dcmemory.h>
#include <wx/imaglist.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

namespace Slic3r {
namespace GUI {

enum { COL_NAME, COL_VERSION, COL_ORIGIN, COL_STATE };

namespace {

std::string bundled_dir() { return resources_dir().empty() ? std::string() : (boost::filesystem::path(resources_dir()) / "mods").string(); }
std::string installed_dir() { return data_dir().empty() ? std::string() : (boost::filesystem::path(data_dir()) / "mods").string(); }

// A bundled mod is official; any other mod is verified only with a valid signature by a key the application trusts.
bool trusted(const ModHost::ModListing &m) { return m.origin == ModHost::ModOrigin::Bundled || m.official || m.verified; }

wxString origin_text(const ModHost::ModListing &m)
{
    if (m.origin == ModHost::ModOrigin::Bundled)
        return _L("Bundled");
    if (m.official)
        return _L("Official");
    if (m.verified)
        return _L("Verified");
    return _L("Not verified");
}

enum { TAG_VERIFIED, TAG_UNVERIFIED };

// The trust tags of the Source column: a gold badge with a check mark for verified mods, a red one with an
// exclamation mark for unverified ones. Drawn here so they follow the font size without new image files.
wxImageList *make_tag_images(int size)
{
    auto badge = [size](const wxColour &fill, bool check) {
        wxBitmap   bmp(size, size);
        wxMemoryDC dc(bmp);
        dc.SetBackground(*wxTRANSPARENT_BRUSH);
        dc.SetBrush(wxBrush(fill));
        dc.SetPen(wxPen(fill));
        dc.Clear();
        dc.DrawCircle(size / 2, size / 2, size / 2 - 1);
        dc.SetPen(wxPen(*wxWHITE, std::max(2, size / 8)));
        if (check) {
            dc.DrawLine(size * 3 / 12, size * 6 / 12, size * 5 / 12, size * 8 / 12);
            dc.DrawLine(size * 5 / 12, size * 8 / 12, size * 9 / 12, size * 4 / 12);
        } else {
            dc.DrawLine(size / 2, size * 3 / 12, size / 2, size * 7 / 12);
            dc.DrawLine(size / 2, size * 9 / 12, size / 2, size * 9 / 12 + 1);
        }
        dc.SelectObject(wxNullBitmap);
        wxImage image = bmp.ConvertToImage();
        // Outside the circle stays transparent in light and dark mode.
        image.InitAlpha();
        const double r = size / 2. - 0.5;
        for (int y = 0; y < size; ++y)
            for (int x = 0; x < size; ++x)
                if ((x + 0.5 - size / 2.) * (x + 0.5 - size / 2.) + (y + 0.5 - size / 2.) * (y + 0.5 - size / 2.) > r * r)
                    image.SetAlpha(x, y, 0);
        return wxBitmap(image);
    };
    auto *images = new wxImageList(size, size, true, 2);
    images->Add(badge(wxColour(0xD4, 0xA0, 0x17), true));  // TAG_VERIFIED: gold
    images->Add(badge(wxColour(0xD9, 0x30, 0x25), false)); // TAG_UNVERIFIED: red
    return images;
}

wxString state_text(const ModHost::ModListing &m)
{
    if (!m.problem.empty())
        return from_u8(m.problem);
    if (m.remove_next)
        return _L("Removed at restart");
    if (m.update_next)
        return _L("Updated at restart");
    if (m.loaded)
        return m.off_next ? _L("On, off after restart") : _L("On");
    // Turned off, or it did not load (the log says why); turned on again, it tries at the next start.
    return m.off_next ? _L("Off") : _L("Not running; see the log");
}

} // namespace

ModManagerDialog::ModManagerDialog(wxWindow *parent)
    : wxDialog(parent, wxID_ANY, _L("Installed Mods"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    const int em  = wxGetApp().em_unit();
    auto     *top = new wxBoxSizer(wxVERTICAL);
    m_list        = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(90 * em, 30 * em), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn(_L("Name"), wxLIST_FORMAT_LEFT, 28 * em);
    m_list->AppendColumn(_L("Version"), wxLIST_FORMAT_LEFT, 8 * em);
    m_list->AppendColumn(_L("Source"), wxLIST_FORMAT_LEFT, 12 * em);
    m_list->AssignImageList(make_tag_images(std::max(12, em * 3 / 2)), wxIMAGE_LIST_SMALL);
    m_list->AppendColumn(_L("State"), wxLIST_FORMAT_LEFT, 40 * em);
    top->Add(m_list, 1, wxEXPAND | wxALL, em);
    top->Add(new wxStaticText(this, wxID_ANY, _L("Turning a mod off or on and removing it apply when you restart the application.")), 0,
             wxLEFT | wxRIGHT, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_enable      = new wxButton(this, wxID_ANY, _L("Turn off"));
    m_remove      = new wxButton(this, wxID_ANY, _L("Remove"));
    auto *close   = new wxButton(this, wxID_CLOSE, _L("Close"));
    buttons->Add(m_enable, 0, wxRIGHT, em);
    buttons->Add(m_remove, 0, wxRIGHT, em);
    buttons->AddStretchSpacer();
    buttons->Add(close, 0);
    top->Add(buttons, 0, wxEXPAND | wxALL, em);

    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &) { update_buttons(); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent &) { update_buttons(); });
    m_enable->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { toggle_enabled(); });
    m_remove->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { toggle_removal(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CLOSE); });

    SetSizerAndFit(top);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
    refresh();
}

void ModManagerDialog::refresh()
{
    const long keep = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    std::shared_ptr<const ModHost::ModSet> running = ModHost::active_mods();
    m_mods = ModHost::list_mods(bundled_dir(), installed_dir(), running.get());
    m_list->DeleteAllItems();
    const std::string language = into_u8(wxGetApp().current_language_code_safe());
    for (size_t i = 0; i < m_mods.size(); ++i) {
        const ModHost::ModListing &m = m_mods[i];
        const wxString name = from_u8(ModHost::localized_text(m.names, language, m.name.empty() ? m.id : m.name));
        // TRN %s: a mod's name.
        const long row = m_list->InsertItem(long(i), m.native ? wxString::Format(_L("%s (native code)"), name) : name, -1);
        m_list->SetItem(row, COL_VERSION, from_u8(m.version));
        if (m.problem.empty()) {
            m_list->SetItem(row, COL_ORIGIN, origin_text(m), trusted(m) ? TAG_VERIFIED : TAG_UNVERIFIED);
        }
        m_list->SetItem(row, COL_STATE, state_text(m));
    }
    if (keep >= 0 && keep < long(m_mods.size()))
        m_list->SetItemState(keep, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    update_buttons();
}

const ModHost::ModListing *ModManagerDialog::selected() const
{
    const long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    return row >= 0 && size_t(row) < m_mods.size() && m_mods[size_t(row)].problem.empty() ? &m_mods[size_t(row)] : nullptr;
}

void ModManagerDialog::update_buttons()
{
    const ModHost::ModListing *m = selected();
    m_enable->Enable(m != nullptr && !installed_dir().empty());
    m_enable->SetLabel(m != nullptr && m->off_next ? _L("Turn on") : _L("Turn off"));
    m_remove->Enable(m != nullptr && m->origin == ModHost::ModOrigin::Installed);
    m_remove->SetLabel(m != nullptr && m->remove_next ? _L("Keep") : _L("Remove"));
    Layout();
}

void ModManagerDialog::toggle_enabled()
{
    const ModHost::ModListing *m = selected();
    if (m == nullptr)
        return;
    try {
        ModHost::set_mod_enabled(installed_dir(), m->id, m->off_next);
        m_changed = true;
    } catch (const std::exception &e) {
        MessageDialog(this, from_u8(e.what()), _L("Installed Mods"), wxOK | wxICON_ERROR).ShowModal();
    }
    refresh();
}

void ModManagerDialog::toggle_removal()
{
    const ModHost::ModListing *m = selected();
    if (m == nullptr)
        return;
    try {
        if (m->remove_next)
            ModHost::cancel_mod_removal(installed_dir(), m->id);
        else
            ModHost::schedule_mod_removal(installed_dir(), m->id);
        m_changed = true;
    } catch (const std::exception &e) {
        MessageDialog(this, from_u8(e.what()), _L("Installed Mods"), wxOK | wxICON_ERROR).ShowModal();
    }
    refresh();
}

} // namespace GUI
} // namespace Slic3r
