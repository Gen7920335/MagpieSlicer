#pragma once

#include "../Flow.hpp"
#include "../PrintConfig.hpp"
#include "../ClipperUtils.hpp"

#include <cmath>
#include <limits>
#include <stdexcept>

namespace Slic3r {

inline Polygons temperature_drop_tower_common_area(
    Polygons bed, const std::vector<Polygons> &tool_areas, const std::vector<size_t> &physical_tools)
{
    for (size_t tool : physical_tools)
        if (tool < tool_areas.size() && !tool_areas[tool].empty())
            bed = intersection(bed, tool_areas[tool]);
    return bed;
}

// The multi-nozzle tower has one fixed physical band. Every tool fills that
// band with its own line count instead of inheriting the first tool's pitch.
inline double temperature_drop_tower_multi_pitch(double maximum_nozzle_mm)
{
    return Flow::auto_extrusion_width(frPerimeter, float(maximum_nozzle_mm));
}

struct TemperatureDropTowerFill {
    Flow flow;
    int line_count;
};

inline TemperatureDropTowerFill temperature_drop_tower_fill(const Flow &nominal, double band_width_mm)
{
    // Rounded-rectangle cap contribution to width, in mm. Flow owns this
    // quantity; use its width/spacing relation rather than a second formula.
    const double cap_width_mm = nominal.width() - nominal.spacing();
    const double requested_lines = (band_width_mm - cap_width_mm) / nominal.spacing();
    if (!std::isfinite(requested_lines) || requested_lines < 1. || requested_lines > std::numeric_limits<int>::max() - 1.)
        throw std::invalid_argument("Invalid temperature tower band or extrusion flow");
    const int line_count = int(std::lround(requested_lines));
    const float spacing_mm = float((band_width_mm - cap_width_mm) / line_count);
    return {nominal.with_spacing(spacing_mm), line_count};
}

} // namespace Slic3r
