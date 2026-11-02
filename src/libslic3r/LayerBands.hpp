#pragma once

// MAGPIE: physical extrusion bands inside a model layer (slot layer.bands, mods/sdk/include/magpie_slot_layer_bands.h).
// A model layer keeps its settings, cooling and layer change; a band plan says which of an object's paths print in
// thinner bands and how. The plan comes from a native mod (ModHost/LayerBands.cpp); the core's band executor
// (HalfLayer* sources, walls, support and execution) slices, builds and prints it.

#include "HalfLayerPlan.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

// Where the later band's block goes in a layer's print order: the chosen boundary with its timing, or
// HalfLayerInsertion::unavailable. Arguments: the layer's seconds without the block, when the lower block starts
// and ends, the block's own seconds, and every boundary it could go to.
using LayerBandScheduler = std::function<HalfLayerInsertion(double base_seconds, double first_start_seconds, double first_end_seconds,
                                                            double block_seconds, const std::vector<HalfLayerInsertionBoundary> &boundaries)>;

struct LayerBandPlan
{
    // The mod that made the plan; empty when nothing of the object is banded.
    std::string mod_id;
    // Wall depths from the outer wall printed in the bands (0: walls keep the model layer's height).
    int wall_depths = 0;
    // The object's support prints in the bands.
    bool support = false;
    // Only the support's contact zones print in the bands: contacts and interfaces, and, on a side without
    // interface layers, the support next to the model (the contact layer and the one beyond it). The rest of the
    // support keeps its own height. False: all of the support is banded.
    bool support_contact_only = false;
    // Each band's height as a fraction of its model layer, bottom band first.
    std::vector<double> band_fractions;
    // Travel over banded paths lifts at least this many model layer heights (0: the configured Z hop).
    double min_travel_lift_layers = 0.;
    // Banded support bands are no thinner than min and, where a cut moves, no thicker than max, as fractions of
    // the object's layer height.
    double support_band_min_layers = 0.;
    double support_band_max_layers = 0.;
    // Why the plan could not be made (Print::validate reports it; nothing is banded then).
    std::string error;
    // The mod's choice of where a later band goes; set whenever something is banded.
    LayerBandScheduler scheduler;

    bool walls() const { return wall_depths > 0; }
    bool active() const { return walls() || support; }
    // Same bands (the scheduler belongs to mod_id).
    bool same_as(const LayerBandPlan &other) const
    {
        return mod_id == other.mod_id && wall_depths == other.wall_depths && support == other.support &&
               support_contact_only == other.support_contact_only &&
               band_fractions == other.band_fractions && min_travel_lift_layers == other.min_travel_lift_layers &&
               support_band_min_layers == other.support_band_min_layers && support_band_max_layers == other.support_band_max_layers &&
               error == other.error;
    }
};

// Why the core's band executor cannot print `plan`, or "" when it can. This version prints two equal bands per
// model layer with the outer two walls (or no walls) banded; the slot carries any band layout, and an executor
// that prints more grows here without a new slot version.
inline std::string layer_band_plan_problem(const LayerBandPlan &plan)
{
    if (!plan.error.empty())
        return plan.error;
    if (!plan.active())
        return {};
    if (plan.band_fractions != std::vector<double>{0.5, 0.5} || (plan.wall_depths != 0 && plan.wall_depths != 2))
        return "The mod " + plan.mod_id + " asks for layer bands this version cannot print: it prints two equal bands per layer with the outer two walls.";
    // Lift in model layer heights; support band limits as fractions of the layer height.
    if (!std::isfinite(plan.min_travel_lift_layers) || plan.min_travel_lift_layers < 0. ||
        (plan.support && (!std::isfinite(plan.support_band_min_layers) || !std::isfinite(plan.support_band_max_layers) ||
                          plan.support_band_min_layers <= 0. || plan.support_band_max_layers < plan.support_band_min_layers)))
        return "The mod " + plan.mod_id + " gives layer band limits out of range.";
    return {};
}

// Travel lift over banded paths, mm: at least min_lift_layers model layer heights, or the configured lift when
// that is more (min_lift_layers 0: the configured lift).
inline double band_travel_lift_mm(double configured_lift_mm, double model_layer_height_mm, double min_lift_layers)
{
    if (!std::isfinite(configured_lift_mm) || configured_lift_mm < 0.)
        throw std::invalid_argument("Invalid configured Z hop");
    if (!std::isfinite(min_lift_layers) || min_lift_layers < 0.)
        throw std::invalid_argument("Invalid band travel lift");
    if (min_lift_layers == 0.)
        return configured_lift_mm;
    if (!std::isfinite(model_layer_height_mm) || model_layer_height_mm <= 0.)
        throw std::invalid_argument("Half-layer Z hop requires the parent model height");
    // Required travel lift, in mm, relative to the actual nominal extrusion Z.
    const double minimum_lift_mm = min_lift_layers * model_layer_height_mm;
    if (!std::isfinite(minimum_lift_mm))
        throw std::invalid_argument("Half-layer Z hop overflow");
    return std::max(configured_lift_mm, minimum_lift_mm);
}

} // namespace Slic3r
