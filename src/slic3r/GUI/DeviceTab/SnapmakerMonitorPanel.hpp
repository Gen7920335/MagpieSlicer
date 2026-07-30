#ifndef slic3r_GUI_SnapmakerMonitorPanel_hpp_
#define slic3r_GUI_SnapmakerMonitorPanel_hpp_

#include <array>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <wx/panel.h>
#include <wx/timer.h>

class wxGauge;
class wxChoice;
class wxListBox;
class wxNotebook;
class wxSpinCtrlDouble;
class wxStaticText;
class wxTextCtrl;
class wxImage;
class Button;

namespace Slic3r {
namespace GUI {

class CameraCanvas;
struct ExcludeObjectShape;
class LayerToolpathCanvas;
class TemperatureHistoryCanvas;
class SnapmakerCameraSession;
class SnapmakerGCodeLayerCache;

class SnapmakerMonitorPanel final : public wxPanel
{
public:
    explicit SnapmakerMonitorPanel(wxWindow *parent);
    ~SnapmakerMonitorPanel() override;

    void set_server(const wxString &url, const wxString &api_key);
    void set_active(bool active);
    void refresh_now();

private:
    void build_ui();
    void request_object_list();
    void request_status();
    void request_camera();
    void request_gcode(const std::string &filename);
    void request_console();
    void request_files();
    void request_history();
    void request_system_info();
    void request_auxiliary(
        const std::string &endpoint,
        bool &in_flight,
        std::function<void(const std::string &)> apply);
    void start_camera_session();
    void stop_camera_session();
    void apply_object_list(const std::string &body);
    void apply_status(const std::string &body);
    void apply_console(const std::string &body);
    void apply_files(const std::string &body);
    void apply_history(const std::string &body);
    void apply_system_info(const std::string &body);
    void apply_camera(wxImage image, std::uint64_t frame_hash, long latency_ms);
    void apply_duplicate_camera_frame(long latency_ms);
    void set_connection_error(const wxString &message);
    void send_print_command(const std::string &endpoint, const wxString &success_message);
    void send_delete_command(const std::string &endpoint, const wxString &success_message);
    void update_controls();
    void refresh_auxiliary();
    void on_home(const std::string &axes);
    void on_jog(char axis, double direction);
    void on_set_temperature(size_t heater_index);
    void on_set_tuning();
    void on_set_fans();
    void on_run_macro();
    void on_send_console();
    void on_start_file();
    void on_delete_file();
    void on_emergency_stop();
    void on_pause_resume();
    void on_cancel();
    void on_skip_object();
    void send_gcode_script(const std::string &script, const wxString &success_message);
    void update_layer_view(int current_layer, int total_layers);
    void update_object_list(const std::vector<ExcludeObjectShape> &objects);
    void update_camera_rate();
    void update_footer();
    void on_status_timer(wxTimerEvent &event);
    void on_camera_timer(wxTimerEvent &event);

    std::string make_status_url() const;
    static std::string normalize_base_url(const wxString &url);

    wxTimer m_status_timer;
    wxTimer m_camera_timer;
    bool m_active {false};
    bool m_status_in_flight {false};
    bool m_camera_in_flight {false};
    bool m_objects_in_flight {false};
    bool m_command_in_flight {false};
    bool m_gcode_in_flight {false};
    bool m_console_in_flight {false};
    bool m_files_in_flight {false};
    bool m_history_in_flight {false};
    bool m_system_in_flight {false};
    bool m_exclude_object_supported {false};
    bool m_connected {false};
    bool m_homed {false};
    bool m_bed_mesh_supported {false};
    bool m_cavity_fan_supported {false};
    bool m_light_supported {false};
    int m_camera_interval_ms {150};
    int m_slow_refresh_tick {0};
    unsigned m_camera_frames_in_window {0};
    double m_camera_fps {0.0};
    long m_camera_latency_ms {0};
    std::uint64_t m_last_frame_hash {0};
    std::uint64_t m_camera_request_sequence {0};
    std::uint64_t m_server_generation {0};
    std::uint64_t m_camera_session_generation {0};
    int m_current_layer {0};
    int m_total_layers {0};
    int m_displayed_layer {0};
    std::chrono::steady_clock::time_point m_fps_window_started;

    std::string m_base_url;
    std::string m_api_key;
    std::string m_gcode_requested_filename;
    std::string m_gcode_cache_filename;
    std::vector<std::string> m_status_objects;
    std::vector<std::string> m_object_names;
    std::vector<std::string> m_object_list_signature;
    std::vector<std::string> m_macro_names;
    std::vector<std::string> m_file_names;
    std::shared_ptr<int> m_lifetime;
    std::shared_ptr<SnapmakerGCodeLayerCache> m_gcode_cache;
    std::unique_ptr<SnapmakerCameraSession> m_camera_session;
    wxString m_camera_session_status;
    std::string m_print_state;

    CameraCanvas *m_camera {nullptr};
    LayerToolpathCanvas *m_layer_view {nullptr};
    TemperatureHistoryCanvas *m_temperature_history {nullptr};
    wxListBox *m_object_list {nullptr};
    wxStaticText *m_connection {nullptr};
    wxStaticText *m_state {nullptr};
    wxStaticText *m_progress_percent {nullptr};
    wxStaticText *m_filename {nullptr};
    wxStaticText *m_layers {nullptr};
    wxStaticText *m_time {nullptr};
    wxGauge *m_progress {nullptr};
    std::array<wxStaticText *, 4> m_toolheads {};
    wxStaticText *m_bed {nullptr};
    wxStaticText *m_fans {nullptr};
    wxStaticText *m_speed {nullptr};
    wxStaticText *m_light {nullptr};
    wxStaticText *m_message {nullptr};
    wxStaticText *m_camera_status {nullptr};
    wxStaticText *m_active_tool {nullptr};
    wxStaticText *m_network_status {nullptr};
    wxStaticText *m_selected_object {nullptr};
    wxStaticText *m_position {nullptr};
    wxStaticText *m_homed_axes {nullptr};
    wxStaticText *m_mesh_status {nullptr};
    wxStaticText *m_system_summary {nullptr};
    wxStaticText *m_file_details {nullptr};
    wxStaticText *m_endstop_status {nullptr};
    wxChoice *m_jog_distance {nullptr};
    std::array<wxSpinCtrlDouble *, 5> m_temperature_targets {};
    wxSpinCtrlDouble *m_speed_target {nullptr};
    wxSpinCtrlDouble *m_flow_target {nullptr};
    wxSpinCtrlDouble *m_main_fan_target {nullptr};
    wxSpinCtrlDouble *m_cavity_fan_target {nullptr};
    wxSpinCtrlDouble *m_light_target {nullptr};
    wxTextCtrl *m_console_output {nullptr};
    wxTextCtrl *m_console_command {nullptr};
    wxTextCtrl *m_macro_arguments {nullptr};
    wxListBox *m_macro_list {nullptr};
    wxListBox *m_file_list {nullptr};
    wxListBox *m_history_list {nullptr};
    Button *m_pause_resume {nullptr};
    Button *m_cancel {nullptr};
    Button *m_refresh {nullptr};
    Button *m_skip_object {nullptr};
    Button *m_console_send {nullptr};
    Button *m_macro_run {nullptr};
    Button *m_file_print {nullptr};
    Button *m_file_delete {nullptr};
    Button *m_home_all {nullptr};
    Button *m_mesh_calibrate {nullptr};
    Button *m_mesh_clear {nullptr};
    Button *m_emergency_stop {nullptr};
    std::vector<Button *> m_connected_buttons;
    std::vector<Button *> m_jog_buttons;
};

} // namespace GUI
} // namespace Slic3r

#endif
