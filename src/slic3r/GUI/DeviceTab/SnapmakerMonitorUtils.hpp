#ifndef slic3r_GUI_SnapmakerMonitorUtils_hpp_
#define slic3r_GUI_SnapmakerMonitorUtils_hpp_

#include <string>
#include <string_view>
#include <cstdint>

namespace Slic3r {
namespace GUI {

// UI-thread state for one editable remote numeric target. A command ACK alone
// must not release an edit to an older, already in-flight status response.
class SnapmakerNumericInputState
{
public:
    void mark_edited();
    void reset();
    std::uint64_t begin_submit(double requested_value);
    void command_finished(std::uint64_t revision, bool success);
    bool allow_remote_update(double remote_value, unsigned display_digits, bool focused);

private:
    std::uint64_t m_revision {0};
    double m_requested_value {0.};
    bool m_edited {false};
    bool m_pending {false};
    bool m_acknowledged {false};
};

struct SnapmakerWebSocketEndpoint
{
    std::string host;
    std::string port;
    std::string host_header;
    std::string target {"/websocket"};
};

struct SnapmakerControlAvailability
{
    bool pause_resume {false};
    bool cancel {false};
    bool refresh {false};
    bool skip_object {false};
    bool resume_mode {false};
};

SnapmakerWebSocketEndpoint parse_snapmaker_websocket_endpoint(std::string base_url);
std::string normalize_snapmaker_base_url(std::string value);
bool is_valid_snapmaker_object_name(std::string_view name);
bool is_public_snapmaker_macro(std::string_view name);
std::string snapmaker_jog_script(char axis, double distance);
std::string snapmaker_heater_script(std::string_view heater, double target);
std::string snapmaker_extrude_script(double distance, double speed_mm_s);
std::string snapmaker_z_offset_script(double adjustment);
std::string snapmaker_motion_limit_script(double velocity, double acceleration, double square_corner_velocity);
std::string snapmaker_pressure_advance_script(
    std::string_view extruder,
    double pressure_advance,
    double smooth_time);
std::string snapmaker_bed_mesh_profile_script(std::string_view operation, std::string_view profile);
bool is_success_http_status(unsigned status);
int valid_snapmaker_layer_number(int requested_layer, int indexed_layer_count);
bool snapmaker_manual_motion_allowed(bool connected, bool command_in_flight, std::string_view print_state);
bool snapmaker_emergency_stop_allowed(bool has_server, bool emergency_in_flight);

SnapmakerControlAvailability snapmaker_control_availability(
    bool connected,
    bool command_in_flight,
    bool has_server,
    std::string_view print_state,
    bool exclude_object_supported,
    bool object_selected);

} // namespace GUI
} // namespace Slic3r

#endif
