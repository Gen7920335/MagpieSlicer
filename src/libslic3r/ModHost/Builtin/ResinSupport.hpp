#pragma once

#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Point.hpp"
#include "libslic3r/Polygon.hpp"

#include <string>

namespace Slic3r {

class PrintObject;

namespace ModHost {
namespace ResinSupport {

// One of PrusaSlicer's two SLA support-tree strategies, with the settings PrusaSlicer keeps per
// strategy. The bundled resin_support mod holds one set for each ("default_*" and "branching_*").
struct StrategySettings
{
    double      head_front_diameter_mm      = 0.4;
    double      head_width_mm               = 1.0;
    double      pillar_diameter_mm          = 1.0;
    double      small_pillar_percent        = 50.;
    int         max_bridges_on_pillar       = 3;
    double      max_weight_on_model         = 10.;
    std::string pillar_connection_mode      = "dynamic"; // dynamic, zigzag or cross
    bool        buildplate_only             = false;
    double      pillar_widening_factor      = 0.5;
    double      base_diameter_mm            = 4.0;
    double      base_height_mm              = 1.0;
    double      base_safety_distance_mm     = 1.0;
    double      critical_angle_deg          = 45.;
    double      max_bridge_length_mm        = 15.;
    double      max_pillar_link_distance_mm = 10.;
    double      object_elevation_mm         = 5.0;
};

struct ModSettings
{
    bool             branching              = false; // tree_type == "branching"
    double           points_density_percent = 100.;
    bool             enforcers_only         = false;
    StrategySettings strategy; // the settings of the selected strategy
};

ModSettings read_settings(const PrintObject &object);

// Decides whether an automatic SLA support point stays: inside a support enforcer it always stays;
// otherwise it must be outside every blocker, enforcers-only must be off, and, when the threshold
// mask is enabled, it must lie inside it. An enabled but empty threshold mask rejects every point.
class PointFilter
{
public:
    PointFilter(const Polygons &enforcers, const Polygons &blockers, const Polygons *threshold, bool enforcers_only);
    bool accepts(const Point &point) const;

private:
    ExPolygons m_enforcers, m_blockers, m_threshold;
    bool       m_threshold_enabled;
    bool       m_enforcers_only;
};

// PrusaSlicer 2.9.6's SLA support points and support tree, printed through Orca's normal-support
// pipeline: the tree's cross-sections become the support body and discs around the tree heads the
// contact areas.
void generate(PrintObject &object);

} // namespace ResinSupport
} // namespace ModHost
} // namespace Slic3r
