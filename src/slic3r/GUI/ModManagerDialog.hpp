#pragma once

#include <libslic3r/ModHost/ModPackage.hpp>

#include <wx/dialog.h>

#include <vector>

class wxButton;
class wxListCtrl;

namespace Slic3r {
namespace GUI {

// MAGPIE: "Installed Mods": the bundled and installed mods, whether each runs now, and what changes at
// the next start. Turning a mod off or on and removing an installed one apply when the application
// restarts (a native mod's code stays loaded until then; docs/HLSD/mods.md).
class ModManagerDialog : public wxDialog
{
public:
    explicit ModManagerDialog(wxWindow *parent);

    // True when something was scheduled, so the caller can offer a restart.
    bool changed() const { return m_changed; }

private:
    void refresh();
    void update_buttons();
    const ModHost::ModListing *selected() const;
    void toggle_enabled();
    void toggle_removal();

    wxListCtrl *m_list   = nullptr;
    wxButton   *m_enable = nullptr;
    wxButton   *m_remove = nullptr;
    std::vector<ModHost::ModListing> m_mods;
    bool m_changed = false;
};

} // namespace GUI
} // namespace Slic3r
