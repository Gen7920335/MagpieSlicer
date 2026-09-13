#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <stdexcept>
#include <vector>

namespace Slic3r {

// A model layer remains the owner of layer-indexed settings. These intervals
// describe physical extrusion bands, not extra model layers.
struct HalfLayerBand {
    size_t model_layer_id;
    unsigned sublayer;
    double bottom_z_mm;
    double print_z_mm;

    double height_mm() const { return print_z_mm - bottom_z_mm; }
    double slice_z_mm() const { return bottom_z_mm + 0.5 * height_mm(); }
};

inline std::array<HalfLayerBand, 2> split_model_layer_half(
    size_t model_layer_id, double bottom_z_mm, double print_z_mm)
{
    if (!std::isfinite(bottom_z_mm) || !std::isfinite(print_z_mm) ||
        bottom_z_mm < 0. || print_z_mm <= bottom_z_mm)
        throw std::invalid_argument("Half-layer plan requires a finite positive Z interval");
    const double middle_z_mm = bottom_z_mm + 0.5 * (print_z_mm - bottom_z_mm);
    if (middle_z_mm <= bottom_z_mm || middle_z_mm >= print_z_mm)
        throw std::invalid_argument("Half-layer interval cannot be represented at this Z");
    return {{{model_layer_id, 0, bottom_z_mm, middle_z_mm},
             {model_layer_id, 1, middle_z_mm, print_z_mm}}};
}

struct HalfLayerWallCounts {
    int outer;
    int inner;
};

// effective_xy_walls is the count AFTER the existing multi-nozzle and local
// one-wall/narrow-geometry rules. A temporal repeat never consumes an XY wall.
inline HalfLayerWallCounts half_layer_wall_counts(int effective_xy_walls, bool enabled)
{
    if (effective_xy_walls < 0)
        throw std::invalid_argument("Negative XY wall count");
    const int outer = std::min(effective_xy_walls, enabled ? 2 : 1);
    return {outer, effective_xy_walls - outer};
}

// Nozzle ownership is deliberately independent of the outer-wall classification.
inline bool is_half_layer_outer_wall(size_t inset_index, bool enabled)
{
    return enabled && inset_index < 2;
}

inline double half_layer_travel_lift_mm(double configured_lift_mm,
                                       double model_layer_height_mm,
                                       bool wall_enabled, bool support_enabled)
{
    if (!std::isfinite(configured_lift_mm) || configured_lift_mm < 0.)
        throw std::invalid_argument("Invalid configured Z hop");
    if (!wall_enabled && !support_enabled)
        return configured_lift_mm;
    if (!std::isfinite(model_layer_height_mm) || model_layer_height_mm <= 0.)
        throw std::invalid_argument("Half-layer Z hop requires the parent model height");
    // Required travel lift, in mm, relative to the actual nominal extrusion Z.
    const double minimum_lift_mm = 1.5 * model_layer_height_mm;
    if (!std::isfinite(minimum_lift_mm))
        throw std::invalid_argument("Half-layer Z hop overflow");
    return std::max(configured_lift_mm, minimum_lift_mm);
}

// Cost of an existing immutable path block. Speed must already include physical
// nozzle, role and volumetric limits. This estimate adds no motion simulation.
inline double half_layer_path_seconds(double length_mm, double effective_speed_mm_s)
{
    if (!std::isfinite(length_mm) || length_mm < 0. ||
        !std::isfinite(effective_speed_mm_s) || effective_speed_mm_s <= 0.)
        throw std::invalid_argument("Invalid half-layer path time input");
    const double seconds = length_mm / effective_speed_mm_s;
    if (!std::isfinite(seconds))
        throw std::invalid_argument("Half-layer path time overflow");
    return seconds;
}

struct HalfLayerInsertionBoundary {
    // Time in the existing order before inserting the second outer-wall block.
    double elapsed_seconds;
    // New transition time before and after the inserted block. Existing removed
    // transition cost is supplied separately, to avoid counting that travel twice.
    double approach_seconds;
    double departure_seconds;
    double replaced_transition_seconds;
    bool allowed;
};

struct HalfLayerInsertion {
    static constexpr size_t unavailable = std::numeric_limits<size_t>::max();
    size_t boundary = unavailable;
    double second_start_seconds = 0.;
    double total_seconds = 0.;
    double midpoint_error_seconds = 0.;
};

// One scan of existing boundaries, with deterministic earliest-boundary tie
// breaking. 'allowed' comes from geometry/ordering, never from the time score.
// The first wall is already part of base_seconds; the second wall is not.
inline HalfLayerInsertion select_half_layer_insertion(
    double base_seconds, double first_start_seconds, double first_end_seconds,
    double second_wall_seconds, const std::vector<HalfLayerInsertionBoundary>& boundaries)
{
    if (!std::isfinite(base_seconds) || !std::isfinite(first_start_seconds) ||
        !std::isfinite(first_end_seconds) || !std::isfinite(second_wall_seconds) ||
        base_seconds < 0. || first_start_seconds < 0. ||
        first_end_seconds < first_start_seconds || first_end_seconds > base_seconds ||
        second_wall_seconds < 0.)
        throw std::invalid_argument("Invalid half-layer timing interval");
    HalfLayerInsertion best;
    double best_error_seconds = std::numeric_limits<double>::infinity();
    double previous_elapsed_seconds = 0.;
    for (size_t i = 0; i < boundaries.size(); ++i) {
        const auto& boundary = boundaries[i];
        if (!std::isfinite(boundary.elapsed_seconds) ||
            !std::isfinite(boundary.approach_seconds) ||
            !std::isfinite(boundary.departure_seconds) ||
            !std::isfinite(boundary.replaced_transition_seconds) ||
            boundary.elapsed_seconds < previous_elapsed_seconds ||
            boundary.elapsed_seconds > base_seconds || boundary.approach_seconds < 0. ||
            boundary.departure_seconds < 0. || boundary.replaced_transition_seconds < 0. ||
            boundary.replaced_transition_seconds > base_seconds - boundary.elapsed_seconds)
            throw std::invalid_argument("Invalid half-layer insertion boundary");
        previous_elapsed_seconds = boundary.elapsed_seconds;
        if (!boundary.allowed || boundary.elapsed_seconds < first_end_seconds)
            continue;
        const double total_seconds = base_seconds + second_wall_seconds +
            boundary.approach_seconds + boundary.departure_seconds - boundary.replaced_transition_seconds;
        const double second_start_seconds = boundary.elapsed_seconds + boundary.approach_seconds;
        if (!std::isfinite(total_seconds) || !std::isfinite(second_start_seconds))
            throw std::invalid_argument("Half-layer insertion time overflow");
        // Match start-to-start spacing, in seconds. first_start need not be zero
        // when support, priming or other objects precede the first outer wall.
        const double error_seconds = std::abs(
            second_start_seconds - first_start_seconds - 0.5 * total_seconds);
        if (error_seconds < best_error_seconds) {
            best = {i, second_start_seconds, total_seconds, error_seconds};
            best_error_seconds = error_seconds;
        }
    }
    return best;
}

struct HalfLayerToolVisit {
    unsigned physical_tool;
    unsigned filament;
};

// A->B->A must survive. Only adjacent visits to the same tool AND material may
// collapse; shared-nozzle material changes still require their normal purge.
inline void append_half_layer_tool_visit(std::vector<HalfLayerToolVisit>& visits,
                                        HalfLayerToolVisit visit)
{
    if (visits.empty() || visits.back().physical_tool != visit.physical_tool ||
        visits.back().filament != visit.filament)
        visits.push_back(visit);
}

} // namespace Slic3r
