#pragma once

// MAGPIE: geometry of the temperature drop tower of the low temperature support interface (Magpie line 1).
// An L-shaped tower at the rear left of the bed that the nozzle prints slowly while it cools to the interface
// temperature, instead of waiting in place.

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../Flow.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

// Lines across each arm of the L.
inline constexpr int TEMPERATURE_DROP_TOWER_LINE_COUNT = 4;
// Brim loops around the tower's footprint.
inline constexpr int TEMPERATURE_DROP_TOWER_BRIM_LINE_COUNT = 5;
// Distance kept from the printable area's edge, mm.
inline constexpr double TEMPERATURE_DROP_TOWER_BED_MARGIN = 3.0;
// A tower position below zero asks for the automatic (rear left) position.
inline constexpr double TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION = -1.0;

// Arm length in mm for a temperature drop in degC: 80 mm up to 30 degC, one more mm per degree, at most
// 110 mm. (Line 1 used 50 to 80 mm; Gen asked for an 80 mm base, 2026-10-19.)
constexpr double temperature_drop_tower_size(double temperature_delta)
{
    const double growth = temperature_delta > 30.0 ? temperature_delta - 30.0 : 0.0;
    const double size   = 80.0 + growth;
    return size < 80.0 ? 80.0 : (size > 110.0 ? 110.0 : size);
}
static_assert(temperature_drop_tower_size(0.0) == 80.0);
static_assert(temperature_drop_tower_size(30.0) == 80.0);
static_assert(temperature_drop_tower_size(50.0) == 100.0);
static_assert(temperature_drop_tower_size(80.0) == 110.0);

// Shortest usable L-arm, mm.
inline constexpr double TEMPERATURE_DROP_TOWER_MINIMUM_SIZE = 8.0;

// Where the tower goes inside `bounds` (mm): the footprint's lower left corner (brim included) and the
// arm length. A position below zero is automatic (rear left); an explicit one is clamped into the
// bounds. Nothing when even the shortest tower does not fit. Shared by G-code export and the 3D scene,
// so the dragged position and the printed tower agree.
struct TemperatureDropTowerPlacement
{
    double origin_x { 0. };       // mm
    double origin_y { 0. };       // mm
    double tower_size { 0. };     // mm, arm length
    double footprint_size { 0. }; // mm, tower plus brim on both sides
};
inline std::optional<TemperatureDropTowerPlacement> place_temperature_drop_tower(const BoundingBoxf &bounds, double configured_x, double configured_y,
                                                                                 double requested_size, double brim_width)
{
    const double usable_margin = TEMPERATURE_DROP_TOWER_BED_MARGIN + brim_width; // mm
    const double tower_size    = std::min(requested_size, std::min(bounds.max.x() - bounds.min.x() - 2.0 * usable_margin,
                                                                   bounds.max.y() - bounds.min.y() - 2.0 * usable_margin));
    if (tower_size < TEMPERATURE_DROP_TOWER_MINIMUM_SIZE)
        return std::nullopt;
    const double footprint_size = tower_size + 2.0 * brim_width;
    const double min_origin_x   = bounds.min.x() + TEMPERATURE_DROP_TOWER_BED_MARGIN;
    const double min_origin_y   = bounds.min.y() + TEMPERATURE_DROP_TOWER_BED_MARGIN;
    const double max_origin_x   = bounds.max.x() - TEMPERATURE_DROP_TOWER_BED_MARGIN - footprint_size;
    const double max_origin_y   = bounds.max.y() - TEMPERATURE_DROP_TOWER_BED_MARGIN - footprint_size;
    if (max_origin_x < min_origin_x || max_origin_y < min_origin_y)
        return std::nullopt;
    TemperatureDropTowerPlacement placement;
    placement.origin_x       = configured_x < 0.0 ? min_origin_x : std::clamp(configured_x, min_origin_x, max_origin_x);
    placement.origin_y       = configured_y < 0.0 ? max_origin_y : std::clamp(configured_y, min_origin_y, max_origin_y);
    placement.tower_size     = tower_size;
    placement.footprint_size = footprint_size;
    return placement;
}

// Lowest positive interface target in degC: support_interface_temperature, or the interface sublayer
// temperature when sublayers are on and colder. The tower is sized for it.
inline int lowest_low_temperature_interface_target(const ConfigBase &config)
{
    auto int_value = [&config](const char *key) {
        const ConfigOption *option = config.option(key);
        return option != nullptr ? option->getInt() : 0;
    };
    int       target   = std::max(0, int_value("support_interface_temperature"));
    const int sublayer = int_value("support_interface_sublayer_temperature");
    // Sublayers exist only from the second interface layer down (the contact layer never is one).
    const bool sublayers = int_value("support_interface_sublayer_start") > 0 && int_value("support_interface_top_layers") >= 2;
    if (sublayers && sublayer > 0 && (target == 0 || sublayer < target))
        target = sublayer;
    return target;
}

// Plate-indexed tower positions (project options): the keys of the x and y vectors.
inline constexpr std::array<const char *, 2> TEMPERATURE_DROP_TOWER_POSITION_KEYS { "support_interface_temperature_drop_tower_x",
                                                                                    "support_interface_temperature_drop_tower_y" };

// Sizes both vectors to plate_count (0: the longer of the two), filling missing or non-finite entries
// with the automatic position. Returns the keys it changed.
inline std::vector<std::string> normalize_temperature_drop_tower_positions(DynamicConfig &config, size_t plate_count = 0)
{
    if (plate_count == 0) {
        plate_count = 1;
        for (const char *key : TEMPERATURE_DROP_TOWER_POSITION_KEYS)
            if (const auto *positions = config.option<ConfigOptionFloats>(key))
                plate_count = std::max(plate_count, positions->values.size());
    }
    std::vector<std::string> repaired;
    for (const char *key : TEMPERATURE_DROP_TOWER_POSITION_KEYS) {
        auto *positions = config.option<ConfigOptionFloats>(key);
        if (positions == nullptr) {
            config.set_key_value(key, new ConfigOptionFloats(std::vector<double>(plate_count, TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION)));
            repaired.emplace_back(key);
            continue;
        }
        bool changed = positions->values.size() != plate_count;
        positions->values.resize(plate_count, TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION);
        for (double &position : positions->values)
            if (!std::isfinite(position)) {
                position = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
                changed  = true;
            }
        if (changed)
            repaired.emplace_back(key);
    }
    return repaired;
}

// The tower position (mm) stored for plate plate_index, or fallback.
inline double temperature_drop_tower_position_at(const ConfigOptionFloats *positions, size_t plate_index,
                                                 double fallback = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION)
{
    if (positions == nullptr || plate_index >= positions->values.size())
        return fallback;
    const double position = positions->values[plate_index];
    return std::isfinite(position) ? position : fallback;
}

// Removes plate plate_index from the position vectors (they had plate_count_before_erase entries).
inline void erase_temperature_drop_tower_plate(DynamicConfig &config, size_t plate_index, size_t plate_count_before_erase)
{
    const size_t old_count = std::max<size_t>(1, plate_count_before_erase);
    normalize_temperature_drop_tower_positions(config, old_count);
    if (old_count <= 1)
        return;
    for (const char *key : TEMPERATURE_DROP_TOWER_POSITION_KEYS)
        if (auto *positions = config.option<ConfigOptionFloats>(key); positions != nullptr && plate_index < positions->values.size())
            positions->values.erase(positions->values.begin() + plate_index);
}

// Carries a manually placed tower to the destination plate when an instance changes plates. A manual
// position already on the destination wins. The source is reset to automatic only when reset_source
// (its plate has no printable instances left).
inline void transfer_temperature_drop_tower_plate(DynamicConfig &config, size_t source_plate_index, size_t destination_plate_index,
                                                  size_t plate_count, bool reset_source)
{
    if (source_plate_index == destination_plate_index || source_plate_index >= plate_count || destination_plate_index >= plate_count)
        return;
    normalize_temperature_drop_tower_positions(config, plate_count);
    auto *tower_x = config.option<ConfigOptionFloats>(TEMPERATURE_DROP_TOWER_POSITION_KEYS[0]);
    auto *tower_y = config.option<ConfigOptionFloats>(TEMPERATURE_DROP_TOWER_POSITION_KEYS[1]);
    if (tower_x == nullptr || tower_y == nullptr)
        return;
    const double source_x      = temperature_drop_tower_position_at(tower_x, source_plate_index);
    const double source_y      = temperature_drop_tower_position_at(tower_y, source_plate_index);
    const double destination_x = temperature_drop_tower_position_at(tower_x, destination_plate_index);
    const double destination_y = temperature_drop_tower_position_at(tower_y, destination_plate_index);
    if (source_x >= 0.0 && source_y >= 0.0 && (destination_x < 0.0 || destination_y < 0.0)) {
        tower_x->values[destination_plate_index] = source_x;
        tower_y->values[destination_plate_index] = source_y;
    }
    if (reset_source) {
        tower_x->values[source_plate_index] = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
        tower_y->values[source_plate_index] = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
    }
}

inline Polygons temperature_drop_tower_common_area(Polygons bed, const std::vector<Polygons> &tool_areas, const std::vector<size_t> &physical_tools)
{
    for (size_t tool : physical_tools)
        if (tool < tool_areas.size() && !tool_areas[tool].empty())
            bed = intersection(bed, tool_areas[tool]);
    return bed;
}

// The multi-nozzle tower has one fixed physical band. Every tool fills that band with its own line count instead of
// inheriting the first tool's pitch.
inline double temperature_drop_tower_multi_pitch(double maximum_nozzle_mm)
{
    return Flow::auto_extrusion_width(frPerimeter, float(maximum_nozzle_mm));
}

struct TemperatureDropTowerFill
{
    Flow flow;
    int  line_count;
};

inline TemperatureDropTowerFill temperature_drop_tower_fill(const Flow &nominal, double band_width_mm)
{
    // Rounded-rectangle cap contribution to width, in mm. Flow owns this quantity; use its width/spacing relation
    // rather than a second formula.
    const double cap_width_mm    = nominal.width() - nominal.spacing();
    const double requested_lines = (band_width_mm - cap_width_mm) / nominal.spacing();
    if (!std::isfinite(requested_lines) || requested_lines < 1. || requested_lines > std::numeric_limits<int>::max() - 1.)
        throw std::invalid_argument("Invalid temperature tower band or extrusion flow");
    const int   line_count = int(std::lround(requested_lines));
    const float spacing_mm = float((band_width_mm - cap_width_mm) / line_count);
    return { nominal.with_spacing(spacing_mm), line_count };
}

} // namespace Slic3r
