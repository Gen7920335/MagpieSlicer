#include "SnapmakerMonitorUtils.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <limits>
#include <iomanip>
#include <sstream>
#include <stdexcept>

namespace Slic3r {
namespace GUI {

namespace {

bool is_valid_port(std::string_view port)
{
    if (port.empty() ||
        !std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;

    unsigned long value = 0;
    try {
        value = std::stoul(std::string(port));
    } catch (const std::exception &) {
        return false;
    }
    return value > 0 && value <= std::numeric_limits<std::uint16_t>::max();
}

} // namespace

SnapmakerWebSocketEndpoint parse_snapmaker_websocket_endpoint(std::string base_url)
{
    constexpr const char *http_prefix = "http://";
    constexpr const char *https_prefix = "https://";

    if (base_url.rfind(https_prefix, 0) == 0)
        throw std::runtime_error("Secure Snapmaker camera sessions are not supported");
    if (base_url.rfind(http_prefix, 0) == 0)
        base_url.erase(0, std::char_traits<char>::length(http_prefix));

    const size_t path_pos = base_url.find('/');
    const std::string authority = base_url.substr(0, path_pos);
    if (authority.empty())
        throw std::runtime_error("Missing Snapmaker camera host");

    SnapmakerWebSocketEndpoint endpoint;
    bool explicit_port = false;
    if (authority.front() == '[') {
        const size_t closing_bracket = authority.find(']');
        if (closing_bracket == std::string::npos)
            throw std::runtime_error("Invalid IPv6 Snapmaker camera host");
        endpoint.host = authority.substr(1, closing_bracket - 1);
        if (closing_bracket + 1 < authority.size()) {
            if (authority[closing_bracket + 1] != ':')
                throw std::runtime_error("Invalid Snapmaker camera port");
            explicit_port = true;
            endpoint.port = authority.substr(closing_bracket + 2);
        }
    } else {
        const size_t colon = authority.rfind(':');
        if (colon != std::string::npos && authority.find(':') == colon) {
            explicit_port = true;
            endpoint.host = authority.substr(0, colon);
            endpoint.port = authority.substr(colon + 1);
        } else {
            endpoint.host = authority;
        }
    }

    if (endpoint.host.empty())
        throw std::runtime_error("Missing Snapmaker camera host");
    if (explicit_port && !is_valid_port(endpoint.port))
        throw std::runtime_error("Invalid Snapmaker camera port");
    if (!explicit_port)
        endpoint.port = "80";
    endpoint.host_header = authority;
    return endpoint;
}

std::string normalize_snapmaker_base_url(std::string value)
{
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    const size_t scheme = value.find("://");
    if (scheme != std::string::npos) {
        const size_t path = value.find('/', scheme + 3);
        if (path != std::string::npos)
            value.erase(path);
    }
    return value;
}

bool is_valid_snapmaker_object_name(std::string_view name)
{
    return !name.empty() &&
           std::all_of(name.begin(), name.end(), [](unsigned char c) {
               return std::isalnum(c) != 0 || c == '_' || c == '-' || c == '.';
           });
}

bool is_public_snapmaker_macro(std::string_view name)
{
    if (name.empty() || name.front() == '_' || name == "CANCEL_PRINT" ||
        name == "PAUSE" || name == "RESUME")
        return false;
    if (name.front() == 'T' && name.size() > 1 &&
        std::all_of(name.begin() + 1, name.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
        return false;
    return is_valid_snapmaker_object_name(name);
}

std::string snapmaker_jog_script(char axis, double distance)
{
    axis = static_cast<char>(std::toupper(static_cast<unsigned char>(axis)));
    if (axis != 'X' && axis != 'Y' && axis != 'Z')
        throw std::invalid_argument("Invalid jog axis");
    if (!std::isfinite(distance) || distance == 0.0 || std::abs(distance) > 100.0)
        throw std::invalid_argument("Invalid jog distance");

    std::ostringstream out;
    out << "G91\nG0 " << axis << std::fixed << std::setprecision(3) << distance
        << " F" << (axis == 'Z' ? 600 : 6000) << "\nG90";
    return out.str();
}

std::string snapmaker_heater_script(std::string_view heater, double target)
{
    if (!is_valid_snapmaker_object_name(heater) || !std::isfinite(target) || target < 0.0 || target > 400.0)
        throw std::invalid_argument("Invalid heater command");
    std::ostringstream out;
    out << "SET_HEATER_TEMPERATURE HEATER=" << heater << " TARGET="
        << std::fixed << std::setprecision(1) << target;
    return out.str();
}

bool is_success_http_status(unsigned status)
{
    return status >= 200 && status < 300;
}

int valid_snapmaker_layer_number(int requested_layer, int indexed_layer_count)
{
    return requested_layer > 0 && requested_layer <= indexed_layer_count ? requested_layer : 0;
}

SnapmakerControlAvailability snapmaker_control_availability(
    bool connected,
    bool command_in_flight,
    bool has_server,
    std::string_view print_state,
    bool exclude_object_supported,
    bool object_selected)
{
    const bool printing = print_state == "printing";
    const bool paused = print_state == "paused";
    const bool controllable_print = connected && !command_in_flight && (printing || paused);

    SnapmakerControlAvailability result;
    result.pause_resume = controllable_print;
    result.cancel = controllable_print;
    result.refresh = has_server && !command_in_flight;
    result.skip_object =
        controllable_print && exclude_object_supported && object_selected;
    result.resume_mode = paused;
    return result;
}

} // namespace GUI
} // namespace Slic3r
