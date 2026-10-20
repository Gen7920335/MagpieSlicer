// Polygon types and operations for the Cura support mod, named as in the slicer core so the port reads
// like the code it came from. Every polygon operation runs in the core (service "core.geometry",
// magpie_geometry.h), so the mod gets exactly the polygons the same calls gave inside the core.
#pragma once

#include <magpie_mod.hpp>
#include <magpie_geometry.h>

#include <cstdint>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace cura {

using coord_t = int64_t;

// One integer unit is magpie::host()->scaling_factor_mm millimetres, read at each call.
inline double scaling_factor() { return magpie::host()->scaling_factor_mm; }
// The core's scale_(): millimetres to integer units, as a double the caller truncates or keeps.
inline double scale_(double mm) { return mm / scaling_factor(); }

// The value the core's PI and M_PI both round to.
constexpr double PI = 3.141592653589793238;
// Geometry::deg2rad.
inline double deg2rad(double angle) { return PI * angle / 180.0; }

// Same layout as mp_point, so polygons are handed to the core without copying.
struct Point
{
    coord_t px = 0, py = 0;
    Point() = default;
    Point(coord_t x, coord_t y) : px(x), py(y) {}
    coord_t x() const { return px; }
    coord_t y() const { return py; }
    Point operator+(const Point &o) const { return {px + o.px, py + o.py}; }
    Point operator-(const Point &o) const { return {px - o.px, py - o.py}; }
    // Eigen's integer division of each coordinate.
    Point operator/(coord_t d) const { return {px / d, py / d}; }
};
static_assert(sizeof(Point) == sizeof(mp_point), "Point must have mp_point's layout");

struct Polygon
{
    std::vector<Point> points;
    size_t size() const { return points.size(); }
    bool   empty() const { return points.empty(); }
    double area() const;
};
using Polygons = std::vector<Polygon>;

struct ExPolygon
{
    Polygon  contour;
    Polygons holes;
};
using ExPolygons = std::vector<ExPolygon>;

enum JoinType { jtSquare, jtRound, jtMiter };

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

inline ExPolygon to_expolygon(const mp_expoly &in)
{
    ExPolygon out;
    for (uint32_t r = 0; r < in.n; ++r) {
        Polygon p;
        p.points.assign(reinterpret_cast<const Point *>(in.rings[r].pts), reinterpret_cast<const Point *>(in.rings[r].pts) + in.rings[r].n);
        if (r == 0)
            out.contour = std::move(p);
        else
            out.holes.push_back(std::move(p));
    }
    return out;
}

// Takes a host result and releases it.
inline Polygons take(mp_rings &out, int32_t rc, const char *what)
{
    if (rc != MP_OK)
        throw std::runtime_error(std::string("geometry service: ") + what + " failed");
    Polygons result = to_polygons(out);
    geometry()->free_rings(&out);
    return result;
}

// offset(const Polygons &, float delta, JoinType, double miterLimit); miter_limit <= 0 is the core's
// DefaultMiterLimit.
inline Polygons offset(const Polygons &polys, float delta, JoinType join, double miter_limit)
{
    const int32_t j = join == jtRound ? MP_JOIN_ROUND : join == jtSquare ? MP_JOIN_SQUARE : MP_JOIN_MITER;
    mp_rings      out{};
    return take(out, geometry()->offset(RingsView(polys).get(), double(delta), j, miter_limit, &out), "offset");
}
constexpr double DefaultMiterLimit = 0.; // "the core's own default" to the service

inline Polygons boolean_op(int32_t op, const Polygons &a, const Polygons *b)
{
    mp_rings out{};
    if (b == nullptr)
        return take(out, geometry()->boolean(op, RingsView(a).get(), nullptr, &out), "boolean");
    return take(out, geometry()->boolean(op, RingsView(a).get(), RingsView(*b).get(), &out), "boolean");
}
inline Polygons union_(const Polygons &a) { return boolean_op(MP_OP_UNION, a, nullptr); }
inline Polygons union_(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_UNION, a, &b); }
// union_(polygons, pftPositive)
inline Polygons union_positive(const Polygons &a) { return boolean_op(MP_OP_UNION_POSITIVE, a, nullptr); }
inline Polygons intersection(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_INTERSECTION, a, &b); }
inline Polygons diff(const Polygons &a, const Polygons &b) { return boolean_op(MP_OP_DIFFERENCE, a, &b); }

inline Polygons polygons_simplify(const Polygons &polys, double tolerance)
{
    mp_rings out{};
    return take(out, geometry()->simplify(RingsView(polys).get(), tolerance, &out), "simplify");
}

inline ExPolygons union_ex(const Polygons &polys)
{
    mp_expolys out{};
    if (geometry()->union_ex(RingsView(polys).get(), &out) != MP_OK)
        throw std::runtime_error("geometry service: union_ex failed");
    ExPolygons result;
    result.reserve(out.n);
    for (uint32_t i = 0; i < out.n; ++i)
        result.push_back(to_expolygon(out.items[i]));
    magpie::host()->free_expolys(&out);
    return result;
}

inline double area(const Polygons &polys) { return geometry()->area(RingsView(polys).get()); }

inline double Polygon::area() const
{
    const mp_ring  ring{reinterpret_cast<const mp_point *>(points.data()), uint32_t(points.size())};
    const mp_rings one{&ring, 1};
    return geometry()->area(&one);
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

// Squared distance from each point to the nearest edge of `polys`.
inline std::vector<double> min_dist2(const std::vector<Point> &points, const Polygons &polys)
{
    std::vector<double> out(points.size(), std::numeric_limits<double>::max());
    if (geometry()->min_dist2(reinterpret_cast<const mp_point *>(points.data()), points.size(), RingsView(polys).get(), out.data()) != MP_OK)
        throw std::runtime_error("geometry service: min_dist2 failed");
    return out;
}

} // namespace cura
