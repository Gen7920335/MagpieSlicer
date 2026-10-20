/* magpie_slot_support_contacts.h -- slot "support.contacts" v1: a support generator that decides where
 * the support touches the object and what its body looks like, and leaves everything else to Orca's
 * normal-support generator. C only; include after magpie_mod_api.h.
 *
 * The core hands the generator one object: its islands per layer, the painted or modifier enforcers
 * and blockers, and its mesh. The generator returns, per object layer, the areas it wants supported
 * (the contacts) and keeps a support body of its own, which the core then cuts at the print Z of every
 * support layer it builds. Orca's normal-support generator prints the result: interface layers, Z and
 * X/Y gaps, raft and toolpaths, the body trimmed by the object like its own base layers. support_type
 * does not apply (the contacts are given). The core picks one support implementation per object among the mods'
 * slot declarations (mod.json "slots": "when", "priority"); host config_str "support_generator" gives the
 * picked implementation's name for the object.
 *
 * Coordinates of polygons are in integer units (1 = host->scaling_factor_mm mm) and in object
 * coordinates; the mesh and every height is in mm. Slicer settings the generator needs (layer height,
 * threshold angle) it reads with host->config_* in the call's ctx, which carries the object's settings.
 */
#ifndef MAGPIE_SLOT_SUPPORT_CONTACTS_H
#define MAGPIE_SLOT_SUPPORT_CONTACTS_H

#include "magpie_geometry.h" /* mp_rings */

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SUPPORT_CONTACTS_SLOT "support.contacts"
#define MP_SUPPORT_CONTACTS_VERSION 1

/* A triangle mesh: vertices x, y, z (mm) and three vertex indices per triangle. */
typedef struct mp_mesh {
    const float   *vertices;       /* [3 * vertex_count] */
    uint32_t       vertex_count;
    const int32_t *indices;        /* [3 * triangle_count] */
    uint32_t       triangle_count;
} mp_mesh;

typedef struct mp_support_contacts_input_v1 {
    uint32_t          size;
    uint32_t          layer_count;
    const mp_expolys *slices;       /* [layer_count] the object's islands on each layer */
    const double     *slice_z;      /* [layer_count] mm, the object-frame height each layer was cut at */
    const double     *layer_height; /* [layer_count] mm */
    const mp_rings   *enforcers;    /* [layer_count] support enforcers (modifiers, then painting) */
    const mp_rings   *blockers;     /* [layer_count] support blockers */
    /* The object's model parts, XY as the layers' object coordinates (mm) and Z at print height: the
     * object's bottom sits at print_z_min. */
    mp_mesh           mesh;
    /* mm: print Z of the object's first layer bottom (above the raft and any lift). */
    double            print_z_min;
    /* mm: top of the raft's interface layers, 0 without raft; the support stands on it. */
    double            raft_interface_top_z;
    /* mm: the support flow's nozzle diameter and line width. */
    double            support_nozzle_diameter;
    double            support_line_width;
} mp_support_contacts_input_v1;

typedef struct mp_support_contacts_output_v1 {
    uint32_t  size;
    uint32_t  layer_count; /* set by the generator: the input's, or 0 for no support */
    mp_rings *contacts;    /* [layer_count] the areas to support on each object layer */
    /* 1: print every support island, even one narrower than the support pattern's line spacing (a
     * body made of thin pillars); 0: Orca's usual filling. */
    int32_t   keep_narrow_islands;
    void     *mod_data;    /* the generator's own: its body, passed to body and freed by release */
} mp_support_contacts_output_v1;

typedef struct mp_support_contacts_v1 {
    uint32_t size;
    /* The contacts and the body of one object. Returns MP_ERR_CANCELLED once host->cancelled(ctx) is set. */
    mp_status (*compute)(void *user, mp_ctx *ctx, const mp_support_contacts_input_v1 *in, mp_support_contacts_output_v1 *out);
    /* The body cut at the print heights z[0..n) (mm), one list of islands per height in out[0..n).
     * The islands stay owned by the generator until the next body call on the same output or release. */
    mp_status (*body)(void *user, mp_ctx *ctx, const mp_support_contacts_output_v1 *of, uint32_t n, const double *z, mp_expolys *out);
    /* Frees what compute put in out. */
    void (*release)(void *user, mp_support_contacts_output_v1 *out);
} mp_support_contacts_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_SUPPORT_CONTACTS_H */
