#pragma once

// MAGPIE: geometry of the temperature drop tower of the low temperature support interface (Magpie line 1).
// An L-shaped tower at the rear left of the bed that the nozzle prints slowly while it cools to the interface
// temperature, instead of waiting in place.

#include "../ClipperUtils.hpp"
#include "../Config.hpp"
#include "../Flow.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>
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

// The tower position (mm) stored for plate plate_index, or fallback.
inline double temperature_drop_tower_position_at(const ConfigOptionFloats *positions, size_t plate_index,
                                                 double fallback = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION)
{
    if (positions == nullptr || plate_index >= positions->values.size())
        return fallback;
    const double position = positions->values[plate_index];
    return std::isfinite(position) ? position : fallback;
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
