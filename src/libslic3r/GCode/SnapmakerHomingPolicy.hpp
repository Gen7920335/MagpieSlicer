#ifndef slic3r_GCode_SnapmakerHomingPolicy_hpp_
#define slic3r_GCode_SnapmakerHomingPolicy_hpp_

#include <algorithm>
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

struct SnapmakerHomingScan
{
    bool safe {false};
    size_t insertion_position {0};
};

inline SnapmakerHomingScan scan_snapmaker_u1_homing(std::string_view gcode)
{
    bool print_start_seen = false;
    bool full_home_seen = false;
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
                full_home_seen = false;
                insertion_position = newline == std::string_view::npos ? line_end : newline + 1;
            } else if (command == "G28") {
                full_home_seen = true;
                if (!print_start_seen)
                    insertion_position = newline == std::string_view::npos ? line_end : newline + 1;
            } else if (coordinate_motion(command)) {
                if (print_start_seen)
                    return {full_home_seen, insertion_position};
                if (!full_home_seen)
                    motion_before_home = true;
            }
        }
        if (newline == std::string_view::npos)
            break;
        line_start = newline + 1;
    }
    return {
        full_home_seen && (print_start_seen || !motion_before_home),
        print_start_seen ? insertion_position : 0
    };
}

} // namespace detail

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
