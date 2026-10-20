// Polygon types and operations for the Mixed support mod, named as in the slicer core so the plan reads like
// the code it came from. Every polygon operation runs in the core (service "core.geometry", magpie_geometry.h),
// so the mod gets exactly the polygons the same calls gave inside the core.
#pragma once

#include <magpie_mod.hpp>
#include <magpie_geometry.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace mixed {

using coord_t = int64_t;

// Same layout as mp_point, so polygons are handed to the core without copying.
struct Point
{
    coord_t px = 0, py = 0;
    coord_t x() const { return px; }
    coord_t y() const { return py; }
    bool    operator<(const Point &o) const { return px < o.px || (px == o.px && py < o.py); }
};
static_assert(sizeof(Point) == sizeof(mp_point), "Point must have mp_point's layout");

struct Polygon
{
    std::vector<Point> points;
};
using Polygons = std::vector<Polygon>;

struct ExPolygon
{
    Polygon  contour;
    Polygons holes;
};
using ExPolygons = std::vector<ExPolygon>;

// The core's BoundingBox of points: min and max per axis.
struct BoundingBox
{
    Point min, max;
    bool  defined = false;
    // BoundingBoxBase::overlap: the boxes touch or overlap.
    bool overlap(const BoundingBox &o) const
    {
        return !(max.x() < o.min.x() || min.x() > o.max.x() || max.y() < o.min.y() || min.y() > o.max.y());
    }
};

inline void merge(BoundingBox &bb, const std::vector<Point> &points)
{
    for (const Point &p : points) {
        if (!bb.defined) {
            bb.min = bb.max = p;
            bb.defined      = true;
        } else {
            bb.min.px = std::min(bb.min.px, p.px);
            bb.min.py = std::min(bb.min.py, p.py);
            bb.max.px = std::max(bb.max.px, p.px);
            bb.max.py = std::max(bb.max.py, p.py);
        }
    }
}
// get_extents(const ExPolygon &): the contour's box.
inline BoundingBox get_extents(const ExPolygon &e)
{
    BoundingBox bb;
    merge(bb, e.contour.points);
    return bb;
}
// get_extents(const Polygons &): the box of every ring.
inline BoundingBox get_extents(const Polygons &polys)
{
    BoundingBox bb;
    for (const Polygon &p : polys)
        merge(bb, p.points);
    return bb;
}

// The geometry service, imported in magpie_mod_init.
inline const mp_geometry_v1 *&geometry()
{
    static const mp_geometry_v1 *g = nullptr;
    return g;
}

// A borrowed C view of polygons.
class RingsView
{
public:
    explicit RingsView(const Polygons &polys)
    {
        m_rings.reserve(polys.size());
        for (const Polygon &p : polys)
            m_rings.push_back({reinterpret_cast<const mp_point *>(p.points.data()), uint32_t(p.points.size())});
        m_view = {m_rings.data(), uint32_t(m_rings.size())};
    }
    const mp_rings *get() const { return &m_view; }

private:
    std::vector<mp_ring> m_rings;
    mp_rings             m_view{};
};

inline Polygons to_polygons(const mp_rings &in)
{
    Polygons out(in.n);
    for (uint32_t r = 0; r < in.n; ++r)
        out[r].points.assign(reinterpret_cast<const Point *>(in.rings[r].pts), reinterpret_cast<const Point *>(in.rings[r].pts) + in.rings[r].n);
    return out;
}

inline Polygons to_polygons(const ExPolygon &e)
{
    Polygons out;
    out.reserve(1 + e.holes.size());
    out.push_back(e.contour);
    out.insert(out.end(), e.holes.begin(), e.holes.end());
    return out;
}

inline Polygons to_polygons(const ExPolygons &es)
{
    Polygons out;
    for (const ExPolygon &e : es) {
        out.push_back(e.contour);
        out.insert(out.end(), e.holes.begin(), e.holes.end());
    }
    return out;
}

inline void append(Polygons &dst, const Polygons &src) { dst.insert(dst.end(), src.begin(), src.end()); }

inline Polygons take(mp_rings &out, int32_t rc, const char *what)
{
    if (rc != MP_OK)
        throw std::runtime_error(std::string("geometry service: ") + what + " failed");
    Polygons result = to_polygons(out);
    geometry()->free_rings(&out);
    return result;
}

inline ExPolygons take_ex(mp_expolys &out, int32_t rc, const char *what)
{
    if (rc != MP_OK)
        throw std::runtime_error(std::string("geometry service: ") + what + " failed");
    ExPolygons result;
    result.reserve(out.n);
    for (uint32_t i = 0; i < out.n; ++i) {
        ExPolygon e;
        for (uint32_t r = 0; r < out.items[i].n; ++r) {
            Polygon p;
            p.points.assign(reinterpret_cast<const Point *>(out.items[i].rings[r].pts),
                            reinterpret_cast<const Point *>(out.items[i].rings[r].pts) + out.items[i].rings[r].n);
            if (r == 0)
                e.contour = std::move(p);
            else
                e.holes.push_back(std::move(p));
        }
        result.push_back(std::move(e));
    }
    magpie::host()->free_expolys(&out);
    return result;
}

inline Polygons boolean_op(int32_t op, const Polygons &a, const Polygons *b)
{
    mp_rings out{};
    if (b == nullptr)
        return take(out, geometry()->boolean(op, RingsView(a).get(), nullptr, &out), "boolean");
    return take(out, geometry()->boolean(op, RingsView(a).get(), RingsView(*b).get(), &out), "boolean");
}
inline Polygons union_(const Polygons &a) { return boolean_op(MP_OP_UNION, a, nullptr); }
inline Polygons union_(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_UNION, a, &b); }
inline Polygons intersection(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_INTERSECTION, a, &b); }
inline Polygons diff(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_DIFFERENCE, a, &b); }

inline ExPolygons union_ex(const Polygons &polys)
{
    mp_expolys out{};
    return take_ex(out, geometry()->union_ex(RingsView(polys).get(), &out), "union_ex");
}
// diff_ex(const ExPolygon &, const Polygons &) and diff_ex(const ExPolygon &, const ExPolygons &): the core's
// Clipper reads an ExPolygon as its rings, contour first, so the rings give the same result.
inline ExPolygons diff_ex(const Polygons &a, const Polygons &b)
{
    mp_expolys out{};
    return take_ex(out, geometry()->boolean_ex(MP_OP_DIFFERENCE, RingsView(a).get(), RingsView(b).get(), &out), "diff_ex");
}

// Polygon::area().
inline double area(const Polygon &p)
{
    const mp_ring  ring{reinterpret_cast<const mp_point *>(p.points.data()), uint32_t(p.points.size())};
    const mp_rings one{&ring, 1};
    return geometry()->area(&one);
}
// ExPolygon::area(): the contour's, holes subtracted as the core writes it.
inline double area(const ExPolygon &e)
{
    double a = area(e.contour);
    for (const Polygon &hole : e.holes)
        a -= -area(hole); // holes have negative area
    return a;
}
// area(const ExPolygons &).
inline double area(const ExPolygons &es)
{
    double s = 0.;
    for (const ExPolygon &e : es)
        s += area(e);
    return s;
}

} // namespace mixed
