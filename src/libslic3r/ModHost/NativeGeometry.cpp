// MAGPIE: the service "core.geometry" v1 (mods/sdk/include/magpie_geometry.h): the core's own polygon
// operations for native mods, so a feature moved out of the core into a mod computes the same polygons
// it computed inside the core. Each entry calls exactly the ClipperUtils overload its comment names.

#include "NativeMods.hpp"

#include "libslic3r/AABBTreeLines.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Polygon.hpp"

#include "../../../mods/sdk/include/magpie_geometry.h"

#include <cstdlib>
#include <exception>
#include <limits>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {
namespace ModHost {

namespace {

Polygons to_polygons(const mp_rings *in)
{
    Polygons out;
    if (in == nullptr)
        return out;
    out.reserve(in->n);
    for (uint32_t r = 0; r < in->n; ++r) {
        Polygon poly;
        poly.points.reserve(in->rings[r].n);
        for (uint32_t k = 0; k < in->rings[r].n; ++k)
            poly.points.emplace_back(in->rings[r].pts[k].x, in->rings[r].pts[k].y);
        out.push_back(std::move(poly));
    }
    return out;
}

// Host-allocated copy, released by geo_free_rings.
void from_polygons(const Polygons &polys, mp_rings *out)
{
    auto *rings = static_cast<mp_ring *>(std::calloc(std::max<size_t>(polys.size(), 1), sizeof(mp_ring)));
    for (size_t r = 0; r < polys.size(); ++r) {
        const Points &pts = polys[r].points;
        auto         *dst = static_cast<mp_point *>(std::malloc(std::max<size_t>(pts.size(), 1) * sizeof(mp_point)));
        for (size_t k = 0; k < pts.size(); ++k)
            dst[k] = {pts[k].x(), pts[k].y()};
        rings[r] = {dst, uint32_t(pts.size())};
    }
    *out = {rings, uint32_t(polys.size())};
}

void geo_free_rings(mp_rings *rings)
{
    if (rings == nullptr || rings->rings == nullptr)
        return;
    for (uint32_t r = 0; r < rings->n; ++r)
        std::free(const_cast<mp_point *>(rings->rings[r].pts));
    std::free(const_cast<mp_ring *>(rings->rings));
    *rings = {nullptr, 0};
}

template<class Fn> int32_t guarded_geometry(Fn &&fn)
{
    try {
        fn();
        return MP_OK;
    } catch (const std::bad_alloc &) {
        return MP_ERR_FAILED;
    } catch (const std::exception &) {
        return MP_ERR_FAILED;
    }
}

int32_t geo_offset(const mp_rings *in, double delta, int32_t join, double miter_limit, mp_rings *out)
{
    if (in == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    const JoinType type = join == MP_JOIN_ROUND ? jtRound : join == MP_JOIN_SQUARE ? jtSquare : jtMiter;
    return guarded_geometry([&] {
        // offset(const Polygons &, const float delta, JoinType, double miterLimit)
        from_polygons(offset(to_polygons(in), float(delta), type, miter_limit > 0. ? miter_limit : DefaultMiterLimit), out);
    });
}

int32_t geo_boolean(int32_t op, const mp_rings *a, const mp_rings *b, mp_rings *out)
{
    if (a == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    if (b == nullptr && op != MP_OP_UNION && op != MP_OP_UNION_POSITIVE)
        return MP_ERR_ARGUMENT;
    // Orca has no xor_ of Polygons, so the service offers none either.
    if (op < MP_OP_UNION || op > MP_OP_UNION_POSITIVE || op == MP_OP_XOR)
        return MP_ERR_ARGUMENT;
    return guarded_geometry([&] {
        const Polygons pa = to_polygons(a);
        switch (op) {
        case MP_OP_UNION: from_polygons(b == nullptr ? union_(pa) : union_(pa, to_polygons(b)), out); break;
        case MP_OP_INTERSECTION: from_polygons(intersection(pa, to_polygons(b)), out); break;
        case MP_OP_DIFFERENCE: from_polygons(diff(pa, to_polygons(b)), out); break;
        case MP_OP_UNION_POSITIVE: from_polygons(union_(pa, pftPositive), out); break;
        }
    });
}

int32_t geo_simplify(const mp_rings *in, double tolerance, mp_rings *out)
{
    if (in == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded_geometry([&] { from_polygons(polygons_simplify(to_polygons(in), tolerance), out); });
}

// The host's ExPolygons copy, released with host->free_expolys.
void from_expolygons(const ExPolygons &islands, mp_expolys *out)
{
    auto *items = static_cast<mp_expoly *>(std::calloc(std::max<size_t>(islands.size(), 1), sizeof(mp_expoly)));
    for (size_t i = 0; i < islands.size(); ++i) {
        mp_rings rings{};
        Polygons one;
        one.reserve(1 + islands[i].holes.size());
        one.push_back(islands[i].contour);
        one.insert(one.end(), islands[i].holes.begin(), islands[i].holes.end());
        from_polygons(one, &rings);
        items[i] = {rings.rings, rings.n};
    }
    *out = {items, uint32_t(islands.size())};
}

int32_t geo_union_ex(const mp_rings *in, mp_expolys *out)
{
    if (in == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    return guarded_geometry([&] { from_expolygons(union_ex(to_polygons(in)), out); });
}

int32_t geo_boolean_ex(int32_t op, const mp_rings *a, const mp_rings *b, mp_expolys *out)
{
    if (a == nullptr || b == nullptr || out == nullptr)
        return MP_ERR_ARGUMENT;
    // Orca has no union_ex of two Polygons operands; union_ex of one list is the entry above.
    if (op != MP_OP_INTERSECTION && op != MP_OP_DIFFERENCE)
        return MP_ERR_ARGUMENT;
    return guarded_geometry([&] {
        const Polygons pa = to_polygons(a), pb = to_polygons(b);
        switch (op) {
        // intersection_ex(const Polygons &, const Polygons &, ApplySafetyOffset::No)
        case MP_OP_INTERSECTION: from_expolygons(intersection_ex(pa, pb), out); break;
        // diff_ex(const Polygons &, const Polygons &, ApplySafetyOffset::No)
        case MP_OP_DIFFERENCE: from_expolygons(diff_ex(pa, pb), out); break;
        }
    });
}

double geo_area(const mp_rings *in)
{
    if (in == nullptr)
        return 0.;
    // area(const Polygons &): the sum of Polygon::area() in ring order.
    double s = 0.;
    for (uint32_t r = 0; r < in->n; ++r) {
        Points pts;
        pts.reserve(in->rings[r].n);
        for (uint32_t k = 0; k < in->rings[r].n; ++k)
            pts.emplace_back(in->rings[r].pts[k].x, in->rings[r].pts[k].y);
        s += Polygon::area(pts);
    }
    return s;
}

int32_t geo_min_dist2(const mp_point *points, uint64_t n, const mp_rings *rings, double *out)
{
    if ((points == nullptr && n > 0) || rings == nullptr || (out == nullptr && n > 0))
        return MP_ERR_ARGUMENT;
    return guarded_geometry([&] {
        // The tree finds the nearest edge; the exact minimum is then taken over the edges within that
        // distance plus one integer unit, so each result is the brute-force minimum bit for bit.
        const Lines                              lines = to_lines(to_polygons(rings));
        const AABBTreeLines::LinesDistancer<Line> distancer(lines);
        for (uint64_t i = 0; i < n; ++i) {
            const Point p(points[i].x, points[i].y);
            double      min_dist2 = std::numeric_limits<double>::max();
            if (!lines.empty()) {
                const double nearest = std::get<0>(distancer.distance_from_lines_extra<false>(p));
                for (size_t k : distancer.all_lines_in_radius(p, nearest + 1.))
                    min_dist2 = std::min(min_dist2, lines[k].distance_to_squared(p));
            }
            out[i] = min_dist2;
        }
    });
}

const mp_geometry_v1 g_geometry = {sizeof(mp_geometry_v1), geo_offset, geo_boolean, geo_simplify, geo_union_ex,
                                   geo_area, geo_min_dist2, geo_free_rings, geo_boolean_ex};

} // namespace

namespace {

struct RegisteredService
{
    std::string name;
    uint32_t    version    = 0;
    const void *table      = nullptr;
    uint32_t    table_size = 0;
};

std::mutex                      g_services_mutex;
std::vector<RegisteredService> &g_services = *new std::vector<RegisteredService>(); // never destroyed, as the mod set

} // namespace

void register_core_service(const std::string &name, uint32_t version, const void *table, uint32_t table_size)
{
    std::lock_guard<std::mutex> lock(g_services_mutex);
    for (RegisteredService &s : g_services)
        if (s.name == name) {
            s = {name, version, table, table_size};
            return;
        }
    g_services.push_back({name, version, table, table_size});
}

const void *core_service(const std::string &name, uint32_t min_version, uint32_t *table_size)
{
    if (name == MP_GEOMETRY_SERVICE && min_version <= MP_GEOMETRY_VERSION) {
        if (table_size)
            *table_size = sizeof(g_geometry);
        return &g_geometry;
    }
    std::lock_guard<std::mutex> lock(g_services_mutex);
    for (const RegisteredService &s : g_services)
        if (s.name == name && s.version >= min_version) {
            if (table_size)
                *table_size = s.table_size;
            return s.table;
        }
    return nullptr;
}

} // namespace ModHost
} // namespace Slic3r
