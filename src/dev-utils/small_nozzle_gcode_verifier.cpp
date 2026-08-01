#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <vector>

namespace {

struct LayerMetrics {
    double small_length { 0.0 };
    double large_length { 0.0 };
};

struct WallSegment {
    double x1 { 0.0 };
    double y1 { 0.0 };
    double x2 { 0.0 };
    double y2 { 0.0 };
    double width { 0.0 };
    double length { 0.0 };
    int tool { -1 };
    std::string role;
};

struct ToolMetrics {
    double wall_length { 0.0 };
    double weighted_width { 0.0 };
    double width_weight { 0.0 };
    std::vector<double> width_samples;
    std::vector<double> speed_samples;
};

bool starts_with(const std::string &text, const char *prefix)
{
    return text.compare(0, std::char_traits<char>::length(prefix), prefix) == 0;
}

std::string json_escape(const std::string &text)
{
    std::string result;
    result.reserve(text.size() + 8);
    for (const char ch : text) {
        switch (ch) {
        case '\\': result += "\\\\"; break;
        case '"':  result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result += ch; break;
        }
    }
    return result;
}

bool is_wall_role(const std::string &role)
{
    return role == "Outer wall" || role == "Inner wall" || role == "Overhang wall";
}

bool is_checked_nonwall_role(const std::string &role)
{
    return role == "Bottom surface" || role == "Top surface" ||
           role == "Internal solid infill" || role == "Sparse infill";
}

double median(std::vector<double> values)
{
    if (values.empty())
        return 0.0;
    const size_t middle = values.size() / 2;
    std::nth_element(values.begin(), values.begin() + middle, values.end());
    const double upper = values[middle];
    if (values.size() % 2 != 0)
        return upper;
    std::nth_element(values.begin(), values.begin() + middle - 1, values.begin() + middle);
    return 0.5 * (values[middle - 1] + upper);
}

void parse_axis_values(const std::string &line, std::map<char, double> &values, int &non_finite)
{
    for (size_t i = 0; i + 1 < line.size(); ++i) {
        const char axis = line[i];
        if (axis != 'X' && axis != 'Y' && axis != 'Z' && axis != 'E' && axis != 'F')
            continue;
        char *end = nullptr;
        const double value = std::strtod(line.c_str() + i + 1, &end);
        if (end == line.c_str() + i + 1)
            continue;
        if (!std::isfinite(value)) {
            ++non_finite;
            continue;
        }
        values[axis] = value;
        i = size_t(end - line.c_str()) - 1;
    }
}

} // namespace

int main(int argc, char **argv)
{
    if (argc != 6 && argc != 12) {
        std::cerr << "Usage: small-nozzle-gcode-verifier <gcode> <small-tool> <large-tool> <tool-count> <output-json>"
                     " [probe-first-layer probe-layer-count min-x min-y max-x max-y]\n";
        return 2;
    }

    const std::string input_path = argv[1];
    const int small_tool = std::stoi(argv[2]);
    const int large_tool = std::stoi(argv[3]);
    const int tool_count = std::stoi(argv[4]);
    const std::string output_path = argv[5];
    const int probe_first_layer = argc == 12 ? std::stoi(argv[6]) : -1;
    const int probe_layer_count = argc == 12 ? std::stoi(argv[7]) : 0;
    const double printable_min_x = argc == 12 ? std::stod(argv[8]) : 0.0;
    const double printable_min_y = argc == 12 ? std::stod(argv[9]) : 0.0;
    const double printable_max_x = argc == 12 ? std::stod(argv[10]) : 0.0;
    const double printable_max_y = argc == 12 ? std::stod(argv[11]) : 0.0;
    if (small_tool < 0 || large_tool < 0 || small_tool >= tool_count || large_tool >= tool_count || small_tool == large_tool) {
        std::cerr << "Invalid tool assignment\n";
        return 2;
    }
    if (probe_first_layer < -1 || probe_layer_count < 0 ||
        (argc == 12 && (printable_max_x <= printable_min_x || printable_max_y <= printable_min_y))) {
        std::cerr << "Invalid probe or printable-area arguments\n";
        return 2;
    }

    std::ifstream input(input_path);
    if (!input) {
        std::cerr << "Cannot open G-code: " << input_path << '\n';
        return 2;
    }

    int layer = -1;
    int tool = -1;
    int unsupported_tools = 0;
    int extrusion_before_tool = 0;
    int non_finite = 0;
    int out_of_bounds_extrusions = 0;
    bool absolute_xy = true;
    bool relative_extrusion = false;
    bool have_x = false;
    bool have_y = false;
    double x = 0.0;
    double y = 0.0;
    double e = 0.0;
    double width = 0.0;
    double current_feed = 0.0;
    bool sparse_infill_seen = false;
    std::string role;
    std::map<int, LayerMetrics> layers;
    std::map<int, std::vector<WallSegment>> probe_segments;
    std::map<int, int> tool_changes;
    std::vector<ToolMetrics> tools(static_cast<std::size_t>(tool_count));
    std::map<std::string, double> small_nonwall_lengths;
    std::map<std::string, std::string> settings;
    const std::vector<std::string> setting_keys {
        "wall_generator",
        "crisp_corner_small_nozzle_wall_count",
        "crisp_corner_interlace_small_nozzle_walls",
        "use_smaller_nozzles_in_crisp_corners"
    };

    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line == ";LAYER_CHANGE") {
            ++layer;
            continue;
        }
        if (starts_with(line, ";TYPE:")) {
            role = line.substr(6);
            if (role == "Sparse infill")
                sparse_infill_seen = true;
            continue;
        }
        if (starts_with(line, ";WIDTH:")) {
            width = std::strtod(line.c_str() + 7, nullptr);
            continue;
        }
        if (starts_with(line, "; ")) {
            const size_t separator = line.find(" = ");
            if (separator != std::string::npos) {
                const std::string key = line.substr(2, separator - 2);
                if (std::find(setting_keys.begin(), setting_keys.end(), key) != setting_keys.end())
                    settings[key] = line.substr(separator + 3);
            }
            continue;
        }
        if (line.size() >= 2 && line[0] == 'T' && std::isdigit(static_cast<unsigned char>(line[1]))) {
            const int new_tool = std::atoi(line.c_str() + 1);
            if (new_tool < 0 || new_tool >= tool_count)
                ++unsupported_tools;
            if (layer >= 0 && tool >= 0 && new_tool != tool)
                ++tool_changes[layer];
            tool = new_tool;
            continue;
        }
        if (line == "G90") { absolute_xy = true; continue; }
        if (line == "G91") { absolute_xy = false; continue; }
        if (starts_with(line, "M82")) { relative_extrusion = false; continue; }
        if (starts_with(line, "M83")) { relative_extrusion = true; continue; }
        if (starts_with(line, "G92")) {
            std::map<char, double> values;
            parse_axis_values(line, values, non_finite);
            if (values.count('E') != 0)
                e = values['E'];
            continue;
        }
        if (!(starts_with(line, "G0 ") || starts_with(line, "G1 ") ||
              starts_with(line, "G2 ") || starts_with(line, "G3 ")))
            continue;

        std::map<char, double> values;
        parse_axis_values(line, values, non_finite);
        const bool has_new_x = values.count('X') != 0;
        const bool has_new_y = values.count('Y') != 0;
        const bool has_new_e = values.count('E') != 0;
        if (values.count('F') != 0)
            current_feed = values['F'];
        double new_x = x;
        double new_y = y;
        if (has_new_x)
            new_x = absolute_xy || !have_x ? values['X'] : x + values['X'];
        if (has_new_y)
            new_y = absolute_xy || !have_y ? values['Y'] : y + values['Y'];
        const double new_e = has_new_e ? values['E'] : e;
        const double extrusion = !has_new_e ? 0.0 : (relative_extrusion ? new_e : new_e - e);

        if (extrusion > 1e-7) {
            if (tool < 0 || tool >= tool_count) {
                ++extrusion_before_tool;
            } else if (layer >= 0 && have_x && have_y && (has_new_x || has_new_y)) {
                if (argc == 12 &&
                    (new_x < printable_min_x - 0.05 || new_x > printable_max_x + 0.05 ||
                     new_y < printable_min_y - 0.05 || new_y > printable_max_y + 0.05))
                    ++out_of_bounds_extrusions;
                const double dx = new_x - x;
                const double dy = new_y - y;
                const double length = std::hypot(dx, dy);
                if (length > 0.005) {
                    if (is_wall_role(role)) {
                        ToolMetrics &tool_metrics = tools[size_t(tool)];
                        tool_metrics.wall_length += length;
                        if (width > 0.0 && width < 2.0) {
                            tool_metrics.weighted_width += width * length;
                            tool_metrics.width_weight += length;
                            if (tool_metrics.width_samples.size() < 50000)
                                tool_metrics.width_samples.push_back(width);
                        }
                        if (current_feed > 0.0 && tool_metrics.speed_samples.size() < 50000)
                            tool_metrics.speed_samples.push_back(current_feed / 60.0);
                        if (tool == small_tool)
                            layers[layer].small_length += length;
                        if (tool == large_tool)
                            layers[layer].large_length += length;
                        if (probe_first_layer >= 0 && layer >= probe_first_layer &&
                            layer < probe_first_layer + probe_layer_count) {
                            probe_segments[layer].push_back(
                                WallSegment { x, y, new_x, new_y, width, length, tool, role });
                        }
                    } else if (tool == small_tool && is_checked_nonwall_role(role)) {
                        small_nonwall_lengths[role] += length;
                    }
                }
            }
        }

        if (has_new_x) { x = new_x; have_x = true; }
        if (has_new_y) { y = new_y; have_y = true; }
        if (has_new_e)
            e = new_e;
    }

    int max_tool_changes = 0;
    for (const auto &entry : tool_changes)
        max_tool_changes = std::max(max_tool_changes, entry.second);

    std::ofstream output(output_path);
    if (!output) {
        std::cerr << "Cannot write JSON: " << output_path << '\n';
        return 2;
    }
    output << std::fixed << std::setprecision(6);
    output << "{\n";
    output << "  \"layer_count\": " << (layer + 1) << ",\n";
    output << "  \"unsupported_tool_commands\": " << unsupported_tools << ",\n";
    output << "  \"extrusion_before_tool\": " << extrusion_before_tool << ",\n";
    output << "  \"non_finite_numbers\": " << non_finite << ",\n";
    output << "  \"out_of_bounds_extrusions\": " << out_of_bounds_extrusions << ",\n";
    output << "  \"max_tool_changes_per_layer\": " << max_tool_changes << ",\n";
    output << "  \"sparse_infill_seen\": " << (sparse_infill_seen ? "true" : "false") << ",\n";
    output << "  \"settings\": {";
    bool first = true;
    for (const auto &entry : settings) {
        if (!first) output << ',';
        output << "\n    \"" << json_escape(entry.first) << "\": \"" << json_escape(entry.second) << '"';
        first = false;
    }
    if (!settings.empty()) output << '\n' << "  ";
    output << "},\n";
    output << "  \"tools\": [\n";
    for (int index = 0; index < tool_count; ++index) {
        const ToolMetrics &metrics = tools[size_t(index)];
        const double mean_width = metrics.width_weight > 0.0 ? metrics.weighted_width / metrics.width_weight : 0.0;
        const double median_width = median(metrics.width_samples);
        const double median_speed = median(metrics.speed_samples);
        output << "    {\"tool\": " << index << ", \"wall_length\": " << metrics.wall_length
               << ", \"mean_width\": " << mean_width << ", \"median_width\": " << median_width
               << ", \"median_speed\": " << median_speed << "}";
        output << (index + 1 == tool_count ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"small_nonwall_lengths\": {";
    first = true;
    for (const auto &entry : small_nonwall_lengths) {
        if (!first) output << ',';
        output << "\n    \"" << json_escape(entry.first) << "\": " << entry.second;
        first = false;
    }
    if (!small_nonwall_lengths.empty()) output << '\n' << "  ";
    output << "},\n";
    output << "  \"layers\": [\n";
    size_t emitted = 0;
    for (const auto &entry : layers) {
        output << "    {\"layer\": " << entry.first << ", \"small_length\": " << entry.second.small_length
               << ", \"large_length\": " << entry.second.large_length << "}";
        output << (++emitted == layers.size() ? "\n" : ",\n");
    }
    output << "  ],\n";
    output << "  \"probe_layers\": [";
    for (int index = 0; index < probe_layer_count; ++index) {
        if (index != 0) output << ", ";
        output << probe_first_layer + index;
    }
    output << "],\n";
    output << "  \"probe_segments\": {";
    first = true;
    for (const auto &entry : probe_segments) {
        if (!first) output << ',';
        output << "\n    \"" << entry.first << "\": [\n";
        for (size_t index = 0; index < entry.second.size(); ++index) {
            const WallSegment &segment = entry.second[index];
            output << "      {\"x1\": " << segment.x1 << ", \"y1\": " << segment.y1
                   << ", \"x2\": " << segment.x2 << ", \"y2\": " << segment.y2
                   << ", \"tool\": " << segment.tool << ", \"width\": " << segment.width
                   << ", \"length\": " << segment.length << ", \"role\": \""
                   << json_escape(segment.role) << "\"}";
            output << (index + 1 == entry.second.size() ? "\n" : ",\n");
        }
        output << "    ]";
        first = false;
    }
    if (!probe_segments.empty()) output << '\n' << "  ";
    output << "}\n";
    output << "}\n";
    return 0;
}
