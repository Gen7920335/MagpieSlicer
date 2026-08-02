#pragma once

#include <cstdint>

#include <boost/multiprecision/cpp_int.hpp>

namespace Slic3r::Gpu {

// GPU kernels use signed 64-bit values. Each request is rebased around its
// scanline and Y midpoint. The host uses a wider type to prove that every GPU
// intermediate and the restored global result fit before dispatch.
using Coord = int64_t;
using WideCoord = boost::multiprecision::int128_t;

constexpr Coord coordinate_scale = 1'000'000; // one million fixed-point units per millimetre.

struct Point {
    Coord x { 0 };
    Coord y { 0 };
};

struct Segment {
    Point a;
    Point b;
};

inline WideCoord orient2d(const Point& a, const Point& b, const Point& c)
{
    const WideCoord abx = WideCoord(b.x) - a.x;
    const WideCoord aby = WideCoord(b.y) - a.y;
    const WideCoord acx = WideCoord(c.x) - a.x;
    const WideCoord acy = WideCoord(c.y) - a.y;
    return abx * acy - aby * acx;
}

// Stable IDs, rather than GPU workgroup order, are the tie-breaker for every
// wall or infill segment emitted by a compute stage.
struct StableSegment {
    Segment segment;
    uint64_t stable_id { 0 };
};

} // namespace Slic3r::Gpu
