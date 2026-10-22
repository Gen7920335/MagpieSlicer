#ifndef slic3r_ModPrinterAgent_hpp_
#define slic3r_ModPrinterAgent_hpp_

// MAGPIE: a printer agent backed by a native mod's "printer.agent" slot (mods/sdk/include/magpie_slot_printer_agent.h).
// Each loaded implementation is registered with Orca's agent registry as "mod:<mod id>" (".<impl>" appended when the
// implementation is not named after its mod), so a printer preset's "Printer Agent" setting picks it and Orca's Device
// tab, AMS view and send dialog drive the printer. The mod speaks the Magpie neutral status and commands
// (ModPrinterStatus.hpp); this class only translates and carries them. Vendor-neutral: nothing here knows a printer
// brand.

#include "IPrinterAgent.hpp"
#include "bambu_networking.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {

class ICloudServiceAgent;

namespace ModHost {
struct Mod;
struct NativeEntry;
} // namespace ModHost

class ModPrinterAgent : public IPrinterAgent
{
public:
    ModPrinterAgent(std::string agent_id, std::string display_name, const ModHost::NativeEntry *entry);
    ~ModPrinterAgent() override;

    // Registers every loaded printer.agent implementation. Called by NetworkAgentFactory::register_all_agents, after
    // the mods loaded; a second call registers nothing new.
    static void register_all();
    // The registry id of an implementation: "mod:<mod id>", or "mod:<mod id>.<impl>".
    static std::string agent_id_for(const ModHost::NativeEntry &entry);

    // Asks the user to agree to a mod's network list; true when they did. The printer panel installs it; without
    // it (or off the GUI thread) a connection to a mod without agreement fails with a reason saying so.
    static std::function<bool(const ModHost::Mod &)> &consent_prompt();

    // The mod and its manifest.
    const ModHost::Mod &mod() const;

    // ---- The printer panel's side (ModPrinterDialog) ----
    // A neutral command from the panel's actions, refused while printing when idle_only. Blocking; worker or GUI thread.
    // Returns the mod's answer text, or an error for the user in `error`.
    bool panel_command(const nlohmann::json &command, bool idle_only, std::string &result, std::string &error);
    // The merged neutral status, an empty object before the first one.
    nlohmann::json status() const;
    // The connection: MP_PRINTER_* state and the mod's last reason text.
    int  connection_state() const { return m_conn_state.load(); }
    std::string connection_reason() const;
    // The connected printer's dev_id, empty when none.
    std::string connected_dev_id() const;
    // Called on the GUI thread with every event the mod sends ("notice", "panel_message", ...) and with
    // {"type": "status"} / {"type": "connection"} when those change. One listener; nullptr removes it.
    void set_panel_listener(std::function<void(const nlohmann::json &)> listener);
    // The mod's answer to camera_info, or an error.
    bool camera_info(nlohmann::json &info, std::string &error);
    bool list_files(const nlohmann::json &query, nlohmann::json &files, std::string &error);

    // ---- IPrinterAgent ----
    AgentInfo get_agent_info() override;
    void      set_cloud_agent(std::shared_ptr<ICloudServiceAgent> cloud) override;

    int send_message(std::string dev_id, std::string json_str, int qos, int flag) override;
    int send_message_to_printer(std::string dev_id, std::string json_str, int qos, int flag) override;
    int connect_printer(const PrinterConnectionParams &params) override;
    int disconnect_printer() override;

    // The Device tab's controls whose default form is G-code: sent as neutral commands instead.
    int command_go_home(std::string dev_id, bool is_printing, bool supports_mqtt_homing, int sequence_id, bool lan_mode) override;
    int command_axis_control(std::string dev_id, std::string axis, double unit, double input_val, int speed, bool is_core_xy,
                             bool supports_mqtt_axis_control, int sequence_id, bool lan_mode) override;
    int command_auto_leveling(std::string dev_id, int sequence_id, bool lan_mode) override;
    int command_xyz_abs(std::string dev_id, int sequence_id, bool lan_mode) override;

    bool start_discovery(bool start, bool sending) override;
    int  bind_detect(std::string dev_ip, std::string sec_link, detectResult &detect) override;
    int  set_server_callback(OnServerErrFn fn) override;
    std::string get_user_selected_machine() override;
    int         set_user_selected_machine(std::string dev_id) override;

    int start_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print_with_record(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_send_gcode_to_sdcard(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn, OnWaitFn wait_fn) override;
    int start_local_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;
    int start_sdcard_print(PrintParams params, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn) override;

    int set_on_ssdp_msg_fn(OnMsgArrivedFn fn) override;
    int set_on_printer_connected_fn(OnPrinterConnectedFn fn) override;
    int set_on_subscribe_failure_fn(GetSubscribeFailureFn fn) override;
    int set_on_message_fn(OnMessageFn fn) override;
    int set_on_user_message_fn(OnMessageFn fn) override;
    int set_on_local_connect_fn(OnLocalConnectedFn fn) override;
    int set_on_local_message_fn(OnMessageFn fn) override;
    int set_queue_on_main_fn(QueueOnMainFn fn) override;

    FilamentSyncMode get_filament_sync_mode() const override { return FilamentSyncMode::subscription; }
    CameraStreamMode get_camera_stream_mode() const override { return CameraStreamMode::none; }

    // The neutral start_print parameters for an Orca print job (exposed for tests): file, mapping and options.
    // `filament_colours`/`filament_types` are the project's, 1-based tool order.
    static nlohmann::json start_params(const PrintParams &params, const std::string &file_name, const std::string &remote_path,
                                       const std::string &md5, uint64_t size, const std::string &printer_model,
                                       const std::vector<std::string> &filament_colours, const std::vector<std::string> &filament_types);

private:
    struct Session;

    // Runs `fn` on the mod's session under its crash guard; false with `error` set when the mod refused or crashed.
    bool call_mod(const std::function<int32_t(void *user, void *session, std::string &message)> &fn, std::string &error);
    int  handle_device_command(const std::string &dev_id, const std::string &json_str);
    int  send_neutral(const nlohmann::json &command, std::string *result = nullptr, std::string *error = nullptr);
    int  upload_and_start(const PrintParams &params, bool start, OnUpdateStatusFn update_fn, WasCancelledFn cancel_fn);

    // Event callbacks from the mod's threads.
    static void on_status(void *host, const char *status_json);
    static void on_connection(void *host, int32_t state, const char *reason);
    static void on_event(void *host, const char *event_json);

    void dispatch_message(const std::string &dev_id, const std::string &payload);
    void dispatch_local_connect(int state, const std::string &dev_id, const std::string &msg);
    void dispatch_printer_connected(const std::string &dev_id);
    void notify_panel(nlohmann::json event);
    void destroy_session();

    std::string                    m_agent_id;
    std::string                    m_display_name;
    const ModHost::NativeEntry    *m_entry = nullptr;

    mutable std::recursive_mutex   m_mutex; // everything below except the atomics
    std::shared_ptr<Session>       m_session; // a call in flight holds its own reference (Session)
    std::string                    m_dev_id;
    std::string                    m_host;
    nlohmann::json                 m_probe_info = nlohmann::json::object();
    nlohmann::json                 m_status     = nlohmann::json::object();
    std::string                    m_conn_reason;
    std::string                    m_selected_machine;
    std::shared_ptr<ICloudServiceAgent> m_cloud_agent;
    std::function<void(const nlohmann::json &)> m_panel_listener;

    OnMsgArrivedFn        m_on_ssdp_msg;
    OnPrinterConnectedFn  m_on_printer_connected;
    OnLocalConnectedFn    m_on_local_connect;
    OnMessageFn           m_on_local_message;
    OnMessageFn           m_on_message;
    QueueOnMainFn         m_queue_on_main;
    OnServerErrFn         m_on_server_err;

    std::atomic<int>      m_conn_state{0};
    bool                  m_was_connected = false;
};

} // namespace Slic3r

#endif
