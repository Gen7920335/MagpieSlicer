#include "ModPrinterDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/TextInput.hpp"
#include "Widgets/WebView.hpp"
#include "format.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModHost/Expr.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"
#include "slic3r/Utils/ModNetService.hpp"
#include "slic3r/Utils/ModPrinterAgent.hpp"
#include "slic3r/Utils/NetworkAgentFactory.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>

#include <wx/filename.h>
#include <wx/filepicker.h>
#include <wx/msgdlg.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>
#include <wx/utils.h>
#include <wx/webview.h>

#include <optional>
#include <sstream>

namespace Slic3r {
namespace GUI {

using json = nlohmann::json;
using ModHost::Mod;

namespace {

// The mod's text for the UI language: its own translation, else the English one.
wxString mod_label(const std::map<std::string, std::string> &translations, const std::string &fallback)
{
    return wxString::FromUTF8(ModHost::localized_text(translations, ModHost::message_language(), fallback).c_str());
}

std::string mod_name(const Mod &mod)
{
    return ModHost::localized_text(mod.names, ModHost::message_language(), mod.name.empty() ? mod.id : mod.name);
}

// The network list as the user reads it: one line per host with its ports and purpose.
wxString network_list_text(const Mod &mod)
{
    std::ostringstream out;
    for (const ModHost::ModNetworkEndpoint &ep : mod.network.endpoints) {
        // TRN The network list of a mod: "lan" stands for the user's own local network.
        out << "  - " << (ep.host == "lan" ? _u8L("printers on your local network") : ep.host) << ", "
            << _u8L("ports") << " ";
        for (size_t i = 0; i < ep.ports.size(); ++i)
            out << (i ? ", " : "") << ep.ports[i];
        if (!ep.purpose.empty())
            out << " (" << ep.purpose << ")";
        out << "\n";
    }
    return wxString::FromUTF8(out.str().c_str());
}

} // namespace

// One form field: its manifest entry and its control.
struct ModPrinterDialog::FieldCtrl
{
    const ModHost::ModGuiFormField *def = nullptr;
    wxWindow                       *label = nullptr;
    wxWindow                       *ctrl  = nullptr;
    wxStaticText                   *help  = nullptr;

    std::string value() const
    {
        if (auto *text = dynamic_cast<::TextInput *>(ctrl))
            return into_u8(text->GetTextCtrl()->GetValue());
        if (auto *choice = dynamic_cast<::ComboBox *>(ctrl)) {
            const int i = choice->GetSelection();
            return i >= 0 && size_t(i) < def->options.size() ? def->options[size_t(i)].value : std::string();
        }
        if (auto *picker = dynamic_cast<wxDirPickerCtrl *>(ctrl))
            return into_u8(picker->GetPath());
        return {};
    }

    void set_value(const std::string &v)
    {
        if (auto *text = dynamic_cast<::TextInput *>(ctrl))
            text->GetTextCtrl()->SetValue(from_u8(v));
        else if (auto *choice = dynamic_cast<::ComboBox *>(ctrl)) {
            for (size_t i = 0; i < def->options.size(); ++i)
                if (def->options[i].value == v)
                    choice->SetSelection(int(i));
        } else if (auto *picker = dynamic_cast<wxDirPickerCtrl *>(ctrl))
            picker->SetPath(from_u8(v));
    }

    void show(bool on)
    {
        label->Show(on);
        ctrl->Show(on);
        if (help)
            help->Show(on);
    }
};

ModPrinterDialog::ModPrinterDialog(wxWindow *parent, std::shared_ptr<ModPrinterAgent> agent, const std::string &host)
    : DPIDialog(parent, wxID_ANY,
                // TRN %1% is the name of a mod that connects a printer.
                format_wxstr(_L("%1%: printer connection"), mod_name(agent->mod())), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_agent(std::move(agent))
    , m_host(ModPrinterAgent::store_host(host))
    , m_language(ModHost::message_language())
{
    SetBackgroundColour(*wxWHITE); // the light palette colour; UpdateDlgDarkUI maps it in dark mode
    build();
    load_fields();
    update_visibility();
    update_state();
    m_agent->set_panel_listener([this](const json &event) { on_agent_event(event); });
    wxGetApp().UpdateDlgDarkUI(this);
}

ModPrinterDialog::~ModPrinterDialog() { m_agent->set_panel_listener(nullptr); }

void ModPrinterDialog::build()
{
    const Mod                   &mod     = m_agent->mod();
    const ModHost::ModGuiPrinter &printer = mod.gui.printer;
    m_main_sizer = new wxBoxSizer(wxVERTICAL);
    const int gap = FromDIP(8);

    // TRN %1% is the printer's network address.
    m_main_sizer->Add(new wxStaticText(this, wxID_ANY, format_wxstr(_L("Printer address: %1%"), m_host.empty() ? std::string("-") : m_host)),
                      0, wxALL, gap);

    // The network list and the agreement.
    if (!mod.network.empty()) {
        m_consent_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
        m_consent_button = new ::Button(this, wxEmptyString);
        m_consent_button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        m_consent_button->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
            const Mod &mod = m_agent->mod();
            if (ModNet::has_consent(mod))
                ModNet::set_consent(mod, false);
            else
                ask_network_consent(this, mod);
            update_state();
        });
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        row->Add(m_consent_text, 1, wxALIGN_CENTER_VERTICAL);
        row->Add(m_consent_button, 0, wxLEFT, gap);
        m_main_sizer->Add(row, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
    }

    // Notices.
    for (size_t i = 0; i < printer.notices.size(); ++i) {
        auto *text = new wxStaticText(this, wxID_ANY, mod_label(printer.notices[i].texts, printer.notices[i].text));
        text->Wrap(FromDIP(520));
        text->SetForegroundColour(wxColour(0xE0, 0x8A, 0x00));
        m_main_sizer->Add(text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);
        m_notices.emplace_back(i, text);
    }

    // The connection form.
    auto *grid = new wxFlexGridSizer(2, FromDIP(4), gap);
    grid->AddGrowableCol(1);
    for (const ModHost::ModGuiFormField &f : printer.fields) {
        auto field   = std::make_unique<FieldCtrl>();
        field->def   = &f;
        field->label = new wxStaticText(this, wxID_ANY, mod_label(f.labels, f.label.empty() ? f.id : f.label));
        if (f.kind == "choice") {
            auto *choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(320), -1), 0, nullptr, wxCB_READONLY);
            for (const ModHost::ModParamOption &o : f.options)
                choice->Append(mod_label(o.labels, o.label));
            choice->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent &e) {
                update_visibility();
                e.Skip();
            });
            field->ctrl = choice;
        } else if (f.kind == "path") {
            field->ctrl = new wxDirPickerCtrl(this, wxID_ANY);
        } else {
            auto *text = new ::TextInput(this, wxEmptyString, "", "", wxDefaultPosition, wxSize(FromDIP(320), -1),
                                         f.kind == "secret" ? wxTE_PASSWORD : 0);
            text->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
                update_visibility();
                e.Skip();
            });
            field->ctrl = text;
        }
        const wxString help = mod_label(f.helps, f.help);
        if (!help.empty())
            field->ctrl->SetToolTip(help);
        grid->Add(field->label, 0, wxALIGN_CENTER_VERTICAL);
        grid->Add(field->ctrl, 1, wxEXPAND);
        m_fields.push_back(std::move(field));
    }
    if (!printer.fields.empty())
        m_main_sizer->Add(grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // Connection and print state.
    m_state_text = new wxStaticText(this, wxID_ANY, wxEmptyString);
    m_main_sizer->Add(m_state_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // Actions, a line per row in manifest order.
    std::vector<std::string>             rows;
    std::map<std::string, wxBoxSizer *>  row_sizers;
    for (size_t i = 0; i < printer.actions.size(); ++i) {
        const ModHost::ModGuiAction &a = printer.actions[i];
        if (row_sizers.find(a.row) == row_sizers.end()) {
            rows.push_back(a.row);
            row_sizers[a.row] = new wxBoxSizer(wxHORIZONTAL);
        }
        auto *button = new ::Button(this, mod_label(a.labels, a.label.empty() ? a.id : a.label));
        button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
        button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { on_action(i); });
        row_sizers[a.row]->Add(button, 0, wxRIGHT, FromDIP(4));
        m_actions.emplace_back(i, button);
    }
    for (const std::string &row : rows)
        m_main_sizer->Add(row_sizers[row], 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);

    // Web panels.
    if (!printer.panels.empty()) {
        auto *row = new wxBoxSizer(wxHORIZONTAL);
        for (size_t i = 0; i < printer.panels.size(); ++i) {
            auto *button = new ::Button(this, mod_label(printer.panels[i].labels, printer.panels[i].label.empty() ? printer.panels[i].id : printer.panels[i].label));
            button->SetStyle(ButtonStyle::Regular, ButtonType::Window);
            button->Bind(wxEVT_BUTTON, [this, i](wxCommandEvent &) { open_web_panel(i); });
            row->Add(button, 0, wxRIGHT, FromDIP(4));
            m_panels.emplace_back(i, button);
        }
        m_main_sizer->Add(row, 0, wxLEFT | wxRIGHT | wxBOTTOM, gap);
    }

    // Save keeps the form; the connection uses it the next time the printer connects.
    auto *buttons = new DialogButtons(this, {"Save", "Close"}, _L("Save"));
    buttons->GetButtonFromIndex(0)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) {
        if (save_fields())
            EndModal(wxID_OK);
    });
    buttons->GetButtonFromIndex(1)->Bind(wxEVT_BUTTON, [this](wxCommandEvent &) { EndModal(wxID_CANCEL); });
    m_main_sizer->Add(buttons, 0, wxEXPAND);

    SetSizerAndFit(m_main_sizer);
    CenterOnParent();
}

void ModPrinterDialog::load_fields()
{
    const Mod  &mod     = m_agent->mod();
    AppConfig  *config  = wxGetApp().app_config;
    const std::string section = ModPrinterAgent::fields_section(mod);
    for (auto &field : m_fields) {
        if (field->def->kind == "secret") {
            // Never read back into the window: an empty box keeps the stored secret, a hint says one is stored.
            if (ModNet::has_secret(mod.id, field->def->id + "." + m_host))
                if (auto *text = dynamic_cast<::TextInput *>(field->ctrl))
                    text->GetTextCtrl()->SetHint(_L("(stored; type to replace)"));
            continue;
        }
        std::string value = field->def->default_value;
        if (config != nullptr && config->has(section, m_host + "/" + field->def->id))
            value = config->get(section, m_host + "/" + field->def->id);
        field->set_value(value);
    }
}

bool ModPrinterDialog::save_fields()
{
    if (m_host.empty()) {
        MessageDialog(this, _L("Set the printer's address (Host) in the printer settings first."), _L("Printer connection"), wxOK | wxICON_WARNING).ShowModal();
        return false;
    }
    const Mod &mod    = m_agent->mod();
    AppConfig *config = wxGetApp().app_config;
    for (auto &field : m_fields) {
        const std::string value = field->value();
        if (field->def->kind == "secret") {
            if (!value.empty() && !ModNet::put_secret(mod.id, field->def->id + "." + m_host, value)) {
                MessageDialog(this, _L("The system's credential store did not accept the value."), _L("Printer connection"), wxOK | wxICON_ERROR).ShowModal();
                return false;
            }
            continue;
        }
        if (config != nullptr)
            config->set(ModPrinterAgent::fields_section(mod), m_host + "/" + field->def->id, value);
    }
    return true;
}

void ModPrinterDialog::update_visibility()
{
    const ModHost::ModGuiPrinter &printer = m_agent->mod().gui.printer;
    const std::string             state   = m_agent->status().value("state", std::string());
    // Conditions read the form's own values by field id, and "printer_state" (the neutral state, "" when unknown).
    const ModHost::ExprLookup lookup = [this, &state](const std::string &name, std::optional<size_t>) -> std::optional<ModHost::ExprValue> {
        if (name == "printer_state")
            return ModHost::ExprValue(state);
        for (const auto &field : m_fields)
            if (field->def->id == name)
                return ModHost::ExprValue(field->value());
        return std::nullopt;
    };
    // A condition that cannot be evaluated shows the item, so a broken condition never hides one.
    auto visible = [&lookup](const std::optional<ModHost::Expr> &cond) {
        if (!cond)
            return true;
        try {
            return ModHost::expr_truthy(cond->evaluate(lookup));
        } catch (const std::exception &) {
            return true;
        }
    };
    for (auto &field : m_fields)
        field->show(visible(field->def->visible));
    for (auto &[i, w] : m_notices)
        w->Show(visible(printer.notices[i].visible));
    for (auto &[i, b] : m_actions)
        b->Show(visible(printer.actions[i].visible));
    for (auto &[i, b] : m_panels)
        b->Show(visible(printer.panels[i].visible));
    Layout();
    GetSizer()->SetSizeHints(this);
}

void ModPrinterDialog::update_state()
{
    const Mod &mod = m_agent->mod();
    if (m_consent_text != nullptr) {
        const bool agreed = ModNet::has_consent(mod);
        m_consent_text->SetLabel(agreed ? _L("Network access: allowed.") : _L("Network access: not allowed yet. The printer cannot connect until you allow it."));
        m_consent_button->SetLabel(agreed ? _L("Withdraw") : _L("Review and allow..."));
    }
    wxString text;
    switch (m_agent->connection_state()) {
    case 0: text = _L("Not connected."); break;
    case 1: text = _L("Connecting..."); break;
    case 2: text = _L("Connected."); break;
    default: text = _L("Connection failed."); break;
    }
    const std::string reason = m_agent->connection_reason();
    if (!reason.empty())
        text += " " + wxString::FromUTF8(reason.c_str());
    const json status = m_agent->status();
    if (status.contains("state") && status["state"].is_string()) {
        text += "\n" + format_wxstr(_L("State: %1%"), status["state"].get<std::string>());
        if (status.contains("progress") && status["progress"].is_number())
            text += "  " + format_wxstr(_L("Progress: %1%"), std::to_string(int(status["progress"].get<double>())) + "%");
    }
    if (status.contains("errors") && status["errors"].is_array())
        for (const json &e : status["errors"])
            if (e.is_object() && e.contains("text") && e["text"].is_string())
                text += "\n" + wxString::FromUTF8(ModHost::mod_text(mod.id, e["text"].get<std::string>()).c_str());
    m_state_text->SetLabel(text);
    m_state_text->Wrap(FromDIP(520));
    update_visibility();
}

void ModPrinterDialog::on_action(size_t index)
{
    const ModHost::ModGuiAction &a = m_agent->mod().gui.printer.actions[index];
    if (!a.confirm.empty()) {
        MessageDialog dlg(this, mod_label(a.confirms, a.confirm), mod_label(a.labels, a.label), wxYES_NO | wxICON_QUESTION);
        if (dlg.ShowModal() != wxID_YES)
            return;
    }
    json command = json::object();
    if (!a.args_json.empty())
        command = json::parse(a.args_json, nullptr, false);
    if (!command.is_object())
        command = json::object();
    command["cmd"] = a.command;
    std::string result, error;
    wxBusyCursor busy;
    if (!m_agent->panel_command(command, a.idle_only, result, error))
        MessageDialog(this, wxString::FromUTF8(error.c_str()), mod_label(a.labels, a.label), wxOK | wxICON_WARNING).ShowModal();
}

void ModPrinterDialog::open_web_panel(size_t index)
{
    if (auto it = m_open_panels.find(index); it != m_open_panels.end()) {
        it->second->Raise();
        return;
    }
    const Mod                    &mod   = m_agent->mod();
    const ModHost::ModGuiWebPanel &panel = mod.gui.printer.panels[index];
    const boost::filesystem::path page  = boost::filesystem::path(mod.dir) / panel.page;
    if (mod.dir.empty() || !boost::filesystem::exists(page)) {
        MessageDialog(this, _L("The mod's page is missing from its folder."), mod_label(panel.labels, panel.label), wxOK | wxICON_ERROR).ShowModal();
        return;
    }
    auto *dlg  = new wxDialog(this, wxID_ANY, mod_label(panel.labels, panel.label), wxDefaultPosition, wxSize(FromDIP(800), FromDIP(520)),
                              wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER);
    wxWebView *view = WebView::CreateWebView(dlg, wxFileName::FileNameToURL(wxString::FromUTF8(page.string().c_str())));
    if (view == nullptr) {
        dlg->Destroy();
        MessageDialog(this, _L("The web view is not available on this system."), mod_label(panel.labels, panel.label), wxOK | wxICON_ERROR).ShowModal();
        return;
    }
    auto *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(view, 1, wxEXPAND);
    dlg->SetSizer(sizer);
    // The page's window.wx.postMessage(text) reaches the session as {"cmd": "panel_message", "panel", "data"}.
    const std::string panel_id = panel.id;
    // Not on the script-message callback's stack: the session may answer with an event that runs script in this view.
    std::weak_ptr<ModPrinterAgent> weak_agent = m_agent;
    view->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, [weak_agent, panel_id](wxWebViewEvent &e) {
        const std::string text = into_u8(e.GetString());
        wxGetApp().CallAfter([weak_agent, panel_id, text]() {
            std::shared_ptr<ModPrinterAgent> agent = weak_agent.lock();
            if (!agent)
                return;
            json data = json::parse(text, nullptr, false);
            if (data.is_discarded())
                data = text;
            std::string result, error;
            agent->panel_command({{"cmd", "panel_message"}, {"panel", panel_id}, {"data", std::move(data)}}, false, result, error);
        });
    });
    dlg->Bind(wxEVT_CLOSE_WINDOW, [this, index, dlg](wxCloseEvent &) {
        m_open_panels.erase(index);
        std::string result, error;
        m_agent->panel_command({{"cmd", "panel_closed"}, {"panel", m_agent->mod().gui.printer.panels[index].id}}, false, result, error);
        dlg->Destroy();
    });
    m_open_panels[index] = dlg;
    dlg->Show();
}

void ModPrinterDialog::on_agent_event(const json &event)
{
    const std::string type = event.value("type", std::string());
    if (type == "status" || type == "connection") {
        update_state();
        return;
    }
    if (type == "panel_message") {
        // To the page of the panel it names: window.magpiePanelMessage(object).
        const std::string panel_id = event.value("panel", std::string());
        const auto       &panels   = m_agent->mod().gui.printer.panels;
        for (auto &[index, dlg] : m_open_panels) {
            if (panels[index].id != panel_id)
                continue;
            for (wxWindow *child : dlg->GetChildren())
                if (auto *view = dynamic_cast<wxWebView *>(child)) {
                    const json data = event.contains("data") ? event["data"] : json();
                    WebView::RunScript(view, wxString::FromUTF8(("window.magpiePanelMessage && window.magpiePanelMessage(" + data.dump() + ");").c_str()));
                }
        }
    }
}

void ModPrinterDialog::on_dpi_changed(const wxRect &)
{
    for (auto &[i, b] : m_actions)
        b->Rescale();
    for (auto &[i, b] : m_panels)
        b->Rescale();
    if (m_consent_button)
        m_consent_button->Rescale();
    GetSizer()->SetSizeHints(this);
    Refresh();
}

bool ModPrinterDialog::ask_network_consent(wxWindow *parent, const Mod &mod)
{
    // TRN %1% is a mod's name, %2% the reason the mod gives, %3% the list of addresses and ports it reaches.
    const wxString text = format_wxstr(_L("The mod \"%1%\" connects to:\n\n%3%\n%2%\n\nAllow it? You can withdraw this in the printer connection panel."),
                                       mod_name(mod), ModHost::localized_text(mod.network.reasons, ModHost::message_language(), mod.network.reason),
                                       network_list_text(mod));
    MessageDialog dlg(parent, text, _L("Network access"), wxYES_NO | wxICON_QUESTION);
    const bool agreed = dlg.ShowModal() == wxID_YES;
    ModNet::set_consent(mod, agreed);
    return agreed;
}

void ModPrinterDialog::install_hooks()
{
    ModPrinterAgent::consent_prompt() = [](const Mod &mod) { return ask_network_consent(wxGetApp().GetTopWindow(), mod); };
}

void ModPrinterDialog::show_for_edited_printer(wxWindow *parent)
{
    const DynamicPrintConfig &config   = wxGetApp().preset_bundle->printers.get_edited_preset().config;
    const std::string         agent_id = config.opt_string("printer_agent");
    std::shared_ptr<IPrinterAgent> agent;
    if (boost::starts_with(agent_id, "mod:"))
        agent = NetworkAgentFactory::create_printer_agent_by_id(agent_id, nullptr, data_dir());
    auto mod_agent = std::dynamic_pointer_cast<ModPrinterAgent>(agent);
    if (!mod_agent) {
        MessageDialog(parent, _L("Choose a mod's printer connection under Printer Agent first."), _L("Printer connection"), wxOK | wxICON_INFORMATION).ShowModal();
        return;
    }
    // The host as MachineObject::connect passes it to the agent: no scheme, no port.
    const std::string host = Http::get_host_from_url(config.opt_string("print_host"));
    ModPrinterDialog  dlg(parent, mod_agent, host.empty() ? config.opt_string("print_host") : host);
    dlg.ShowModal();
}

} // namespace GUI
} // namespace Slic3r
