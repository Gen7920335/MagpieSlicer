/* magpie_slot_support_style.h -- slot "support.style" v1: how an object's support is printed, for
 * every support generator: the interface pattern, the wall counts of normal and tree support, and a
 * pattern and angle of its own for some interface layers. C only; include after magpie_mod_api.h.
 *
 * The core opens every loaded implementation for an object while it sets up the object's support
 * toolpaths (its ctx carries the object's settings, so an object-scope parameter applies). An
 * implementation that has nothing to change for the object returns no state and is not used. At
 * most one implementation may style an object; two that both do stop the slice. With a state, the
 * core asks once for each interface layer number of each interface stack size it prints (both
 * counted from the contact layer, 1) and keeps the answers for the object.
 */
#ifndef MAGPIE_SLOT_SUPPORT_STYLE_H
#define MAGPIE_SLOT_SUPPORT_STYLE_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SUPPORT_STYLE_SLOT "support.style"
#define MP_SUPPORT_STYLE_VERSION 1

/* Interface fill patterns. */
enum {
    MP_SUPPORT_PATTERN_KEEP        = -1, /* the core's own choice */
    MP_SUPPORT_PATTERN_RECTILINEAR = 0,
    MP_SUPPORT_PATTERN_GRID        = 1,
    MP_SUPPORT_PATTERN_TRIANGLES   = 2,
    MP_SUPPORT_PATTERN_CONCENTRIC  = 3
};

typedef struct mp_support_style_object_v1 {
    uint32_t size;
    /* The pattern of every interface layer (MP_SUPPORT_PATTERN_*); KEEP leaves Orca's
     * support_interface_pattern. A triangle pattern keeps the interface line spacing in each of its
     * three directions. */
    int32_t interface_pattern;
    /* Walls around normal support, 0-10; -1 keeps Orca's. */
    int32_t normal_wall_loops;
    /* Walls around each tree branch as far as it has room, 0-10; -1 keeps Orca's. */
    int32_t tree_wall_loops;
    /* Appended in API 1.1 (write it only when size covers it): the nozzle temperature of the restyled
     * interface layers, degC; 0 keeps the filament's. The core groups those layers' paths and prints them
     * at it (a gcode.events mod may take that over, magpie_slot_gcode_events.h). */
    int32_t sublayer_temperature;
} mp_support_style_object_v1;

typedef struct mp_support_interface_style_v1 {
    uint32_t size;
    /* 1: this interface layer is printed with the pattern and angle below (an interface sublayer). */
    int32_t restyle;
    int32_t pattern;   /* MP_SUPPORT_PATTERN_*; KEEP: the object's interface pattern */
    double  angle_deg; /* fill angle, degrees */
} mp_support_interface_style_v1;

typedef struct mp_support_style_v1 {
    uint32_t size;
    /* The style of the object of ctx. *state = null: nothing to change, *out unused. */
    mp_status (*open)(void *user, mp_ctx *ctx, mp_support_style_object_v1 *out, void **state);
    /* Interface layer `number` (1 = the contact layer) of a stack of `total` interface layers. */
    mp_status (*interface_layer)(void *user, void *state, int32_t number, int32_t total, mp_support_interface_style_v1 *out);
    void (*close)(void *user, void *state);
} mp_support_style_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_SUPPORT_STYLE_H */
