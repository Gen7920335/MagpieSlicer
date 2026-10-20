/* magpie_accel.h -- slot "accel.batch" v1: a batch accelerator (GPU or other) for four inner loops
 * of the core. C only; include after magpie_mod_api.h.
 *
 * The core collects a whole loop's work items into arrays and offers them to the accelerator; the
 * accelerator either computes every item or declines, and on a decline the core computes the batch
 * itself. Each of the four operations has a CPU reference function in the core, and the contract is
 * that an accelerator returns exactly what that function returns, bit for bit:
 *
 *   MP_ACCEL_OP_FACET_LAYER_RANGES  TriangleMeshSlicer facet_layer_range(): the layers a facet spans
 *   MP_ACCEL_OP_VERTICAL_CROSSINGS  FillRectilinear vertical_crossing(): where a segment crosses x
 *   MP_ACCEL_OP_MIN_DIST2           Lightning DistanceField: squared distance to the nearest edge
 *   MP_ACCEL_OP_POINTS_IN_EXPOLYS   TreeSupport is_inside_ex(): point inside any ExPolygon
 *
 * The core enforces the contract: in MP_ACCEL_VERIFY mode it recomputes every item on the CPU and
 * throws the batch away on any difference, so an accelerator cannot change the G-code. Only an
 * official mod may run in MP_ACCEL_TRUSTED mode (results used unchecked); the core turns TRUSTED from
 * any other mod into VERIFY.
 *
 * Units: coordinates are integer units (1 = host->scaling_factor_mm mm), z heights mm (float as the
 * slicer keeps them), squared distances integer units squared.
 */
#ifndef MAGPIE_ACCEL_H
#define MAGPIE_ACCEL_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_ACCEL_SLOT "accel.batch"
#define MP_ACCEL_SLOT_VERSION 1

/* Modes an accelerator asks for at the start of a slicing session. */
enum {
    MP_ACCEL_OFF     = 0, /* not used in this session */
    MP_ACCEL_VERIFY  = 1, /* results checked against the CPU reference */
    MP_ACCEL_TRUSTED = 2  /* results used unchecked; official mods only */
};

enum {
    MP_ACCEL_OP_FACET_LAYER_RANGES = 0,
    MP_ACCEL_OP_VERTICAL_CROSSINGS = 1,
    MP_ACCEL_OP_MIN_DIST2          = 2,
    MP_ACCEL_OP_POINTS_IN_EXPOLYS  = 3,
    MP_ACCEL_OP_COUNT              = 4
};

typedef struct mp_accel_batch_v1 {
    uint32_t size;
    /* A slicing session starts. ctx carries the print's settings, so the mod reads its own parameters
     * (host->param_*); *mode is one of MP_ACCEL_*. Called once per session, before any batch. */
    mp_status (*begin)(void *user, mp_ctx *ctx, int32_t *mode);
    /* The session ended; no batch follows until the next begin. */
    void (*end)(void *user);
    /* Nonzero when the accelerator takes a batch of `items` items of `op` now (worth the round trip,
     * device free). Called from several threads at once. */
    int32_t (*accepts)(void *user, uint32_t op, uint64_t items);
    /* Nonzero when the device works; otherwise `reason` (NUL-terminated, cut to cap) says why. */
    int32_t (*available)(void *user, char *reason, uint32_t cap);

    /* The batches. Each returns MP_OK with every output item filled, or another code and the core
     * computes the batch on the CPU. Called from several threads at once.
     *
     * facet_z: 3 vertex heights per facet (n facets), zs: n_zs slice heights in ascending order.
     * out: per facet {first layer, last layer (exclusive), index 0..2 of the lowest vertex}. */
    mp_status (*facet_layer_ranges)(void *user, const float *facet_z, uint64_t n, const float *zs, uint64_t n_zs,
                                    uint32_t *out);
    /* p1p2x: per crossing {p1.x, p1.y, p2.x, p2.y, x}, neither end on the line x. out_p / out_q: the
     * crossing's y as the rational p / q, q truncated to 32 bits as the core keeps it. */
    mp_status (*vertical_crossings)(void *user, const int64_t *p1p2x, uint64_t n, int64_t *out_p, uint32_t *out_q);
    /* Squared distance from each of n points to the nearest of m edges (edges[2k], edges[2k+1]);
     * DBL_MAX when m is 0. */
    mp_status (*min_dist2)(void *user, const mp_point *points, uint64_t n, const mp_point *edges, uint64_t m,
                           double *out);
    /* out[i] = 1 when any ExPolygon of polys contains points[i], borders included, else 0. */
    mp_status (*points_in_expolys)(void *user, const mp_point *points, uint64_t n, const mp_expolys *polys,
                                   uint8_t *out);
} mp_accel_batch_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_ACCEL_H */
