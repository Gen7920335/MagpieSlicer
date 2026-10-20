#pragma once

// LESIC ring annotations (Magpie line 1): the bottom label, ring value labels and the inner brim printed on the first
// layer of the LESIC calibration ring, as G-code line segments. Moved from the core (GCode/LesicAnnotations.hpp)
// with its arithmetic unchanged; vec2.hpp stands in for the core's Vec2d, the calibration's values and the settings
// the label reads come through the gcode.events writer (lesic.cpp).

#include "vec2.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace lesic {

constexpr double LESIC_ZERO_ANGLE_DEG = -90.0;
constexpr bool   LESIC_CLOCKWISE      = true;
constexpr double LESIC_LABEL_X_SCALE  = 0.55;
constexpr double LESIC_LABEL_STROKE_WIDTH = 0.25;
constexpr double LESIC_LABEL_CONNECTOR_WIDTH = 0.20;
constexpr double LESIC_LABEL_ADVANCE_UNITS = 6.8;
constexpr double LESIC_LABEL_GLYPH_WIDTH_UNITS = 5.0;
constexpr double LESIC_LABEL_LETTER_GAP_RATIO = 0.4;
constexpr double LESIC_LABEL_TEXT_WIDTH = 1.0;
constexpr double LESIC_LABEL_INNER_OUTLINE_RADIUS = LESIC_LABEL_TEXT_WIDTH * 0.5 + LESIC_LABEL_STROKE_WIDTH * 0.5;
constexpr double LESIC_LABEL_OUTER_RADIUS = LESIC_LABEL_INNER_OUTLINE_RADIUS + LESIC_LABEL_STROKE_WIDTH;
constexpr int    LESIC_INNER_BRIM_LINES = 5;
constexpr int    LESIC_INNER_BRIM_ARC_SEGMENTS = 2880;
constexpr double LESIC_LABEL_MASK_EXTRA_CLEARANCE = 0.02;

struct LesicAnnotationSegment
{
    Vec2d a;
    Vec2d b;
    double width { LESIC_LABEL_STROKE_WIDTH };
};

static bool lesic_is_small_label_plate(const Calib_Params &params)
{
    return params.lesic_small_bed ||
        (params.lesic_circle_diameter > 0.0 && params.lesic_circle_diameter < 180.0);
}

static double lesic_bottom_label_stroke_width(const Calib_Params &params)
{
    return lesic_is_small_label_plate(params) ?
        LESIC_LABEL_STROKE_WIDTH * 0.85 :
        LESIC_LABEL_STROKE_WIDTH;
}

static double lesic_normalize_degrees(double angle)
{
    while (angle < 0.0)
        angle += 360.0;
    while (angle >= 360.0)
        angle -= 360.0;
    return angle;
}

static double lesic_angle_progress(double angle_deg)
{
    const double delta = LESIC_CLOCKWISE ?
        LESIC_ZERO_ANGLE_DEG - angle_deg :
        angle_deg - LESIC_ZERO_ANGLE_DEG;
    return lesic_normalize_degrees(delta) / 360.0;
}

static Vec2d lesic_point_on_circle(const Vec2d &center, double radius, double angle_deg)
{
    const double radians = angle_deg * M_PI / 180.0;
    return center + Vec2d(radius * std::cos(radians), radius * std::sin(radians));
}

static void lesic_append_segment(
    std::vector<LesicAnnotationSegment> &segments,
    const Vec2d &a,
    const Vec2d &b,
    double width = LESIC_LABEL_STROKE_WIDTH)
{
    if ((b - a).norm() > EPSILON)
        segments.push_back({ a, b, width });
}

static std::vector<LesicAnnotationSegment> lesic_rect_loop_segments(double width, double height)
{
    std::vector<LesicAnnotationSegment> segments;
    if (width <= 0.0 || height <= 0.0)
        return segments;

    const double half_w = width * 0.5;
    const double half_h = height * 0.5;
    const std::vector<Vec2d> loop {
        Vec2d(-half_w, -half_h),
        Vec2d( half_w, -half_h),
        Vec2d( half_w,  half_h),
        Vec2d(-half_w,  half_h),
        Vec2d(-half_w, -half_h)
    };
    for (size_t i = 0; i + 1 < loop.size(); ++i)
        lesic_append_segment(segments, loop[i], loop[i + 1]);
    return segments;
}

static std::vector<LesicAnnotationSegment> lesic_outlined_rect_segments(double width, double height, double inset = 0.25)
{
    std::vector<LesicAnnotationSegment> segments = lesic_rect_loop_segments(width, height);
    const double inner_w = width - inset * 2.0;
    const double inner_h = height - inset * 2.0;
    if (inner_w > inset && inner_h > inset) {
        std::vector<LesicAnnotationSegment> inner = lesic_rect_loop_segments(inner_w, inner_h);
        segments.insert(segments.end(), inner.begin(), inner.end());
    }
    return segments;
}

static std::vector<LesicAnnotationSegment> lesic_transform_segments(
    const std::vector<LesicAnnotationSegment> &segments,
    double angle_deg,
    const Vec2d &anchor)
{
    const double radians = angle_deg * M_PI / 180.0;
    const double c = std::cos(radians);
    const double s = std::sin(radians);
    const auto transform = [c, s, &anchor](const Vec2d &p) {
        return anchor + Vec2d(p.x() * c - p.y() * s, p.x() * s + p.y() * c);
    };

    std::vector<LesicAnnotationSegment> out;
    out.reserve(segments.size());
    for (const LesicAnnotationSegment &segment : segments)
        lesic_append_segment(out, transform(segment.a), transform(segment.b), segment.width);
    return out;
}

static std::vector<std::vector<Vec2d>> lesic_font_strokes(char ch)
{
    ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    switch (ch) {
    case '0': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 0), Vec2d(0, 0), Vec2d(5, 7) }};
    case '1': return {{ Vec2d(2.5, 0), Vec2d(2.5, 7) }, { Vec2d(1.2, 5.8), Vec2d(2.5, 7) }, { Vec2d(1, 0), Vec2d(4, 0) }};
    case '2': return {{ Vec2d(0, 5.5), Vec2d(1, 7), Vec2d(5, 7), Vec2d(5, 4.8), Vec2d(0, 0), Vec2d(5, 0) }};
    case '3': return {{ Vec2d(0, 7), Vec2d(5, 7), Vec2d(3, 3.5), Vec2d(5, 3.5), Vec2d(5, 0), Vec2d(0, 0) }};
    case '4': return {{ Vec2d(5, 0), Vec2d(5, 7), Vec2d(0, 2.5), Vec2d(5, 2.5) }};
    case '5': return {{ Vec2d(5, 7), Vec2d(0, 7), Vec2d(0, 3.5), Vec2d(5, 3.5), Vec2d(5, 0), Vec2d(0, 0) }};
    case '6': return {{ Vec2d(5, 7), Vec2d(0, 3.5), Vec2d(0, 0), Vec2d(5, 0), Vec2d(5, 3.5), Vec2d(0, 3.5) }};
    case '7': return {{ Vec2d(0, 7), Vec2d(5, 7), Vec2d(1.5, 0) }};
    case '8': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 0), Vec2d(0, 0) }, { Vec2d(0, 3.5), Vec2d(5, 3.5) }};
    case '9': return {{ Vec2d(5, 3.5), Vec2d(0, 3.5), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 0), Vec2d(0, 0) }};
    case 'A': return {{ Vec2d(0, 0), Vec2d(2.5, 7), Vec2d(5, 0) }, { Vec2d(1.2, 3), Vec2d(3.8, 3) }};
    case 'B': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(4, 7), Vec2d(5, 6), Vec2d(5, 4.3), Vec2d(4, 3.5), Vec2d(0, 3.5) }, { Vec2d(0, 3.5), Vec2d(4, 3.5), Vec2d(5, 2.7), Vec2d(5, 1), Vec2d(4, 0), Vec2d(0, 0) }};
    case 'C': return {{ Vec2d(5, 7), Vec2d(0, 7), Vec2d(0, 0), Vec2d(5, 0) }};
    case 'D': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(3.5, 7), Vec2d(5, 5.5), Vec2d(5, 1.5), Vec2d(3.5, 0), Vec2d(0, 0) }};
    case 'E': return {{ Vec2d(5, 7), Vec2d(0, 7), Vec2d(0, 0), Vec2d(5, 0) }, { Vec2d(0, 3.5), Vec2d(3.8, 3.5) }};
    case 'F': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7) }, { Vec2d(0, 3.5), Vec2d(3.8, 3.5) }};
    case 'G': return {{ Vec2d(5, 7), Vec2d(0, 7), Vec2d(0, 0), Vec2d(5, 0), Vec2d(5, 3), Vec2d(3, 3) }};
    case 'H': return {{ Vec2d(0, 0), Vec2d(0, 7) }, { Vec2d(5, 0), Vec2d(5, 7) }, { Vec2d(0, 3.5), Vec2d(5, 3.5) }};
    case 'I': return {{ Vec2d(0, 7), Vec2d(5, 7) }, { Vec2d(2.5, 7), Vec2d(2.5, 0) }, { Vec2d(0, 0), Vec2d(5, 0) }};
    case 'J': return {{ Vec2d(5, 7), Vec2d(5, 0), Vec2d(2, 0), Vec2d(0, 2) }};
    case 'K': return {{ Vec2d(0, 0), Vec2d(0, 7) }, { Vec2d(5, 7), Vec2d(0, 3.5), Vec2d(5, 0) }};
    case 'L': return {{ Vec2d(0, 7), Vec2d(0, 0), Vec2d(5, 0) }};
    case 'M': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(2.5, 3), Vec2d(5, 7), Vec2d(5, 0) }};
    case 'N': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 0), Vec2d(5, 7) }};
    case 'O': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 0), Vec2d(0, 0) }};
    case 'P': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 3.5), Vec2d(0, 3.5) }};
    case 'Q': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 0), Vec2d(0, 0) }, { Vec2d(3, 1.5), Vec2d(5, 0) }};
    case 'R': return {{ Vec2d(0, 0), Vec2d(0, 7), Vec2d(5, 7), Vec2d(5, 3.5), Vec2d(0, 3.5) }, { Vec2d(0, 3.5), Vec2d(5, 0) }};
    case 'S': return {{ Vec2d(5, 7), Vec2d(0, 7), Vec2d(0, 3.5), Vec2d(5, 3.5), Vec2d(5, 0), Vec2d(0, 0) }};
    case 'T': return {{ Vec2d(0, 7), Vec2d(5, 7) }, { Vec2d(2.5, 7), Vec2d(2.5, 0) }};
    case 'U': return {{ Vec2d(0, 7), Vec2d(0, 0), Vec2d(5, 0), Vec2d(5, 7) }};
    case 'V': return {{ Vec2d(0, 7), Vec2d(2.5, 0), Vec2d(5, 7) }};
    case 'W': return {{ Vec2d(0, 7), Vec2d(1, 0), Vec2d(2.5, 4), Vec2d(4, 0), Vec2d(5, 7) }};
    case 'X': return {{ Vec2d(0, 7), Vec2d(5, 0), Vec2d(2.5, 3.5), Vec2d(5, 7), Vec2d(0, 0) }};
    case 'Y': return {{ Vec2d(0, 7), Vec2d(2.5, 3.5), Vec2d(5, 7) }, { Vec2d(2.5, 3.5), Vec2d(2.5, 0) }};
    case 'Z': return {{ Vec2d(0, 7), Vec2d(5, 7), Vec2d(0, 0), Vec2d(5, 0) }};
    case '/': return {{ Vec2d(0, 0), Vec2d(5, 7) }};
    case '*': return {{ Vec2d(0, 7), Vec2d(5, 0) }, { Vec2d(0, 0), Vec2d(5, 7) }};
    case '_': return {{ Vec2d(0, 0), Vec2d(5, 0) }};
    case '-': return {{ Vec2d(0, 3.5), Vec2d(5, 3.5) }};
    case ':': return {{ Vec2d(2.3, 5.7), Vec2d(2.7, 5.7), Vec2d(2.7, 5.3), Vec2d(2.3, 5.3), Vec2d(2.3, 5.7) }, { Vec2d(2.3, 1.7), Vec2d(2.7, 1.7), Vec2d(2.7, 1.3), Vec2d(2.3, 1.3), Vec2d(2.3, 1.7) }};
    case '.': return {{ Vec2d(2.32, 0.22), Vec2d(2.68, 0.22), Vec2d(2.68, 0.58), Vec2d(2.32, 0.58), Vec2d(2.32, 0.22) }};
    case '~': return {{ Vec2d(2, 5), Vec2d(2, 7), Vec2d(4, 7), Vec2d(4, 5), Vec2d(2, 5) }};
    case '#': return {{ Vec2d(1.2, 7), Vec2d(4.2, 7), Vec2d(3, 5.6), Vec2d(4.2, 5.6), Vec2d(4.2, 4.2), Vec2d(1.2, 4.2) }};
    case '^': return {{ Vec2d(0, 0), Vec2d(2.5, 7), Vec2d(5, 0) }};
    case ' ': return {};
    default:  return {};
    }
}

static double lesic_label_advance_units(double /*cell*/, double /*x_scale*/, double /*line_width*/)
{
    const double target_advance = LESIC_LABEL_GLYPH_WIDTH_UNITS * (1.0 + LESIC_LABEL_LETTER_GAP_RATIO);
    return std::max(LESIC_LABEL_ADVANCE_UNITS, target_advance);
}

static Vec2d lesic_transform_font_point(const Vec2d &p, double x0, double y0, double cell, double x_scale)
{
    return Vec2d(x0 + p.x() * cell * x_scale, y0 + p.y() * cell);
}

static double lesic_point_to_segment_distance(const Vec2d &p, const Vec2d &a, const Vec2d &b)
{
    const Vec2d ab = b - a;
    const double l2 = ab.squaredNorm();
    if (l2 <= 1e-12)
        return (p - a).norm();
    const double t = std::clamp((p - a).dot(ab) / l2, 0.0, 1.0);
    return (p - (a + ab * t)).norm();
}

static int lesic_orientation(const Vec2d &a, const Vec2d &b, const Vec2d &c)
{
    const double v = (b.y() - a.y()) * (c.x() - b.x()) - (b.x() - a.x()) * (c.y() - b.y());
    if (std::abs(v) < 1e-9)
        return 0;
    return v > 0.0 ? 1 : 2;
}

static bool lesic_on_segment(const Vec2d &a, const Vec2d &b, const Vec2d &c)
{
    return b.x() <= std::max(a.x(), c.x()) + 1e-9 &&
           b.x() + 1e-9 >= std::min(a.x(), c.x()) &&
           b.y() <= std::max(a.y(), c.y()) + 1e-9 &&
           b.y() + 1e-9 >= std::min(a.y(), c.y());
}

static bool lesic_segments_intersect(const Vec2d &a1, const Vec2d &a2, const Vec2d &b1, const Vec2d &b2)
{
    const int o1 = lesic_orientation(a1, a2, b1);
    const int o2 = lesic_orientation(a1, a2, b2);
    const int o3 = lesic_orientation(b1, b2, a1);
    const int o4 = lesic_orientation(b1, b2, a2);
    if (o1 != o2 && o3 != o4)
        return true;
    if (o1 == 0 && lesic_on_segment(a1, b1, a2))
        return true;
    if (o2 == 0 && lesic_on_segment(a1, b2, a2))
        return true;
    if (o3 == 0 && lesic_on_segment(b1, a1, b2))
        return true;
    if (o4 == 0 && lesic_on_segment(b1, a2, b2))
        return true;
    return false;
}

static bool lesic_segment_near_segment(
    const Vec2d &a0,
    const Vec2d &a1,
    const Vec2d &b0,
    const Vec2d &b1,
    double clearance)
{
    if (lesic_segments_intersect(a0, a1, b0, b1))
        return true;
    if (lesic_point_to_segment_distance(a0, b0, b1) <= clearance)
        return true;
    if (lesic_point_to_segment_distance(a1, b0, b1) <= clearance)
        return true;
    if (lesic_point_to_segment_distance((a0 + a1) * 0.5, b0, b1) <= clearance)
        return true;
    if (lesic_point_to_segment_distance(b0, a0, a1) <= clearance)
        return true;
    if (lesic_point_to_segment_distance(b1, a0, a1) <= clearance)
        return true;
    if (lesic_point_to_segment_distance((b0 + b1) * 0.5, a0, a1) <= clearance)
        return true;
    return false;
}

static std::vector<LesicAnnotationSegment> lesic_filter_segments_by_obstacles(
    const std::vector<LesicAnnotationSegment> &segments,
    const std::vector<LesicAnnotationSegment> &obstacles,
    double clearance)
{
    if (segments.empty() || obstacles.empty())
        return segments;

    std::vector<LesicAnnotationSegment> out;
    out.reserve(segments.size());
    for (const LesicAnnotationSegment &segment : segments) {
        bool blocked = false;
        const double min_x = std::min(segment.a.x(), segment.b.x()) - clearance;
        const double max_x = std::max(segment.a.x(), segment.b.x()) + clearance;
        const double min_y = std::min(segment.a.y(), segment.b.y()) - clearance;
        const double max_y = std::max(segment.a.y(), segment.b.y()) + clearance;
        for (const LesicAnnotationSegment &obstacle : obstacles) {
            if (std::max(obstacle.a.x(), obstacle.b.x()) + clearance < min_x ||
                std::min(obstacle.a.x(), obstacle.b.x()) - clearance > max_x ||
                std::max(obstacle.a.y(), obstacle.b.y()) + clearance < min_y ||
                std::min(obstacle.a.y(), obstacle.b.y()) - clearance > max_y)
                continue;
            if (lesic_segment_near_segment(segment.a, segment.b, obstacle.a, obstacle.b, clearance)) {
                blocked = true;
                break;
            }
        }
        if (!blocked)
            out.push_back(segment);
    }
    return out;
}

static std::vector<LesicAnnotationSegment> lesic_glyph_centerlines(
    char ch,
    double x0,
    double y0,
    double cell,
    double x_scale)
{
    std::vector<LesicAnnotationSegment> out;
    for (const std::vector<Vec2d> &stroke : lesic_font_strokes(ch)) {
        for (size_t i = 0; i + 1 < stroke.size(); ++i) {
            lesic_append_segment(
                out,
                lesic_transform_font_point(stroke[i], x0, y0, cell, x_scale),
                lesic_transform_font_point(stroke[i + 1], x0, y0, cell, x_scale));
        }
    }
    return out;
}

static std::vector<Vec2d> lesic_glyph_source_points(
    char ch,
    double x0,
    double y0,
    double cell,
    double x_scale)
{
    std::vector<Vec2d> out;
    for (const std::vector<Vec2d> &stroke : lesic_font_strokes(ch))
        for (const Vec2d &p : stroke)
            out.push_back(lesic_transform_font_point(p, x0, y0, cell, x_scale));
    return out;
}

using LesicPointKey = std::pair<long long, long long>;
using LesicEdgeKey = std::pair<LesicPointKey, LesicPointKey>;

struct LesicContourEdge
{
    Vec2d start;
    Vec2d end;
};

static LesicPointKey lesic_point_key(const Vec2d &p)
{
    return { static_cast<long long>(std::llround(p.x() * 100000.0)),
             static_cast<long long>(std::llround(p.y() * 100000.0)) };
}

static LesicEdgeKey lesic_edge_key(const Vec2d &a, const Vec2d &b)
{
    return { lesic_point_key(a), lesic_point_key(b) };
}

static double lesic_polygon_area(const std::vector<Vec2d> &loop)
{
    double area = 0.0;
    for (size_t i = 0; i + 1 < loop.size(); ++i)
        area += loop[i].x() * loop[i + 1].y() - loop[i + 1].x() * loop[i].y();
    return area;
}

struct LesicLoopBounds
{
    double min_x { 0.0 };
    double max_x { 0.0 };
    double min_y { 0.0 };
    double max_y { 0.0 };
};

static LesicLoopBounds lesic_loop_bounds(const std::vector<Vec2d> &loop)
{
    LesicLoopBounds bounds;
    if (loop.empty())
        return bounds;
    bounds.min_x = bounds.max_x = loop.front().x();
    bounds.min_y = bounds.max_y = loop.front().y();
    for (const Vec2d &p : loop) {
        bounds.min_x = std::min(bounds.min_x, p.x());
        bounds.max_x = std::max(bounds.max_x, p.x());
        bounds.min_y = std::min(bounds.min_y, p.y());
        bounds.max_y = std::max(bounds.max_y, p.y());
    }
    return bounds;
}

static Vec2d lesic_loop_center(const std::vector<Vec2d> &loop)
{
    const LesicLoopBounds bounds = lesic_loop_bounds(loop);
    return Vec2d((bounds.min_x + bounds.max_x) * 0.5, (bounds.min_y + bounds.max_y) * 0.5);
}

static bool lesic_point_in_loop(const Vec2d &p, const std::vector<Vec2d> &loop)
{
    bool inside = false;
    if (loop.empty())
        return inside;
    for (size_t i = 0, j = loop.size() - 1; i < loop.size(); j = i++) {
        const Vec2d &pi = loop[i];
        const Vec2d &pj = loop[j];
        const bool intersects = ((pi.y() > p.y()) != (pj.y() > p.y())) &&
            (p.x() < ((pj.x() - pi.x()) * (p.y() - pi.y())) / ((pj.y() - pi.y()) == 0.0 ? 1e-12 : (pj.y() - pi.y())) + pi.x());
        if (intersects)
            inside = !inside;
    }
    return inside;
}

static std::vector<std::vector<Vec2d>> lesic_extract_all_loops(const std::vector<LesicAnnotationSegment> &segments)
{
    std::map<LesicPointKey, std::vector<LesicContourEdge>> outgoing;
    for (const LesicAnnotationSegment &segment : segments)
        outgoing[lesic_point_key(segment.a)].push_back({ segment.a, segment.b });

    std::set<LesicEdgeKey> visited;
    std::vector<std::vector<Vec2d>> loops;

    for (const LesicAnnotationSegment &segment : segments) {
        const LesicEdgeKey start_edge = lesic_edge_key(segment.a, segment.b);
        if (visited.find(start_edge) != visited.end())
            continue;

        std::vector<Vec2d> loop { segment.a, segment.b };
        Vec2d cur_b = segment.b;
        visited.insert(start_edge);

        for (int guard = 0; guard < 20000; ++guard) {
            auto it = outgoing.find(lesic_point_key(cur_b));
            if (it == outgoing.end())
                break;

            const LesicContourEdge *next = nullptr;
            for (const LesicContourEdge &candidate : it->second) {
                if (visited.find(lesic_edge_key(candidate.start, candidate.end)) == visited.end()) {
                    next = &candidate;
                    break;
                }
            }
            if (next == nullptr)
                break;

            visited.insert(lesic_edge_key(next->start, next->end));
            cur_b = next->end;
            if (lesic_point_key(cur_b) == lesic_point_key(loop.front())) {
                loop.push_back(loop.front());
                break;
            }
            loop.push_back(cur_b);
        }

        if (loop.size() >= 4)
            loops.push_back(std::move(loop));
    }

    std::sort(loops.begin(), loops.end(), [](const std::vector<Vec2d> &lhs, const std::vector<Vec2d> &rhs) {
        return std::abs(lesic_polygon_area(lhs)) > std::abs(lesic_polygon_area(rhs));
    });
    return loops;
}

static std::vector<std::vector<Vec2d>> lesic_inner_loops_for_outer(
    const std::vector<Vec2d> &outer_loop,
    const std::vector<std::vector<Vec2d>> &loops)
{
    std::vector<std::vector<Vec2d>> out;
    if (outer_loop.empty())
        return out;
    for (size_t i = 1; i < loops.size(); ++i) {
        if (lesic_point_in_loop(lesic_loop_center(loops[i]), outer_loop))
            out.push_back(loops[i]);
    }
    return out;
}

static bool lesic_should_bridge_inner_loops(char ch)
{
    return ch != ':' && ch != '.' && ch != ',';
}

static Vec2d lesic_pick_horizontal_loop_point(const std::vector<Vec2d> &loop, double target_y, bool left_side)
{
    if (loop.empty())
        return Vec2d::Zero();
    return *std::min_element(loop.begin(), loop.end(), [target_y, left_side](const Vec2d &a, const Vec2d &b) {
        const double dy = std::abs(a.y() - target_y) - std::abs(b.y() - target_y);
        if (std::abs(dy) > 1e-9)
            return dy < 0.0;
        return left_side ? a.x() < b.x() : a.x() > b.x();
    });
}

static Vec2d lesic_pick_vertical_loop_point(const std::vector<Vec2d> &loop, double target_x, bool top_side)
{
    if (loop.empty())
        return Vec2d::Zero();
    return *std::min_element(loop.begin(), loop.end(), [target_x, top_side](const Vec2d &a, const Vec2d &b) {
        const double dx = std::abs(a.x() - target_x) - std::abs(b.x() - target_x);
        if (std::abs(dx) > 1e-9)
            return dx < 0.0;
        return top_side ? a.y() > b.y() : a.y() < b.y();
    });
}

static bool lesic_horizontal_loop_bridge_points(
    const std::vector<Vec2d> &loop,
    double target_y,
    Vec2d &left,
    Vec2d &right)
{
    std::vector<double> xs;
    for (size_t i = 0; i + 1 < loop.size(); ++i) {
        const Vec2d &a = loop[i];
        const Vec2d &b = loop[i + 1];
        if (std::abs(a.y() - b.y()) <= 1e-9) {
            if (std::abs(a.y() - target_y) <= 1e-9) {
                xs.push_back(a.x());
                xs.push_back(b.x());
            }
            continue;
        }
        if (target_y < std::min(a.y(), b.y()) - 1e-9 || target_y > std::max(a.y(), b.y()) + 1e-9)
            continue;
        const double t = (target_y - a.y()) / (b.y() - a.y());
        if (t < -1e-9 || t > 1.0 + 1e-9)
            continue;
        xs.push_back(a.x() + (b.x() - a.x()) * t);
    }
    if (xs.size() < 2)
        return false;
    const auto minmax = std::minmax_element(xs.begin(), xs.end());
    left = Vec2d(*minmax.first, target_y);
    right = Vec2d(*minmax.second, target_y);
    return true;
}

static std::vector<LesicAnnotationSegment> lesic_build_inner_loop_bridges(
    const std::vector<Vec2d> &outer_loop,
    const std::vector<std::vector<Vec2d>> &inner_loops)
{
    std::vector<LesicAnnotationSegment> bridges;
    if (outer_loop.empty() || inner_loops.empty())
        return bridges;

    for (const std::vector<Vec2d> &loop : inner_loops) {
        const LesicLoopBounds bounds = lesic_loop_bounds(loop);
        const double target_y = (bounds.min_y + bounds.max_y) * 0.5;
        const Vec2d inner_left = lesic_pick_horizontal_loop_point(loop, target_y, true);
        const Vec2d inner_right = lesic_pick_horizontal_loop_point(loop, target_y, false);

        std::vector<Vec2d> left_candidates;
        std::vector<Vec2d> right_candidates;
        for (const Vec2d &p : outer_loop) {
            if (p.x() <= inner_left.x() + 1e-9)
                left_candidates.push_back(p);
            if (p.x() >= inner_right.x() - 1e-9)
                right_candidates.push_back(p);
        }

        const Vec2d outer_left = lesic_pick_horizontal_loop_point(left_candidates.empty() ? outer_loop : left_candidates, target_y, false);
        const Vec2d outer_right = lesic_pick_horizontal_loop_point(right_candidates.empty() ? outer_loop : right_candidates, target_y, true);
        lesic_append_segment(bridges, outer_left, inner_left);
        lesic_append_segment(bridges, inner_right, outer_right);
    }

    return bridges;
}

static std::vector<LesicAnnotationSegment> lesic_build_forced_loop_bridge(
    char ch,
    const std::vector<std::vector<Vec2d>> &loops)
{
    std::vector<LesicAnnotationSegment> bridges;
    if (ch != '~' || loops.empty())
        return bridges;

    const std::vector<Vec2d> &loop = loops.front();
    const double target_y = lesic_loop_center(loop).y();
    Vec2d left;
    Vec2d right;
    if (!lesic_horizontal_loop_bridge_points(loop, target_y, left, right)) {
        left = lesic_pick_horizontal_loop_point(loop, target_y, true);
        right = lesic_pick_horizontal_loop_point(loop, target_y, false);
    }
    lesic_append_segment(bridges, left, right);
    return bridges;
}

struct LesicGlyphBuild
{
    char ch { ' ' };
    std::vector<LesicAnnotationSegment> segments;
    std::vector<Vec2d> outer_loop;
    std::vector<std::vector<Vec2d>> inner_loops;
    std::vector<std::vector<Vec2d>> outline_loops;
    std::vector<Vec2d> source_points;
    LesicLoopBounds bbox;
};

static LesicGlyphBuild lesic_build_glyph_geometry(
    char ch,
    double x0,
    double y0,
    double cell,
    double x_scale,
    double line_width,
    double outline_width = LESIC_LABEL_STROKE_WIDTH)
{
    LesicGlyphBuild glyph;
    glyph.ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    glyph.bbox = { x0, x0, y0, y0 };

    const std::vector<LesicAnnotationSegment> centerlines = lesic_glyph_centerlines(glyph.ch, x0, y0, cell, x_scale);
    glyph.source_points = lesic_glyph_source_points(glyph.ch, x0, y0, cell, x_scale);
    if (centerlines.empty() || glyph.source_points.empty())
        return glyph;

    glyph.bbox.min_x = glyph.bbox.max_x = centerlines.front().a.x();
    glyph.bbox.min_y = glyph.bbox.max_y = centerlines.front().a.y();
    const auto merge_point = [&glyph](const Vec2d &p) {
        glyph.bbox.min_x = std::min(glyph.bbox.min_x, p.x());
        glyph.bbox.max_x = std::max(glyph.bbox.max_x, p.x());
        glyph.bbox.min_y = std::min(glyph.bbox.min_y, p.y());
        glyph.bbox.max_y = std::max(glyph.bbox.max_y, p.y());
    };
    for (const LesicAnnotationSegment &segment : centerlines) {
        merge_point(segment.a);
        merge_point(segment.b);
    }

    const double inner_outline_radius = LESIC_LABEL_TEXT_WIDTH * 0.5 + outline_width * 0.5;
    const double outer_radius = inner_outline_radius + outline_width;
    const double radii[] = { inner_outline_radius, outer_radius };
    const double sample = std::max(0.12, line_width / 3.0);

    for (size_t radius_idx = 0; radius_idx < 2; ++radius_idx) {
        const double radius = radii[radius_idx];
        const double pad = radius + sample * 2.0;
        const double origin_x = glyph.bbox.min_x - pad;
        const double origin_y = glyph.bbox.min_y - pad;
        const int cols = std::max(1, static_cast<int>(std::ceil((glyph.bbox.max_x - glyph.bbox.min_x + pad * 2.0) / sample)));
        const int rows = std::max(1, static_cast<int>(std::ceil((glyph.bbox.max_y - glyph.bbox.min_y + pad * 2.0) / sample)));

        std::vector<std::vector<bool>> filled(rows, std::vector<bool>(cols, false));
        for (int gy = 0; gy < rows; ++gy) {
            for (int gx = 0; gx < cols; ++gx) {
                const Vec2d p(origin_x + (gx + 0.5) * sample, origin_y + (gy + 0.5) * sample);
                filled[gy][gx] = std::any_of(centerlines.begin(), centerlines.end(), [&p, radius](const LesicAnnotationSegment &segment) {
                    return lesic_point_to_segment_distance(p, segment.a, segment.b) <= radius;
                });
            }
        }

        std::vector<LesicAnnotationSegment> contour;
        const auto x_at = [origin_x, sample](int gx) { return origin_x + gx * sample; };
        const auto y_at = [origin_y, sample](int gy) { return origin_y + gy * sample; };

        for (int gy = 0; gy < rows; ++gy) {
            for (int gx = 0; gx < cols; ++gx) {
                if (!filled[gy][gx])
                    continue;
                const bool left_empty = gx == 0 || !filled[gy][gx - 1];
                const bool right_empty = gx == cols - 1 || !filled[gy][gx + 1];
                const bool bottom_empty = gy == 0 || !filled[gy - 1][gx];
                const bool top_empty = gy == rows - 1 || !filled[gy + 1][gx];
                if (bottom_empty)
                    lesic_append_segment(contour, Vec2d(x_at(gx), y_at(gy)), Vec2d(x_at(gx + 1), y_at(gy)));
                if (right_empty)
                    lesic_append_segment(contour, Vec2d(x_at(gx + 1), y_at(gy)), Vec2d(x_at(gx + 1), y_at(gy + 1)));
                if (top_empty)
                    lesic_append_segment(contour, Vec2d(x_at(gx + 1), y_at(gy + 1)), Vec2d(x_at(gx), y_at(gy + 1)));
                if (left_empty)
                    lesic_append_segment(contour, Vec2d(x_at(gx), y_at(gy + 1)), Vec2d(x_at(gx), y_at(gy)));
            }
        }

        const std::vector<std::vector<Vec2d>> loops = lesic_extract_all_loops(contour);
        if (radius_idx == 1) {
            glyph.outer_loop = loops.empty() ? std::vector<Vec2d>() : loops.front();
            glyph.inner_loops = lesic_should_bridge_inner_loops(glyph.ch) ? lesic_inner_loops_for_outer(glyph.outer_loop, loops) : std::vector<std::vector<Vec2d>>();
            glyph.outline_loops = loops;
        }
        for (const std::vector<Vec2d> &loop : loops) {
            for (size_t i = 0; i + 1 < loop.size(); ++i)
                lesic_append_segment(glyph.segments, loop[i], loop[i + 1], outline_width);
        }
        if (radius_idx == 1) {
            std::vector<LesicAnnotationSegment> bridges = lesic_build_inner_loop_bridges(glyph.outer_loop, glyph.inner_loops);
            for (LesicAnnotationSegment &segment : bridges)
                segment.width = outline_width;
            glyph.segments.insert(glyph.segments.end(), bridges.begin(), bridges.end());
            bridges = lesic_build_forced_loop_bridge(glyph.ch, loops);
            for (LesicAnnotationSegment &segment : bridges)
                segment.width = outline_width;
            glyph.segments.insert(glyph.segments.end(), bridges.begin(), bridges.end());
        }
    }

    return glyph;
}

static std::vector<LesicAnnotationSegment> lesic_loop_to_segments(
    const std::vector<Vec2d> &loop,
    double width = LESIC_LABEL_STROKE_WIDTH)
{
    std::vector<LesicAnnotationSegment> out;
    for (size_t i = 0; i + 1 < loop.size(); ++i)
        lesic_append_segment(out, loop[i], loop[i + 1], width);
    return out;
}

static std::string lesic_format_label_value(double value)
{
    if (std::abs(value - std::round(value)) < 1e-9)
        return std::to_string(static_cast<int>(std::lround(value)));

    std::ostringstream ss;
    ss << std::fixed << std::setprecision(4) << value;
    std::string out = ss.str();
    while (!out.empty() && out.back() == '0')
        out.pop_back();
    if (!out.empty() && out.back() == '.')
        out.pop_back();
    return out;
}

static std::vector<double> lesic_ring_label_values(double mvs_min, double mvs_max)
{
    std::vector<double> values { mvs_min };
    const int start_multiple = static_cast<int>(std::ceil(mvs_min / 5.0) * 5.0);
    for (int value = start_multiple; value <= static_cast<int>(std::floor(mvs_max + 1e-9)); value += 5) {
        if (std::abs(value - mvs_min) < 1e-9)
            continue;
        if (std::abs(value - mvs_max) < 1e-9)
            continue;
        values.push_back(static_cast<double>(value));
    }
    if (values.empty() || std::abs(values.back() - mvs_max) > 1e-9)
        values.push_back(mvs_max);
    return values;
}

static std::vector<LesicAnnotationSegment> lesic_single_line_label_segments(
    const std::string &text,
    double char_h,
    double line_width = 0.48,
    double advance_scale = 1.0)
{
    std::vector<LesicAnnotationSegment> out;
    if (text.empty())
        return out;

    const double cell = char_h / 7.0;
    const double advance_units = lesic_label_advance_units(cell, LESIC_LABEL_X_SCALE, line_width) * std::max(0.1, advance_scale);
    const double width_units = std::max(0.0, text.size() * advance_units - (advance_units - 1.0));
    const double x_left = -width_units * cell * LESIC_LABEL_X_SCALE * 0.5;
    const double y0 = -char_h * 0.5;

    for (size_t char_idx = 0; char_idx < text.size(); ++char_idx) {
        const double x0 = x_left + char_idx * advance_units * cell * LESIC_LABEL_X_SCALE;
        const LesicGlyphBuild glyph = lesic_build_glyph_geometry(text[char_idx], x0, y0, cell, LESIC_LABEL_X_SCALE, line_width);
        if (glyph.outline_loops.size() > 1 && !glyph.outer_loop.empty()) {
            std::vector<LesicAnnotationSegment> outer = lesic_loop_to_segments(glyph.outer_loop);
            out.insert(out.end(), outer.begin(), outer.end());
        } else {
            out.insert(out.end(), glyph.segments.begin(), glyph.segments.end());
        }
    }
    return out;
}

static std::vector<Vec2d> lesic_segment_points(const std::vector<LesicAnnotationSegment> &segments)
{
    std::vector<Vec2d> points;
    points.reserve(segments.size() * 2);
    for (const LesicAnnotationSegment &segment : segments) {
        points.push_back(segment.a);
        points.push_back(segment.b);
    }
    std::sort(points.begin(), points.end(), [](const Vec2d &lhs, const Vec2d &rhs) {
        if (std::abs(lhs.x() - rhs.x()) > 1e-6)
            return lhs.x() < rhs.x();
        return lhs.y() < rhs.y();
    });
    points.erase(std::unique(points.begin(), points.end(), [](const Vec2d &lhs, const Vec2d &rhs) {
        return (lhs - rhs).norm() < 1e-5;
    }), points.end());
    return points;
}

static double lesic_cross(const Vec2d &a, const Vec2d &b, const Vec2d &c)
{
    return (b.x() - a.x()) * (c.y() - a.y()) - (b.y() - a.y()) * (c.x() - a.x());
}

static std::vector<Vec2d> lesic_convex_hull(std::vector<Vec2d> points)
{
    if (points.size() <= 1)
        return points;
    std::sort(points.begin(), points.end(), [](const Vec2d &lhs, const Vec2d &rhs) {
        if (std::abs(lhs.x() - rhs.x()) > 1e-6)
            return lhs.x() < rhs.x();
        return lhs.y() < rhs.y();
    });
    points.erase(std::unique(points.begin(), points.end(), [](const Vec2d &lhs, const Vec2d &rhs) {
        return (lhs - rhs).norm() < 1e-5;
    }), points.end());

    std::vector<Vec2d> lower;
    for (const Vec2d &point : points) {
        while (lower.size() >= 2 && lesic_cross(lower[lower.size() - 2], lower[lower.size() - 1], point) <= 0.0)
            lower.pop_back();
        lower.push_back(point);
    }
    std::vector<Vec2d> upper;
    for (auto it = points.rbegin(); it != points.rend(); ++it) {
        while (upper.size() >= 2 && lesic_cross(upper[upper.size() - 2], upper[upper.size() - 1], *it) <= 0.0)
            upper.pop_back();
        upper.push_back(*it);
    }
    lower.pop_back();
    upper.pop_back();
    lower.insert(lower.end(), upper.begin(), upper.end());
    return lower;
}

static void lesic_append_loop(
    std::vector<LesicAnnotationSegment> &segments,
    const std::vector<Vec2d> &loop,
    int repetitions,
    double width = LESIC_LABEL_STROKE_WIDTH)
{
    if (loop.size() < 3)
        return;
    for (int repetition = 0; repetition < repetitions; ++repetition) {
        for (size_t i = 0; i < loop.size(); ++i)
            lesic_append_segment(segments, loop[i], loop[(i + 1) % loop.size()], width);
    }
}

static bool lesic_pick_punctuation_rail_anchor(const LesicGlyphBuild &glyph, bool top_side, Vec2d &anchor)
{
    std::vector<std::vector<Vec2d>> loops = glyph.outline_loops;
    if (loops.empty() && !glyph.outer_loop.empty())
        loops.push_back(glyph.outer_loop);
    if (loops.empty())
        return false;

    const std::vector<Vec2d> *selected = &loops.front();
    for (const std::vector<Vec2d> &loop : loops) {
        const LesicLoopBounds selected_bounds = lesic_loop_bounds(*selected);
        const LesicLoopBounds loop_bounds = lesic_loop_bounds(loop);
        if ((top_side && loop_bounds.max_y > selected_bounds.max_y) ||
            (!top_side && loop_bounds.min_y < selected_bounds.min_y))
            selected = &loop;
    }

    anchor = lesic_pick_vertical_loop_point(*selected, lesic_loop_center(*selected).x(), top_side);
    return true;
}

static Vec2d lesic_offset_point_toward_rail(const Vec2d &start, double rail_y, double clearance)
{
    const double dy = rail_y - start.y();
    const double distance = std::abs(dy);
    if (distance <= 1e-9)
        return start;
    if (distance <= clearance)
        return Vec2d(start.x(), rail_y);
    return Vec2d(start.x(), start.y() + (dy < 0.0 ? -clearance : clearance));
}

static std::vector<LesicAnnotationSegment> lesic_build_interline_rails(
    const std::vector<std::vector<LesicGlyphBuild>> &lines_glyphs,
    double cell,
    double connector_clearance)
{
    std::vector<LesicAnnotationSegment> out;
    const auto is_punctuation = [](char ch) { return ch == ':' || ch == '.' || ch == '~'; };
    const auto append_points = [](std::vector<Vec2d> &points, const std::vector<LesicGlyphBuild> &glyphs) {
        for (const LesicGlyphBuild &glyph : glyphs)
            points.insert(points.end(), glyph.outer_loop.begin(), glyph.outer_loop.end());
    };
    const auto filtered = [](const std::vector<LesicGlyphBuild> &glyphs) {
        std::vector<LesicGlyphBuild> out;
        for (const LesicGlyphBuild &glyph : glyphs)
            if (!glyph.outer_loop.empty())
                out.push_back(glyph);
        return out;
    };

    for (size_t i = 0; i + 1 < lines_glyphs.size(); ++i) {
        const std::vector<LesicGlyphBuild> upper = filtered(lines_glyphs[i]);
        const std::vector<LesicGlyphBuild> lower = filtered(lines_glyphs[i + 1]);
        if (upper.empty() || lower.empty())
            continue;

        std::vector<Vec2d> upper_points;
        std::vector<Vec2d> lower_points;
        append_points(upper_points, upper);
        append_points(lower_points, lower);
        const LesicLoopBounds upper_bounds = lesic_loop_bounds(upper_points);
        const LesicLoopBounds lower_bounds = lesic_loop_bounds(lower_points);
        const double eps = std::max(0.02, cell * 0.03);
        const double upper_rail_y = upper_bounds.min_y - eps;
        const double lower_rail_y = lower_bounds.max_y + eps;

        lesic_append_segment(out, Vec2d(upper_bounds.min_x, upper_rail_y), Vec2d(upper_bounds.max_x, upper_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);
        lesic_append_segment(out, Vec2d(lower_bounds.min_x, lower_rail_y), Vec2d(lower_bounds.max_x, lower_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);

        for (const LesicGlyphBuild &glyph : lower) {
            if (!is_punctuation(glyph.ch))
                continue;
            Vec2d start;
            if (lesic_pick_punctuation_rail_anchor(glyph, true, start))
                lesic_append_segment(out, lesic_offset_point_toward_rail(start, lower_rail_y, connector_clearance), Vec2d(start.x(), lower_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);
        }
        for (const LesicGlyphBuild &glyph : upper) {
            if (!is_punctuation(glyph.ch))
                continue;
            Vec2d start;
            if (lesic_pick_punctuation_rail_anchor(glyph, false, start))
                lesic_append_segment(out, lesic_offset_point_toward_rail(start, upper_rail_y, connector_clearance), Vec2d(start.x(), upper_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);
        }
    }

    const std::vector<LesicGlyphBuild> last_line = lines_glyphs.empty() ? std::vector<LesicGlyphBuild>() : filtered(lines_glyphs.back());
    if (!last_line.empty()) {
        std::vector<Vec2d> last_points;
        append_points(last_points, last_line);
        const LesicLoopBounds last_bounds = lesic_loop_bounds(last_points);
        const double eps = std::max(0.02, cell * 0.03);
        const double bottom_rail_y = last_bounds.min_y - eps;
        lesic_append_segment(out, Vec2d(last_bounds.min_x, bottom_rail_y), Vec2d(last_bounds.max_x, bottom_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);

        for (const LesicGlyphBuild &glyph : last_line) {
            if (!is_punctuation(glyph.ch))
                continue;
            Vec2d start;
            if (lesic_pick_punctuation_rail_anchor(glyph, false, start))
                lesic_append_segment(out, lesic_offset_point_toward_rail(start, bottom_rail_y, connector_clearance), Vec2d(start.x(), bottom_rail_y), LESIC_LABEL_CONNECTOR_WIDTH);
        }
    }

    return out;
}

static double lesic_line_width_units(const std::string &text, double advance_units)
{
    if (text.empty())
        return 0.0;
    return std::max(0.0, text.size() * advance_units - (advance_units - 1.0));
}

static std::string lesic_clean_label_text(std::string text)
{
    for (char &ch : text) {
        const char upper = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        if (!lesic_font_strokes(upper).empty() || ch == ' ')
            ch = upper;
        else
            ch = ' ';
    }
    return text;
}

static std::vector<std::string> lesic_label_lines(
    const Calib_Params &params,
    const LabelConfig &config)
{
    std::string printer_name = config.printer_model.empty() ? "PRINTER" : config.printer_model;
    std::string filament_type = config.filament_type.empty() ? "FILAMENT" : config.filament_type;
    std::string filament_vendor = config.filament_vendor;
    if (filament_vendor == "(Undefined)")
        filament_vendor.clear();
    const std::string filament_label = filament_vendor.empty() ? filament_type : filament_vendor + "/" + filament_type;
    const double nozzle = config.nozzle_diameter_0;
    const double layer_height = params.lesic_layer_height > 0.0 ? params.lesic_layer_height : config.layer_height;

    return {
        lesic_clean_label_text(printer_name + "/" + filament_label + "/ND " + lesic_format_label_value(nozzle) + "MM"),
        lesic_clean_label_text("START:" + lesic_format_label_value(params.start) + "~C/DEC:-" +
                               lesic_format_label_value(std::abs(params.step)) + "~C*" +
                               std::to_string(std::max(1, params.lesic_layers_per_temp)) + "*" +
                               lesic_format_label_value(layer_height) + "MM"),
        lesic_clean_label_text("MAX MVS:" + lesic_format_label_value(params.mvs_end) + "MM#/S")
    };
}

static double lesic_bottom_label_height(const Calib_Params &params, const std::vector<std::string> &lines)
{
    const double diameter = std::max(1.0, params.lesic_circle_diameter);
    constexpr double label_margin = 6.0;
    const double advance_units = lesic_label_advance_units(1.0, LESIC_LABEL_X_SCALE, params.lesic_line_width > 0.0 ? params.lesic_line_width : 0.48);
    double max_units = 1.0;
    for (const std::string &line : lines)
        max_units = std::max(max_units, lesic_line_width_units(line, advance_units));
    const double max_width = std::max(1.0, diameter - 2.0 * label_margin);
    const double h_by_width = (max_width * 7.0) / (max_units * LESIC_LABEL_X_SCALE);
    const double h_by_height = (diameter * 0.38) / (lines.size() + std::max<size_t>(0, lines.size() - 1) * 0.65);
    return std::min(
        std::max(1.0, std::min(h_by_width, h_by_height)),
        lesic_is_small_label_plate(params) ? 4.0 : 10.0);
}

static std::vector<LesicAnnotationSegment> lesic_bottom_label_segments(
    const Calib_Params &params,
    const LabelConfig &config)
{
    std::vector<LesicAnnotationSegment> out;
    if (params.lesic_circle_diameter <= 0.0)
        return out;

    const std::vector<std::string> lines = lesic_label_lines(params, config);
    const double char_h = lesic_bottom_label_height(params, lines);
    const double line_width = params.lesic_line_width > 0.0 ? params.lesic_line_width : 0.48;
    const double bottom_stroke_width = lesic_bottom_label_stroke_width(params);
    const double cell = char_h / 7.0;
    const double line_gap = char_h * 0.65;
    const Vec2d center(params.lesic_center_x, params.lesic_center_y);
    const double advance_units = lesic_label_advance_units(cell, LESIC_LABEL_X_SCALE, line_width);
    std::vector<double> widths;
    widths.reserve(lines.size());
    for (const std::string &line : lines)
        widths.push_back(lesic_line_width_units(line, advance_units) * cell * LESIC_LABEL_X_SCALE);

    const double block_h = lines.size() * char_h + std::max<size_t>(0, lines.size() - 1) * line_gap;
    const double top_y = center.y() + block_h * 0.5 - char_h;
    std::vector<std::vector<LesicGlyphBuild>> lines_glyphs;
    lines_glyphs.reserve(lines.size());

    for (size_t line_idx = 0; line_idx < lines.size(); ++line_idx) {
        const double y0 = top_y - line_idx * (char_h + line_gap);
        const double x_left = center.x() - widths[line_idx] * 0.5;
        std::vector<LesicGlyphBuild> glyphs;
        glyphs.reserve(lines[line_idx].size());
        for (size_t char_idx = 0; char_idx < lines[line_idx].size(); ++char_idx) {
            const double x0 = x_left + char_idx * advance_units * cell * LESIC_LABEL_X_SCALE;
            LesicGlyphBuild glyph = lesic_build_glyph_geometry(lines[line_idx][char_idx], x0, y0, cell, LESIC_LABEL_X_SCALE, line_width, bottom_stroke_width);
            out.insert(out.end(), glyph.segments.begin(), glyph.segments.end());
            glyphs.push_back(std::move(glyph));
        }
        lines_glyphs.push_back(std::move(glyphs));
    }

    const std::vector<LesicAnnotationSegment> rails = lesic_build_interline_rails(lines_glyphs, cell, std::max(0.0, LESIC_LABEL_CONNECTOR_WIDTH * 0.5));
    out.insert(out.end(), rails.begin(), rails.end());

    const std::vector<Vec2d> hull = lesic_convex_hull(lesic_segment_points(out));
    lesic_append_loop(out, hull, 2, bottom_stroke_width);

    return out;
}

static std::vector<LesicAnnotationSegment> lesic_inner_brim_segments(
    const Calib_Params &params,
    const std::vector<LesicAnnotationSegment> &obstacles)
{
    std::vector<LesicAnnotationSegment> out;
    if (params.lesic_circle_diameter <= 0.0)
        return out;

    const Vec2d center(params.lesic_center_x, params.lesic_center_y);
    const double radius = params.lesic_circle_diameter * 0.5;
    const double line_width = params.lesic_line_width > 0.0 ? params.lesic_line_width : 0.48;
    const double sign = LESIC_CLOCKWISE ? -1.0 : 1.0;

    for (int line_idx = 0; line_idx < LESIC_INNER_BRIM_LINES; ++line_idx) {
        const double brim_radius = radius - line_width * (line_idx + 1);
        if (brim_radius <= line_width * 0.75)
            continue;

        for (int segment_idx = 0; segment_idx < LESIC_INNER_BRIM_ARC_SEGMENTS; ++segment_idx) {
            const double a0 = LESIC_ZERO_ANGLE_DEG + sign * 360.0 * segment_idx / LESIC_INNER_BRIM_ARC_SEGMENTS;
            const double a1 = LESIC_ZERO_ANGLE_DEG + sign * 360.0 * (segment_idx + 1) / LESIC_INNER_BRIM_ARC_SEGMENTS;
            lesic_append_segment(
                out,
                lesic_point_on_circle(center, brim_radius, a0),
                lesic_point_on_circle(center, brim_radius, a1),
                line_width);
        }
    }

    const double clearance = line_width * 0.5 +
        std::max(LESIC_LABEL_STROKE_WIDTH, LESIC_LABEL_CONNECTOR_WIDTH) * 0.5 +
        LESIC_LABEL_MASK_EXTRA_CLEARANCE;
    return lesic_filter_segments_by_obstacles(out, obstacles, clearance);
}

static std::vector<LesicAnnotationSegment> lesic_ring_annotation_segments(const Calib_Params &params)
{
    std::vector<LesicAnnotationSegment> out;
    if (params.lesic_circle_diameter <= 0.0 || params.mvs_end <= params.mvs_start)
        return out;

    const Vec2d center(params.lesic_center_x, params.lesic_center_y);
    const double radius = params.lesic_circle_diameter * 0.5;
    const double line_width = params.lesic_line_width > 0.0 ? params.lesic_line_width : 0.48;
    const double sign = LESIC_CLOCKWISE ? -1.0 : 1.0;
    const double safe_outer_offset = std::max(1.0, line_width * 2.0);

    const int start = static_cast<int>(std::ceil(params.mvs_start));
    const int end = static_cast<int>(std::floor(params.mvs_end));
    for (int value = start; value <= end; ++value) {
        const double t = (value - params.mvs_start) / (params.mvs_end - params.mvs_start);
        if (t <= 1e-9 || t >= 1.0 - 1e-9 || value % 5 == 0)
            continue;

        const double angle = LESIC_ZERO_ANGLE_DEG + sign * 360.0 * t;
        const double outer_radius = radius - safe_outer_offset;
        const double inner_radius = outer_radius - 3.4;
        const double radial_depth = std::max(2.6, outer_radius - inner_radius);
        const double marker_radius = (outer_radius + inner_radius) * 0.5;
        const Vec2d anchor = lesic_point_on_circle(center, marker_radius, angle);
        const std::vector<LesicAnnotationSegment> marker = lesic_outlined_rect_segments(1.55 * 0.75, radial_depth, 0.25);
        const std::vector<LesicAnnotationSegment> transformed = lesic_transform_segments(marker, angle, anchor);
        out.insert(out.end(), transformed.begin(), transformed.end());
    }

    const double char_h = std::clamp(params.lesic_circle_diameter * 0.025, 2.5, 5.0);
    const double text_radius = radius - char_h * 0.5 - safe_outer_offset;
    for (double value : lesic_ring_label_values(params.mvs_start, params.mvs_end)) {
        const double t = (value - params.mvs_start) / (params.mvs_end - params.mvs_start);
        const double angle = LESIC_ZERO_ANGLE_DEG + sign * 360.0 * t;
        const Vec2d anchor = lesic_point_on_circle(center, text_radius, angle);
        const double tangent_deg = angle + (LESIC_CLOCKWISE ? -90.0 : 90.0);
        const std::vector<LesicAnnotationSegment> text_segments =
            lesic_single_line_label_segments(lesic_format_label_value(value), char_h, line_width, 1.2);
        const std::vector<LesicAnnotationSegment> transformed = lesic_transform_segments(text_segments, tangent_deg, anchor);
        out.insert(out.end(), transformed.begin(), transformed.end());
    }

    return out;
}

} // namespace lesic
