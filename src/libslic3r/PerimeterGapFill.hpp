#pragma once

#include "ExPolygon.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "Flow.hpp"

namespace Slic3r {

// Geometry before variable-width conversion. Replanning owns this demand,
// rather than clipping or scaling already emitted extrusion paths.
struct PerimeterGapDemand {
    ExPolygons regions;
    Flow flow;
    double min_spacing_scaled;       // Fixed-point distance, 1 unit = 1e-6 mm.
    double max_spacing_scaled;       // Fixed-point distance, 1 unit = 1e-6 mm.
    double simplify_distance_scaled; // Fixed-point distance, 1 unit = 1e-6 mm.
    double minimum_length_scaled;    // Fixed-point distance, 1 unit = 1e-6 mm.
    // MAGPIE: when set, the gap fill is tagged for the wall planner's nozzles: a gap lying in detail_regions (between
    // two detail walls) goes to the detail nozzle, every other gap to the large walls' route.
    bool       route_by_wall_nozzle = false;
    ExPolygons detail_regions;
};

// Same conversion for the ordinary generator and a jointly assigned demand.
ExtrusionEntityCollection make_perimeter_gap_fill(PerimeterGapDemand demand);

} // namespace Slic3r
