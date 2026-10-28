#pragma once

// What the LESIC annotations used of the core: its 2D vector (Eigen's Vec2d: element-wise arithmetic, norm and dot
// product in double precision), EPSILON and the calibration values (Calib_Params, named as the core names them).

#include <cmath>
#include <string>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace lesic {

// The core's EPSILON (libslic3r.h), mm.
constexpr double EPSILON = 1e-4;

struct Vec2d
{
    double v[2] = {0., 0.};
    Vec2d() = default;
    Vec2d(double x, double y) : v{x, y} {}
    static Vec2d  Zero() { return Vec2d(0., 0.); }
    double       &x() { return v[0]; }
    double       &y() { return v[1]; }
    const double &x() const { return v[0]; }
    const double &y() const { return v[1]; }
    Vec2d  operator+(const Vec2d &o) const { return Vec2d(v[0] + o.v[0], v[1] + o.v[1]); }
    Vec2d  operator-(const Vec2d &o) const { return Vec2d(v[0] - o.v[0], v[1] - o.v[1]); }
    Vec2d  operator*(double s) const { return Vec2d(v[0] * s, v[1] * s); }
    Vec2d  operator-() const { return Vec2d(-v[0], -v[1]); }
    double dot(const Vec2d &o) const { return v[0] * o.v[0] + v[1] * o.v[1]; }
    double squaredNorm() const { return v[0] * v[0] + v[1] * v[1]; }
    double norm() const { return std::sqrt(this->squaredNorm()); }
};

// The values of the calibration the annotations read (the core's Calib_Params fields).
struct Calib_Params
{
    double start = 0., end = 1., step = 0.1;
    double mvs_start = 8., mvs_end = 24.;
    int    lesic_layers_per_temp = 10;
    double lesic_center_x = 0., lesic_center_y = 0.;
    double lesic_circle_diameter = 0.;
    double lesic_line_width = 0.;
    double lesic_layer_height = 0.;
    bool   lesic_small_bed = false;
};

// The settings the bottom label names: printer, filament (of the active filament), nozzle 0 and layer height.
struct LabelConfig
{
    std::string printer_model;
    std::string filament_type;
    std::string filament_vendor;
    double      nozzle_diameter_0 = 0.;
    double      layer_height      = 0.;
};

} // namespace lesic
