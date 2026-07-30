#include "SnapmakerGCodeLayer.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <stdexcept>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

constexpr double PI = 3.14159265358979323846;
constexpr double MIN_EXTRUSION = 1e-7;
constexpr double MIN_MOVE = 1e-7;

enum class LayerMarkerStyle
{
    Unknown,
    LayerChange,
    CuraLayer,
    NumberedLayer
};

struct ParsedCommand
{
    std::string command;
    std::array<double, 26> values {};
    std::array<bool, 26> present {};

    bool has(char axis) const
    {
        const unsigned char upper = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(axis)));
        return upper >= 'A' && upper <= 'Z' && present[upper - 'A'];
    }

    double value(char axis) const
    {
        const unsigned char upper = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(axis)));
        return values[upper - 'A'];
    }
};

std::string_view trim_left(std::string_view value)
{
    while (!value.empty() && (value.front() == ' ' || value.front() == '\t' || value.front() == '\r'))
        value.remove_prefix(1);
    return value;
}

bool starts_with(std::string_view value, std::string_view prefix)
{
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

std::string uppercase(std::string_view value)
{
    std::string result(value);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return result;
}

std::string object_name_from_command(std::string_view line)
{
    const std::string upper = uppercase(line);
    const size_t name_pos = upper.find("NAME=");
    if (name_pos == std::string::npos)
        return {};
    const size_t begin = name_pos + 5;
    size_t end = begin;
    while (end < upper.size() && !std::isspace(static_cast<unsigned char>(upper[end])) && upper[end] != ';')
        ++end;
    return std::string(line.substr(begin, end - begin));
}

ParsedCommand parse_command(std::string_view line)
{
    ParsedCommand result;
    const size_t comment = line.find(';');
    if (comment != std::string_view::npos)
        line = line.substr(0, comment);
    line = trim_left(line);
    if (line.empty())
        return result;

    const char *cursor = line.data();
    const char *end = cursor + line.size();
    auto skip_space = [&]() {
        while (cursor < end && std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
    };
    auto skip_token = [&]() {
        while (cursor < end && !std::isspace(static_cast<unsigned char>(*cursor)))
            ++cursor;
    };

    if (cursor < end && std::toupper(static_cast<unsigned char>(*cursor)) == 'N') {
        skip_token();
        skip_space();
    }

    const char *command_begin = cursor;
    skip_token();
    result.command = uppercase(std::string_view(command_begin, static_cast<size_t>(cursor - command_begin)));

    while (cursor < end) {
        skip_space();
        if (cursor >= end)
            break;
        const unsigned char letter = static_cast<unsigned char>(std::toupper(static_cast<unsigned char>(*cursor)));
        if (letter < 'A' || letter > 'Z') {
            skip_token();
            continue;
        }
        ++cursor;
        char *number_end = nullptr;
        const double parsed = std::strtod(cursor, &number_end);
        if (number_end == cursor) {
            skip_token();
            continue;
        }
        const size_t index = letter - 'A';
        result.values[index] = parsed;
        result.present[index] = true;
        cursor = number_end;
    }
    return result;
}

LayerMarkerStyle marker_style(std::string_view line)
{
    line = trim_left(line);
    if (starts_with(line, ";LAYER_CHANGE"))
        return LayerMarkerStyle::LayerChange;
    if (starts_with(line, ";LAYER:"))
        return LayerMarkerStyle::CuraLayer;
    if (starts_with(line, "; layer num/total_layer_count:"))
        return LayerMarkerStyle::NumberedLayer;
    return LayerMarkerStyle::Unknown;
}

int declared_layer_count(std::string_view line)
{
    const std::string lower = [&]() {
        std::string result(line);
        std::transform(result.begin(), result.end(), result.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return result;
    }();
    constexpr std::string_view marker = "total layer number:";
    const size_t pos = lower.find(marker);
    if (pos == std::string::npos)
        return 0;
    const char *begin = lower.c_str() + pos + marker.size();
    char *end = nullptr;
    const long value = std::strtol(begin, &end, 10);
    return end != begin && value > 0 ? static_cast<int>(value) : 0;
}

} // namespace

struct ModalState
{
    double x {0.0};
    double y {0.0};
    double z {0.0};
    bool absolute_xyz {true};
    bool absolute_e {true};
    unsigned tool {0};
    std::unordered_map<unsigned, double> tool_e;
    std::string object;

    double e() const
    {
        const auto it = tool_e.find(tool);
        return it == tool_e.end() ? 0.0 : it->second;
    }

    void set_e(double value)
    {
        tool_e[tool] = value;
    }
};

struct LayerIndex
{
    size_t begin {0};
    size_t end {0};
    ModalState state;
};

namespace {

void append_linear_move(
    ModalState &state,
    const ParsedCommand &command,
    std::vector<SnapmakerGCodeSegment> *segments)
{
    const double start_x = state.x;
    const double start_y = state.y;
    const double start_e = state.e();
    const double target_x = command.has('X')
        ? (state.absolute_xyz ? command.value('X') : start_x + command.value('X'))
        : start_x;
    const double target_y = command.has('Y')
        ? (state.absolute_xyz ? command.value('Y') : start_y + command.value('Y'))
        : start_y;
    const double target_z = command.has('Z')
        ? (state.absolute_xyz ? command.value('Z') : state.z + command.value('Z'))
        : state.z;
    const double target_e = command.has('E')
        ? (state.absolute_e ? command.value('E') : start_e + command.value('E'))
        : start_e;

    const double dx = target_x - start_x;
    const double dy = target_y - start_y;
    const bool extruding = target_e - start_e > MIN_EXTRUSION;
    if (segments != nullptr && extruding && dx * dx + dy * dy > MIN_MOVE * MIN_MOVE) {
        segments->push_back({
            {start_x, start_y},
            {target_x, target_y},
            state.tool,
            state.object
        });
    }

    state.x = target_x;
    state.y = target_y;
    state.z = target_z;
    state.set_e(target_e);
}

void append_arc_move(
    ModalState &state,
    const ParsedCommand &command,
    bool clockwise,
    std::vector<SnapmakerGCodeSegment> *segments)
{
    const double start_x = state.x;
    const double start_y = state.y;
    const double start_e = state.e();
    const double target_x = command.has('X')
        ? (state.absolute_xyz ? command.value('X') : start_x + command.value('X'))
        : start_x;
    const double target_y = command.has('Y')
        ? (state.absolute_xyz ? command.value('Y') : start_y + command.value('Y'))
        : start_y;
    const double target_z = command.has('Z')
        ? (state.absolute_xyz ? command.value('Z') : state.z + command.value('Z'))
        : state.z;
    const double target_e = command.has('E')
        ? (state.absolute_e ? command.value('E') : start_e + command.value('E'))
        : start_e;
    const bool extruding = target_e - start_e > MIN_EXTRUSION;

    if (segments != nullptr && extruding && command.has('I') && command.has('J')) {
        const double center_x = start_x + command.value('I');
        const double center_y = start_y + command.value('J');
        const double radius = std::hypot(start_x - center_x, start_y - center_y);
        double start_angle = std::atan2(start_y - center_y, start_x - center_x);
        double end_angle = std::atan2(target_y - center_y, target_x - center_x);
        double sweep = end_angle - start_angle;
        if (clockwise) {
            while (sweep >= 0.0)
                sweep -= 2.0 * PI;
        } else {
            while (sweep <= 0.0)
                sweep += 2.0 * PI;
        }
        const int steps = std::clamp(static_cast<int>(std::ceil(std::abs(sweep) * radius / 0.8)), 1, 512);
        SnapmakerGCodePoint previous {start_x, start_y};
        for (int step = 1; step <= steps; ++step) {
            const double fraction = static_cast<double>(step) / steps;
            const double angle = start_angle + sweep * fraction;
            SnapmakerGCodePoint next {
                step == steps ? target_x : center_x + std::cos(angle) * radius,
                step == steps ? target_y : center_y + std::sin(angle) * radius
            };
            segments->push_back({previous, next, state.tool, state.object});
            previous = next;
        }
    } else if (segments != nullptr && extruding &&
               std::hypot(target_x - start_x, target_y - start_y) > MIN_MOVE) {
        segments->push_back({
            {start_x, start_y},
            {target_x, target_y},
            state.tool,
            state.object
        });
    }

    state.x = target_x;
    state.y = target_y;
    state.z = target_z;
    state.set_e(target_e);
}

void process_line(
    std::string_view line,
    ModalState &state,
    std::vector<SnapmakerGCodeSegment> *segments)
{
    const std::string_view trimmed = trim_left(line);
    const std::string upper = uppercase(trimmed);
    if (starts_with(upper, "EXCLUDE_OBJECT_START")) {
        state.object = object_name_from_command(trimmed);
        return;
    }
    if (starts_with(upper, "EXCLUDE_OBJECT_END")) {
        state.object.clear();
        return;
    }

    const ParsedCommand command = parse_command(trimmed);
    if (command.command.empty())
        return;
    if (command.command == "G90") {
        state.absolute_xyz = true;
    } else if (command.command == "G91") {
        state.absolute_xyz = false;
    } else if (command.command == "M82") {
        state.absolute_e = true;
    } else if (command.command == "M83") {
        state.absolute_e = false;
    } else if (command.command == "G92") {
        if (command.has('X'))
            state.x = command.value('X');
        if (command.has('Y'))
            state.y = command.value('Y');
        if (command.has('Z'))
            state.z = command.value('Z');
        if (command.has('E'))
            state.set_e(command.value('E'));
    } else if (command.command == "G0" || command.command == "G1") {
        append_linear_move(state, command, segments);
    } else if (command.command == "G2" || command.command == "G3") {
        append_arc_move(state, command, command.command == "G2", segments);
    } else if (command.command.size() > 1 && command.command.front() == 'T') {
        char *end = nullptr;
        const unsigned long tool = std::strtoul(command.command.c_str() + 1, &end, 10);
        if (end != command.command.c_str() + 1 && *end == '\0')
            state.tool = static_cast<unsigned>(tool);
    }
}

} // namespace

struct SnapmakerGCodeLayerCache::Impl
{
    explicit Impl(std::string source)
        : gcode(std::move(source))
    {}

    std::string gcode;
    std::vector<LayerIndex> layers;
    int declared_layer_count {0};
};

SnapmakerGCodeLayerCache::SnapmakerGCodeLayerCache(std::string gcode)
    : m_impl(std::make_unique<Impl>(std::move(gcode)))
{
    build_index();
}

SnapmakerGCodeLayerCache::~SnapmakerGCodeLayerCache() = default;

std::shared_ptr<SnapmakerGCodeLayerCache> SnapmakerGCodeLayerCache::parse(std::string gcode)
{
    auto cache = std::shared_ptr<SnapmakerGCodeLayerCache>(
        new SnapmakerGCodeLayerCache(std::move(gcode)));
    if (cache->empty())
        throw std::runtime_error("No layer markers found in G-code");
    return cache;
}

void SnapmakerGCodeLayerCache::build_index()
{
    ModalState state;
    LayerMarkerStyle selected_style = LayerMarkerStyle::Unknown;
    size_t offset = 0;

    while (offset < m_impl->gcode.size()) {
        const size_t newline = m_impl->gcode.find('\n', offset);
        const size_t line_end = newline == std::string::npos ? m_impl->gcode.size() : newline;
        const size_t next = newline == std::string::npos ? m_impl->gcode.size() : newline + 1;
        const std::string_view line(m_impl->gcode.data() + offset, line_end - offset);

        if (m_impl->declared_layer_count == 0)
            m_impl->declared_layer_count = declared_layer_count(line);

        const LayerMarkerStyle style = marker_style(line);
        if (style != LayerMarkerStyle::Unknown &&
            (selected_style == LayerMarkerStyle::Unknown || style == selected_style)) {
            if (selected_style == LayerMarkerStyle::Unknown)
                selected_style = style;
            if (!m_impl->layers.empty())
                m_impl->layers.back().end = offset;
            m_impl->layers.push_back({next, m_impl->gcode.size(), state});
        } else {
            process_line(line, state, nullptr);
        }
        offset = next;
    }
}

SnapmakerGCodeLayer SnapmakerGCodeLayerCache::layer(int one_based_layer) const
{
    SnapmakerGCodeLayer result;
    result.number = one_based_layer;
    if (one_based_layer <= 0 || static_cast<size_t>(one_based_layer) > m_impl->layers.size())
        return result;

    const LayerIndex &index = m_impl->layers[static_cast<size_t>(one_based_layer - 1)];
    ModalState state = index.state;
    size_t offset = index.begin;
    while (offset < index.end) {
        const size_t newline = m_impl->gcode.find('\n', offset);
        const size_t line_end = newline == std::string::npos
            ? std::min(m_impl->gcode.size(), index.end)
            : std::min(newline, index.end);
        const size_t next = newline == std::string::npos
            ? index.end
            : std::min(newline + 1, index.end);
        process_line(
            std::string_view(m_impl->gcode.data() + offset, line_end - offset),
            state,
            &result.segments);
        offset = next;
    }
    return result;
}

int SnapmakerGCodeLayerCache::layer_count() const
{
    return m_impl->declared_layer_count > 0
        ? m_impl->declared_layer_count
        : static_cast<int>(m_impl->layers.size());
}

int SnapmakerGCodeLayerCache::indexed_layer_count() const
{
    return static_cast<int>(m_impl->layers.size());
}

bool SnapmakerGCodeLayerCache::empty() const
{
    return m_impl->layers.empty();
}

} // namespace GUI
} // namespace Slic3r
