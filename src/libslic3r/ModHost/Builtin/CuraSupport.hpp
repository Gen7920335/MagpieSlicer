#pragma once

#include <map>
#include <string>
#include <vector>

#include "libslic3r/Polygon.hpp"

namespace Slic3r {

class PrintObject;

namespace ModHost {
namespace CuraSupport {

// Settings of CuraEngine's normal support that Orca has no setting for. They are the parameters of
// the bundled cura_support mod ("cura_support.<name>"); every value here is in Cura's own terms.
struct ModSettings
{
    double join_distance_mm         = 2.0;  // support_join_distance
    double min_feature_width_mm     = 0.;   // min_even_wall_line_width; 0 = 0.85 x support line width
    bool   z_overrides_xy           = true; // support_xy_overrides_z == z_overrides_xy
    double xy_distance_overhang_mm  = 0.2;  // support_xy_distance_overhang
    double stair_step_height_mm     = 0.3;  // support_bottom_stair_step_height
    double stair_step_width_mm      = 5.0;  // support_bottom_stair_step_width
    double stair_step_min_slope_deg = 10.;  // support_bottom_stair_step_min_slope
    double minimum_support_area_mm2 = 0.;   // minimum_support_area
    double minimum_roof_area_mm2    = 1.;   // minimum_roof_area
    double minimum_bottom_area_mm2  = 1.;   // minimum_bottom_area
    double roof_offset_mm           = 0.;   // support_roof_offset
    double bottom_offset_mm         = 0.;   // support_bottom_offset
    bool   use_towers               = false; // support_use_towers
    double tower_diameter_mm        = 3.0;  // support_tower_diameter
    double tower_max_supported_mm   = 3.0;  // support_tower_maximum_supported_diameter
    double tower_roof_angle_deg     = 65.;  // support_tower_roof_angle
    bool   conical_enabled          = false; // support_conical_enabled
    double conical_angle_deg        = 30.;  // support_conical_angle
    double conical_min_width_mm     = 5.0;  // support_conical_min_width
    bool   solid_first_layer        = false; // Magpie line 1's cura_solid_support_raft
};

// Cura's support areas for one object, one entry per object layer, before they become Orca
// support layers. Exposed for tests.
struct Areas
{
    std::vector<Polygons> support; // body, with roofs and bottoms taken out
    std::vector<Polygons> roofs;
    std::vector<Polygons> bottoms;
};

// mask: when set, per object layer, the only places overhangs may be supported (Mixed support).
Areas compute_areas(const PrintObject &object, const ModSettings &settings, const std::vector<Polygons> *mask = nullptr);

// Cura's overhang per object layer (AreaSupport's full overhang area): what Cura support would
// support at that layer.
std::vector<Polygons> overhang_demand(const PrintObject &object, const ModSettings &settings);

// The support.generator module "cura": CuraEngine's normal support areas, printed with Orca's
// support toolpaths. mask as for compute_areas.
void generate(PrintObject &object, const std::vector<Polygons> *mask = nullptr);

// Reads the cura_support mod parameters from the object's full print config.
ModSettings read_settings(const PrintObject &object);

} // namespace CuraSupport
} // namespace ModHost
} // namespace Slic3r
