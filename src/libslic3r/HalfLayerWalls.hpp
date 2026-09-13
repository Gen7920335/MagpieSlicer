#pragma once

#include "Layer.hpp"
#include "HalfLayerPlan.hpp"
#include "PerimeterGapFill.hpp"
#include <memory>
#include <optional>

namespace Slic3r {

// Detached generator output. It does not modify the source LayerRegion or
// replace normal print output. A shell/core assembler must select the matching
// depth groups and verify their physical contact before scheduling these paths.
struct HalfLayerWallGeometry {
    ExtrusionEntityCollection perimeters;
    ExtrusionEntityCollection thin_fills;
    SurfaceCollection fill_surfaces;
    ExPolygons fill_no_overlap;
    std::vector<PerimeterGapDemand> gap_demands;
};

// The source owns the actual cross section / physical neighbors; its id remains
// the logical parent id. parent_height_mm is explicit to avoid halving an
// already half-height source again. Outer two depths use parent H/2, others H.
HalfLayerWallGeometry make_half_layer_wall_geometry(
    const LayerRegion &source, const SurfaceCollection &slices,
    const LayerRegionPtrs &compatible_regions, double parent_height_mm);

enum class HalfLayerWallGroup { Shell, Core };

// Preserve complete loops (including mixed bridge roles), nested ordering and
// tool hints. Selection is by XY inset identity, never by role or nozzle. The
// returned tree owns its paths. Unknown inset identities are rejected, not
// silently assigned to the core. Gap fill has a separate geometry owner.
std::unique_ptr<ExtrusionEntityCollection> select_half_layer_wall_group(
    const ExtrusionEntityCollection &perimeters, HalfLayerWallGroup group);

struct HalfLayerGapFill {
    std::array<ExtrusionEntityCollection, 2> phases;
    ExtrusionEntityCollection core;
};

// PERMANENT DIAGNOSTIC: distinguish a construction-only core from a physical
// core and a source height mismatch; neither may silently omit gap deposition.
enum class HalfLayerGapOwnership { NoGaps, ShellOnly, JointDemand, Unsplit, NeedsJointCore, IncompatiblePhaseHeight };

// Geometry candidate, deliberately not an executable print schedule. An absent
// gap_fill means ownership is unresolved, not that gaps may be omitted. Raw
// generator outputs are retained for inspection/replanning, never concatenated
// with the assigned output. Joint contact/material coverage is a separate gate.
struct HalfLayerRegionWallCandidate {
    int print_object_region_id = -1;
    std::array<HalfLayerBand, 2> bands;
    std::array<std::unique_ptr<ExtrusionEntityCollection>, 2> shells;
    std::unique_ptr<ExtrusionEntityCollection> core;
    std::array<ExtrusionEntityCollection, 2> unassigned_phase_gap_fill;
    ExtrusionEntityCollection unassigned_core_gap_fill;
    std::array<std::vector<PerimeterGapDemand>, 2> phase_gap_demands;
    std::vector<PerimeterGapDemand> core_gap_demands;
    std::optional<HalfLayerGapFill> gap_fill;
    HalfLayerGapOwnership gap_ownership = HalfLayerGapOwnership::NeedsJointCore;
    SurfaceCollection core_slices;
    SurfaceCollection core_fill_surfaces;
    ExPolygons core_fill_no_overlap;
};

// Same-region, homogeneous-surface input only: region grouping / mixed surface
// classification belongs to the layer planner, and must not be guessed here.
// Original layers and source grids are never modified. The returned paths own
// their geometry and do not borrow pointers into the temporary grids.
HalfLayerRegionWallCandidate make_half_layer_region_wall_candidate(
    const LayerRegion &parent, const LayerRegion &lower, const LayerRegion &upper);

struct HalfLayerRegionFill {
    SurfaceCollection surfaces;
    ExPolygons no_overlap;
};

struct HalfLayerLayerCandidate {
    std::vector<HalfLayerRegionWallCandidate> groups;
    // Indexed by original parent region, including empty regions. Geometry of
    // compatible regions is generated together, then fill ownership split back.
    std::vector<HalfLayerRegionFill> fills;
};

HalfLayerLayerCandidate make_half_layer_layer_candidate(
    const Layer &parent, const Layer &lower, const Layer &upper);

} // namespace Slic3r
