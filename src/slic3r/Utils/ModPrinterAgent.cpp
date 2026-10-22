#include "ModPrinterAgent.hpp"

#include "ModNetService.hpp"
#include "ModPrinterStatus.hpp"
#include "MoonrakerPrinterAgent.hpp"
#include "NetworkAgentFactory.hpp"

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/ModHost/NativeMods.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"

#include "../../../mods/sdk/include/magpie_slot_printer_agent.h"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <wx/app.h>
#include <wx/thread.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace Slic3r {

using json = nlohmann::json;
using ModHost::Mod;
using ModHost::NativeEntry;

// One session of the mod: created on connect_printer, destroyed when the last call using it returns after the agent
// let go of it (a call in flight keeps it alive, so destroy never runs under a call into the same session).
struct ModPrinterAgent::Session
{
    const NativeEntry   *entry   = nullptr;
    void                *session = nullptr;
    mp_printer_events_v1 events{};

    ~Session()
    {
        if (session == nullptr)
            return;
        const mp_printer_agent_v1 &t = *static_cast<const mp_printer_agent_v1 *>(entry->table);
        entry->mod->guarded_call([&]() {
            t.destroy(entry->user, session);
            return int32_t(MP_OK);
        });
    }
};

namespace {

const mp_printer_agent_v1 &table_of(const NativeEntry &entry) { return *static_cast<const mp_printer_agent_v1 *>(entry.table); }

bool usable(const NativeEntry &entry)
{
    return entry.version >= MP_PRINTER_AGENT_VERSION && entry.table_size >= sizeof(mp_printer_agent_v1) && entry.mod->manifest().thread_safe;
}

// Agents by registry id, for the printer panel; the factory's cache holds the same instances.
std::mutex                                             g_agents_mutex;
std::map<std::string, std::weak_ptr<ModPrinterAgent>> &g_agents = *new std::map<std::string, std::weak_ptr<ModPrinterAgent>>();

std::string field_section(const Mod &mod) { return "mod_printer:" + mod.id; }

// A printer address as a credential-store key part: IPv6 brackets dropped, lower case.
std::string host_key(std::string host)
{
    if (host.size() > 2 && host.front() == '[' && host.back() == ']')
        host = host.substr(1, host.size() - 2);
    std::transform(host.begin(), host.end(), host.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return host;
}

std::string json_text(const char *text) { return text != nullptr ? std::string(text) : std::string(); }

// The upload name: the file's own name without directories, characters printers choke on replaced, ".gcode" kept.
std::string upload_file_name(const std::string &path, const std::string &task_name)
{
    std::string name = boost::filesystem::path(path).filename().string();
    if (name.empty())
        name = task_name;
    for (char &c : name)
        if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|' ||
            static_cast<unsigned char>(c) < 0x20)
            c = '_';
    if (!boost::iends_with(name, ".gcode") && !boost::iends_with(name, ".gcode.3mf"))
        name += ".gcode";
    return name;
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------
// Registration

std::string ModPrinterAgent::agent_id_for(const NativeEntry &entry)
{
    const std::string &mod_id = entry.mod->id();
    return "mod:" + mod_id + (entry.impl == mod_id ? std::string() : "." + entry.impl);
}

void ModPrinterAgent::register_all()
{
    std::shared_ptr<const ModHost::NativeModSet> mods = ModHost::active_native_mods();
    if (!mods)
        return;
    for (const NativeEntry *entry : mods->slots(MP_PRINTER_AGENT_SLOT)) {
        if (!usable(*entry)) {
            BOOST_LOG_TRIVIAL(warning) << "Mod printer agent " << entry->mod->id()
                                       << ": not registered (slot version, table size, or the mod is not thread_safe)";
            continue;
        }
        const std::string id   = agent_id_for(*entry);
        const Mod        &mod  = entry->mod->manifest();
        const std::string name = mod.name.empty() ? mod.id : mod.name;
        NetworkAgentFactory::register_printer_agent(id, name,
            [id, name, entry](std::shared_ptr<ICloudServiceAgent> cloud, const std::string &) -> std::shared_ptr<IPrinterAgent> {
                auto agent = std::make_shared<ModPrinterAgent>(id, name, entry);
                if (cloud)
                    agent->set_cloud_agent(cloud);
                std::lock_guard<std::mutex> lock(g_agents_mutex);
                g_agents[id] = agent;
                return agent;
            });
    }
}

std::function<bool(const Mod &)> &ModPrinterAgent::consent_prompt()
{
    static std::function<bool(const Mod &)> prompt;
    return prompt;
}

ModPrinterAgent::ModPrinterAgent(std::string agent_id, std::string display_name, const NativeEntry *entry)
    : m_agent_id(std::move(agent_id)), m_display_name(std::move(display_name)), m_entry(entry)
{}

ModPrinterAgent::~ModPrinterAgent()
{
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_panel_listener = nullptr;
    }
    destroy_session();
}

const Mod &ModPrinterAgent::mod() const { return m_entry->mod->manifest(); }

AgentInfo ModPrinterAgent::get_agent_info()
{
    return AgentInfo{m_agent_id, m_display_name, mod().version, "Printer connection provided by the mod " + mod().id};
}

void ModPrinterAgent::set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_cloud_agent = std::move(cloud);
}

// ---------------------------------------------------------------------------------------------------------------
// Calls into the mod

bool ModPrinterAgent::call_mod(const std::function<int32_t(void *, void *, std::string &)> &fn, std::string &error)
{
    // This call's own reference: a disconnect meanwhile lets go of the agent's, and the session ends when this returns.
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        session = m_session;
    }
    if (!session) {
        error = _u8L("The printer is not connected.");
        return false;
    }
    std::string   message, guard_message;
    const int32_t rc = m_entry->mod->guarded_call([&]() { return fn(m_entry->user, session->session, message); }, &guard_message);
    if (rc == MP_OK)
        return true;
    if (rc == MP_ERR_NOT_FOUND) {
        error = "not_found";
        return false;
    }
    error = !message.empty() ? ModHost::mod_text(mod().id, message) : !guard_message.empty() ? guard_message : _u8L("The printer mod failed.");
    return false;
}

int ModPrinterAgent::send_neutral(const json &command, std::string *result, std::string *error)
{
    std::string err, answer;
    const std::string text = command.dump();
    const bool ok = call_mod([&](void *user, void *session, std::string &message) {
        char           *out = nullptr;
        const mp_status st  = table_of(*m_entry).send_command(user, session, text.c_str(), &out);
        if (out != nullptr) {
            answer = out;
            table_of(*m_entry).release(user, out);
        }
        message = json_text(st.message);
        return st.code;
    }, err);
    if (result)
        *result = answer;
    if (ok)
        return BAMBU_NETWORK_SUCCESS;
    if (error)
        *error = err == "not_found" ? std::string() : err;
    return err == "not_found" ? ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED : ORCA_NETWORK_ERR_CAP_NOT_AVAILABLE;
}

bool ModPrinterAgent::panel_command(const json &command, bool idle_only, std::string &result, std::string &error)
{
    if (idle_only) {
        const std::string state = status().value("state", std::string("idle"));
        if (state == "printing" || state == "paused" || state == "preparing") {
            error = _u8L("This cannot be done while the printer is printing.");
            return false;
        }
    }
    const int rc = send_neutral(command, &result, &error);
    if (rc == ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED && error.empty())
        error = _u8L("The printer mod does not know this command.");
    return rc == BAMBU_NETWORK_SUCCESS;
}

json ModPrinterAgent::status() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_status;
}

std::string ModPrinterAgent::connection_reason() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_conn_reason;
}

std::string ModPrinterAgent::connected_dev_id() const
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_conn_state.load() == MP_PRINTER_CONNECTED ? m_dev_id : std::string();
}

void ModPrinterAgent::set_panel_listener(std::function<void(const json &)> listener)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_panel_listener = std::move(listener);
}

bool ModPrinterAgent::camera_info(json &info, std::string &error)
{
    std::string text;
    const bool  ok = call_mod([&](void *user, void *session, std::string &message) {
        char           *out = nullptr;
        const mp_status st  = table_of(*m_entry).camera_info(user, session, &out);
        if (out != nullptr) {
            text = out;
            table_of(*m_entry).release(user, out);
        }
        message = json_text(st.message);
        return st.code;
    }, error);
    if (!ok)
        return false;
    info = json::parse(text, nullptr, false);
    if (!info.is_object()) {
        error = _u8L("The printer mod gave no camera information.");
        return false;
    }
    return true;
}

bool ModPrinterAgent::list_files(const json &query, json &files, std::string &error)
{
    std::string       text;
    const std::string q  = query.dump();
    const bool        ok = call_mod([&](void *user, void *session, std::string &message) {
        char           *out = nullptr;
        const mp_status st  = table_of(*m_entry).list_files(user, session, q.c_str(), &out);
        if (out != nullptr) {
            text = out;
            table_of(*m_entry).release(user, out);
        }
        message = json_text(st.message);
        return st.code;
    }, error);
    if (!ok)
        return false;
    files = json::parse(text, nullptr, false);
    return files.is_object();
}

// ---------------------------------------------------------------------------------------------------------------
// Connection

int ModPrinterAgent::connect_printer(const PrinterConnectionParams &params)
{
    if (params.dev_id.empty() || params.host.empty())
        return BAMBU_NETWORK_ERR_INVALID_HANDLE;

    // The user agrees to the mod's network list before its first connection (ModNetService checks it on every call).
    if (!mod().network.empty() && !ModNet::has_consent(mod())) {
        bool agreed = false;
        if (wxIsMainThread() && consent_prompt())
            agreed = consent_prompt()(mod());
        if (!agreed) {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            m_conn_reason = _u8L("Agree to the mod's network access in the printer connection panel first.");
            m_conn_state.store(MP_PRINTER_FAILED);
            notify_panel({{"type", "connection"}});
            return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
        }
    }

    destroy_session();

    auto session   = std::make_shared<Session>();
    session->entry = m_entry;
    session->events.size       = sizeof(mp_printer_events_v1);
    session->events.host       = this;
    session->events.status     = &ModPrinterAgent::on_status;
    session->events.connection = &ModPrinterAgent::on_connection;
    session->events.event      = &ModPrinterAgent::on_event;

    std::string message, guard_message;
    void       *mod_session = nullptr;
    const int32_t rc = m_entry->mod->guarded_call([&]() {
        const mp_status st = table_of(*m_entry).create(m_entry->user, &session->events, &mod_session);
        message            = json_text(st.message);
        return st.code;
    }, &guard_message);
    if (rc != MP_OK || mod_session == nullptr) {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_conn_reason = !message.empty() ? ModHost::mod_text(mod().id, message) : guard_message;
        m_conn_state.store(MP_PRINTER_FAILED);
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }
    session->session = mod_session;

    // The connection form's values for this printer (the printer panel stores them), and the preset's model.
    json conn = {{"dev_id", params.dev_id}, {"host", params.host}, {"port", params.port}};
    json fields = json::object();
    if (AppConfig *config = GUI::wxGetApp().app_config; config != nullptr) {
        const std::string section = field_section(mod());
        const std::string prefix  = host_key(params.host) + "/";
        if (config->has_section(section))
            for (const auto &[key, value] : config->get_section(section))
                if (boost::starts_with(key, prefix))
                    fields[key.substr(prefix.size())] = value;
    }
    conn["fields"] = std::move(fields);
    json profile   = json::object();
    if (PresetBundle *bundle = GUI::wxGetApp().preset_bundle; bundle != nullptr) {
        const Preset &preset     = bundle->printers.get_edited_preset();
        profile["printer_model"] = preset.config.opt_string("printer_model");
        profile["printer_name"]  = preset.name;
        if (auto *nozzle = preset.config.option<ConfigOptionFloats>("nozzle_diameter"); nozzle != nullptr && !nozzle->values.empty())
            profile["nozzle_diameter"] = nozzle->values.front();
    }
    conn["profile"] = std::move(profile);
    // The secret fields' values are in the credential store (core.net secret_get) under "<field id>.<host>", which the
    // printer panel wrote; the mod gets the store key of each.
    json secret_keys = json::object();
    for (const ModHost::ModGuiFormField &f : mod().gui.printer.fields)
        if (f.kind == "secret")
            secret_keys[f.id] = f.id + "." + host_key(params.host);
    conn["secret_keys"] = std::move(secret_keys);

    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_session       = std::move(session);
        m_dev_id        = params.dev_id;
        m_host          = params.host;
        m_status        = json::object();
        m_probe_info    = json::object();
        m_conn_reason.clear();
        m_was_connected = false;
        m_conn_state.store(MP_PRINTER_CONNECTING);
    }
    notify_panel({{"type", "connection"}});

    const std::string text = conn.dump();
    std::string       error;
    const bool        ok = call_mod([&](void *user, void *s, std::string &msg) {
        const mp_status st = table_of(*m_entry).connect(user, s, text.c_str());
        msg                = json_text(st.message);
        return st.code;
    }, error);
    if (!ok) {
        BOOST_LOG_TRIVIAL(warning) << "Mod printer agent " << m_agent_id << ": connect refused: " << ModNet::redact(error);
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        m_conn_reason = error;
        m_conn_state.store(MP_PRINTER_FAILED);
        notify_panel({{"type", "connection"}});
        return BAMBU_NETWORK_ERR_CONNECTION_TO_PRINTER_FAILED;
    }
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::disconnect_printer()
{
    destroy_session();
    m_conn_state.store(MP_PRINTER_DISCONNECTED);
    notify_panel({{"type", "connection"}});
    return BAMBU_NETWORK_SUCCESS;
}

void ModPrinterAgent::destroy_session()
{
    std::shared_ptr<Session> session;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        session = std::move(m_session);
    }
    if (session) {
        m_entry->mod->guarded_call([&]() {
            table_of(*m_entry).disconnect(m_entry->user, session->session);
            return int32_t(MP_OK);
        });
    }
    // ~Session calls destroy outside the lock (the mod's threads may be waiting in a callback for it), here or when
    // the last call in flight returns.
}

// ---------------------------------------------------------------------------------------------------------------
// Callbacks from the mod's threads

void ModPrinterAgent::on_status(void *host, const char *status_json)
{
    auto *self = static_cast<ModPrinterAgent *>(host);
    if (self == nullptr || status_json == nullptr)
        return;
    const json update = json::parse(status_json, nullptr, false);
    std::string dev_id, payload;
    {
        std::lock_guard<std::recursive_mutex> lock(self->m_mutex);
        if (!merge_neutral_status(self->m_status, update))
            return;
        dev_id  = self->m_dev_id;
        payload = neutral_status_to_device_push(self->m_status, &MoonrakerPrinterAgent::map_filament_type_to_generic_id).dump();
    }
    self->dispatch_message(dev_id, payload);
    self->notify_panel({{"type", "status"}});
}

void ModPrinterAgent::on_connection(void *host, int32_t state, const char *reason)
{
    auto *self = static_cast<ModPrinterAgent *>(host);
    if (self == nullptr)
        return;
    std::string dev_id;
    bool        was_connected = false;
    {
        std::lock_guard<std::recursive_mutex> lock(self->m_mutex);
        self->m_conn_reason = reason != nullptr ? ModHost::mod_text(self->mod().id, reason) : std::string();
        self->m_conn_state.store(state);
        dev_id        = self->m_dev_id;
        was_connected = self->m_was_connected;
        if (state == MP_PRINTER_CONNECTED)
            self->m_was_connected = true;
    }
    if (state == MP_PRINTER_CONNECTED) {
        self->dispatch_local_connect(ConnectStatusOk, dev_id, "0");
        self->dispatch_printer_connected(dev_id);
    } else if ((state == MP_PRINTER_DISCONNECTED || state == MP_PRINTER_FAILED) && was_connected) {
        // Lost after it worked. A first attempt that fails is not reported to the device code, which would drop the
        // selected machine (the Moonraker agent's reason); the printer panel and a notification say why instead.
        self->dispatch_local_connect(ConnectStatusLost, dev_id, "lost");
    }
    if (state == MP_PRINTER_FAILED) {
        const std::string text = self->m_display_name + ": " + self->connection_reason();
        BOOST_LOG_TRIVIAL(warning) << "Mod printer agent " << self->m_agent_id << ": " << ModNet::redact(self->connection_reason());
        if (wxTheApp != nullptr)
            GUI::wxGetApp().CallAfter([text]() {
                if (GUI::wxGetApp().plater() != nullptr)
                    GUI::wxGetApp().plater()->get_notification_manager()->push_notification(text);
            });
    }
    self->notify_panel({{"type", "connection"}});
}

void ModPrinterAgent::on_event(void *host, const char *event_json)
{
    auto *self = static_cast<ModPrinterAgent *>(host);
    if (self == nullptr || event_json == nullptr)
        return;
    json event = json::parse(event_json, nullptr, false);
    if (!event.is_object() || !event.contains("type") || !event["type"].is_string())
        return;
    if (event["type"] == "probe" && event.contains("info") && event["info"].is_object()) {
        // Who the connected printer is ({"model_id", "model_name", "name", "firmware", ...}), for the device list.
        std::lock_guard<std::recursive_mutex> lock(self->m_mutex);
        self->m_probe_info = event["info"];
    }
    if (event["type"] == "notice" && event.contains("text") && event["text"].is_string() && wxTheApp != nullptr) {
        const std::string text = self->m_display_name + ": " + ModHost::mod_text(self->mod().id, event["text"].get<std::string>());
        GUI::wxGetApp().CallAfter([text]() {
            if (GUI::wxGetApp().plater() != nullptr)
                GUI::wxGetApp().plater()->get_notification_manager()->push_notification(text);
        });
    }
    self->notify_panel(std::move(event));
}

void ModPrinterAgent::notify_panel(json event)
{
    if (wxTheApp == nullptr)
        return;
    std::weak_ptr<ModPrinterAgent> weak;
    {
        std::lock_guard<std::mutex> lock(g_agents_mutex);
        auto it = g_agents.find(m_agent_id);
        if (it != g_agents.end())
            weak = it->second;
    }
    // Through the registry's weak pointer: the agent may be gone by the time the GUI thread runs this.
    GUI::wxGetApp().CallAfter([weak, event = std::move(event)]() {
        std::shared_ptr<ModPrinterAgent> agent = weak.lock();
        if (!agent)
            return;
        std::function<void(const json &)> listener;
        {
            std::lock_guard<std::recursive_mutex> lock(agent->m_mutex);
            listener = agent->m_panel_listener;
        }
        if (listener)
            listener(event);
    });
}

void ModPrinterAgent::dispatch_message(const std::string &dev_id, const std::string &payload)
{
    OnMessageFn   local_fn;
    QueueOnMainFn queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        local_fn = m_on_local_message ? m_on_local_message : m_on_message;
        queue_fn = m_queue_on_main;
    }
    if (!local_fn || dev_id.empty())
        return;
    auto dispatch = [dev_id, payload, local_fn]() { local_fn(dev_id, payload); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void ModPrinterAgent::dispatch_local_connect(int state, const std::string &dev_id, const std::string &msg)
{
    OnLocalConnectedFn fn;
    QueueOnMainFn      queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        fn       = m_on_local_connect;
        queue_fn = m_queue_on_main;
    }
    if (!fn)
        return;
    auto dispatch = [state, dev_id, msg, fn]() { fn(state, dev_id, msg); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

void ModPrinterAgent::dispatch_printer_connected(const std::string &dev_id)
{
    OnPrinterConnectedFn fn;
    QueueOnMainFn        queue_fn;
    {
        std::lock_guard<std::recursive_mutex> lock(m_mutex);
        fn       = m_on_printer_connected;
        queue_fn = m_queue_on_main;
    }
    if (!fn)
        return;
    auto dispatch = [dev_id, fn]() { fn(dev_id); };
    if (queue_fn)
        queue_fn(dispatch);
    else
        dispatch();
}

// ---------------------------------------------------------------------------------------------------------------
// The Device tab's commands

int ModPrinterAgent::send_message(std::string dev_id, std::string json_str, int, int) { return handle_device_command(dev_id, json_str); }

int ModPrinterAgent::send_message_to_printer(std::string dev_id, std::string json_str, int, int)
{
    return handle_device_command(dev_id, json_str);
}

int ModPrinterAgent::handle_device_command(const std::string &dev_id, const std::string &json_str)
{
    const json command = json::parse(json_str, nullptr, false);
    if (command.is_discarded())
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    // The device code asks for the firmware versions once connected; answered from what the mod said about the printer.
    if (command.contains("info") && command["info"].is_object() && command["info"].value("command", std::string()) == "get_version") {
        json probe;
        {
            std::lock_guard<std::recursive_mutex> lock(m_mutex);
            probe = m_probe_info;
        }
        json module = {{"name", "ota"},
                       {"sw_ver", probe.value("firmware", std::string("0.0.0"))},
                       {"product_name", probe.value("model_name", m_display_name)}};
        dispatch_message(dev_id, json{{"info", {{"command", "get_version"}, {"result", "success"}, {"module", json::array({module})}}}}.dump());
        return BAMBU_NETWORK_SUCCESS;
    }
    std::optional<json> neutral = device_command_to_neutral(command);
    if (!neutral)
        return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED;
    std::string error;
    const int   rc = send_neutral(*neutral, nullptr, &error);
    if (rc != BAMBU_NETWORK_SUCCESS && !error.empty() && wxTheApp != nullptr) {
        const std::string text = m_display_name + ": " + error;
        GUI::wxGetApp().CallAfter([text]() {
            if (GUI::wxGetApp().plater() != nullptr)
                GUI::wxGetApp().plater()->get_notification_manager()->push_notification(text);
        });
    }
    return rc;
}

int ModPrinterAgent::command_go_home(std::string, bool, bool, int, bool) { return send_neutral({{"cmd", "home"}, {"axes", "xyz"}}); }

int ModPrinterAgent::command_axis_control(std::string, std::string axis, double unit, double input_val, int speed, bool, bool, int, bool)
{
    // The distance as the Device tab means it, mm; the printer's own axis directions are the mod's business.
    if (axis != "X" && axis != "Y" && axis != "Z" && axis != "E")
        return BAMBU_NETWORK_ERR_INVALID_RESULT;
    std::string lower = axis;
    lower[0]          = char(std::tolower(static_cast<unsigned char>(lower[0])));
    return send_neutral({{"cmd", axis == "E" ? "extrude" : "move"}, {"axis", lower}, {"distance_mm", unit * input_val}, {"speed_mm_min", speed}});
}

int ModPrinterAgent::command_auto_leveling(std::string, int, bool) { return send_neutral({{"cmd", "auto_level"}}); }

int ModPrinterAgent::command_xyz_abs(std::string, int, bool)
{
    // Orca's G90 before a move: the neutral move is relative already.
    return BAMBU_NETWORK_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------
// Discovery, binding, selection: a LAN printer at the preset's address, nothing to bind.

bool ModPrinterAgent::start_discovery(bool, bool) { return true; }

int ModPrinterAgent::bind_detect(std::string dev_ip, std::string, detectResult &detect)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    detect.dev_id       = m_dev_id.empty() ? dev_ip : m_dev_id;
    detect.model_id     = m_probe_info.value("model_id", std::string());
    detect.dev_name     = m_probe_info.value("name", dev_ip);
    detect.version      = m_probe_info.value("firmware", std::string());
    detect.connect_type = "lan";
    detect.bind_state   = "free";
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_server_callback(OnServerErrFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_server_err = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

std::string ModPrinterAgent::get_user_selected_machine()
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_selected_machine;
}

int ModPrinterAgent::set_user_selected_machine(std::string dev_id)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_selected_machine = std::move(dev_id);
    return BAMBU_NETWORK_SUCCESS;
}

// ---------------------------------------------------------------------------------------------------------------
// Printing

json ModPrinterAgent::start_params(const PrintParams &params, const std::string &file_name, const std::string &remote_path,
                                   const std::string &md5, uint64_t size, const std::string &printer_model,
                                   const std::vector<std::string> &filament_colours, const std::vector<std::string> &filament_types)
{
    // Tool i (0-based filament) -> global tray unit * 4 + slot, from ams_mapping2 ([{"ams_id", "slot_id"}]) when it
    // parses, else ams_mapping ([tray, ...]); a negative tray or ams_id 255 (the external spool) maps nothing.
    std::vector<int> trays;
    const json m2 = json::parse(params.ams_mapping2, nullptr, false);
    if (m2.is_array() && !m2.empty()) {
        for (const json &e : m2) {
            const int ams  = e.is_object() ? e.value("ams_id", -1) : -1;
            const int slot = e.is_object() ? e.value("slot_id", -1) : -1;
            trays.push_back(ams >= 0 && ams < 255 && slot >= 0 && slot < 4 ? ams * 4 + slot : -1);
        }
    } else {
        const json m1 = json::parse(params.ams_mapping, nullptr, false);
        if (m1.is_array())
            for (const json &e : m1)
                trays.push_back(e.is_number_integer() ? e.get<int>() : -1);
    }
    json mapping = json::array();
    for (size_t tool = 0; tool < trays.size(); ++tool) {
        if (trays[tool] < 0)
            continue;
        mapping.push_back({{"tool", int(tool)},
                           {"unit", trays[tool] / 4},
                           {"slot", trays[tool] % 4},
                           {"slot_index", trays[tool]},
                           {"color", tool < filament_colours.size() ? filament_colours[tool] : std::string()},
                           {"material", tool < filament_types.size() ? filament_types[tool] : std::string()}});
    }
    return {{"file_name", file_name},
            {"remote_path", remote_path},
            {"md5", md5},
            {"size", size},
            {"printer_model", printer_model},
            {"use_ams", params.task_use_ams && !mapping.empty()},
            {"ams_mapping", std::move(mapping)},
            {"options",
             {{"auto_leveling", params.task_bed_leveling},
              {"vibration_compensation", params.task_vibration_cali},
              {"flow_calibration", params.task_flow_cali},
              {"timelapse", params.task_record_timelapse}}}};
}

int ModPrinterAgent::upload_and_start(const PrintParams &params, bool start, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    if (update_fn)
        update_fn(PrintingStageCreate, 0, _u8L("Preparing..."));
    std::string path = !params.dst_file.empty() ? params.dst_file : params.filename;
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec))
        return BAMBU_NETWORK_ERR_FILE_NOT_EXIST;
    const uint64_t    size      = uint64_t(boost::filesystem::file_size(path, ec));
    std::string       md5;
    bbl_calc_md5(path, md5);
    const std::string file_name = upload_file_name(path, params.task_name);
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    // Upload, with the dialog's progress and cancel.
    struct Progress
    {
        OnUpdateStatusFn update;
        WasCancelledFn   cancelled;
    } progress{update_fn, cancel_fn};
    auto progress_fn = [](void *user, int32_t percent) -> int32_t {
        auto *p = static_cast<Progress *>(user);
        if (p->update)
            p->update(PrintingStageUpload, std::clamp(percent, 0, 100), _u8L("Uploading G-code..."));
        return p->cancelled && p->cancelled() ? 1 : 0;
    };
    if (update_fn)
        update_fn(PrintingStageUpload, 0, _u8L("Uploading G-code..."));
    const json        upload_params = {{"file_name", file_name}, {"md5", md5}, {"size", size}};
    const std::string upload_text   = upload_params.dump();
    std::string       upload_result, error;
    bool ok = call_mod([&](void *user, void *session, std::string &message) {
        char           *out = nullptr;
        const mp_status st  = table_of(*m_entry).upload(user, session, path.c_str(), upload_text.c_str(), progress_fn, &progress, &out);
        if (out != nullptr) {
            upload_result = out;
            table_of(*m_entry).release(user, out);
        }
        message = json_text(st.message);
        return st.code;
    }, error);
    if (!ok) {
        if (cancel_fn && cancel_fn())
            return BAMBU_NETWORK_ERR_CANCELED;
        if (update_fn)
            update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED, error);
        return BAMBU_NETWORK_ERR_PRINT_LP_UPLOAD_FTP_FAILED;
    }
    if (!start) {
        if (update_fn)
            update_fn(PrintingStageFinished, 100, _u8L("File uploaded"));
        return BAMBU_NETWORK_SUCCESS;
    }
    if (cancel_fn && cancel_fn())
        return BAMBU_NETWORK_ERR_CANCELED;

    // The project's filament colours and types, 1-based tools in order, and the preset's printer model.
    std::vector<std::string> colours, types;
    std::string              printer_model;
    if (PresetBundle *bundle = GUI::wxGetApp().preset_bundle; bundle != nullptr) {
        if (auto *c = bundle->project_config.option<ConfigOptionStrings>("filament_colour"); c != nullptr)
            colours = c->values;
        const DynamicPrintConfig full = bundle->full_config();
        if (auto *t = full.option<ConfigOptionStrings>("filament_type"); t != nullptr)
            types = t->values;
        printer_model = bundle->printers.get_edited_preset().config.opt_string("printer_model");
    }
    const json        uploaded    = json::parse(upload_result, nullptr, false);
    const std::string remote_path = uploaded.is_object() ? uploaded.value("remote_path", std::string()) : std::string();
    const std::string start_text  = start_params(params, file_name, remote_path, md5, size, printer_model, colours, types).dump();

    if (update_fn)
        update_fn(PrintingStageSending, 0, _u8L("Starting print..."));
    ok = call_mod([&](void *user, void *session, std::string &message) {
        char           *out = nullptr;
        const mp_status st  = table_of(*m_entry).start_print(user, session, start_text.c_str(), &out);
        if (out != nullptr)
            table_of(*m_entry).release(user, out);
        message = json_text(st.message);
        return st.code;
    }, error);
    if (!ok) {
        // The mod refuses a start it judges unsafe (another printer model, a tray of another material) with a reason.
        if (update_fn)
            update_fn(PrintingStageERROR, BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED, error);
        return BAMBU_NETWORK_ERR_PRINT_LP_PUBLISH_MSG_FAILED;
    }
    if (update_fn)
        update_fn(PrintingStageFinished, 100, _u8L("Print started"));
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return upload_and_start(params, true, update_fn, cancel_fn);
}

int ModPrinterAgent::start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return upload_and_start(params, true, update_fn, cancel_fn);
}

int ModPrinterAgent::start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn)
{
    return upload_and_start(params, false, update_fn, cancel_fn);
}

int ModPrinterAgent::start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn)
{
    return upload_and_start(params, true, update_fn, cancel_fn);
}

int ModPrinterAgent::start_sdcard_print(PrintParams, OnUpdateStatusFn, WasCancelledFn) { return ORCA_NETWORK_ERR_CMD_NOT_SUPPORTED; }

// ---------------------------------------------------------------------------------------------------------------
// Callback registration

int ModPrinterAgent::set_on_ssdp_msg_fn(OnMsgArrivedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_ssdp_msg = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_on_printer_connected_fn(OnPrinterConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_printer_connected = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_on_subscribe_failure_fn(GetSubscribeFailureFn) { return BAMBU_NETWORK_SUCCESS; }

int ModPrinterAgent::set_on_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_on_user_message_fn(OnMessageFn) { return BAMBU_NETWORK_SUCCESS; }

int ModPrinterAgent::set_on_local_connect_fn(OnLocalConnectedFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_local_connect = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_on_local_message_fn(OnMessageFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_on_local_message = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

int ModPrinterAgent::set_queue_on_main_fn(QueueOnMainFn fn)
{
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    m_queue_on_main = std::move(fn);
    return BAMBU_NETWORK_SUCCESS;
}

} // namespace Slic3r
