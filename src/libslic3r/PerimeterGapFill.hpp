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
};

// Same conversion for the ordinary generator and a jointly assigned demand.
ExtrusionEntityCollection make_perimeter_gap_fill(PerimeterGapDemand demand);

} // namespace Slic3r
