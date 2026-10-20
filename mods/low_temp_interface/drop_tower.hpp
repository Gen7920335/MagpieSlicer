#pragma once

// The temperature drop tower of the low temperature support interface (Magpie line 1): an L-shaped tower the nozzle
// prints slowly while it cools to the interface temperature, instead of waiting in place. Its shape and size are
// this mod's; the core places it on the plate (gcode.events place_tower) and prints what the mod asks for.

#include <magpie_slot_gcode_events.h>

#include <cmath>
#include <limits>
#include <stdexcept>

namespace lti {

// A plate position, mm.
struct Vec2
{
    double x = 0., y = 0.;
    Vec2() = default;
    Vec2(double x, double y) : x(x), y(y) {}
    Vec2   operator-(const Vec2 &o) const { return Vec2(x - o.x, y - o.y); }
    double norm() const { return std::sqrt(x * x + y * y); }
};

// Lines across each arm of the L.
constexpr int DROP_TOWER_LINE_COUNT = 4;
// Brim loops around the tower's footprint.
constexpr int DROP_TOWER_BRIM_LINE_COUNT = 5;

// Arm length in mm for a temperature drop in degC: 80 mm up to 30 degC, one more mm per degree, at most 110 mm.
// (Line 1 used 50 to 80 mm; Gen asked for an 80 mm base, 2026-10-19.)
constexpr double drop_tower_size(double temperature_delta)
{
    const double growth = temperature_delta > 30.0 ? temperature_delta - 30.0 : 0.0;
    const double size   = 80.0 + growth;
    return size < 80.0 ? 80.0 : (size > 110.0 ? 110.0 : size);
}
static_assert(drop_tower_size(0.0) == 80.0);
static_assert(drop_tower_size(30.0) == 80.0);
static_assert(drop_tower_size(50.0) == 100.0);
static_assert(drop_tower_size(80.0) == 110.0);

// The core's automatic perimeter width for a nozzle (Flow::auto_extrusion_width, frPerimeter), in its own single
// precision: 1.125 x the nozzle.
inline float auto_perimeter_width(double nozzle_mm) { return 1.125f * float(nozzle_mm); }

// The multi-nozzle tower has one fixed physical band. Every tool fills that band with its own line count instead of
// inheriting the first tool's pitch.
inline double drop_tower_multi_pitch(double maximum_nozzle_mm) { return auto_perimeter_width(maximum_nozzle_mm); }

struct DropTowerFill
{
    mp_flow_v1 flow;
    int        line_count;
};

inline DropTowerFill drop_tower_fill(const mp_gcode_writer_v1 &w, const mp_flow_v1 &nominal, double band_width_mm)
{
    // Rounded-rectangle cap contribution to width, in mm. The core's flow owns this quantity; use its width/spacing
    // relation rather than a second formula.
    const double cap_width_mm    = nominal.width - nominal.spacing;
    const double requested_lines = (band_width_mm - cap_width_mm) / nominal.spacing;
    if (!std::isfinite(requested_lines) || requested_lines < 1. || requested_lines > std::numeric_limits<int>::max() - 1.)
        throw std::invalid_argument("Invalid temperature tower band or extrusion flow");
    const int   line_count = int(std::lround(requested_lines));
    const float spacing_mm = float((band_width_mm - cap_width_mm) / line_count);
    DropTowerFill fill{};
    fill.flow.size = sizeof(fill.flow);
    if (w.flow_with_spacing(w.w, &nominal, spacing_mm, &fill.flow) != MP_OK)
        throw std::invalid_argument("Invalid spacing supplied to Flow::with_spacing(), check your layer height and extrusion width.");
    fill.line_count = line_count;
    return fill;
}

} // namespace lti
