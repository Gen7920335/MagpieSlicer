/* magpie_geometry.h -- service "core.geometry" v1: the core's own polygon operations (Orca's
 * ClipperUtils), for mods whose output must match what the same code inside the core produced.
 * C only; include after magpie_mod_api.h. Import it in magpie_mod_init:
 *
 *   const mp_geometry_v1 *geo = (const mp_geometry_v1 *) registrar.import_service(&registrar,
 *       MP_GEOMETRY_SERVICE, MP_GEOMETRY_VERSION, &size, NULL);
 *
 * Polygons here are a flat list of rings (mp_rings): contours counter-clockwise, holes clockwise, as
 * Orca's Polygons. Every result is allocated by the core and released with free_rings or
 * host->free_expolys. Coordinates are integer units (1 = host->scaling_factor_mm mm).
 */
#ifndef MAGPIE_GEOMETRY_H
#define MAGPIE_GEOMETRY_H

#include "magpie_mod_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MP_GEOMETRY_SERVICE "core.geometry"
#define MP_GEOMETRY_VERSION 1

/* A list of closed rings: Orca's Polygons. */
typedef struct mp_rings { const mp_ring *rings; uint32_t n; } mp_rings;

/* boolean: the operations of magpie_mod_api.h (MP_OP_*) and this one, a union with the positive fill
 * rule (overlapping rings of one orientation merge, as union_(polygons, pftPositive)). */
enum { MP_OP_UNION_POSITIVE = 4 };

typedef struct mp_geometry_v1 {
    uint32_t size;
    /* offset(polygons, float(delta), join, miter_limit): delta in integer units, passed through float
     * as the core's offset() takes it. miter_limit <= 0 means the core's default limit. */
    int32_t (*offset)(const mp_rings *in, double delta, int32_t join, double miter_limit, mp_rings *out);
    /* union_(a) when b is null, union_(a, b), intersection(a, b), diff(a, b), or union_(a, pftPositive)
     * (b ignored). MP_OP_XOR is refused: the core has no such operation on Polygons. */
    int32_t (*boolean)(int32_t op, const mp_rings *a, const mp_rings *b, mp_rings *out);
    /* polygons_simplify(in, tolerance): Douglas-Peucker per ring, tolerance in integer units. */
    int32_t (*simplify)(const mp_rings *in, double tolerance, mp_rings *out);
    /* union_ex(in): the islands, each contour with its holes. Release with host->free_expolys. */
    int32_t (*union_ex)(const mp_rings *in, mp_expolys *out);
    /* The signed area of the rings (sum of Polygon::area()), integer units squared. */
    double (*area)(const mp_rings *in);
    /* Squared distance from each of n points to the nearest edge of the rings (each ring closed),
     * DBL_MAX when there is no edge. */
    int32_t (*min_dist2)(const mp_point *points, uint64_t n, const mp_rings *rings, double *out);
    void (*free_rings)(mp_rings *rings);
    /* Appended in API 1.0 later than the entries above; check the table size the import gave.
     * intersection_ex(a, b) or diff_ex(a, b) (MP_OP_INTERSECTION, MP_OP_DIFFERENCE): the islands of a
     * boolean operation on rings, as the core's *_ex functions of Polygons (an ExPolygon operand gives
     * the same result as its rings). Release with host->free_expolys. */
    int32_t (*boolean_ex)(int32_t op, const mp_rings *a, const mp_rings *b, mp_expolys *out);
} mp_geometry_v1;

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MAGPIE_GEOMETRY_H */
