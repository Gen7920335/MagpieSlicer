/* magpie_mod_api.h -- the only header the slicer core and every native mod share. C only.
 *
 * A native mod is a shared library (bin/<platform>/<binary>.dll|.so|.dylib in its mod folder) that
 * exports exactly three functions, magpie_mod_query, magpie_mod_init and magpie_mod_shutdown. The
 * core and a mod exchange numbers, polygons, strings and opaque handles only, never C++ objects, so
 * a mod built against API major 1 keeps loading in every core of major 1 without a rebuild.
 *
 * Units, as every declaration below repeats:
 *   - coordinates are the core's integer coordinates; one unit is host->scaling_factor_mm millimetres,
 *     read at each call (1e-6 mm usually, 1e-5 mm once a very large printer is chosen), never a constant;
 *   - other lengths and heights are mm, times s, temperatures degrees C, speeds mm/s.
 *
 * Rules (docs/HLSD/native-mods.md):
 *   - No exception crosses this boundary. Every entry point returns a status; magpie_mod.hpp wraps
 *     C++ code for that.
 *   - Memory is freed by the side that allocated it: host results with the host's free functions,
 *     mod results with host->alloc (then the host frees them) or the slot table's own release.
 *   - The core calls a mod from several threads only when its manifest says "thread_safe": true;
 *     otherwise it serialises the calls into that mod. Parallel work goes through host->parallel_for.
 *   - Same input, same output: a mod is deterministic.
 *   - Versioning: every table starts with its size in bytes. A newer side ignores what lies past the
 *     size the older side gave; MAGPIE_MOD_API_MINOR grows when entries are appended, MAJOR changes
 *     only on a breaking change and a core refuses a mod of another major.
 */
#ifndef MAGPIE_MOD_API_H
#define MAGPIE_MOD_API_H

#include <stddef.h>
#include <stdint.h>

#define MAGPIE_MOD_API_MAJOR 1
#define MAGPIE_MOD_API_MINOR 1

#if defined(_WIN32)
#define MAGPIE_EXPORT __declspec(dllexport)
#else
#define MAGPIE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* Status codes. 0 is success; a slot function returns one of these in mp_status.code. */
enum {
    MP_OK            = 0,
    MP_ERR_ARGUMENT  = 1, /* a null pointer, a size or an index out of range */
    MP_ERR_NOT_FOUND = 2, /* no such parameter, setting, slot or service */
    MP_ERR_TYPE      = 3, /* the value exists with another type */
    MP_ERR_BUFFER    = 4, /* a string did not fit; the buffer holds it cut short, NUL-terminated */
    MP_ERR_FAILED    = 5, /* the operation failed; the message says why */
    MP_ERR_CANCELLED = 6  /* slicing was cancelled */
};

/* A point in integer coordinates: 1 = host->scaling_factor_mm mm. Same 16-byte layout as the
 * core's Point, so the core can hand its own polygons over without copying. */
typedef struct mp_point { int64_t x, y; } mp_point;
/* A closed polygon, its last point not repeating the first. */
typedef struct mp_ring { const mp_point *pts; uint32_t n; } mp_ring;
/* rings[0] is the contour (counter-clockwise), the others holes (clockwise). */
typedef struct mp_expoly { const mp_ring *rings; uint32_t n; } mp_expoly;
typedef struct mp_expolys { const mp_expoly *items; uint32_t n; } mp_expolys;

/* What a slot function reports. message is static or owned by the mod until its next call. */
typedef struct mp_status { int32_t code; const char *message; } mp_status;

/* Opaque: the context of one call (the print, the object and region it is about, the cancel flag). */
typedef struct mp_ctx mp_ctx;
/* Opaque number naming a core object. Not a pointer. */
typedef uint64_t mp_handle;

/* Log levels for host->log and host->warn. */
enum { MP_LOG_DEBUG = 0, MP_LOG_INFO = 1, MP_LOG_WARNING = 2, MP_LOG_ERROR = 3 };
/* host->offset joins. */
enum { MP_JOIN_MITER = 0, MP_JOIN_ROUND = 1, MP_JOIN_SQUARE = 2 };
/* host->boolean operations. */
enum { MP_OP_UNION = 0, MP_OP_INTERSECTION = 1, MP_OP_DIFFERENCE = 2, MP_OP_XOR = 3 };

typedef struct MagpieHostApi {
    uint32_t size;  /* sizeof(MagpieHostApi) as the core was built */
    uint32_t major; /* the core's MAGPIE_MOD_API_MAJOR */
    uint32_t minor; /* the core's MAGPIE_MOD_API_MINOR */
    /* Length of one integer coordinate unit, mm. The application changes it when a printer with a
     * very large bed is chosen, never while a slice runs; read it in each call, do not cache it. */
    double scaling_factor_mm;
    /* The application version, such as "2.5.0.0". */
    const char *app_version;

    /* This mod's parameters, the value that applies to the context: a region-scope value of the
     * context's region, else its object's, else the process preset's, else the default. key is the
     * parameter name, "<name>[i]" for filament i (0-based) of a filament-scope parameter.
     * param_f64 reads float, int and bool (as 0/1); param_i64 int and bool; param_str string and
     * float_or_percent ("50%" or "12.5"). */
    int (*param_f64)(mp_ctx *ctx, const char *key, double *out);
    int (*param_i64)(mp_ctx *ctx, const char *key, int64_t *out);
    int (*param_str)(mp_ctx *ctx, const char *key, char *buf, uint32_t cap);

    /* Read a slicer setting by its key. index picks an element of a list setting (per filament or per
     * extruder), -1 for a single value. The value is the context's region or object setting when the
     * context has one. A percentage reads as its number from config_f64 and as "50%" from config_str; a text
     * setting reads as it is (line breaks and quotes included), any other in its file form. */
    int (*config_f64)(mp_ctx *ctx, const char *key, int32_t index, double *out);
    int (*config_str)(mp_ctx *ctx, const char *key, int32_t index, char *buf, uint32_t cap);

    /* Geometry with the core's Clipper. delta is in integer units (positive grows). Results are
     * allocated by the host and released with free_expolys. */
    int (*offset)(const mp_expolys *in, double delta_scaled, int32_t join, mp_expolys *out);
    int (*boolean)(int32_t op, const mp_expolys *a, const mp_expolys *b, mp_expolys *out);
    void (*free_expolys)(mp_expolys *polys);

    /* Memory the host frees, or the mod frees with host->free. */
    void *(*alloc)(size_t bytes);
    void (*free)(void *ptr);

    /* A line in the application log. */
    void (*log)(int32_t level, const char *mod_id, const char *text);
    /* A slicing warning shown to the user (MP_LOG_WARNING) or a slicing error (MP_LOG_ERROR, which
     * stops the slice once the call returns). */
    void (*warn)(mp_ctx *ctx, int32_t level, const char *text);
    /* Nonzero once the user cancelled; long loops check it and return MP_ERR_CANCELLED. */
    int (*cancelled)(mp_ctx *ctx);
    /* Calls fn(user, i) for i in [0, n) on the core's thread pool and returns when all are done. */
    void (*parallel_for)(mp_ctx *ctx, uint32_t n, void (*fn)(void *user, uint32_t i), void *user);
} MagpieHostApi;

typedef struct MagpieRegistrar {
    uint32_t size; /* sizeof(MagpieRegistrar) as the core was built */
    /* Offers an implementation of a core slot: slot "layer.bands", slot_version its interface version,
     * impl_name the value of the setting that picks it, table the slot's function table (starting
     * with its uint32_t size), user passed back to every call. The table stays valid until shutdown. */
    int (*register_slot)(struct MagpieRegistrar *self, const char *slot, uint32_t slot_version,
                         const char *impl_name, const void *table, uint32_t table_size, void *user);
    /* Offers a service to other mods ("cura.support_areas"), and asks for one another mod offers. A
     * mod that imports a service lists its owner under "depends" so that owner initialises first.
     * import_service returns the table, or null when no loaded mod offers it at min_version or newer.
     * Services named "core.*" are the core's own (magpie_geometry.h) and need no "depends". */
    int (*export_service)(struct MagpieRegistrar *self, const char *name, uint32_t version,
                          const void *table, uint32_t table_size, void *user);
    const void *(*import_service)(struct MagpieRegistrar *self, const char *name, uint32_t min_version,
                                  uint32_t *table_size, void **user);
    void *host_data; /* the core's own */
} MagpieRegistrar;

/* Every service and slot table starts with this. */
typedef struct mp_table_header { uint32_t size; } mp_table_header;

/* The three functions a mod library exports.
 *
 * magpie_mod_query: no side effects, callable before init. A static string of space-separated
 *   tokens: "<id> <version> <api major> <api minor> fp=<mode>". fp is the floating-point mode the
 *   library was compiled with ("precise"; anything else is refused, since it changes G-code).
 * magpie_mod_init: registers slots and services. Returns MP_OK, or another code after host->log
 *   said why; the mod is then left out.
 * magpie_mod_shutdown: the last call. Every table registered becomes invalid. */
typedef const char *(*magpie_mod_query_fn)(void);
typedef int32_t (*magpie_mod_init_fn)(const MagpieHostApi *host, MagpieRegistrar *registrar);
typedef void (*magpie_mod_shutdown_fn)(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

/* The floating-point mode token for magpie_mod_query, from how this file is compiled. The SDK's
 * CMake template (MagpieMod.cmake) defines MAGPIE_MOD_FP_CONTRACT_OFF after turning contraction off
 * on compilers whose macros cannot show it. */
#if defined(__FAST_MATH__) || defined(_M_FP_FAST)
#define MAGPIE_MOD_FP_MODE "fast"
#elif defined(_M_FP_CONTRACT)
#define MAGPIE_MOD_FP_MODE "contract"
#elif defined(_M_FP_STRICT)
#define MAGPIE_MOD_FP_MODE "strict"
#elif defined(__clang__) || defined(__GNUC__)
/* GCC and Clang contract a*b+c into an FMA by default and no macro tells; only the template's
 * -ffp-contract=off, which defines MAGPIE_MOD_FP_CONTRACT_OFF, makes this precise. */
#if defined(MAGPIE_MOD_FP_CONTRACT_OFF)
#define MAGPIE_MOD_FP_MODE "precise"
#else
#define MAGPIE_MOD_FP_MODE "unknown"
#endif
#elif defined(_M_FP_PRECISE)
#define MAGPIE_MOD_FP_MODE "precise"
#else
#define MAGPIE_MOD_FP_MODE "unknown"
#endif

#define MAGPIE_MOD_STR2(x) #x
#define MAGPIE_MOD_STR(x) MAGPIE_MOD_STR2(x)
/* The query string for a mod: MAGPIE_MOD_QUERY_STRING("my_mod", "1.0.0"). */
#define MAGPIE_MOD_QUERY_STRING(id, version)                                                              \
    id " " version " " MAGPIE_MOD_STR(MAGPIE_MOD_API_MAJOR) " " MAGPIE_MOD_STR(MAGPIE_MOD_API_MINOR)  \
       " fp=" MAGPIE_MOD_FP_MODE

#endif /* MAGPIE_MOD_API_H */
