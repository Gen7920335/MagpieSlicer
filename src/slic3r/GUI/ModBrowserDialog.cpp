#include "ModBrowserDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "slic3r/Utils/ModIndexFetch.hpp"

#include <wx/button.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>

#include <algorithm>
#include <thread>

namespace Slic3r {
namespace GUI {

enum { COL_NAME, COL_VERSION, COL_TRUST, COL_STATE, COL_DESCRIPTION };

std::string ModBrowserDialog::index_url()
{
    const std::string configured = wxGetApp().app_config ? wxGetApp().app_config->get("mod_index_url") : std::string();
    return configured.empty() ? std::string(DEFAULT_MOD_INDEX_URL) : configured;
}

static wxString trust_label(ModHost::ModTrust trust)
{
    switch (trust) {
    case ModHost::ModTrust::Official: return _L("Official");
    case ModHost::ModTrust::Verified: return _L("Verified");
    default: return _L("Community");
    }
}

// The version of the loaded mod with this id, or empty when none is loaded.
static std::string loaded_version(const std::string &id)
{
    if (auto mods = ModHost::active_mods())
        for (const ModHost::Mod &mod : mods->mods())
            if (mod.id == id)
                return mod.version.empty() ? std::string("?") : mod.version;
    return {};
}

ModBrowserDialog::ModBrowserDialog(wxWindow *parent, const std::vector<std::string> &wanted_ids)
    : wxDialog(parent, wxID_ANY, _L("Get Mods"), wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_wanted(wanted_ids)
{
    const int em = wxGetApp().em_unit();
    auto *top = new wxBoxSizer(wxVERTICAL);

    m_search = new wxTextCtrl(this, wxID_ANY, "", wxDefaultPosition, wxSize(60 * em, -1));
    m_search->SetHint(_L("Search mods"));
    top->Add(m_search, 0, wxEXPAND | wxALL, em);

    m_list = new wxListCtrl(this, wxID_ANY, wxDefaultPosition, wxSize(90 * em, 30 * em), wxLC_REPORT | wxLC_SINGLE_SEL);
    m_list->AppendColumn(_L("Name"), wxLIST_FORMAT_LEFT, 22 * em);
    m_list->AppendColumn(_L("Version"), wxLIST_FORMAT_LEFT, 8 * em);
    m_list->AppendColumn(_L("Trust"), wxLIST_FORMAT_LEFT, 10 * em);
    m_list->AppendColumn(_L("Installed"), wxLIST_FORMAT_LEFT, 9 * em);
    m_list->AppendColumn(_L("Description"), wxLIST_FORMAT_LEFT, 40 * em);
    top->Add(m_list, 1, wxEXPAND | wxLEFT | wxRIGHT, em);

    m_status = new wxStaticText(this, wxID_ANY, "");
    top->Add(m_status, 0, wxEXPAND | wxALL, em);

    auto *buttons = new wxBoxSizer(wxHORIZONTAL);
    m_install = new wxButton(this, wxID_ANY, _L("Install"));
    auto *close = new wxButton(this, wxID_CLOSE, _L("Close"));
    buttons->AddStretchSpacer();
    buttons->Add(m_install, 0, wxRIGHT, em);
    buttons->Add(close, 0);
    top->Add(buttons, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em);

    m_search->Bind(wxEVT_TEXT, [this](wxCommandEvent &) { refresh_list(); });
    m_list->Bind(wxEVT_LIST_ITEM_SELECTED, [this](wxListEvent &) { m_install->Enable(true); });
    m_list->Bind(wxEVT_LIST_ITEM_DESELECTED, [this](wxListEvent &) { m_install->Enable(false); });
    m_list->Bind(wxEVT_LIST_ITEM_ACTIVATED, [this](wxListEvent &) { install_selected(); });
    m_install->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { install_selected(); });
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { if (!m_busy) EndModal(wxID_CLOSE); });
    // ESC and the close box end the dialog too; not while an install is writing files.
    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent &e) {
        if (m_busy && e.CanVeto())
            e.Veto();
        else
            e.Skip();
    });
    m_install->Enable(false);

    SetSizerAndFit(top);
    wxGetApp().UpdateDlgDarkUI(this);
    CenterOnParent();
    load_index();
}

ModBrowserDialog::~ModBrowserDialog()
{
    m_alive->store(false);
}

void ModBrowserDialog::set_busy(bool busy)
{
    m_busy = busy;
    m_list->Enable(!busy);
    m_search->Enable(!busy);
    m_install->Enable(!busy && m_list->GetSelectedItemCount() > 0);
}

// The index is downloaded on a worker thread (up to the HTTP timeout), so the app stays responsive.
void ModBrowserDialog::load_index()
{
    const std::string url = index_url();
    m_status->SetLabel(_L("Reading the mod index..."));
    set_busy(true);
    std::thread([this, alive = m_alive, url]() {
        std::vector<ModHost::ModIndexEntry> entries;
        std::string                         error;
        try {
            entries = fetch_mod_index(url);
        } catch (const std::exception &e) {
            error = e.what();
        }
        wxGetApp().CallAfter([this, alive, url, entries = std::move(entries), error]() mutable {
            if (!alive->load())
                return;
            set_busy(false);
            m_entries = std::move(entries);
            if (error.empty())
                m_status->SetLabel(wxString::Format(_L("%zu mods in %s"), m_entries.size(), from_u8(url)));
            else
                m_status->SetLabel(_L("Could not read the mod index:") + " " + from_u8(error));
            // Mods the project needs go first.
            std::stable_partition(m_entries.begin(), m_entries.end(), [this](const ModHost::ModIndexEntry &e) {
                return std::find(m_wanted.begin(), m_wanted.end(), e.id) != m_wanted.end();
            });
            refresh_list();
        });
    }).detach();
}

void ModBrowserDialog::refresh_list()
{
    m_shown = ModHost::search_mod_index(m_entries, into_u8(m_search->GetValue()));
    m_list->DeleteAllItems();
    for (size_t i = 0; i < m_shown.size(); ++i) {
        const ModHost::ModIndexEntry &e = m_shown[i];
        const long row = m_list->InsertItem(long(i), from_u8(e.name.empty() ? e.id : e.name));
        m_list->SetItem(row, COL_VERSION, from_u8(e.version));
        m_list->SetItem(row, COL_TRUST, trust_label(e.trust));
        const std::string have = loaded_version(e.id);
        m_list->SetItem(row, COL_STATE, have.empty() ? wxString() : from_u8(have));
        m_list->SetItem(row, COL_DESCRIPTION, from_u8(e.description));
        if (std::find(m_wanted.begin(), m_wanted.end(), e.id) != m_wanted.end() && have.empty())
            m_list->SetItemState(row, wxLIST_STATE_SELECTED, wxLIST_STATE_SELECTED);
    }
}

void ModBrowserDialog::install_selected()
{
    const long row = m_list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (row < 0 || size_t(row) >= m_shown.size())
        return;
    const ModHost::ModIndexEntry entry = m_shown[size_t(row)];
    if (entry.trust == ModHost::ModTrust::Community) {
        MessageDialog ask(this,
                          wxString::Format(_L("\"%s\" is a community mod that the index has not checked. Install it?"),
                                           from_u8(entry.name.empty() ? entry.id : entry.name)),
                          _L("Get Mods"), wxYES_NO | wxICON_QUESTION);
        if (ask.ShowModal() != wxID_YES)
            return;
    }
    m_status->SetLabel(wxString::Format(_L("Installing %s..."), from_u8(entry.name.empty() ? entry.id : entry.name)));
    set_busy(true);
    std::thread([this, alive = m_alive, entry]() {
        std::string error;
        try {
            download_and_install_mod(entry);
        } catch (const std::exception &e) {
            error = e.what();
        }
        wxGetApp().CallAfter([this, alive, entry, error]() {
            if (!alive->load())
                return;
            set_busy(false);
            if (error.empty()) {
                m_installed_any = true;
                m_status->SetLabel(wxString::Format(_L("Installed %s %s."), from_u8(entry.id), from_u8(entry.version)));
            } else {
                m_status->SetLabel(_L("Could not install the mod:") + " " + from_u8(error));
            }
            refresh_list();
        });
    }).detach();
}

} // namespace GUI
} // namespace Slic3r
