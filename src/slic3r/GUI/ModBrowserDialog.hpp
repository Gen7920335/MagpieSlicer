#pragma once

#include <libslic3r/ModHost/ModIndex.hpp>

#include <wx/dialog.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

class wxListCtrl;
class wxTextCtrl;
class wxButton;
class wxStaticText;

namespace Slic3r {
namespace GUI {

// MAGPIE: "Get Mods": searches the mod index and installs a mod with one button. The index
// address is the app config's "mod_index_url", else DEFAULT_MOD_INDEX_URL. `wanted_ids`, when
// given (mods a project needs), is shown first and preselected.
class ModBrowserDialog : public wxDialog
{
public:
    ModBrowserDialog(wxWindow *parent, const std::vector<std::string> &wanted_ids = {});
    ~ModBrowserDialog() override;

    // True when a mod was installed, so the caller should rebuild the settings pages.
    bool installed_any() const { return m_installed_any; }

    // The index address the app uses.
    static std::string index_url();

private:
    void load_index();
    void refresh_list();
    void install_selected();
    void set_busy(bool busy);

    wxTextCtrl   *m_search  = nullptr;
    wxListCtrl   *m_list    = nullptr;
    wxButton     *m_install = nullptr;
    wxStaticText *m_status  = nullptr;

    std::vector<ModHost::ModIndexEntry> m_entries;
    std::vector<ModHost::ModIndexEntry> m_shown;
    std::vector<std::string>            m_wanted;
    bool                                m_installed_any = false;
    // Downloads run on a worker thread; their result comes back through CallAfter and is dropped once
    // the dialog is gone. While an install runs the dialog cannot be closed.
    std::shared_ptr<std::atomic<bool>>  m_alive = std::make_shared<std::atomic<bool>>(true);
    bool                                m_busy  = false;
};

} // namespace GUI
} // namespace Slic3r
