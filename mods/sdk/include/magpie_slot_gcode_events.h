/* magpie_slot_gcode_events.h -- slot "gcode.events" v1: what a mod adds to the G-code export at fixed points,
 * and the export orders it asks for. C only; include after magpie_mod_api.h.
 *
 * The core opens every loaded implementation once per G-code export, before anything is written. An
 * implementation with nothing to do for the print returns no state and is not called again for that
 * export. With a state, the core raises the events below while it exports; the mod answers through
 * the writer table it was given at open, which writes into the G-code at the point of the event, with
 * the core's own G-code writer: temperatures, dwells and fans as commands (the core picks each
 * firmware's form), extrusions through the core's extrusion code, free moves for a nozzle brush or a
 * calibration's marks, and comment lines. A mod never writes other G-code text.
 *
 * Several implementations get each event in load order. Every call gets a ctx whose settings are the
 * export's current ones (the object being printed applied), so host->config_* reads what the core
 * prints with; host->param_* reads the mod's parameters (read them at open: a call per path is hot).
 *
 * Units: mm, mm/s, s, degrees C, percent where named. Coordinates are doubles in mm:
 *   - machine: as written in the G-code (the extruder offset of the active tool included),
 *   - plate:   the plate's frame, without the extruder offset (the core adds it when it prints).
 */
#ifndef MAGPIE_SLOT_GCODE_EVENTS_H
#define MAGPIE_SLOT_GCODE_EVENTS_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_GCODE_EVENTS_SLOT "gcode.events"
#define MP_GCODE_EVENTS_VERSION 1

/* Extrusion roles, stable numbers (the core maps its own to these). */
enum {
    MP_ROLE_OTHER               = 0,
    MP_ROLE_PERIMETER           = 1,
    MP_ROLE_EXTERNAL_PERIMETER  = 2,
    MP_ROLE_OVERHANG_PERIMETER  = 3,
    MP_ROLE_INTERNAL_INFILL     = 4,
    MP_ROLE_SOLID_INFILL        = 5,
    MP_ROLE_TOP_SOLID_INFILL    = 6,
    MP_ROLE_BOTTOM_SURFACE      = 7,
    MP_ROLE_IRONING             = 8,
    MP_ROLE_BRIDGE_INFILL       = 9,
    MP_ROLE_GAP_FILL            = 10,
    MP_ROLE_SKIRT               = 11,
    MP_ROLE_BRIM                = 12,
    MP_ROLE_SUPPORT             = 13,
    MP_ROLE_SUPPORT_INTERFACE   = 14,
    MP_ROLE_SUPPORT_TRANSITION  = 15,
    MP_ROLE_WIPE_TOWER          = 16, /* also every auxiliary tower */
    MP_ROLE_INTERNAL_BRIDGE     = 17
};

/* Events. */
enum {
    /* After the layer change, where calibration modes set the layer's values. */
    MP_GCODE_EV_LAYER_BEGIN     = 1,
    /* Before an extrusion path prints (only with MP_GCODE_WANTS_PATHS); role, in_support and
     * tagged_temperature describe it. */
    MP_GCODE_EV_PATH            = 2,
    /* A visit of the layer's band execution (half-height bands) is about to print with `filament`; it
     * may change tools first. */
    MP_GCODE_EV_TOOL_AHEAD      = 3,
    /* An object's support, printed after its other extrusions on this layer (support_last), ended. */
    MP_GCODE_EV_SUPPORT_END     = 4,
    /* Every visit of the layer's band execution printed. */
    MP_GCODE_EV_OBJECTS_END     = 5,
    /* The first layer's objects printed; the core then raises LAYER_END. */
    MP_GCODE_EV_FIRST_LAYER_END = 6,
    /* The layer's extrusions ended. */
    MP_GCODE_EV_LAYER_END       = 7
};

typedef struct mp_gcode_event_v1 {
    uint32_t size;
    int32_t  kind;               /* MP_GCODE_EV_* */
    /* PATH: */
    int32_t  role;               /* MP_ROLE_* */
    int32_t  in_support;         /* 1 while printing an object's support (support ironing included) */
    /* The nozzle temperature the support style gives this path's interface layer (degC; 0: none), see
     * owns_temperature. */
    int32_t  tagged_temperature;
    /* TOOL_AHEAD: the filament the visit prints with, 0-based. */
    int32_t  filament;
    /* PATH, written by the mod: 1 when it sets the nozzle temperature of tagged paths itself. Otherwise
     * the core prints a tagged path at its temperature (no wait) and returns to the filament's
     * temperature once the support ends. */
    int32_t  owns_temperature;
} mp_gcode_event_v1;

/* What the export of the print is. */
typedef struct mp_gcode_export_v1 {
    uint32_t    size;
    /* The calibration the print is ("lesic", "temp_tower", ...; "" for a normal print), and its
     * values by name through writer->calibration_value. */
    const char *calibration;
} mp_gcode_export_v1;

/* A flow: the extrusion cross-section the core computes, as it keeps it (single precision). */
typedef struct mp_flow_v1 {
    uint32_t size;
    float    width;      /* mm */
    float    height;     /* mm */
    float    spacing;    /* mm, between neighbouring lines */
    float    nozzle;     /* mm */
    double   mm3_per_mm; /* mm3 per mm of path */
} mp_flow_v1;

/* Flow roles for writer->flow. */
enum { MP_FLOW_PERIMETER = 0, MP_FLOW_EXTERNAL_PERIMETER = 1, MP_FLOW_SUPPORT = 2 };

/* An extrusion the core prints with its extrusion code (speed, fan, cooling and every other rule of
 * the role apply). */
typedef struct mp_gcode_path_v1 {
    uint32_t       size;
    const double  *xy;          /* n points, x0 y0 x1 y1 ..., plate frame, mm */
    uint32_t       n;
    int32_t        role;        /* MP_ROLE_* */
    double         mm3_per_mm;
    double         width;       /* mm */
    double         height;      /* mm */
    double         speed;       /* mm/s */
    const char    *description; /* the G-code comment of its moves */
    /* Optional slower end: with tail_length > 0 the last tail_length mm print at tail_speed (mm/s). The
     * core writes the text head_comment before the rest and tail_comment before the tail; a path too
     * short to split prints whole at tail_speed after tail_comment. Comments as writer->text takes them. */
    double         tail_length;
    double         tail_speed;
    const char    *head_comment;
    const char    *tail_comment;
} mp_gcode_path_v1;

/* writer->place_tower: an auxiliary tower on the plate being exported. */
typedef struct mp_tower_request_v1 {
    uint32_t       size;
    int32_t        tower;          /* which stored position: 0 (the only one so far) */
    int32_t        filament;       /* the filament printing it; its nozzle's area for an automatic position */
    const int32_t *nozzles;        /* the physical nozzles that print it, n_nozzles of them */
    uint32_t       n_nozzles;
    double         requested_size; /* mm, the tower's side */
    double         brim_width;     /* mm, around it */
} mp_tower_request_v1;

enum {
    MP_TOWER_PLACED          = 0,
    MP_TOWER_DOES_NOT_FIT    = 1, /* no room on the plate: no tower */
    MP_TOWER_NO_COMMON_AREA  = 2, /* several nozzles share no printable area */
    MP_TOWER_OUTSIDE_COMMON  = 3  /* several nozzles: it does not fit inside the area they share */
};

typedef struct mp_tower_place_v1 {
    uint32_t size;
    int32_t  result;         /* MP_TOWER_* */
    double   origin_x;       /* mm, plate frame: lower left corner of the footprint (brim included) */
    double   origin_y;
    double   tower_size;     /* mm, the side it got (at most the requested one) */
    double   footprint_size; /* mm, the tower and its brim on both sides */
} mp_tower_place_v1;

/* Flags for writer->set_temperature. */
enum {
    /* With wait: a firmware whose wait only heats (Marlin M109 S) waits for cooling too (M109 R). */
    MP_TEMP_WAIT_COOLING = 1
};
/* Flags for writer->raw_move. */
enum { MP_MOVE_NO_EXTRUSION = 1 };
/* Frames for writer->travel. */
enum { MP_FRAME_MACHINE = 0, MP_FRAME_PLATE = 1 };

/* The core's export, valid from open until close. Output functions write at the point of the event
 * being handled and may only be called during an event. A function the core could not complete (an
 * extrusion its line width check refuses) writes nothing, nor does any later one in the same call; the
 * core stops the slice with its own error once the mod returns. */
typedef struct mp_gcode_writer_v1 {
    uint32_t size;
    void    *w;

    /* -- State -- */
    int32_t  (*filament)(void *w);                   /* the active filament, 0-based; -1 none yet */
    int32_t  (*nozzle_of)(void *w, int32_t filament);/* the physical nozzle printing a filament */
    /* The filaments this export prints with, in the writer's order; returns how many (fills up to cap). */
    uint32_t (*filaments)(void *w, int32_t *out, uint32_t cap);
    int32_t  (*layer_index)(void *w);
    double   (*nominal_z)(void *w);                  /* mm, the Z being printed, z offset included */
    double   (*layer_height)(void *w);               /* mm, the current layer's height; 0 between layers */
    double   (*last_height)(void *w);                /* mm, height of the last extrusion */
    int32_t  (*first_layer)(void *w);                /* 1 while printing the first layer */
    int32_t  (*over_raft)(void *w);                  /* 1 while printing an object layer right over a raft */
    int32_t  (*position_known)(void *w);             /* 1 once the nozzle position is known */
    /* The printable area's corners (machine frame, mm), x0 y0 x1 y1 ...; returns how many corners (fills up
     * to cap of them). */
    uint32_t (*printable_area)(void *w, double *xy, uint32_t cap);
    /* The highest support layer with interface paths over every object (mm, object frame), or a very
     * low number without one; whether any object has support layers. */
    double   (*top_support_interface_z)(void *w);
    int32_t  (*has_support_layers)(void *w);
    /* The nozzle temperature the support style gives restyled interface layers with the current
     * settings (degC), 0 for none. */
    int32_t  (*style_interface_temperature)(void *w);
    /* A value of the print's calibration (mp_gcode_export_v1.calibration) by name ("start", "end",
     * "step", ...); MP_ERR_NOT_FOUND for a name it does not have. */
    int32_t  (*calibration_value)(void *w, const char *name, double *out);
    /* The mod affected this export: it is named in the G-code's "; mods:" line when that is still to
     * be written (an open flag says it from the start). */
    void     (*mark_used)(void *w);

    /* -- Output -- */
    /* Text whose every line is empty or a comment (starting with ';'); "\n" ends a line. */
    int32_t  (*text)(void *w, const char *text);
    /* A nozzle temperature, degC, for `filament` (-1: the active one); wait: 0 or 1; MP_TEMP_*. */
    void     (*set_temperature)(void *w, int32_t degc, int32_t wait, int32_t filament, int32_t flags);
    /* A dwell of `seconds` in the firmware's form, followed by " ; comment" when comment is given. */
    void     (*dwell)(void *w, double seconds, const char *comment);
    /* The auxiliary part cooling fan, percent. */
    void     (*aux_fan)(void *w, int32_t percent);
    /* A travel with the core's travel rules (retraction, lift, avoid crossing walls). */
    void     (*travel)(void *w, double x, double y, int32_t frame, int32_t role, const char *comment);
    /* Free moves, written as they are, machine frame: a travel, and a move extruding e mm of filament
     * (MP_MOVE_NO_EXTRUSION: none). The nozzle's position is known afterwards. */
    void     (*raw_travel)(void *w, double x, double y, const char *comment);
    void     (*raw_move)(void *w, double x, double y, double e, int32_t flags, const char *comment);
    /* The feed rate of the following free moves, mm/min. */
    void     (*set_feedrate)(void *w, double mm_per_min, const char *comment);
    /* The export's frame becomes the plate's (as when no object is printed) until the core sets an
     * object's again. */
    void     (*plate_frame)(void *w);
    /* An extrusion; MP_ERR_ARGUMENT for fewer than two points. */
    int32_t  (*extrude)(void *w, const mp_gcode_path_v1 *path);
    /* The nozzle arrives at an auxiliary tower whose first line starts at (x, y) (plate frame): a
     * planner waiting for a tower visit (layer time blending) gets it here, after a travel there. */
    void     (*tower_arrival)(void *w, double x, double y, int32_t role, const char *comment);

    /* -- Services -- */
    /* The flow the core prints a role with: width_mm <= 0 is the role's automatic width for the nozzle. */
    void     (*flow)(void *w, int32_t flow_role, double nozzle_mm, double height_mm, double width_mm, mp_flow_v1 *out);
    /* The same flow with a new spacing (mm); MP_ERR_ARGUMENT when that would make it narrower than tall. */
    int32_t  (*flow_with_spacing)(void *w, const mp_flow_v1 *in, double spacing_mm, mp_flow_v1 *out);
    /* Where an auxiliary tower goes on the plate (design E17: the core owns the stored position and the
     * placement; the mod owns its shape). */
    void     (*place_tower)(void *w, const mp_tower_request_v1 *request, mp_tower_place_v1 *out);
} mp_gcode_writer_v1;

/* path_speeds: one path, before it prints. */
typedef struct mp_gcode_speed_query_v1 {
    uint32_t      size;
    int32_t       role;        /* MP_ROLE_* */
    double        mm3_per_mm;
    double        speed;       /* mm/s the core would print it at */
    const double *xy;          /* n points, machine frame, mm */
    uint32_t      n;
} mp_gcode_speed_query_v1;

/* Start G-code phases. */
enum {
    MP_START_TEMPLATE  = 0, /* the configured start G-code, before placeholders are filled in */
    MP_START_PROCESSED = 1  /* after */
};

typedef struct mp_gcode_text_v1 {
    uint32_t size;
    /* The new text (host->alloc, freed by the core), or null to keep it. */
    char    *text;
    /* A comment line the core writes just before the start G-code (host->alloc), or null. */
    char    *note;
} mp_gcode_text_v1;

/* validate: one object about to be sliced. */
typedef struct mp_gcode_validate_v1 {
    uint32_t       size;
    int32_t        has_support;         /* the object has support (or a raft) */
    const int32_t *support_filaments;   /* 0-based filaments its support prints with */
    uint32_t       n_support_filaments;
} mp_gcode_validate_v1;

typedef struct mp_gcode_problem_v1 {
    uint32_t size;
    /* An error stopping the slice, or null: English with placeholders %1%..%4% for args (the core translates
     * the text, then fills them in); the setting it is about, or null. Both static or owned by the mod until
     * its next call. */
    const char *message;
    const char *setting;
    int64_t     args[4];
    uint32_t    n_args;
} mp_gcode_problem_v1;

/* tower_preview: an auxiliary tower the 3D scene shows before slicing (ctx: the plate's settings). */
typedef struct mp_tower_preview_v1 {
    uint32_t size;
    int32_t  show;           /* 1: there is a tower */
    double   requested_size; /* mm, its side */
    double   band_width;     /* mm, the width of its arms */
    double   brim_width;     /* mm */
} mp_tower_preview_v1;

/* Flags open returns. */
enum {
    MP_GCODE_WANTS_PATHS         = 1,  /* PATH events */
    MP_GCODE_WANTS_PATH_SPEEDS   = 2,  /* path_speeds calls */
    MP_GCODE_WANTS_START_GCODE   = 4,  /* start_gcode calls */
    /* It changes nozzle temperatures within layers (time estimates count the waits). */
    MP_GCODE_CHANGES_TEMPERATURE = 8,
    /* It affects this export: named in the "; mods:" line. */
    MP_GCODE_USED                = 16
};

typedef struct mp_gcode_events_v1 {
    uint32_t size;
    /* The mod's state for one export. *state = null: nothing to do. *flags: MP_GCODE_*. */
    mp_status (*open)(void *user, mp_ctx *ctx, const mp_gcode_export_v1 *info, const mp_gcode_writer_v1 *writer,
                      void **state, uint32_t *flags);
    mp_status (*event)(void *user, void *state, mp_ctx *ctx, mp_gcode_event_v1 *ev);
    /* 1: the object of ctx prints its support after its other extrusions on each layer, mixed support by
     * role (body, transition, interface). */
    mp_status (*support_last)(void *user, void *state, mp_ctx *ctx, int32_t *out);
    /* 1: an auxiliary tower of this mod prints on the layer at print_z (mm, object frame). */
    mp_status (*tower_on_layer)(void *user, void *state, mp_ctx *ctx, double print_z, int32_t *out);
    /* Speeds per point (mm/s, n of them) for the path; *changed = 0 leaves the path's speed. */
    mp_status (*path_speeds)(void *user, void *state, mp_ctx *ctx, const mp_gcode_speed_query_v1 *query, double *speeds,
                             int32_t *changed);
    /* The start G-code at `phase`; out->text replaces it. A slicing warning goes through host->warn. */
    mp_status (*start_gcode)(void *user, void *state, mp_ctx *ctx, int32_t phase, const char *in, mp_gcode_text_v1 *out);
    void      (*close)(void *user, void *state);

    /* Without an export. Print validation of one object (ctx: its settings); out->message set: an error. */
    mp_status (*validate)(void *user, mp_ctx *ctx, const mp_gcode_validate_v1 *object, mp_gcode_problem_v1 *out);
    /* The tower the 3D scene previews for these settings, before slicing. */
    mp_status (*tower_preview)(void *user, mp_ctx *ctx, int32_t style_interface_temperature, mp_tower_preview_v1 *out);
    /* 1: an auxiliary tower prints with the support filament at full layer height for these settings,
     * whatever else decides it (the line width checks cover it). */
    mp_status (*tower_requested)(void *user, mp_ctx *ctx, int32_t *out);
    /* 1: the layer with this layer_index (0 = the first) prints its objects in the reverse of the object list
     * (print_order as_obj_list only; other orders ignore it). A table without this member reverses nothing. */
    mp_status (*reverse_object_order)(void *user, void *state, mp_ctx *ctx, int32_t layer_index, int32_t *out);
    /* Without an export. 1: the object of ctx (its settings) prints its support last (support_last), told before
     * the export when the core plans a layer's order ahead (a half-layer frame). A table without this member or a
     * null one plans nothing. */
    mp_status (*support_last_planned)(void *user, mp_ctx *ctx, int32_t *out);
} mp_gcode_events_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_GCODE_EVENTS_H */
