#pragma once

// MAGPIE: auxiliary towers (mod design E17): small towers a mod prints next to the objects, such as a tower the
// nozzle cools down on (gcode.events slot) or the layer time tower. The core owns where a tower goes on each
// plate: the stored per-plate position (project options the 3D scene drags), the placement inside the printable
// area and the area several nozzles share. The tower's shape, size and paths are its owner's.

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Config.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

namespace Slic3r {

// Distance a tower's footprint keeps from the printable area's edge, mm.
inline constexpr double AUX_TOWER_BED_MARGIN = 3.0;
// A stored position below zero asks for the automatic (rear left) position.
inline constexpr double AUX_TOWER_AUTOMATIC_POSITION = -1.0;
// Shortest usable tower side, mm.
inline constexpr double AUX_TOWER_MINIMUM_SIZE = 8.0;

// Where a tower goes inside `bounds` (mm): the footprint's lower left corner (brim included) and the side it
// gets. A position below zero is automatic (rear left); an explicit one is clamped into the bounds. Nothing
// when even the shortest tower does not fit. Shared by G-code export and the 3D scene, so the dragged
// position and the printed tower agree.
struct AuxTowerPlacement
{
    double origin_x { 0. };       // mm
    double origin_y { 0. };       // mm
    double tower_size { 0. };     // mm, the tower's side
    double footprint_size { 0. }; // mm, tower plus brim on both sides
};
inline std::optional<AuxTowerPlacement> place_aux_tower(const BoundingBoxf &bounds, double configured_x, double configured_y,
                                                        double requested_size, double brim_width)
{
    const double usable_margin = AUX_TOWER_BED_MARGIN + brim_width; // mm
    const double tower_size    = std::min(requested_size, std::min(bounds.max.x() - bounds.min.x() - 2.0 * usable_margin,
                                                                   bounds.max.y() - bounds.min.y() - 2.0 * usable_margin));
    if (tower_size < AUX_TOWER_MINIMUM_SIZE)
        return std::nullopt;
    const double footprint_size = tower_size + 2.0 * brim_width;
    const double min_origin_x   = bounds.min.x() + AUX_TOWER_BED_MARGIN;
    const double min_origin_y   = bounds.min.y() + AUX_TOWER_BED_MARGIN;
    const double max_origin_x   = bounds.max.x() - AUX_TOWER_BED_MARGIN - footprint_size;
    const double max_origin_y   = bounds.max.y() - AUX_TOWER_BED_MARGIN - footprint_size;
    if (max_origin_x < min_origin_x || max_origin_y < min_origin_y)
        return std::nullopt;
    AuxTowerPlacement placement;
    placement.origin_x       = configured_x < 0.0 ? min_origin_x : std::clamp(configured_x, min_origin_x, max_origin_x);
    placement.origin_y       = configured_y < 0.0 ? max_origin_y : std::clamp(configured_y, min_origin_y, max_origin_y);
    placement.tower_size     = tower_size;
    placement.footprint_size = footprint_size;
    return placement;
}

// Plate-indexed tower positions (project options): the keys of the x and y vectors. The names are the ones
// the first tower's projects were saved with.
inline constexpr std::array<const char *, 2> AUX_TOWER_POSITION_KEYS { "support_interface_temperature_drop_tower_x",
                                                                       "support_interface_temperature_drop_tower_y" };
// Layer time blending's tower (GCode/LayerTimeTower.hpp).
inline constexpr std::array<const char *, 2> LAYER_TIME_TOWER_POSITION_KEYS { "layer_time_blend_tower_x", "layer_time_blend_tower_y" };
// Every tower's keys: the plate helpers below keep all of them in step with the plates.
inline constexpr std::array<std::array<const char *, 2>, 2> AUX_TOWER_POSITION_KEY_PAIRS { AUX_TOWER_POSITION_KEYS,
                                                                                           LAYER_TIME_TOWER_POSITION_KEYS };

// Sizes every tower's vectors to plate_count (0: the longest of them), filling missing or non-finite entries
// with the automatic position. Returns the keys it changed.
inline std::vector<std::string> normalize_aux_tower_positions(DynamicConfig &config, size_t plate_count = 0)
{
    if (plate_count == 0) {
        plate_count = 1;
        for (const auto &keys : AUX_TOWER_POSITION_KEY_PAIRS)
            for (const char *key : keys)
                if (const auto *positions = config.option<ConfigOptionFloats>(key))
                    plate_count = std::max(plate_count, positions->values.size());
    }
    std::vector<std::string> repaired;
    for (const auto &keys : AUX_TOWER_POSITION_KEY_PAIRS)
        for (const char *key : keys) {
            auto *positions = config.option<ConfigOptionFloats>(key);
            if (positions == nullptr) {
                config.set_key_value(key, new ConfigOptionFloats(std::vector<double>(plate_count, AUX_TOWER_AUTOMATIC_POSITION)));
                repaired.emplace_back(key);
                continue;
            }
            bool changed = positions->values.size() != plate_count;
            positions->values.resize(plate_count, AUX_TOWER_AUTOMATIC_POSITION);
            for (double &position : positions->values)
                if (!std::isfinite(position)) {
                    position = AUX_TOWER_AUTOMATIC_POSITION;
                    changed  = true;
                }
            if (changed)
                repaired.emplace_back(key);
        }
    return repaired;
}

// The tower position (mm) stored for plate plate_index, or fallback.
inline double aux_tower_position_at(const ConfigOptionFloats *positions, size_t plate_index, double fallback = AUX_TOWER_AUTOMATIC_POSITION)
{
    if (positions == nullptr || plate_index >= positions->values.size())
        return fallback;
    const double position = positions->values[plate_index];
    return std::isfinite(position) ? position : fallback;
}

// Removes plate plate_index from the position vectors (they had plate_count_before_erase entries).
inline void erase_aux_tower_plate(DynamicConfig &config, size_t plate_index, size_t plate_count_before_erase)
{
    const size_t old_count = std::max<size_t>(1, plate_count_before_erase);
    normalize_aux_tower_positions(config, old_count);
    if (old_count <= 1)
        return;
    for (const auto &keys : AUX_TOWER_POSITION_KEY_PAIRS)
        for (const char *key : keys)
            if (auto *positions = config.option<ConfigOptionFloats>(key); positions != nullptr && plate_index < positions->values.size())
                positions->values.erase(positions->values.begin() + plate_index);
}

// Carries manually placed towers to the destination plate when an instance changes plates. A manual
// position already on the destination wins. The source is reset to automatic only when reset_source
// (its plate has no printable instances left).
inline void transfer_aux_tower_plate(DynamicConfig &config, size_t source_plate_index, size_t destination_plate_index, size_t plate_count,
                                     bool reset_source)
{
    if (source_plate_index == destination_plate_index || source_plate_index >= plate_count || destination_plate_index >= plate_count)
        return;
    normalize_aux_tower_positions(config, plate_count);
    for (const auto &keys : AUX_TOWER_POSITION_KEY_PAIRS) {
        auto *tower_x = config.option<ConfigOptionFloats>(keys[0]);
        auto *tower_y = config.option<ConfigOptionFloats>(keys[1]);
        if (tower_x == nullptr || tower_y == nullptr)
            continue;
        const double source_x      = aux_tower_position_at(tower_x, source_plate_index);
        const double source_y      = aux_tower_position_at(tower_y, source_plate_index);
        const double destination_x = aux_tower_position_at(tower_x, destination_plate_index);
        const double destination_y = aux_tower_position_at(tower_y, destination_plate_index);
        if (source_x >= 0.0 && source_y >= 0.0 && (destination_x < 0.0 || destination_y < 0.0)) {
            tower_x->values[destination_plate_index] = source_x;
            tower_y->values[destination_plate_index] = source_y;
        }
        if (reset_source) {
            tower_x->values[source_plate_index] = AUX_TOWER_AUTOMATIC_POSITION;
            tower_y->values[source_plate_index] = AUX_TOWER_AUTOMATIC_POSITION;
        }
    }
}

// The printable area every nozzle in physical_tools reaches: the bed cut by each one's own area.
inline Polygons aux_tower_common_area(Polygons bed, const std::vector<Polygons> &tool_areas, const std::vector<size_t> &physical_tools)
{
    for (size_t tool : physical_tools)
        if (tool < tool_areas.size() && !tool_areas[tool].empty())
            bed = intersection(bed, tool_areas[tool]);
    return bed;
}

} // namespace Slic3r
