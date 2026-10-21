/* magpie_slot_wall_plan.h -- the core slot "wall.plan", interface version 1. C only.
 *
 * A wall planner splits a region's walls between nozzles of different sizes on a printer with several
 * nozzles: on each layer it may give the outermost walls of an island to a second, "detail" nozzle and
 * leave the rest to the region's own, and it may move a whole layer's walls to another nozzle. The
 * core generates the walls (Classic and Arachne) with the widths and spacings of the plan, tags each
 * wall as a detail or a large wall, and asks the planner which filament prints each tag. The core picks
 * the implementation per region among the mods' slot declarations (mod.json "slots": "when", "priority",
 * read with the region's parameter values); none picked keeps Orca's walls. Setting "detail_wall_speed"
 * (mm/s, or "<n>%" of the outer wall speed) is the speed of the walls tagged detail.
 *
 * Calls come per region: the core opens a planner state for one region of one print (open), asks
 * the other functions with that state from any thread, and closes it. Everything a planner needs
 * besides the per-call input it reads in open: its parameters (host->param_*) and the region's
 * settings (host->config_*, the ctx carries the region and its object).
 *
 * Units: coordinates and the widths and spacings named *_scaled are integer units (1 =
 * host->scaling_factor_mm mm); other lengths mm. Filaments and toolheads count from 1; 0 means none.
 */
#ifndef MAGPIE_SLOT_WALL_PLAN_H
#define MAGPIE_SLOT_WALL_PLAN_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SLOT_WALL_PLAN "wall.plan"
#define MP_SLOT_WALL_PLAN_VERSION 1

/* The printer's nozzles and filaments as its settings list them. Lists can differ in length; an index
 * past a list's end reads its first entry, as the core's own reads do. A filament is printed on the
 * toolhead filament_toolhead gives it (on a BBL printer filament_map routes filaments to nozzles); a
 * host whose struct ends before filament_toolhead prints filament n on toolhead n. */
typedef struct mp_wall_printer {
    uint32_t           size;
    uint32_t           nozzle_count;             /* nozzle_diameter: one entry per toolhead */
    const double      *nozzle_diameter_mm;
    uint32_t           filament_diameter_count;  /* length of filament_diameter */
    uint32_t           filament_type_count;
    const char *const *filament_type;
    uint32_t           filament_colour_count;
    const char *const *filament_colour;           /* "#RRGGBB", or "" for none */
    uint32_t           filament_soluble_count;
    const uint8_t     *filament_soluble;          /* nonzero: soluble */
    uint32_t           filament_toolhead_count;
    const uint32_t    *filament_toolhead;         /* the 1-based toolhead printing each filament */
} mp_wall_printer;

/* A filament and the toolhead that prints it. All zero: none. */
typedef struct mp_wall_tool {
    uint32_t filament;    /* 1-based */
    uint32_t toolhead;    /* 1-based */
    double   nozzle_mm;   /* its nozzle diameter, mm */
} mp_wall_tool;

/* Which walls a filament question is about: the tags the core puts on walls. */
enum {
    MP_WALL_HINT_AUTO   = 0, /* untagged walls, and the layer as a whole */
    MP_WALL_HINT_DETAIL = 1, /* walls printed with the detail flow */
    MP_WALL_HINT_LARGE  = 2  /* walls of a planned island printed with the large flow */
};

/* One island of one layer, with the flows the core resolved for it. */
typedef struct mp_wall_island {
    uint32_t   size;
    uint64_t   layer_id;            /* 0-based object layer */
    int32_t    orca_walls;          /* walls Orca would print on the island (may be 0) */
    int32_t    reserved;
    const mp_expoly *island;        /* the island as sliced */
    /* The island simplified at the print's resolution setting and united, as the wall generator
     * simplifies islands: the shape the walls follow. */
    const mp_expolys *simplified;
    int64_t    detail_spacing_scaled; /* the detail flow's spacing */
    int64_t    large_spacing_scaled;  /* the region's inner wall flow's spacing */
    double     detail_nozzle_mm, detail_width_mm; /* the detail flow */
    double     large_nozzle_mm, large_width_mm;   /* the region's outer wall flow */
} mp_wall_island;

/* The plan for one island. The core sets it to "Orca's walls" (active 0, total_walls = orca_walls)
 * before the call. */
typedef struct mp_wall_plan {
    uint32_t size;
    uint8_t  active;               /* nonzero: the island is printed with this plan */
    uint8_t  route;                /* nonzero: tag the walls detail/large so they reach their nozzles */
    uint8_t  reserved[2];
    int32_t  total_walls;          /* every wall of the island */
    int32_t  detail_walls;         /* walls 0 .. detail_walls-1 (from the outside) use the detail flow */
    int32_t  requested_detail_walls; /* detail walls asked for, before this layer's adjustment */
    double   overlap_ratio;        /* detail/large boundary overlap, a fraction of the narrower spacing, [0, 1) */
    int64_t  infill_compensation_scaled; /* grows the infill boundary when a layer prints fewer detail walls */
} mp_wall_plan;

/* What a filament may print in a region, for the core's line width and layer height checks. */
typedef struct mp_wall_tool_use {
    uint32_t filament;     /* 1-based */
    /* Outer walls it prints, counted from the outside (0: none); the core checks them with the outer
     * wall width, and at full layer height when the count goes past the walls the core prints at a
     * reduced height. */
    uint32_t outer_walls;
    /* Nonzero: it prints inner walls (the core checks them when the region has inner walls). */
    uint32_t inner_walls;
} mp_wall_tool_use;

typedef struct mp_wall_plan_v1 {
    uint32_t size; /* sizeof(mp_wall_plan_v1) as the mod was built */
    /* A state for the ctx's region. *state stays the planner's until close. */
    mp_status (*open)(void *user, mp_ctx *ctx, const mp_wall_printer *printer, void **state);
    void (*close)(void *user, void *state);
    /* The detail nozzle of layer layer_id, or all zero when the layer has none (then the core prints
     * Orca's walls on it and asks no plan). */
    mp_status (*detail_tool)(void *user, void *state, uint64_t layer_id, mp_wall_tool *out);
    /* The plan for one island of a layer with a detail nozzle. */
    mp_status (*plan)(void *user, void *state, const mp_wall_island *in, mp_wall_plan *out);
    /* The toolhead that prints every wall of layer layer_id, or 0 to keep the region's own: the core
     * then resolves the walls' widths on that toolhead's nozzle. */
    mp_status (*layer_toolhead)(void *user, void *state, uint64_t layer_id, uint32_t *toolhead);
    /* The filament that prints walls tagged `hint` on layer layer_id, where Orca would print them
     * with base_filament; 0 keeps Orca's. */
    mp_status (*wall_filament)(void *user, void *state, uint64_t layer_id, int32_t hint, uint32_t base_filament,
                               uint32_t *filament);
    /* Every filament this planner may print walls of the region with, besides the region's own wall
     * filaments: up to cap entries in out, their number in *n. base_inner_walls: nonzero when the
     * region's own inner wall filament prints inner walls even where the region asks for few walls. */
    mp_status (*tool_uses)(void *user, void *state, mp_wall_tool_use *out, uint32_t cap, uint32_t *n,
                           uint32_t *base_inner_walls);
} mp_wall_plan_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_WALL_PLAN_H */
