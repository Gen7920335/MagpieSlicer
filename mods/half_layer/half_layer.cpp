// H/2: half-height outer walls and support (Magpie line 1). Moved from the core's settings onto the layer.bands
// slot: this mod answers which of an object's paths print in two half-height bands per model layer, how far a
// travel over them lifts, how the half-height support grid avoids slivers, and where in the layer's print time
// the later band goes (start-to-start spacing of half the layer's time). The core slices, builds and prints
// the bands. Built with the SDK only.

#include <magpie_mod.hpp>
#include <magpie_slot_layer_bands.h>

#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

#define HALF_LAYER_MOD_ID "half_layer"

MAGPIE_MOD_DEFINE(HALF_LAYER_MOD_ID, "2.0.1")

namespace half_layer {

mp_status plan(void *, mp_ctx *ctx, mp_layer_bands_object_v1 *out)
{
    return magpie::guarded([&] {
        const bool outer_walls = magpie::param_i64(ctx, "outer_walls").value_or(0) != 0;
        // Half-height outer walls need support built on the same half-height grid underneath them (line 1 beta 6).
        const bool support = outer_walls || magpie::param_i64(ctx, "support").value_or(0) != 0;
        out->wall_depths = outer_walls ? 2 : 0; // the outer two walls
        out->support     = support ? 1 : 0;
        if (!outer_walls && !support)
            return;
        out->band_count       = 2;
        out->band_fraction[0] = 0.5;
        out->band_fraction[1] = 0.5;
        // Travel lift over the bands, in model layer heights.
        out->min_travel_lift_layers = 1.5;
        // Support bands: the nominal band (half a layer) and half of it, as fractions of the layer height. The
        // logical support grid keeps independent support layer heights, whose boundaries sit off the model grid
        // (0.011 mm bands under 0.2 mm layers without these limits).
        out->support_band_min_layers = 0.25;
        out->support_band_max_layers = 0.5;
    });
}

// One scan of the boundaries, earliest boundary on ties. `allowed` comes from the core's geometry and ordering,
// never from the time score. The lower block is part of base_s; the later block is not.
mp_status schedule(void *, const mp_band_timing_v1 *timing, const mp_band_boundary_v1 *boundaries, uint32_t n,
                   mp_band_choice_v1 *out)
{
    return magpie::guarded([&] {
        const double base_seconds = timing->base_s, first_start_seconds = timing->first_start_s,
                     first_end_seconds = timing->first_end_s, second_wall_seconds = timing->block_s;
        if (!std::isfinite(base_seconds) || !std::isfinite(first_start_seconds) || !std::isfinite(first_end_seconds) ||
            !std::isfinite(second_wall_seconds) || base_seconds < 0. || first_start_seconds < 0. ||
            first_end_seconds < first_start_seconds || first_end_seconds > base_seconds || second_wall_seconds < 0.)
            throw std::invalid_argument("Invalid half-layer timing interval");
        out->boundary              = UINT32_MAX;
        out->block_start_s         = 0.;
        out->total_s               = 0.;
        out->error_s               = 0.;
        double best_error_seconds  = std::numeric_limits<double>::infinity();
        double previous_elapsed_seconds = 0.;
        for (uint32_t i = 0; i < n; ++i) {
            const mp_band_boundary_v1 &boundary = boundaries[i];
            if (!std::isfinite(boundary.elapsed_s) || !std::isfinite(boundary.approach_s) || !std::isfinite(boundary.departure_s) ||
                !std::isfinite(boundary.replaced_transition_s) || boundary.elapsed_s < previous_elapsed_seconds ||
                boundary.elapsed_s > base_seconds || boundary.approach_s < 0. || boundary.departure_s < 0. ||
                boundary.replaced_transition_s < 0. || boundary.replaced_transition_s > base_seconds - boundary.elapsed_s)
                throw std::invalid_argument("Invalid half-layer insertion boundary");
            previous_elapsed_seconds = boundary.elapsed_s;
            if (boundary.allowed == 0 || boundary.elapsed_s < first_end_seconds)
                continue;
            const double total_seconds = base_seconds + second_wall_seconds + boundary.approach_s + boundary.departure_s -
                                         boundary.replaced_transition_s;
            const double second_start_seconds = boundary.elapsed_s + boundary.approach_s;
            if (!std::isfinite(total_seconds) || !std::isfinite(second_start_seconds))
                throw std::invalid_argument("Half-layer insertion time overflow");
            // Match start-to-start spacing, in seconds. first_start need not be zero when support, priming or
            // other objects precede the first outer wall.
            const double error_seconds = std::abs(second_start_seconds - first_start_seconds - 0.5 * total_seconds);
            if (error_seconds < best_error_seconds) {
                out->boundary      = i;
                out->block_start_s = second_start_seconds;
                out->total_s       = total_seconds;
                out->error_s       = error_seconds;
                best_error_seconds = error_seconds;
            }
        }
    });
}

const mp_layer_bands_v1 table = {sizeof(mp_layer_bands_v1), plan, schedule};

} // namespace half_layer

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    return registrar.register_slot(&registrar, MP_LAYER_BANDS_SLOT, MP_LAYER_BANDS_VERSION, HALF_LAYER_MOD_ID, &half_layer::table,
                                   sizeof(half_layer::table), nullptr);
}

void magpie::mod_shutdown() {}
