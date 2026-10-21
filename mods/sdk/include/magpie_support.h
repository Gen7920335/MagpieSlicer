/* magpie_support.h -- slot "support.areas" v1: a support generator that decides where support goes
 * and leaves the toolpaths to the core. C only; include after magpie_geometry.h.
 *
 * The core hands the generator one object's layers (its islands, the painted or modifier enforcers
 * and blockers, the printable area) and gets back three areas per object layer: the support body, the
 * top interface (roof) and the bottom interface. The core turns them into its support layers (raft,
 * interface, toolpaths) as Orca's normal support does. The core picks one support implementation per
 * object among the mods' slot declarations (mod.json "slots": "when", "priority").
 *
 * Every coordinate is in integer units (1 = host->scaling_factor_mm mm) and in object coordinates.
 * Slicer settings the generator needs (layer height, support distances, support type) it reads with
 * host->config_* in the call's ctx, which carries the object's settings.
 */
#ifndef MAGPIE_SUPPORT_H
#define MAGPIE_SUPPORT_H

#include "magpie_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SUPPORT_AREAS_SLOT "support.areas"
#define MP_SUPPORT_AREAS_VERSION 1

typedef struct mp_support_input_v1 {
    uint32_t          size;
    uint32_t          layer_count;
    const mp_expolys *slices;    /* [layer_count] the object's islands on each layer */
    const mp_rings   *enforcers; /* [layer_count] support enforcers (painted and modifiers) */
    const mp_rings   *blockers;  /* [layer_count] support blockers */
    /* [layer_count] or null: when set, the only places where overhangs may be supported (another
     * generator owns the rest, as in Mixed support). */
    const mp_rings   *mask;
    mp_ring           printable_area;       /* the bed's printable area */
    int64_t           support_line_width;   /* the support flow's width, integer units */
    int64_t           interface_line_width; /* the support interface flow's width, integer units */
    int64_t           support_nozzle_diameter; /* the nozzle printing the support flow, integer units */
    int64_t           object_nozzle_diameter;  /* the nozzle printing the object's walls, integer units */
    /* How far the first layer's brim, skirt or raft reaches past the object on the bed, integer units:
     * room a generator leaves free between its support and the edge of the printable area. */
    int64_t           adhesion_margin;
} mp_support_input_v1;

typedef struct mp_support_output_v1 {
    uint32_t  size;
    uint32_t  layer_count; /* set by the generator: the input's, or 0 for no support */
    mp_rings *body;        /* [layer_count] support body, interfaces taken out */
    mp_rings *roof;        /* [layer_count] top interface */
    mp_rings *bottom;      /* [layer_count] bottom interface */
    /* 1: the first support layer is printed solid; 0: with the lines of the layers above. */
    int32_t   first_layer_dense;
    void     *mod_data;    /* the generator's own, for release */
    /* Appended; written only when size covers them. Parts of a layer's body and roof (taken out of body
     * and roof) printed fractional_drop lower, so the gap to the object above is the top Z distance when
     * that is not a whole number of layers (CuraEngine's fractional roof). Null or a drop of 0: none. */
    mp_rings *fractional_body; /* [layer_count] */
    mp_rings *fractional_roof; /* [layer_count] */
    int64_t   fractional_drop; /* integer units, less than the layer height */
} mp_support_output_v1;

typedef struct mp_support_areas_v1 {
    uint32_t size;
    /* The three areas per layer. Returns MP_ERR_CANCELLED once host->cancelled(ctx) is set. */
    mp_status (*compute)(void *user, mp_ctx *ctx, const mp_support_input_v1 *in, mp_support_output_v1 *out);
    /* What the generator would support on each layer (its overhang areas), in out->body; roof and
     * bottom stay empty. Another generator uses it to split an object between generators. */
    mp_status (*overhang_demand)(void *user, mp_ctx *ctx, const mp_support_input_v1 *in, mp_support_output_v1 *out);
    /* Frees what compute or overhang_demand put in out. */
    void (*release)(void *user, mp_support_output_v1 *out);
} mp_support_areas_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SUPPORT_H */
