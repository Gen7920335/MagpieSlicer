/* magpie_slot_object_z_offset.h -- slot "object.z_offset" v1: lifting an object off the bed (or the
 * raft) so that support can go under its bottom too. C only; include after magpie_mod_api.h.
 *
 * The core asks every loaded implementation, per object, how far to lift the object when it has
 * support (mm, 0 = not at all). It lifts only an object with support of its own, so nothing prints in
 * the air: the support generator fills the gap. Two implementations lifting one object stop the slice.
 * Mod parameters and settings are read with host->param_* and host->config_* in the call's ctx, which
 * carries the object's settings. The answer must depend on nothing else: the core asks again whenever
 * it rebuilds the object's slicing heights.
 */
#ifndef MAGPIE_SLOT_OBJECT_Z_OFFSET_H
#define MAGPIE_SLOT_OBJECT_Z_OFFSET_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_OBJECT_Z_OFFSET_SLOT "object.z_offset"
#define MP_OBJECT_Z_OFFSET_VERSION 1

typedef struct mp_object_z_offset_v1 {
    uint32_t size;
    /* *lift_mm: how far to lift the object in ctx when it has support, mm, 0 or more. */
    mp_status (*z_offset)(void *user, mp_ctx *ctx, double *lift_mm);
} mp_object_z_offset_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_OBJECT_Z_OFFSET_H */
