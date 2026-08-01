#ifndef slic3r_GUI_SnapmakerMonitorUtils_hpp_
#define slic3r_GUI_SnapmakerMonitorUtils_hpp_

#include <string>
#include <string_view>

namespace Slic3r {
namespace GUI {

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
