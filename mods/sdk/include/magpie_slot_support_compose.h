/* magpie_slot_support_compose.h -- slot "support.compose" v1: a support generator that builds an
 * object's support out of the core's own generators. C only; include after magpie_geometry.h.
 *
 * The core runs its generators for the composer: Orca's normal support, a support.areas mod (by its
 * implementation name) or Orca's tree support, each on a mask, and keeps their support layers as a
 * set the composer names by a handle. The composer decides which generator supports what and finally
 * merges at most two sets into the object's support layers; it never touches a layer itself. Layers
 * of the first set are marked as normal support, of the second as tree support, and a layer both
 * have at the same height as a layer with both channels (Layer.hpp SupportInnerType). Support is one job for
 * support.areas, support.compose and support.contacts: the core picks one implementation of them per object among
 * the mods' slot declarations (mod.json "slots": "when", "priority"); none picked keeps Orca's own.
 *
 * Per object layer lists (mp_rings[layer_count]) are in integer units (1 = host->scaling_factor_mm
 * mm) and object coordinates. Lists the host returns are released with free_layers; lists the
 * composer passes in stay its own.
 */
#ifndef MAGPIE_SLOT_SUPPORT_COMPOSE_H
#define MAGPIE_SLOT_SUPPORT_COMPOSE_H

#include "magpie_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SUPPORT_COMPOSE_SLOT "support.compose"
#define MP_SUPPORT_COMPOSE_VERSION 1

/* Opaque: one compose call (the object, its support sets). */
typedef struct mp_compose mp_compose;

/* A list per object layer, allocated by the host. */
typedef struct mp_layer_rings { mp_rings *layers; uint32_t n; } mp_layer_rings;

typedef struct mp_compose_host_v1 {
    uint32_t size;
    uint32_t layer_count; /* object layers */
    int32_t  has_raft;    /* 1: the object stands on a raft */

    /* 1 when `generator` can run: "orca" or a loaded support.areas implementation. */
    int32_t (*has_generator)(mp_compose *c, const char *generator);
    /* What `generator` would support on each object layer: for "orca" the overhangs of Orca's normal
     * support's top contacts (support_type ignored, painting and modifiers included), for a
     * support.areas implementation its overhang_demand. */
    int32_t (*demand)(mp_compose *c, const char *generator, mp_layer_rings *out);
    /* Per object layer, the area the object's layers below cover: support standing on the build
     * plate cannot pass there. */
    int32_t (*buildplate_shadow)(mp_compose *c, mp_layer_rings *out);
    /* The support channel painting of the object (channel 0 or 1, the two states the support paint
     * tool stores besides plain enforcers), per object layer, kept only where the surface is also
     * painted as a support enforcer. MP_ERR_NOT_FOUND when no part of the object has channel painting. */
    int32_t (*paint_channel)(mp_compose *c, int32_t channel, mp_layer_rings *out);
    void (*free_layers)(mp_compose *c, mp_layer_rings *layers);

    /* Runs normal support `generator` ("orca" or a support.areas implementation) with overhangs
     * supported only inside mask[layer_count] (support_type ignored). *out names the new set,
     * *support_layers its layer count (0: the generator made nothing; the handle stays valid). */
    int32_t (*run_normal)(mp_compose *c, const char *generator, const mp_rings *mask, mp_handle *out, uint32_t *support_layers);
    /* Runs Orca's tree support of `style` ("organic", "tree_slim", "tree_strong", "tree_hybrid";
     * anything else is organic) on the demand mask[layer_count]. With avoid_set nonzero the branches
     * keep clear of the plastic of set `avoid` (0: a set with no layers), which must be a normal set. */
    int32_t (*run_tree)(mp_compose *c, const char *style, const mp_rings *mask, int32_t avoid_set, mp_handle avoid,
                        mp_handle *out, uint32_t *support_layers);
    /* Drops a set. */
    void (*release)(mp_compose *c, mp_handle set);
    /* Makes `first` (normal) and `second` (tree) the object's support layers, merged by height; where
     * both have a layer the first owns any overlap. Either may be 0. Both sets are used up. Call once,
     * last. */
    int32_t (*merge)(mp_compose *c, mp_handle first, mp_handle second);
    /* Clears the tree support preview a tree run left, before falling back to normal support. */
    void (*discard_tree_preview)(mp_compose *c);

    /* Progress line (percent of the whole slice) and slicing warnings, in English; the core translates. */
    void (*status)(mp_compose *c, int32_t percent, const char *text);
    void (*warn)(mp_compose *c, int32_t critical, const char *text);
} mp_compose_host_v1;

typedef struct mp_support_compose_v1 {
    uint32_t size;
    /* Builds the object's support with the host's functions. An error status (message in English)
     * stops the slice; the core then drops every set. MP_ERR_CANCELLED once host->cancelled(ctx). */
    mp_status (*compose)(void *user, mp_ctx *ctx, const mp_compose_host_v1 *host, mp_compose *c);
} mp_support_compose_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_SUPPORT_COMPOSE_H */
