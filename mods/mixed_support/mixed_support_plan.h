/* mixed_support_plan.h -- service "mixed_support.plan" v1 of the Mixed support mod: which support generator
 * takes each support island (the split Mixed support composes its support from). For other mods and for the
 * core's tests. C only.
 *
 * Lists are per object layer (mp_rings[layer_count]), integer units. The result is the mod's own and is
 * released with release.
 */
#ifndef MIXED_SUPPORT_PLAN_H
#define MIXED_SUPPORT_PLAN_H

/* Relative, so the core's tests reach it without the SDK include path. */
#include "../sdk/include/magpie_geometry.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIXED_SUPPORT_PLAN_SERVICE "mixed_support.plan"
#define MIXED_SUPPORT_PLAN_VERSION 1

enum { MIXED_CHANNEL_NORMAL = 0, MIXED_CHANNEL_TREE = 1, MIXED_CHANNEL_SPLIT = 2 };

typedef struct mixed_plan_input_v1 {
    uint32_t        size;
    uint32_t        layer_count;
    const mp_rings *demand;            /* [layer_count] support demand */
    const mp_rings *buildplate_shadow; /* [layer_count] what support standing on the plate cannot pass */
    double          coverage_threshold_percent;
    int32_t         selective_merge;
    const mp_rings *painted_normal;    /* [layer_count] or null */
    const mp_rings *painted_tree;      /* [layer_count] or null */
} mixed_plan_input_v1;

typedef struct mixed_plan_output_v1 {
    uint32_t  size;
    uint32_t  layer_count;
    mp_rings *normal_mask;           /* [layer_count] */
    mp_rings *tree_mask;             /* [layer_count] */
    mp_rings *normal_paint_fallback; /* [layer_count] normal painting that cannot reach the plate */
    int32_t  *decisions;             /* MIXED_CHANNEL_* per island, in layer order, then by bounding box */
    uint32_t  decision_count;
    void     *mod_data;
} mixed_plan_output_v1;

typedef struct mixed_support_plan_v1 {
    uint32_t size;
    mp_status (*plan)(const mixed_plan_input_v1 *in, mixed_plan_output_v1 *out);
    void (*release)(mixed_plan_output_v1 *out);
} mixed_support_plan_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MIXED_SUPPORT_PLAN_H */
