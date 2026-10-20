/* magpie_slot_layer_bands.h -- slot "layer.bands" v1: printing some of a model layer's paths in
 * several thinner physical bands. C only; include after magpie_mod_api.h.
 *
 * A model layer stays the owner of its settings, cooling and layer change; the bands are physical
 * extrusion heights inside it. The core slices the bands, builds the banded walls and support on
 * them and runs them in order with the rest of the layer (tool changes, prime tower, Z moves); an
 * implementation decides which paths are banded, how a layer is divided, how far a travel over
 * banded paths lifts, and when in the layer's print time a later band starts.
 *
 * plan: asked once per object whenever the print's settings are applied (its ctx carries the
 * object's settings, so object-scope parameters apply). Nothing banded (wall_depths 0 and support
 * 0) means the implementation does not touch the object. At most one implementation may band an
 * object, and every banded object of a print must use the same one; otherwise Print::validate
 * stops the slice. The answer must depend on the mod's parameters only, and those parameters
 * declare "invalidates" for what they change (walls: posPerimeters, support: posSupportMaterial).
 *
 * The plan is shaped for any number of bands of any height. The core's band executor of this
 * version prints two equal bands (0.5, 0.5) with the outer two walls; it refuses other plans in
 * Print::validate rather than print untested geometry.
 *
 * schedule: asked for each model layer of a banded object that has a lower and a later band block,
 * to pick where in the layer's print order the later band's block goes. The core lists every place
 * it could go (a boundary between two tasks of the layer, in print order) with its timing; the
 * implementation returns the boundary it picks. Times are seconds.
 */
#ifndef MAGPIE_SLOT_LAYER_BANDS_H
#define MAGPIE_SLOT_LAYER_BANDS_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_LAYER_BANDS_SLOT "layer.bands"
#define MP_LAYER_BANDS_VERSION 1
/* Most bands a model layer can be divided into. */
#define MP_LAYER_BANDS_MAX 8

typedef struct mp_layer_bands_object_v1 {
    uint32_t size;
    /* Wall depths, counted from the outer wall (1 = the outer wall only), printed in the bands;
     * deeper walls keep the model layer's height. 0: walls are not banded. */
    int32_t wall_depths;
    /* Nonzero: the object's support is printed in bands. */
    int32_t support;
    /* Bands per model layer and each band's height as a fraction of its model layer, bottom band
     * first; the fractions are above 0 and add up to 1. Ignored when nothing is banded. */
    uint32_t band_count;
    double band_fraction[MP_LAYER_BANDS_MAX];
    /* A travel over banded paths lifts at least this many model layer heights above the nozzle's
     * nominal Z (or the configured Z hop, when that is more). 0: the configured Z hop. */
    double min_travel_lift_layers;
    /* Banded support divides each support layer into bands no thinner than min and, where a cut
     * has to move, no thicker than max, both as fractions of the object's layer height. */
    double support_band_min_layers;
    double support_band_max_layers;
} mp_layer_bands_object_v1;

/* One place the later band's block can go in a layer's print order. */
typedef struct mp_band_boundary_v1 {
    /* Time from the layer's start to this boundary in the order without the block. */
    double elapsed_s;
    /* Travel or tool change into the block and out of it again, when it goes here, and the transition
     * between the two tasks it would separate (no longer made then). */
    double approach_s;
    double departure_s;
    double replaced_transition_s;
    /* Zero: the block cannot go here (it would split a path group or come after a task that needs
     * the later band first). */
    int32_t allowed;
} mp_band_boundary_v1;

typedef struct mp_band_timing_v1 {
    uint32_t size;
    /* The layer's time without the block. */
    double base_s;
    /* When the lower band's block starts and ends in that order. */
    double first_start_s;
    double first_end_s;
    /* The later band's block on its own. */
    double block_s;
} mp_band_timing_v1;

typedef struct mp_band_choice_v1 {
    uint32_t size;
    /* Index of the chosen boundary, or UINT32_MAX when none can be used. */
    uint32_t boundary;
    /* When the block then starts, the layer's whole time with it, and how far the block's start is
     * from where the implementation wanted it (its own measure; the core keeps it for tests). */
    double block_start_s;
    double total_s;
    double error_s;
} mp_band_choice_v1;

typedef struct mp_layer_bands_v1 {
    uint32_t size;
    /* The object's bands, for the object in ctx. */
    mp_status (*plan)(void *user, mp_ctx *ctx, mp_layer_bands_object_v1 *out);
    /* Where the later band's block goes: one of boundaries[0..n). */
    mp_status (*schedule)(void *user, const mp_band_timing_v1 *timing, const mp_band_boundary_v1 *boundaries, uint32_t n,
                          mp_band_choice_v1 *out);
} mp_layer_bands_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_LAYER_BANDS_H */
