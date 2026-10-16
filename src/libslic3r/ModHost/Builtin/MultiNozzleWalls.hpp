#pragma once

#include <cstddef>
#include <optional>
#include <string>

namespace Slic3r {

class PrintConfig;
class PrintRegionConfig;

namespace ModHost {

struct WallPlanner;

namespace MultiNozzleWalls {

// A filament and the toolhead (hotend) that prints it. Toolheads are the printer's extruders: on a
// tool changer such as the Snapmaker U1, filament n is printed by toolhead n.
struct WallTool
{
    unsigned int filament_1based { 0 };
    unsigned int toolhead_1based { 0 };
    double       nozzle_diameter { 0. }; // mm

    explicit operator bool() const { return filament_1based > 0 && toolhead_1based > 0 && nozzle_diameter > 0.; }
};

WallTool tool_for_filament(const PrintConfig &config, unsigned int filament_1based);

// The tool for the detail (outer) walls of walls printed by base_filament: the smallest nozzle that
// holds the same material in the same colour; failing that, crisp_corner_detail_toolhead when it is
// smaller; otherwise the base tool itself (no detail nozzle).
WallTool detail_tool(const PrintConfig &config, const PrintRegionConfig &region, unsigned int base_filament_1based);

struct OverrideRange
{
    std::size_t  first_layer { 0 }; // 1-based, inclusive
    std::size_t  last_layer { 0 };  // 1-based, inclusive
    unsigned int toolhead_1based { 0 };
};
// "first:last:toolhead"; a reversed range is put in order. Nothing when malformed.
std::optional<OverrideRange> parse_override_range(const std::string &text);
// The toolhead that prints every wall of layer layer_id (0-based), or 0.
unsigned int override_toolhead(const PrintRegionConfig &region, std::size_t layer_id, std::size_t toolhead_count);

// Detail walls printed on layer layer_id: 0 when off or overridden; with interlocking, one fewer on
// odd layers (never below one).
int detail_wall_count_for_layer(const PrintRegionConfig &region, std::size_t layer_id, std::size_t toolhead_count);

// The "multi_nozzle" wall planner.
WallPlanner make_planner();

} // namespace MultiNozzleWalls
} // namespace ModHost
} // namespace Slic3r
