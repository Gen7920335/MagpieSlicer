/* magpie_slot_time_plan.h -- the core slot "gcode.time_plan", interface version 1. C only.
 *
 * A time planner decides how long the print waits at each wait site of its G-code. The core marks the
 * sites while it exports (at the start of a full layer, before its first extrusion), estimates the
 * G-code's times, asks the planner, then writes the waits and re-estimates. The planner sees numbers
 * only: it never touches G-code or geometry. The core picks the implementation among the mods' slot
 * declarations (mod.json "slots": "when", "priority"); none picked marks no sites. Its "settings" give
 * what the export reads: wait_at, tower_size, park_x, park_y (mm), heat_rate, cool_rate (degC/s).
 *
 * Units: times s. Indices count sites from 0 in G-code order.
 */
#ifndef MAGPIE_SLOT_TIME_PLAN_H
#define MAGPIE_SLOT_TIME_PLAN_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_SLOT_TIME_PLAN "gcode.time_plan"
#define MP_SLOT_TIME_PLAN_VERSION 1

/* One wait site, as the core measured it in the G-code before any wait was written. */
typedef struct mp_wait_site {
    uint64_t layer_id;    /* G-code layer printed just before the site (the layer whose time interval_s is) */
    /* Estimated time from the previous site to this one, s. The first site's is the time before it and
     * is not a layer time. */
    double interval_s;
    double park_trip_s;   /* time a park trip from this site takes, s; 0 = the site cannot park */
    /* A tower block around the site: the time from the block's start to the site, and the whole block's
     * time, s (both 0 when there is none). Leaving the blocks out from some site on shortens the layers
     * there. */
    double tower_pre_s;
    double tower_block_s;
    /* Nonzero: starts a new chain, like site 0 (the next object of a by-object print). Its interval is
     * not a layer time and no constraint links the layers on either side of it. */
    uint8_t constraint_break;
    /* Nonzero: the interval holds time the estimator only guesses (heating waits, tool changes). */
    uint8_t uncertain;
    uint8_t reserved[6];
} mp_wait_site;

/* Warning kinds; the core words and translates the message. */
enum {
    MP_WAIT_WARNING_TOTAL_SHARE = 0, /* value: the waits' share of the print time, % */
    MP_WAIT_WARNING_SINGLE_WAIT = 1, /* value: the longest single wait, s */
    MP_WAIT_WARNING_UNCERTAIN   = 2  /* value: number of sites with a guessed interval next to a wait */
};

typedef struct mp_wait_warning {
    int32_t  kind;
    uint32_t reserved;
    double   value;
    /* Sites: the one whose length causes the waits, and the first and last site of the waits it causes.
     * MP_WAIT_WARNING_UNCERTAIN: the first and last uncertain site, source = first. */
    uint64_t source_site, first_site, last_site;
} mp_wait_warning;

/* The plan. The core allocates every array for n sites and fills it with "no wait" before the call
 * (dwell_s, added_s 0; use_park 0; target_s = interval_s; tower_end = n; warnings_n = 0); the
 * planner overwrites what it decides. */
typedef struct mp_wait_plan {
    double          *dwell_s;      /* G4 time per site, s (the wait less any park trip) */
    uint8_t         *use_park;     /* per site: nonzero parks the head for this wait */
    double          *added_s;      /* time added per site, s (dwell plus park trip) */
    double          *target_s;     /* planned layer time per site, s (interval_s + added_s) */
    mp_wait_warning *warnings;     /* room for warnings_cap warnings */
    uint32_t         warnings_cap;
    uint32_t         warnings_n;   /* written by the planner, at most warnings_cap */
    /* The first site whose tower block is left out (no wait at or after it needs the tower); n keeps
     * every block. */
    uint64_t         tower_end;
} mp_wait_plan;

typedef struct mp_time_plan_v1 {
    uint32_t size; /* sizeof(mp_time_plan_v1) as the mod was built */
    /* The plan for the n sites of one print. print_time_s is the print's estimated time before any
     * wait. ctx gives the planner its parameters (host->param_*) and the slicer settings. */
    mp_status (*plan)(void *user, mp_ctx *ctx, const mp_wait_site *sites, uint64_t n, double print_time_s,
                      mp_wait_plan *out);
} mp_time_plan_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_TIME_PLAN_H */
