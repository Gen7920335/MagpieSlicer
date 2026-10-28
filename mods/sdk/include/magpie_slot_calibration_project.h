/* magpie_slot_calibration_project.h -- slot "calibration.project" v1: the project a calibration of the Calibration
 * menu starts. C only; include after magpie_mod_api.h.
 *
 * A mod declares its calibrations in its manifest ("gui"."calibrations"), each with the form the application shows
 * for it ("form"). The application draws the form; a mod never runs GUI code. While the user edits the form the core
 * calls check(); on OK it opens a new project and calls build(), which describes the project through `project`:
 * objects made of boxes and cylinders in plate coordinates (mm, the bed's frame, z = 0 on the bed), settings of the
 * print, filament and printer presets and of each object, and the calibration's values the mod's G-code side reads
 * back (gcode.events: mp_gcode_export_v1.calibration is "<mod id>.<calibration id>", values through
 * writer->calibration_value). Form fields bound to a setting ("setting" fields) are already written into the
 * presets when build() runs; their values are among `fields` too.
 *
 * `fields` holds every field of the form, hidden ones included, as the text the form shows (numbers in C locale).
 * The ctx reads the presets being edited (host->config_*), parameters of the mod with host->param_*. Both calls
 * must depend on nothing else.
 */
#ifndef MAGPIE_SLOT_CALIBRATION_PROJECT_H
#define MAGPIE_SLOT_CALIBRATION_PROJECT_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_CALIBRATION_PROJECT_SLOT "calibration.project"
#define MP_CALIBRATION_PROJECT_VERSION 1

/* One form field's value. */
typedef struct mp_calib_field_v1 {
    const char *id;
    const char *value;
} mp_calib_field_v1;

/* project->report kinds. */
enum {
    /* The form cannot start the calibration; the OK button stays off and the text says why. */
    MP_CALIB_REPORT_ERROR   = 0,
    /* Shown under the form; the calibration can start. */
    MP_CALIB_REPORT_WARNING = 1,
    /* One line under the form about what the calibration prints. */
    MP_CALIB_REPORT_SUMMARY = 2
};

/* Volume types for project->add_box and add_cylinder. */
enum {
    MP_CALIB_PART              = 0, /* a model part */
    MP_CALIB_SUPPORT_ENFORCER  = 1, /* support is generated inside it */
    MP_CALIB_SUPPORT_BLOCKER   = 2
};

/* Presets for project->set_option. */
enum { MP_CALIB_PRINT = 0, MP_CALIB_FILAMENT = 1, MP_CALIB_PRINTER = 2 };

/* What check() and build() describe the calibration with. Texts are English; the mod's message catalog
 * (i18n/<language>.json) translates them. Functions returning int32_t give MP_OK, or MP_ERR_ARGUMENT /
 * MP_ERR_NOT_FOUND for an unknown object, key or value (nothing is changed then). */
typedef struct mp_calib_project_v1 {
    uint32_t size;
    void    *p;
    /* A text for the user with %1%, %2%, ... standing for `args` (numbers, written shortest), which the core puts
     * in after translating the text. */
    void     (*report)(void *p, int32_t kind, const char *text, const double *args, uint32_t n_args);
    /* build() only from here on. A new object; its index, or -1. Objects print in the order they are added when
     * the print sets print_order to as_obj_list. */
    int32_t  (*add_object)(void *p, const char *name);
    /* An axis-aligned box from (min_x, min_y, min_z) by its size, mm, plate coordinates. */
    int32_t  (*add_box)(void *p, int32_t object, int32_t type, double min_x, double min_y, double min_z, double size_x, double size_y,
                        double size_z);
    /* A vertical cylinder standing on min_z, its axis at (center_x, center_y), mm; facet_angle in radians between
     * the corners of its outline (the core's its_make_cylinder). */
    int32_t  (*add_cylinder)(void *p, int32_t object, int32_t type, double center_x, double center_y, double min_z, double radius,
                             double height, double facet_angle);
    /* A setting of the object: an Orca option key (serialized as the preset files write it) or "<mod id>.<name>"
     * for a parameter of object or region scope. */
    int32_t  (*set_object_option)(void *p, int32_t object, const char *key, const char *value);
    /* A setting of the print, filament or printer preset being edited (MP_CALIB_*), as set_object_option. A list
     * setting takes the serialized list, or one value for every element. */
    int32_t  (*set_option)(void *p, int32_t preset, const char *key, const char *value);
    /* A value of the calibration, read back by name at G-code export. */
    int32_t  (*set_value)(void *p, const char *name, double value);
    /* The object's layers from min_z to max_z (mm above the bed) are layer_height thick: a height range of the
     * object (Orca's height range modifier). Ranges of one object must not overlap. Layers begin at min_z only when
     * a layer ends there, so a mod that places layers gives every layer of a stretch its own range. */
    int32_t  (*add_layer_range)(void *p, int32_t object, double min_z, double max_z, double layer_height);
} mp_calib_project_v1;

typedef struct mp_calibration_project_v1 {
    uint32_t  size;
    /* The form's values are checked: report errors, warnings and the summary. `calibration` is the calibration's
     * id in the manifest. */
    mp_status (*check)(void *user, mp_ctx *ctx, const char *calibration, const mp_calib_field_v1 *fields, uint32_t n_fields,
                       const mp_calib_project_v1 *project);
    /* The project of the calibration, for values check() passed. */
    mp_status (*build)(void *user, mp_ctx *ctx, const char *calibration, const mp_calib_field_v1 *fields, uint32_t n_fields,
                       const mp_calib_project_v1 *project);
} mp_calibration_project_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_SLOT_CALIBRATION_PROJECT_H */
