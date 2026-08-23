#ifndef slic3r_GCode_SnapmakerHomingPolicy_hpp_
#define slic3r_GCode_SnapmakerHomingPolicy_hpp_

#include <algorithm>
#include <array>
#include <cctype>
#include <string>
#include <string_view>

namespace Slic3r {

namespace detail {

inline std::string normalized_gcode_command(std::string_view line)
{
    const size_t comment = line.find(';');
    if (comment != std::string_view::npos)
        line = line.substr(0, comment);
    const size_t first = line.find_first_not_of(" \t\r");
    if (first == std::string_view::npos)
        return {};
    const size_t last = line.find_last_not_of(" \t\r");
    std::string command(line.substr(first, last - first + 1));
    std::transform(command.begin(), command.end(), command.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return command;
}

inline bool command_starts_with(const std::string &line, const char *command)
{
    const size_t length = std::char_traits<char>::length(command);
    return line.compare(0, length, command) == 0 &&
           (line.size() == length || std::isspace(static_cast<unsigned char>(line[length])) != 0);
}

inline bool coordinate_motion(const std::string &line)
{
    if (!command_starts_with(line, "G0") && !command_starts_with(line, "G1"))
        return false;
    for (size_t i = 2; i < line.size(); ++i) {
        if ((line[i] == 'X' || line[i] == 'Y' || line[i] == 'Z') &&
            std::isspace(static_cast<unsigned char>(line[i - 1])) != 0)
            return true;
    }
    return false;
}

inline unsigned int homed_axes(const std::string &command)
{
    if (command == "G28")
        return 0x7;
    if (!command_starts_with(command, "G28"))
        return 0;

    unsigned int axes = 0;
    for (size_t i = 3; i < command.size(); ++i) {
        if (std::isspace(static_cast<unsigned char>(command[i - 1])) == 0)
            continue;
        if (command[i] == 'X')
            axes |= 0x1;
        else if (command[i] == 'Y')
            axes |= 0x2;
        else if (command[i] == 'Z')
            axes |= 0x4;
    }
    return axes;
}

struct SnapmakerHomingScan
{
    bool safe {false};
    size_t insertion_position {0};
};

inline SnapmakerHomingScan scan_snapmaker_u1_homing(std::string_view gcode)
{
    bool print_start_seen = false;
    unsigned int axes_homed = 0;
    bool motion_before_home = false;
    size_t insertion_position = 0;
    size_t line_start = 0;

    while (line_start <= gcode.size()) {
        const size_t newline = gcode.find('\n', line_start);
        const size_t line_end = newline == std::string_view::npos ? gcode.size() : newline;
        const std::string command = normalized_gcode_command(gcode.substr(line_start, line_end - line_start));
        if (!command.empty()) {
            if (command_starts_with(command, "PRINT_START") || command_starts_with(command, "PRINT_STRAT")) {
                print_start_seen = true;
                axes_homed = 0;
                insertion_position = newline == std::string_view::npos ? line_end : newline + 1;
            } else if (command_starts_with(command, "G28")) {
                axes_homed |= homed_axes(command);
                if (!print_start_seen)
                    insertion_position = newline == std::string_view::npos ? line_end : newline + 1;
            } else if (coordinate_motion(command)) {
                if (print_start_seen)
                    return {axes_homed == 0x7, insertion_position};
                if (axes_homed != 0x7)
                    motion_before_home = true;
            }
        }
        if (newline == std::string_view::npos)
            break;
        line_start = newline + 1;
    }
    return {
        axes_homed == 0x7 && (print_start_seen || !motion_before_home),
        print_start_seen ? insertion_position : 0
    };
}

} // namespace detail

inline bool is_snapmaker_u1_model(std::string_view printer_model)
{
    return printer_model == "797581801" || printer_model == "Snapmaker U1";
}

inline constexpr std::string_view SNAPMAKER_U1_NATIVE_START_GCODE = R"SNAPMAKER(
; Snapmaker U1 native pre-print sequence

PRINT_START
DEFECT_DETECTION_START
SET_PRINT_STATS_INFO TOTAL_LAYER={total_layer_count} CURRENT_LAYER=0
TIMELAPSE_START
M140 S{bed_temperature_initial_layer_single}
M104 T{initial_extruder} S140
M204 S10000

G28 X Y
DEFECT_DETECT_NOODLE_FIRST
T{initial_extruder}
G90
DEFECT_DETECTION_DETECT_BED
SM_PRINT_CHECK_SWITCH_EXTRUDER

SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=1 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=0
SM_PRINT_FLOW_CALIBRATE EXTRUDER=0
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=2 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=1
SM_PRINT_FLOW_CALIBRATE EXTRUDER=1
SM_PRINT_EXTRUDER_PREHEAT EXTRUDER=3 TEMP=140
SM_PRINT_AUTO_FEED EXTRUDER=2
SM_PRINT_FLOW_CALIBRATE EXTRUDER=2
SM_PRINT_AUTO_FEED EXTRUDER=3
SM_PRINT_FLOW_CALIBRATE EXTRUDER=3
M104 S0 T0 A0
M104 S0 T1 A0
M104 S0 T2 A0
M104 S0 T3 A0
M104 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}

T{initial_extruder}
M106 S255
M106 P2 S0
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 T{initial_extruder} S{nozzle_temperature[initial_extruder] - 90}
ROUGHLY_CLEAN_NOZZLE_WITH_DISCARD
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
G28 Z I140 J140

DETECT_BED_PLATE

G90
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
M109 S{nozzle_temperature[initial_extruder] - 50}
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_1
M104 S{nozzle_temperature[initial_extruder] - 90}
G0 Z5 F10000
MOVE_TO_DISCARD_FILAMENT_POSITION
ROUGHLY_CLEAN_NOZZLE
MOVE_TO_XY_IDLE_POSITION_EXTRUDER
FINELY_CLEAN_NOZZLE_STAGE_2

M106 S255
M109 S{nozzle_temperature[initial_extruder] - 90}
M190 S{bed_temperature_initial_layer_single}
M107 P2
G90
G0 Z5 F10000
WAIT_CHAMBER_TEMP TIMEOUT=180
{if curr_bed_type=="High Temp Plate"}
G28 Z Z_OFFSET -0.07
{else}
G28 Z
{endif}

{if curr_bed_type=="High Temp Plate"}
BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07
{else}
BED_MESH_CALIBRATE PROBE_COUNT=11,11
{endif}

G90
G1 Z1.5
G0 X85 Y1 Z2 F18000
M109 S{nozzle_temperature_initial_layer[initial_extruder]}
G1 Z0.2
M83
G1 X185 E15 F360
G1 Z1.5

G90
M106 S0
)SNAPMAKER";

inline bool snapmaker_u1_has_native_start(std::string_view gcode)
{
    static constexpr std::array<std::string_view, 9> required_commands {
        "PRINT_START",
        "M140 S",
        "G28 X Y",
        "G28 Z I140 J140",
        "M190 S",
        "G28 Z",
        "BED_MESH_CALIBRATE",
        "M109 S",
        "G1 X185 E15 F360"
    };

    size_t position = 0;
    for (const std::string_view command : required_commands) {
        position = gcode.find(command, position);
        if (position == std::string_view::npos)
            return false;
        position += command.size();
    }
    return true;
}

inline std::string snapmaker_u1_start_gcode_template(std::string_view configured_gcode)
{
    return std::string(snapmaker_u1_has_native_start(configured_gcode) ?
        configured_gcode : SNAPMAKER_U1_NATIVE_START_GCODE);
}

inline std::string machine_start_gcode_template_for_printer(
    std::string_view printer_model, std::string_view configured_gcode)
{
    return is_snapmaker_u1_model(printer_model) ?
        snapmaker_u1_start_gcode_template(configured_gcode) :
        std::string(configured_gcode);
}

inline bool snapmaker_u1_has_safe_homing(std::string_view gcode)
{
    return detail::scan_snapmaker_u1_homing(gcode).safe;
}

inline bool ensure_snapmaker_u1_safe_homing(std::string &gcode)
{
    const detail::SnapmakerHomingScan scan = detail::scan_snapmaker_u1_homing(gcode);
    if (scan.safe)
        return false;
    static constexpr std::string_view homing =
        "; Magpie: mandatory Snapmaker U1 homing after PRINT_START\nG28\n";
    gcode.insert(scan.insertion_position, homing);
    return true;
}

} // namespace Slic3r

#endif
