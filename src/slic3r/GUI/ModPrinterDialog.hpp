#ifndef slic3r_ModPrinterDialog_hpp_
#define slic3r_ModPrinterDialog_hpp_

// MAGPIE: the printer panel of a printer.agent mod, drawn from its manifest's "gui"."printer" (ModSet.hpp): the
// connection form (stored per printer address: values in the application config, secrets in the credential store),
// the mod's network list with the user's agreement, its notices, its action buttons (neutral commands, with a
// confirmation and the idle-only lock), the connection and print state, and its web panels (pages in the mod's folder
// exchanging JSON with the session). Opened from the printer settings' "Printer connection" button. Nothing in it
// knows a printer brand.

#include "GUI_Utils.hpp"

#include <nlohmann/json.hpp>

#include <map>
#include <memory>
#include <string>
#include <vector>

class wxBoxSizer;
class wxButton;
class wxStaticText;
class wxWindow;

namespace Slic3r {

class ModPrinterAgent;

namespace ModHost {
struct Mod;
}

namespace GUI {

class ModPrinterDialog : public DPIDialog
{
public:
    // `agent_id` is a registry id "mod:..."; `host` the printer preset's print_host.
    ModPrinterDialog(wxWindow *parent, std::shared_ptr<ModPrinterAgent> agent, const std::string &host);
    ~ModPrinterDialog() override;

    // Opens the panel for the edited printer preset when its Printer Agent is a mod's, else says why not.
    static void show_for_edited_printer(wxWindow *parent);
    // Installs ModPrinterAgent's agreement prompt (the network list dialog below). Called once the agents exist.
    static void install_hooks();
    // Shows the mod's network list and asks the user to agree; stores the answer. True when they agreed.
    static bool ask_network_consent(wxWindow *parent, const ModHost::Mod &mod);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    struct FieldCtrl;

    void build();
    void load_fields();
    bool save_fields();
    void update_visibility();
    void update_state();
    void on_action(size_t index);
    void open_web_panel(size_t index);
    void on_agent_event(const nlohmann::json &event);

    std::shared_ptr<ModPrinterAgent> m_agent;
    std::string                      m_host;       // store_host() of the preset's address
    std::string                      m_language;

    std::vector<std::unique_ptr<FieldCtrl>> m_fields;
    std::vector<std::pair<size_t, wxWindow *>> m_notices;  // manifest index -> its text
    std::vector<std::pair<size_t, wxButton *>> m_actions;  // manifest index -> its button
    std::vector<std::pair<size_t, wxButton *>> m_panels;   // manifest index -> its button
    wxStaticText *m_consent_text = nullptr;
    wxButton     *m_consent_button = nullptr;
    wxStaticText *m_state_text = nullptr;
    wxBoxSizer   *m_main_sizer = nullptr;

    // Open web panels by manifest index, so the session's panel_message events reach them.
    std::map<size_t, wxDialog *> m_open_panels;
};

} // namespace GUI
} // namespace Slic3r

#endif
