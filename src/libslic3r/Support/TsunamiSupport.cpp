#include "TsunamiSupport.hpp"

#include "ClipperUtils.hpp"
#include "Fill/FillBase.hpp"
#include "Layer.hpp"
#include "Print.hpp"
#include "Surface.hpp"
#include "SupportCommon.hpp"
#include "SupportMaterial.hpp"
#include "SupportParameters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <set>
#include <tuple>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace Tsunami {

namespace {

constexpr double PI = 3.14159265358979323846;


Point from_mm(const Vec2d &point)
{
    return Point(scale_(point.x()), scale_(point.y()));
}

Vec2d to_mm(const Point &point)
{
    return Vec2d(unscale<double>(point.x()), unscale<double>(point.y()));
}

Point rib_endpoint(const PhysicalRib &rib, double sign)
{
    return from_mm(to_mm(rib.origin) + sign * 0.5 * rib.length * rib.direction);
}

PathSegment make_rib_segment(const PhysicalRib &rib, bool forward)
{
    PathSegment segment;
    segment.kind = SegmentKind::StraightRib;
    segment.polyline.points = forward ? Points { rib_endpoint(rib, -1.), rib_endpoint(rib, 1.) }
                                      : Points { rib_endpoint(rib, 1.), rib_endpoint(rib, -1.) };
    return segment;
}

PathSegment make_turn_segment(const VirtualRibField &field, const PhysicalRib &rib, double fraction, bool upper)
{
    PathSegment segment;
    segment.kind = SegmentKind::Turn;

    const double clamped_fraction = std::clamp(fraction, 0., 1.);
    const Vec2d start = to_mm(rib.origin) + (upper ? 0.5 : -0.5) * rib.length * field.rib_direction;
    const double radius = 0.5 * field.spacing;
    const Vec2d center = start + radius * field.guide_direction;
    const Vec2d outward = (upper ? 1. : -1.) * field.rib_direction;
    const int steps = std::max(1, int(std::ceil(8. * clamped_fraction)));
    const double end_angle = std::acos(2. * clamped_fraction - 1.);
    segment.polyline.points.reserve(size_t(steps) + 1);
    for (int step = 0; step <= steps; ++step) {
        const double t = clamped_fraction * double(step) / double(steps);
        const double angle = PI + (end_angle - PI) * (clamped_fraction > 0. ? t / clamped_fraction : 0.);
        const Vec2d point = center + std::cos(angle) * radius * field.guide_direction
                                   + std::sin(angle) * radius * outward;
        segment.polyline.points.emplace_back(from_mm(point));
    }
    return segment;
}

void append_segment(Polyline &path, const PathSegment &segment)
{
    if (segment.polyline.points.empty())
        return;
    size_t begin = !path.points.empty() && path.points.back() == segment.polyline.points.front() ? 1 : 0;
    path.points.insert(path.points.end(), segment.polyline.points.begin() + begin, segment.polyline.points.end());
}

struct CompletedTurnGeometry {
    Point start;
    Point finish;
    Point apex;
    size_t apex_index { 0 };
    Vec2d outward_direction { 1., 0. };
};

std::optional<CompletedTurnGeometry> analyze_completed_turn(const PathSegment &turn)
{
    if (turn.kind != SegmentKind::Turn || turn.polyline.points.size() < 3)
        return std::nullopt;

    const Vec2d start = to_mm(turn.polyline.points.front());
    const Vec2d finish = to_mm(turn.polyline.points.back());
    const Vec2d chord = finish - start;
    const double chord_length = chord.norm();
    if (chord_length <= 1e-9)
        return std::nullopt;

    size_t apex_index = 0;
    double apex_signed_distance = 0.;
    for (size_t index = 1; index + 1 < turn.polyline.points.size(); ++index) {
        const Vec2d relative = to_mm(turn.polyline.points[index]) - start;
        const double signed_distance =
            (chord.x() * relative.y() - chord.y() * relative.x()) / chord_length;
        if (std::abs(signed_distance) > std::abs(apex_signed_distance) + 1e-9) {
            apex_signed_distance = signed_distance;
            apex_index = index;
        }
    }
    if (apex_index == 0 || std::abs(apex_signed_distance) <= 1e-6)
        return std::nullopt;

    CompletedTurnGeometry result;
    result.start = turn.polyline.points.front();
    result.finish = turn.polyline.points.back();
    result.apex = turn.polyline.points[apex_index];
    result.apex_index = apex_index;
    result.outward_direction = to_mm(result.apex) - 0.5 * (start + finish);
    if (result.outward_direction.norm() <= 1e-9)
        return std::nullopt;
    result.outward_direction.normalize();
    return result;
}

ExPolygons extrusion_footprint(const Polyline &path, double extrusion_width)
{
    if (path.points.size() < 2 || extrusion_width <= 0.)
        return {};
    const ClipperLib::EndType end_type = path.points.front() == path.points.back()
        ? ClipperLib::etClosedLine : ClipperLib::etOpenRound;
    return union_ex(offset(
        path, float(scale_(0.5 * extrusion_width)), ClipperLib::jtRound,
        DefaultLineMiterLimit, end_type));
}

double supported_area_ratio(const ExPolygons &current, const ExPolygons &lower)
{
    const double current_area = std::abs(area(current));
    if (current_area <= 0.)
        return 0.;
    return std::clamp(std::abs(area(intersection_ex(current, lower))) / current_area, 0., 1.);
}

struct ContourLocation {
    double parameter { 0. };
    double squared_distance { std::numeric_limits<double>::max() };
};

double contour_length_mm(const Polygon &contour)
{
    double length = 0.;
    for (size_t index = 0; index < contour.points.size(); ++index)
        length += (to_mm(contour.points[(index + 1) % contour.points.size()]) - to_mm(contour.points[index])).norm();
    return length;
}

ContourLocation locate_on_contour(const Polygon &contour, const Point &point)
{
    ContourLocation result;
    double accumulated = 0.;
    const Vec2d query = to_mm(point);
    for (size_t index = 0; index < contour.points.size(); ++index) {
        const Vec2d from = to_mm(contour.points[index]);
        const Vec2d to = to_mm(contour.points[(index + 1) % contour.points.size()]);
        const Vec2d edge = to - from;
        const double edge_length = edge.norm();
        if (edge_length <= 1e-9)
            continue;
        const double fraction = std::clamp((query - from).dot(edge) / edge.squaredNorm(), 0., 1.);
        const Vec2d projection = from + fraction * edge;
        const double squared_distance = (query - projection).squaredNorm();
        const double parameter = accumulated + fraction * edge_length;
        if (squared_distance < result.squared_distance - 1e-12 ||
            (std::abs(squared_distance - result.squared_distance) <= 1e-12 && parameter < result.parameter)) {
            result.parameter = parameter;
            result.squared_distance = squared_distance;
        }
        accumulated += edge_length;
    }
    return result;
}

std::pair<Vec2d, Vec2d> sample_contour(const Polygon &contour, double parameter)
{
    const double total_length = contour_length_mm(contour);
    if (contour.points.empty() || total_length <= 1e-9)
        return { Vec2d::Zero(), Vec2d(1., 0.) };
    parameter = std::fmod(parameter, total_length);
    if (parameter < 0.)
        parameter += total_length;
    for (size_t index = 0; index < contour.points.size(); ++index) {
        const Vec2d from = to_mm(contour.points[index]);
        const Vec2d to = to_mm(contour.points[(index + 1) % contour.points.size()]);
        const Vec2d edge = to - from;
        const double edge_length = edge.norm();
        if (edge_length <= 1e-9)
            continue;
        if (parameter <= edge_length || index + 1 == contour.points.size())
            return { from + std::clamp(parameter / edge_length, 0., 1.) * edge, edge / edge_length };
        parameter -= edge_length;
    }
    return { to_mm(contour.points.front()), Vec2d(1., 0.) };
}

// interior_counts_as_reached == false always measures to the region boundary,
// even when the query point is already inside. Callers that must still grow
// outward from an enclosing region need that boundary distance, not zero.
Point closest_region_point(const Point &point, const ExPolygons &regions, const Point &fallback,
                           bool interior_counts_as_reached = true)
{
    double best_squared_distance = std::numeric_limits<double>::max();
    Point best_point = fallback;
    for (const ExPolygon &region : regions) {
        if (interior_counts_as_reached && region.contains(point, false))
            return point;
        const auto update_distance = [&point, &best_squared_distance, &best_point](const Polygon &boundary) {
            if (boundary.points.empty())
                return;
            const ContourLocation location = locate_on_contour(boundary, point);
            if (!std::isfinite(location.squared_distance))
                return;
            const Point candidate = from_mm(sample_contour(boundary, location.parameter).first);
            if (location.squared_distance < best_squared_distance - 1e-12 ||
                (std::abs(location.squared_distance - best_squared_distance) <= 1e-12 &&
                 std::tie(candidate.x(), candidate.y()) < std::tie(best_point.x(), best_point.y()))) {
                best_squared_distance = location.squared_distance;
                best_point = candidate;
            }
        };
        update_distance(region.contour);
        for (const Polygon &hole : region.holes)
            update_distance(hole);
    }
    return best_point;
}

double footprint_area_mm2(const Polygons &polygons)
{
    return std::abs(area(union_ex(polygons))) * SCALING_FACTOR * SCALING_FACTOR;
}

// Slic3r's offset() reuses its miter-limit argument as Clipper's ArcTolerance
// whenever the join type is round. DefaultLineMiterLimit is 0, which makes
// Clipper substitute its own default of 0.25 -- a chord tolerance meant for
// unscaled coordinates. Against Slic3r's 1e-6 mm fixed point that asks for
// roughly two thousand segments per half turn, so a two-point rib footprint came
// back with ~2500 vertices and root selection paid for all of them in every
// offset, difference, intersection and union it ran.
//
// Only root-candidate screening uses this. The footprints that feed coverage and
// collision decisions deliberately keep the original tolerance: a coarser arc is
// inscribed, so it shrinks the footprint slightly, and measurement showed that
// shrinkage is enough to drop the snug overhang fixture below its coverage gate.
constexpr double root_screening_arc_tolerance = 1000.;  // scale_(0.001)

bool footprint_is_valid(const Polyline &path, double extrusion_width, const Polygon &bed_region,
                        const ExPolygons &blocked_region, double *area_mm2 = nullptr)
{
    if (path.points.size() < 2)
        return false;
    const ClipperLib::EndType end_type = path.points.front() == path.points.back()
        ? ClipperLib::etClosedLine : ClipperLib::etOpenRound;
    const Polygons footprint = offset(
        path, float(scale_(0.5 * extrusion_width)), ClipperLib::jtRound,
        root_screening_arc_tolerance, end_type);
    if (footprint.empty() || !diff_ex(footprint, Polygons { bed_region }).empty() ||
        !intersection_ex(footprint, blocked_region).empty())
        return false;
    if (area_mm2 != nullptr)
        *area_mm2 = footprint_area_mm2(footprint);
    return true;
}

double root_turn_radius(const RootSelectionInput &input)
{
    return std::max(0.5 * input.extrusion_width, 0.25 * input.rib_spacing);
}

void append_contour_connector(Polyline &connector, const Polygon &contour, double from_parameter,
                              double to_parameter, double offset_distance, double turn_radius)
{
    const double length = contour_length_mm(contour);
    while (to_parameter < from_parameter)
        to_parameter += length;
    const auto [from_inner, from_tangent] = sample_contour(contour, from_parameter);
    const auto [to_inner, to_tangent] = sample_contour(contour, to_parameter);
    const bool counter_clockwise = contour.is_counter_clockwise();
    const Vec2d from_outward = counter_clockwise ? Vec2d(from_tangent.y(), -from_tangent.x())
                                                 : Vec2d(-from_tangent.y(), from_tangent.x());
    const Vec2d to_outward = counter_clockwise ? Vec2d(to_tangent.y(), -to_tangent.x())
                                               : Vec2d(-to_tangent.y(), to_tangent.x());
    const Vec2d start = from_inner + offset_distance * from_outward;
    const Vec2d finish = to_inner + offset_distance * to_outward;
    const double bulge_sign = offset_distance > 0. ? 1. : -1.;
    const Vec2d control1 = start + bulge_sign * turn_radius * from_outward;
    const Vec2d control2 = finish + bulge_sign * turn_radius * to_outward;
    const int steps = std::max(24, int(std::ceil((to_parameter - from_parameter) / 0.1)));
    connector.points.reserve(size_t(steps) + 1);
    for (int step = 0; step <= steps; ++step) {
        const double t = double(step) / double(steps);
        const double inverse = 1. - t;
        connector.points.emplace_back(from_mm(
            inverse * inverse * inverse * start
            + 3. * inverse * inverse * t * control1
            + 3. * inverse * t * t * control2
            + t * t * t * finish));
    }
}

std::pair<double, double> target_contour_span(const RootSelectionInput &input, const Polygon &contour)
{
    const double length = contour_length_mm(contour);
    std::vector<double> parameters;
    for (const ExPolygon &region : input.target_region) {
        for (const Point &point : region.contour.points)
            parameters.emplace_back(locate_on_contour(contour, point).parameter);
    }
    if (parameters.empty())
        parameters.emplace_back(locate_on_contour(contour, input.target).parameter);
    std::sort(parameters.begin(), parameters.end());

    double start = parameters.front();
    double span = 0.;
    if (parameters.size() > 1) {
        double largest_gap = -1.;
        size_t gap_end = 0;
        for (size_t index = 0; index < parameters.size(); ++index) {
            const double next = index + 1 < parameters.size() ? parameters[index + 1] : parameters.front() + length;
            const double gap = next - parameters[index];
            if (gap > largest_gap) {
                largest_gap = gap;
                gap_end = (index + 1) % parameters.size();
            }
        }
        start = parameters[gap_end];
        span = length - largest_gap;
    }
    // The padding gives the arc a little margin past the target's angular extent.
    // When another root already follows this same contour there is no margin to
    // give: measured on the hollow gear, whose two targets are opposite halves of
    // one annulus, both arcs came out at 192.78 degrees and overlapped by 12.78
    // degrees at each seam -- one rib spacing of padding from each arc meeting
    // there. Dropping it leaves the two arcs complementary at roughly 180 degrees
    // instead of relying on the reservation below to cut the later one back, which
    // cost that trunk far more than the padding (25 ribs against 31, and its
    // uncovered demand went from 54.92 to 91.01 mm2).
    const double padding = input.shares_contour ? 0. : input.rib_spacing;
    span = std::clamp(std::max(span + 2. * padding, 2. * input.rib_spacing),
                      2. * input.rib_spacing, std::max(2. * input.rib_spacing, length - input.rib_spacing));
    return { start - padding, span };
}

struct ContourRootSample {
    double parameter { 0. };
    Vec2d inner { 0., 0. };
    Vec2d outer { 0., 0. };
    Vec2d tangent { 1., 0. };
    PhysicalRib rib;
    bool printable { false };
};

PathSegment make_contour_rib_segment(const ContourRootSample &sample, bool forward)
{
    PathSegment segment;
    segment.kind = SegmentKind::StraightRib;
    segment.polyline.points = forward ? Points { from_mm(sample.inner), from_mm(sample.outer) }
                                      : Points { from_mm(sample.outer), from_mm(sample.inner) };
    return segment;
}

PathSegment make_contour_turn_segment(const Polygon &contour, const ContourRootSample &from,
                                      const ContourRootSample &to, double offset_distance, double turn_radius)
{
    PathSegment segment;
    segment.kind = SegmentKind::Turn;
    append_contour_connector(
        segment.polyline, contour, from.parameter, to.parameter, offset_distance, turn_radius);
    return segment;
}

std::optional<RootCandidate> build_contour_root_candidate(
    const RootSelectionInput &input, const Polygon &contour, const std::vector<ContourRootSample> &samples,
    size_t begin, size_t end, int phase, double root_depth, double target_arc_span)
{
    if (end <= begin)
        return std::nullopt;

    RootCandidate candidate;
    candidate.rib_length = root_depth;
    candidate.physical_ribs.reserve(end - begin + 1);
    candidate.segments.reserve(2 * (end - begin) + 1);
    for (size_t index = begin; index <= end; ++index) {
        const bool forward = ((index + size_t(phase)) % 2 == 0);
        PhysicalRib rib = samples[index].rib;
        rib.id = candidate.physical_ribs.size();
        candidate.physical_ribs.emplace_back(rib);
        PathSegment rib_segment = make_contour_rib_segment(samples[index], forward);
        append_segment(candidate.path, rib_segment);
        candidate.segments.emplace_back(std::move(rib_segment));
        if (index < end) {
            PathSegment turn = make_contour_turn_segment(
                contour, samples[index], samples[index + 1], forward ? root_depth : 0.,
                root_turn_radius(input));
            append_segment(candidate.path, turn);
            candidate.segments.emplace_back(std::move(turn));
        }
    }

    if (!footprint_is_valid(candidate.path, input.extrusion_width, input.bed_region,
                            input.blocked_region, &candidate.bed_contact_area))
        return std::nullopt;

    const bool last_rib_ends_outside = ((end + size_t(phase)) % 2 == 0);
    const size_t frontier_index = last_rib_ends_outside ? begin : end;
    if (last_rib_ends_outside) {
        candidate.path.reverse();
        std::reverse(candidate.segments.begin(), candidate.segments.end());
        for (PathSegment &segment : candidate.segments)
            segment.polyline.reverse();
    }
    candidate.frontier_point = from_mm(samples[frontier_index].inner);
    candidate.frontier_tangent = samples[frontier_index].tangent;
    candidate.frontier_rib_index = frontier_index - begin;
    candidate.position = samples[frontier_index].rib.origin;
    candidate.xy_distance = std::numeric_limits<double>::max();
    for (const PathSegment &segment : candidate.segments) {
        const std::optional<CompletedTurnGeometry> turn = analyze_completed_turn(segment);
        if (!turn)
            continue;
        const Point target_point = closest_region_point(
            turn->apex, input.target_region, input.target);
        const Vec2d delta = to_mm(target_point) - to_mm(turn->apex);
        if (delta.norm() > 1e-9 && delta.dot(turn->outward_direction) < -1e-9)
            continue;
        const double distance = delta.norm();
        if (distance < candidate.xy_distance - 1e-9 ||
            (std::abs(distance - candidate.xy_distance) <= 1e-9 &&
             std::tie(turn->apex.x(), turn->apex.y()) <
                 std::tie(candidate.approach_point.x(), candidate.approach_point.y()))) {
            candidate.xy_distance = distance;
            candidate.approach_point = turn->apex;
        }
    }
    if (!std::isfinite(candidate.xy_distance) ||
        candidate.xy_distance > input.maximum_xy_distance + 1e-9)
        return std::nullopt;
    candidate.target_coverage = std::clamp(
        (samples[end].parameter - samples[begin].parameter) / std::max(target_arc_span, 1e-9), 0., 1.);
    candidate.score = 1000. * candidate.target_coverage - 10. * candidate.xy_distance
                    + std::min(candidate.bed_contact_area, input.minimum_bed_contact_area)
                    - 0.1 * root_depth;
    return candidate;
}

std::vector<RootCandidate> make_contour_root_candidates(
    const RootSelectionInput &input, const Polygon &contour, double root_depth, double maximum_area)
{
    const auto [target_arc_start, target_arc_span] = target_contour_span(input, contour);
    const int rib_count = std::max(2, int(std::floor(target_arc_span / input.rib_spacing)) + 1);
    const double actual_spacing = target_arc_span / double(rib_count - 1);
    const bool counter_clockwise = contour.is_counter_clockwise();
    const double turn_radius = root_turn_radius(input);

    std::vector<ContourRootSample> samples(static_cast<size_t>(rib_count), ContourRootSample {});
    for (int index = 0; index < rib_count; ++index) {
        ContourRootSample &sample = samples[size_t(index)];
        sample.parameter = target_arc_start + double(index) * actual_spacing;
        std::tie(sample.inner, sample.tangent) = sample_contour(contour, sample.parameter);
        const Vec2d outward = counter_clockwise ? Vec2d(sample.tangent.y(), -sample.tangent.x())
                                                : Vec2d(-sample.tangent.y(), sample.tangent.x());
        sample.outer = sample.inner + root_depth * outward;
        sample.rib.id = size_t(index);
        sample.rib.virtual_rib_index = index;
        sample.rib.origin = from_mm(0.5 * (sample.inner + sample.outer));
        sample.rib.direction = outward;
        sample.rib.length = root_depth;
        sample.rib.birth_layer = 0;
        const PathSegment rib = make_contour_rib_segment(sample, true);
        sample.printable = footprint_is_valid(
            rib.polyline, input.extrusion_width, input.bed_region, input.blocked_region);
    }

    std::vector<RootCandidate> candidates;
    for (int phase = 0; phase < 2; ++phase) {
        std::vector<bool> connector_printable(samples.size() - 1, false);
        for (size_t index = 0; index + 1 < samples.size(); ++index) {
            const bool forward = ((index + size_t(phase)) % 2 == 0);
            const PathSegment connector = make_contour_turn_segment(
                contour, samples[index], samples[index + 1], forward ? root_depth : 0., turn_radius);
            connector_printable[index] = footprint_is_valid(
                connector.polyline, input.extrusion_width, input.bed_region, input.blocked_region);
        }

        for (size_t run_begin = 0; run_begin < samples.size();) {
            while (run_begin < samples.size() && !samples[run_begin].printable)
                ++run_begin;
            if (run_begin == samples.size())
                break;
            size_t run_end = run_begin;
            while (run_end + 1 < samples.size() && connector_printable[run_end] && samples[run_end + 1].printable)
                ++run_end;

            size_t candidate_begin = run_begin;
            size_t candidate_end = run_end;
            while (candidate_end > candidate_begin) {
                std::optional<RootCandidate> candidate = build_contour_root_candidate(
                    input, contour, samples, candidate_begin, candidate_end, phase,
                    root_depth, target_arc_span);
                if (candidate && candidate->bed_contact_area <= maximum_area + 1e-9) {
                    if (candidate->bed_contact_area + 1e-9 >= input.minimum_bed_contact_area)
                        candidates.emplace_back(std::move(*candidate));
                    break;
                }
                const double target_middle = target_arc_start + 0.5 * target_arc_span;
                const double begin_distance = std::abs(samples[candidate_begin].parameter - target_middle);
                const double end_distance = std::abs(samples[candidate_end].parameter - target_middle);
                if (end_distance >= begin_distance)
                    --candidate_end;
                else
                    ++candidate_begin;
            }
            run_begin = run_end + 1;
        }
    }
    return candidates;
}

struct DirectRibInterval {
    int index { 0 };
    double guide_position { 0. };
    double rib_begin { 0. };
    double rib_end { 0. };
};

std::optional<RootCandidate> build_direct_root_candidate(
    const RootSelectionInput &input, const ExPolygons &centerline_domain,
    const Vec2d &rib_direction, const Vec2d &guide_direction,
    const std::vector<DirectRibInterval> &intervals, size_t begin, size_t end,
    double rib_begin, double rib_end, double target_rib_position,
    double target_guide_position, double target_span_rib, double target_span_guide,
    double maximum_area)
{
    if (end < begin || rib_end <= rib_begin)
        return std::nullopt;

    const size_t rib_count = end - begin + 1;
    const double turn_length = double(rib_count - 1) * 0.5 * PI * input.rib_spacing;
    double rib_length = std::min(input.rib_length, rib_end - rib_begin);
    if (std::isfinite(maximum_area)) {
        const double available_line_area = maximum_area - turn_length * input.extrusion_width;
        if (available_line_area <= 0.)
            return std::nullopt;
        rib_length = std::min(rib_length,
            available_line_area / (double(rib_count) * input.extrusion_width));
    }
    const double minimum_rib_length = std::max(input.extrusion_width,
        0.5 * std::min(input.rib_spacing, input.rib_length));
    if (rib_length + 1e-9 < minimum_rib_length)
        return std::nullopt;

    const double center_min = rib_begin + 0.5 * rib_length;
    const double center_max = rib_end - 0.5 * rib_length;
    const double rib_center = std::clamp(target_rib_position, center_min, center_max);

    RootCandidate candidate;
    candidate.direct_projection = true;
    candidate.rib_length = rib_length;
    candidate.physical_ribs.reserve(rib_count);
    candidate.segments.reserve(2 * rib_count - 1);

    VirtualRibField field;
    field.guide_direction = guide_direction;
    field.rib_direction = rib_direction;
    field.spacing = input.rib_spacing;

    size_t closest_rib = 0;
    double closest_distance = std::numeric_limits<double>::max();
    for (size_t local_index = 0; local_index < rib_count; ++local_index) {
        const DirectRibInterval &interval = intervals[begin + local_index];
        PhysicalRib rib;
        rib.id = local_index;
        rib.virtual_rib_index = interval.index;
        rib.origin = from_mm(rib_center * rib_direction + interval.guide_position * guide_direction);
        rib.direction = rib_direction;
        rib.length = rib_length;
        rib.birth_layer = 0;
        candidate.physical_ribs.emplace_back(rib);
        const double distance = std::abs(interval.guide_position - target_guide_position);
        if (distance < closest_distance - 1e-9) {
            closest_distance = distance;
            closest_rib = local_index;
        }
    }

    for (size_t local_index = 0; local_index < rib_count; ++local_index) {
        const bool forward = local_index % 2 == 0;
        PathSegment rib_segment = make_rib_segment(candidate.physical_ribs[local_index], forward);
        append_segment(candidate.path, rib_segment);
        candidate.segments.emplace_back(std::move(rib_segment));
        if (local_index + 1 < rib_count) {
            PathSegment turn = make_turn_segment(field, candidate.physical_ribs[local_index], 1., forward);
            const PathSegment next_rib = make_rib_segment(candidate.physical_ribs[local_index + 1], !forward);
            turn.polyline.points.front() = candidate.segments.back().polyline.points.back();
            turn.polyline.points.back() = next_rib.polyline.points.front();
            append_segment(candidate.path, turn);
            candidate.segments.emplace_back(std::move(turn));
        }
    }

    if (!footprint_is_valid(candidate.path, input.extrusion_width, input.bed_region,
                            input.blocked_region, &candidate.bed_contact_area) ||
        candidate.bed_contact_area > maximum_area + 1e-9 ||
        candidate.bed_contact_area + 1e-9 < input.minimum_bed_contact_area)
        return std::nullopt;

    candidate.frontier_rib_index = closest_rib;
    candidate.position = candidate.physical_ribs[closest_rib].origin;
    candidate.approach_point = candidate.position;
    candidate.frontier_point = candidate.position;
    candidate.frontier_tangent = guide_direction;
    candidate.xy_distance = 0.;
    const double covered_rib_span = rib_length + input.extrusion_width;
    const double covered_guide_span = double(rib_count - 1) * input.rib_spacing + input.extrusion_width;
    candidate.target_coverage = std::clamp(
        covered_rib_span / std::max(target_span_rib, input.extrusion_width) *
        covered_guide_span / std::max(target_span_guide, input.extrusion_width), 0., 1.);
    candidate.score = 1000. * candidate.target_coverage
                    + std::min(candidate.bed_contact_area, input.minimum_bed_contact_area)
                    - 0.1 * rib_length;

    const Polygons footprint = offset(
        candidate.path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
    if (intersection_ex(footprint, centerline_domain).empty())
        return std::nullopt;
    return candidate;
}

std::vector<RootCandidate> make_direct_root_candidates_for_centerlines(
    const RootSelectionInput &input, double maximum_area,
    const ExPolygons &target_centerlines, const ExPolygons &safe_bed,
    double clearance)
{
    std::vector<RootCandidate> candidates;
    ExPolygons centerline_domain = intersection_ex(target_centerlines, safe_bed);
    if (centerline_domain.empty())
        return candidates;

    if (!input.blocked_region.empty()) {
        const ExPolygons forbidden = offset_ex(
            input.blocked_region, float(scale_(clearance)), ClipperLib::jtRound);
        centerline_domain = diff_ex(centerline_domain, forbidden);
        if (centerline_domain.empty())
            return candidates;
    }

    const Points domain_points = to_points(centerline_domain);
    if (domain_points.empty())
        return candidates;
    const Vec2d target = to_mm(input.target);

    std::vector<double> orientations;
    orientations.reserve(10);
    const auto add_orientation = [&](double angle) {
        angle = std::fmod(angle, PI);
        if (angle < 0.)
            angle += PI;
        if (std::none_of(orientations.begin(), orientations.end(), [angle](double existing) {
                return std::abs(std::sin(angle - existing)) < 1e-6;
            }))
            orientations.emplace_back(angle);
    };
    constexpr int fixed_orientation_count = 8;
    for (int index = 0; index < fixed_orientation_count; ++index)
        add_orientation(double(index) * PI / double(fixed_orientation_count));

    Vec2d mean = Vec2d::Zero();
    for (const Point &point : domain_points)
        mean += to_mm(point);
    mean /= double(domain_points.size());
    double covariance_xx = 0.;
    double covariance_xy = 0.;
    double covariance_yy = 0.;
    for (const Point &point : domain_points) {
        const Vec2d delta = to_mm(point) - mean;
        covariance_xx += delta.x() * delta.x();
        covariance_xy += delta.x() * delta.y();
        covariance_yy += delta.y() * delta.y();
    }
    if (covariance_xx + covariance_yy > 1e-12)
        add_orientation(0.5 * std::atan2(2. * covariance_xy, covariance_xx - covariance_yy));

    const Polygons domain_polygons = to_polygons(centerline_domain);
    double longest_edge_squared = 0.;
    Vec2d longest_edge(1., 0.);
    for (const Polygon &polygon : domain_polygons) {
        for (size_t index = 0; index < polygon.points.size(); ++index) {
            const Vec2d edge = to_mm(polygon.points[(index + 1) % polygon.points.size()])
                              - to_mm(polygon.points[index]);
            if (edge.squaredNorm() > longest_edge_squared + 1e-12) {
                longest_edge_squared = edge.squaredNorm();
                longest_edge = edge;
            }
        }
    }
    if (longest_edge_squared > 1e-12)
        add_orientation(std::atan2(longest_edge.y(), longest_edge.x()));

    for (double angle : orientations) {
        const Vec2d rib_direction(std::cos(angle), std::sin(angle));
        const Vec2d guide_direction(-rib_direction.y(), rib_direction.x());
        double minimum_rib = std::numeric_limits<double>::max();
        double maximum_rib = std::numeric_limits<double>::lowest();
        double minimum_guide = std::numeric_limits<double>::max();
        double maximum_guide = std::numeric_limits<double>::lowest();
        for (const Point &point : domain_points) {
            const Vec2d mm = to_mm(point);
            const double rib = mm.dot(rib_direction);
            const double guide = mm.dot(guide_direction);
            minimum_rib = std::min(minimum_rib, rib);
            maximum_rib = std::max(maximum_rib, rib);
            minimum_guide = std::min(minimum_guide, guide);
            maximum_guide = std::max(maximum_guide, guide);
        }
        const double target_rib = target.dot(rib_direction);
        const double target_guide = target.dot(guide_direction);
        const double extension = std::hypot(maximum_rib - minimum_rib,
                                            maximum_guide - minimum_guide) + input.rib_spacing;

        std::vector<double> phase_offsets;
        phase_offsets.reserve(2 + 3 * domain_polygons.size());
        const auto add_phase = [&](double guide_position) {
            double offset = std::fmod(guide_position - target_guide, input.rib_spacing);
            if (offset < 0.)
                offset += input.rib_spacing;
            if (offset >= input.rib_spacing - 1e-9)
                offset = 0.;
            if (std::none_of(phase_offsets.begin(), phase_offsets.end(), [&](double existing) {
                    const double difference = std::abs(offset - existing);
                    return std::min(difference, input.rib_spacing - difference) < 1e-6;
                }))
                phase_offsets.emplace_back(offset);
        };
        add_phase(target_guide);
        add_phase(target_guide + 0.5 * input.rib_spacing);
        for (const Polygon &polygon : domain_polygons) {
            if (polygon.points.empty())
                continue;
            double polygon_minimum = std::numeric_limits<double>::max();
            double polygon_maximum = std::numeric_limits<double>::lowest();
            for (const Point &point : polygon.points) {
                const double position = to_mm(point).dot(guide_direction);
                polygon_minimum = std::min(polygon_minimum, position);
                polygon_maximum = std::max(polygon_maximum, position);
            }
            const double span = polygon_maximum - polygon_minimum;
            if (span <= 1e-9)
                continue;
            const double boundary_inset = std::min(0.5 * input.extrusion_width, 0.25 * span);
            add_phase(polygon_minimum + boundary_inset);
            add_phase(0.5 * (polygon_minimum + polygon_maximum));
            add_phase(polygon_maximum - boundary_inset);
        }

        for (double phase_offset : phase_offsets) {
            const double phase_origin = target_guide + phase_offset;
            const int first_index = int(std::ceil((minimum_guide - phase_origin) / input.rib_spacing));
            const int last_index = int(std::floor((maximum_guide - phase_origin) / input.rib_spacing));
            std::vector<DirectRibInterval> intervals;
            intervals.reserve(size_t(std::max(0, last_index - first_index + 1)));
            for (int index = first_index; index <= last_index; ++index) {
                const double guide = phase_origin + double(index) * input.rib_spacing;
                Polyline line;
                line.points = {
                    from_mm((minimum_rib - extension) * rib_direction + guide * guide_direction),
                    from_mm((maximum_rib + extension) * rib_direction + guide * guide_direction)
                };
                const Polylines clipped = intersection_pl(Polylines { line }, centerline_domain);
                double best_distance = std::numeric_limits<double>::max();
                double best_begin = 0.;
                double best_end = 0.;
                for (const Polyline &piece : clipped) {
                    if (piece.points.size() < 2)
                        continue;
                    double piece_begin = std::numeric_limits<double>::max();
                    double piece_end = std::numeric_limits<double>::lowest();
                    for (const Point &point : piece.points) {
                        const double position = to_mm(point).dot(rib_direction);
                        piece_begin = std::min(piece_begin, position);
                        piece_end = std::max(piece_end, position);
                    }
                    const double distance = target_rib < piece_begin ? piece_begin - target_rib
                        : target_rib > piece_end ? target_rib - piece_end : 0.;
                    if (distance < best_distance - 1e-9 ||
                        (std::abs(distance - best_distance) <= 1e-9 &&
                         piece_end - piece_begin > best_end - best_begin + 1e-9)) {
                        best_distance = distance;
                        best_begin = piece_begin;
                        best_end = piece_end;
                    }
                }
                if (best_end > best_begin + 1e-9)
                    intervals.push_back({ index, guide, best_begin, best_end });
            }

            struct Run {
                size_t begin { 0 };
                size_t end { 0 };
                double rib_begin { 0. };
                double rib_end { 0. };
                double score { -1. };
            };
            Run best_run;
            const double minimum_length = std::max(input.extrusion_width,
                0.5 * std::min(input.rib_spacing, input.rib_length));
            for (size_t begin = 0; begin < intervals.size(); ++begin) {
                double common_begin = intervals[begin].rib_begin;
                double common_end = intervals[begin].rib_end;
                for (size_t end = begin; end < intervals.size(); ++end) {
                    if (end > begin && intervals[end].index != intervals[end - 1].index + 1)
                        break;
                    common_begin = std::max(common_begin, intervals[end].rib_begin);
                    common_end = std::min(common_end, intervals[end].rib_end);
                    if (common_end - common_begin + 1e-9 < minimum_length)
                        break;
                    const size_t count = end - begin + 1;
                    const double turn_area = double(count - 1) * 0.5 * PI * input.rib_spacing
                                           * input.extrusion_width;
                    const double area_limited_length = std::isfinite(maximum_area)
                        ? (maximum_area - turn_area) / (double(count) * input.extrusion_width)
                        : std::numeric_limits<double>::max();
                    const double length = std::min({ input.rib_length, common_end - common_begin,
                                                     area_limited_length });
                    if (length + 1e-9 < minimum_length)
                        continue;
                    const double coverage = length / std::max(maximum_rib - minimum_rib, input.extrusion_width)
                        * (double(count - 1) * input.rib_spacing + input.extrusion_width)
                        / std::max(maximum_guide - minimum_guide, input.extrusion_width);
                    const double centering = std::abs(0.5 * (intervals[begin].guide_position
                        + intervals[end].guide_position) - target_guide);
                    const double score = coverage - 1e-6 * centering;
                    if (score > best_run.score + 1e-12)
                        best_run = { begin, end, common_begin, common_end, score };
                }
            }
            if (best_run.score < 0.)
                continue;
            std::optional<RootCandidate> candidate = build_direct_root_candidate(
                input, centerline_domain, rib_direction, guide_direction, intervals,
                best_run.begin, best_run.end, best_run.rib_begin, best_run.rib_end,
                target_rib, target_guide, maximum_rib - minimum_rib,
                maximum_guide - minimum_guide, maximum_area);
            if (candidate) {
                candidates.emplace_back(std::move(*candidate));
                continue;
            }

            size_t best_single = intervals.size();
            double best_single_length = 0.;
            double best_single_distance = std::numeric_limits<double>::max();
            for (size_t index = 0; index < intervals.size(); ++index) {
                const double length = intervals[index].rib_end - intervals[index].rib_begin;
                const double distance = std::abs(intervals[index].guide_position - target_guide);
                if (length > best_single_length + 1e-9 ||
                    (std::abs(length - best_single_length) <= 1e-9 && distance < best_single_distance - 1e-9)) {
                    best_single = index;
                    best_single_length = length;
                    best_single_distance = distance;
                }
            }
            if (best_single != intervals.size()) {
                candidate = build_direct_root_candidate(
                    input, centerline_domain, rib_direction, guide_direction, intervals,
                    best_single, best_single, intervals[best_single].rib_begin,
                    intervals[best_single].rib_end, target_rib, target_guide,
                    maximum_rib - minimum_rib, maximum_guide - minimum_guide, maximum_area);
                if (candidate)
                    candidates.emplace_back(std::move(*candidate));
            }
        }
    }
    return candidates;
}

std::vector<RootCandidate> make_direct_root_candidates(
    const RootSelectionInput &input, double maximum_area)
{
    if (input.target_region.empty())
        return {};

    const double clearance = 0.5 * input.extrusion_width + 0.01;
    const ExPolygons safe_bed = offset_ex(
        input.bed_region, float(-scale_(clearance)), ClipperLib::jtRound);
    if (safe_bed.empty())
        return {};

    const ExPolygons inset_target = offset_ex(
        input.target_region, float(-scale_(clearance)), ClipperLib::jtRound);
    if (!inset_target.empty()) {
        std::vector<RootCandidate> candidates = make_direct_root_candidates_for_centerlines(
            input, maximum_area, inset_target, safe_bed, clearance);
        if (!candidates.empty())
            return candidates;
    }

    return make_direct_root_candidates_for_centerlines(
        input, maximum_area, input.target_region, safe_bed, clearance);
}

ExPolygons target_projection(const SupportTargetSpec &target, double tolerance)
{
    if (!target.region.empty()) {
        ExPolygons projection = union_ex(target.region);
        if (tolerance > 0.)
            projection = offset_ex(projection, float(scale_(0.5 * tolerance)), ClipperLib::jtRound);
        return projection;
    }

    const coord_t half_size = std::max<coord_t>(1, scale_(std::max(0.001, 0.5 * tolerance)));
    Polygon point_region {
        Point(target.center.x() - half_size, target.center.y() - half_size),
        Point(target.center.x() + half_size, target.center.y() - half_size),
        Point(target.center.x() + half_size, target.center.y() + half_size),
        Point(target.center.x() - half_size, target.center.y() + half_size)
    };
    return ExPolygons { ExPolygon(std::move(point_region)) };
}

std::vector<const SupportTargetSpec *> ordered_targets(const std::vector<SupportTargetSpec> &targets)
{
    std::vector<const SupportTargetSpec *> ordered;
    ordered.reserve(targets.size());
    for (const SupportTargetSpec &target : targets)
        ordered.emplace_back(&target);
    std::sort(ordered.begin(), ordered.end(), [](const SupportTargetSpec *left, const SupportTargetSpec *right) {
        return std::tie(left->layer_index, left->center.x(), left->center.y(), left->id) <
               std::tie(right->layer_index, right->center.x(), right->center.y(), right->id);
    });
    return ordered;
}

bool all_targets_have_similar_xy(const std::vector<const SupportTargetSpec *> &targets, double tolerance)
{
    const double squared_tolerance = tolerance * tolerance;
    for (size_t left = 0; left < targets.size(); ++left) {
        const Vec2d left_center = to_mm(targets[left]->center);
        for (size_t right = left + 1; right < targets.size(); ++right)
            if ((left_center - to_mm(targets[right]->center)).squaredNorm() > squared_tolerance + 1e-12)
                return false;
    }
    return true;
}

} // namespace

double maximum_lateral_growth(double layer_height, double branch_angle)
{
    if (layer_height <= 0.)
        return 0.;
    const double clamped_angle = std::clamp(branch_angle, 0., 89.);
    return layer_height * std::tan(clamped_angle * PI / 180.);
}

TsunamiObjectPlan plan_target_topology(const MultiTargetPlanningInput &input)
{
    TsunamiObjectPlan result;
    const std::vector<const SupportTargetSpec *> targets = ordered_targets(input.targets);
    if (targets.empty())
        return result;

    const double xy_tolerance = std::max(0., input.xy_similarity_tolerance);
    const double z_tolerance = input.z_similarity_tolerance > 0.
        ? input.z_similarity_tolerance : std::max(1e-6, 0.5 * input.layer_height);
    const auto [minimum_z, maximum_z] = std::minmax_element(
        targets.begin(), targets.end(), [](const SupportTargetSpec *left, const SupportTargetSpec *right) {
            return left->print_z < right->print_z;
        });
    const bool same_z = (*maximum_z)->print_z - (*minimum_z)->print_z <= z_tolerance + 1e-9;
    const bool similar_xy = all_targets_have_similar_xy(targets, xy_tolerance);
    if (targets.size() == 1 || (same_z && similar_xy))
        result.layout = TargetLayout::SingleOrCoincident;
    else if (same_z)
        result.layout = TargetLayout::SameZDifferentXY;
    else if (similar_xy)
        result.layout = TargetLayout::SimilarXYDifferentZ;
    else
        result.layout = TargetLayout::DifferentXYZ;

    ExPolygons common_projection = target_projection(*targets.front(), xy_tolerance);
    for (size_t index = 1; index < targets.size() && !common_projection.empty(); ++index)
        common_projection = intersection_ex(
            common_projection, target_projection(*targets[index], xy_tolerance));

    const bool zero_angle = maximum_lateral_growth(input.layer_height, input.branch_angle) <= 1e-12;
    if (!zero_angle) {
        TsunamiIslandPlan island;
        island.id = 0;
        island.target_ids.reserve(targets.size());
        for (const SupportTargetSpec *target : targets)
            island.target_ids.emplace_back(target->id);
        island.common_vertical_projection = std::move(common_projection);
        island.requires_lateral_growth = island.common_vertical_projection.empty() ||
            (result.layout == TargetLayout::SimilarXYDifferentZ && targets.size() > 1);
        result.islands.emplace_back(std::move(island));
        return result;
    }

    // At zero branch angle a trunk may only serve targets sharing one common
    // vertical projection. Keeping the running intersection prevents a chain
    // of pairwise overlaps from silently introducing lateral propagation.
    for (const SupportTargetSpec *target : targets) {
        const ExPolygons projection = target_projection(*target, xy_tolerance);
        size_t selected_island = result.islands.size();
        ExPolygons selected_intersection;
        for (size_t island_index = 0; island_index < result.islands.size(); ++island_index) {
            ExPolygons common = intersection_ex(
                result.islands[island_index].common_vertical_projection, projection);
            if (!common.empty()) {
                selected_island = island_index;
                selected_intersection = std::move(common);
                break;
            }
        }

        if (selected_island == result.islands.size()) {
            TsunamiIslandPlan island;
            island.id = result.islands.size();
            island.target_ids.emplace_back(target->id);
            island.common_vertical_projection = projection;
            result.islands.emplace_back(std::move(island));
        } else {
            TsunamiIslandPlan &island = result.islands[selected_island];
            island.target_ids.emplace_back(target->id);
            island.common_vertical_projection = std::move(selected_intersection);
        }
    }
    return result;
}

bool BranchTopologyPlan::preserves_even_trunk_degree() const
{
    return std::all_of(branches.begin(), branches.end(), [](const MacroBranchPlan &branch) {
        return branch.closed_turn_loop;
    });
}

BranchTopologyPlan plan_macro_branches(const BranchTopologyInput &input)
{
    BranchTopologyPlan result;
    const std::vector<const SupportTargetSpec *> targets = ordered_targets(input.targets);
    const double maximum_angle = std::clamp(input.branch_angle, 0., 89.);
    std::map<TurnId, size_t> turn_use_count;
    const std::set<std::pair<TargetId, TurnId>> forbidden_assignments(
        input.forbidden_assignments.begin(), input.forbidden_assignments.end());

    for (const SupportTargetSpec *target : targets) {
        const TrunkTurnCandidate *selected = nullptr;
        double selected_angle = 0.;
        double selected_distance = 0.;
        bool has_convex_turn = false;
        bool has_printable_turn = false;
        bool has_turn_below_target = false;
        bool has_turn_on_convex_side = false;
        bool has_turn_within_angle = false;
        bool has_turn_capacity = false;

        const Vec2d target_center = to_mm(target->center);
        for (const TrunkTurnCandidate &turn : input.trunk_turns) {
            if (!turn.convex)
                continue;
            has_convex_turn = true;
            if (!turn.printable)
                continue;
            if (forbidden_assignments.count({ target->id, turn.id }) != 0)
                continue;
            has_printable_turn = true;

            const double height = target->print_z - turn.print_z;
            if (height <= 1e-9)
                continue;
            has_turn_below_target = true;

            const Vec2d delta = target_center - to_mm(turn.center);
            const double distance = delta.norm();
            const double outward_length = turn.outward_direction.norm();
            if (distance > 1e-9 && outward_length > 1e-9 &&
                delta.dot(turn.outward_direction / outward_length) < -1e-9)
                continue;
            has_turn_on_convex_side = true;

            const double required_angle = std::atan2(distance, height) * 180. / PI;
            if (required_angle > maximum_angle + 1e-9)
                continue;
            has_turn_within_angle = true;

            const auto use_count = turn_use_count.find(turn.id);
            const size_t used = use_count == turn_use_count.end() ? 0 : use_count->second;
            if (used >= input.max_branches_per_turn)
                continue;
            has_turn_capacity = true;

            const bool preferred_height = selected != nullptr &&
                (input.prefer_earliest_birth
                    ? turn.print_z < selected->print_z - 1e-9
                    : turn.print_z > selected->print_z + 1e-9);
            const bool better = selected == nullptr || preferred_height ||
                (std::abs(turn.print_z - selected->print_z) <= 1e-9 &&
                 (required_angle < selected_angle - 1e-9 ||
                  (std::abs(required_angle - selected_angle) <= 1e-9 &&
                   (distance < selected_distance - 1e-9 ||
                    (std::abs(distance - selected_distance) <= 1e-9 && turn.id < selected->id)))));
            if (better) {
                selected = &turn;
                selected_angle = required_angle;
                selected_distance = distance;
            }
        }

        if (selected == nullptr) {
            TargetFailureReason reason = TargetFailureReason::NoConvexTurn;
            if (has_convex_turn)
                reason = TargetFailureReason::NoPrintableTurn;
            if (has_printable_turn)
                reason = TargetFailureReason::InsufficientHeight;
            if (has_turn_below_target)
                reason = TargetFailureReason::OutsideConvexSide;
            if (has_turn_on_convex_side)
                reason = TargetFailureReason::BranchAngleExceeded;
            if (has_turn_within_angle && !has_turn_capacity)
                reason = TargetFailureReason::TurnCapacityExceeded;
            result.failures.push_back({ target->id, reason });
            continue;
        }

        ++turn_use_count[selected->id];
        MacroBranchPlan branch;
        branch.id = result.branches.size() + 1;
        branch.parent = BranchId(0);
        branch.birth_turn_index = selected->id;
        branch.birth_layer = selected->layer_index;
        branch.birth_z = selected->print_z;
        branch.attachment = selected->center;
        branch.required_angle = selected_angle;
        branch.closed_turn_loop = true;
        branch.target_ids.emplace_back(target->id);
        result.branches.emplace_back(std::move(branch));
    }
    return result;
}

ClosedMacroBranchResult plan_closed_macro_branch(const ClosedMacroBranchInput &input)
{
    ClosedMacroBranchResult result;
    const std::optional<CompletedTurnGeometry> source = analyze_completed_turn(input.source_turn);
    const auto fail = [&result](MacroBranchGeometryFailureReason reason, size_t layer = size_t(-1)) {
        result.failure = reason;
        result.failure_layer = layer;
        return result;
    };
    if (!source || input.extrusion_width <= 0. || input.layer_height <= 0.)
        return fail(MacroBranchGeometryFailureReason::InvalidSourceTurn);
    if (input.minimum_anchor_length > 0. &&
        input.anchor_length + 1e-9 < input.minimum_anchor_length)
        return fail(MacroBranchGeometryFailureReason::InsufficientAnchor);

    const double available_height = input.target_z - input.birth_z;
    if (input.target_layer <= input.birth_layer || available_height <= 1e-9)
        return fail(MacroBranchGeometryFailureReason::InsufficientHeight);

    const Vec2d attachment = to_mm(source->apex);
    const Vec2d target = to_mm(input.target);
    const Vec2d target_delta = target - attachment;
    // Rigid parallel U-module (contract invariant 17): rails always inherit
    // the local source Trunk direction and never rotate toward the target.
    // Endpoint-directed growth_direction produced Macro branches angled off
    // their source Trunk rib; that output was rejected (CODEX_HANDOFF.md
    // "Failed Approach: Aim every Macro branch directly at its target
    // endpoint"). target_distance is now the forward projection of the
    // target onto that fixed direction, not the euclidean distance to it.
    const Vec2d growth_direction = source->outward_direction;
    // Forward projection onto the fixed direction. Lateral offset is served by
    // the branch's own width and by neighbouring branches, never by rotating
    // this module toward the target.
    const double target_distance = target_delta.dot(growth_direction);
    if (target_distance < -1e-9)
        return fail(MacroBranchGeometryFailureReason::TargetOutsideConvexSide);

    const double maximum_angle = std::clamp(input.branch_angle, 0., 89.);
    const double required_angle = target_distance > 1e-9
        ? std::atan2(target_distance, available_height) * 180. / PI : 0.;
    if (required_angle > maximum_angle + 1e-9)
        return fail(MacroBranchGeometryFailureReason::BranchAngleExceeded);

    ClosedMacroBranchPlan plan;
    plan.branch_id = input.branch_id;
    plan.target_id = input.target_id;
    plan.attachment = source->apex;
    plan.growth_direction = growth_direction;
    plan.required_angle = required_angle;
    if (target_distance <= 1e-9) {
        plan.reached_target = true;
        result.plan = std::move(plan);
        return result;
    }

    const double minimum_support_ratio = std::clamp(input.minimum_layer_support_ratio, 0., 1.);
    const ExPolygons source_footprint = extrusion_footprint(
        input.source_turn.polyline, input.extrusion_width);
    Vec2d source_tangent = to_mm(source->finish) - to_mm(source->start);
    const double source_turn_radius = 0.5 * source_tangent.norm();
    if (source_tangent.norm() <= 1e-9)
        return fail(MacroBranchGeometryFailureReason::InvalidSourceTurn);
    source_tangent.normalize();
    const double maximum_cap_radius = std::max(
        0.5 * input.extrusion_width, source_turn_radius - 0.5 * input.extrusion_width);
    PathSegment source_cap;
    Polyline base_loop;
    const size_t maximum_span = std::min(
        source->apex_index, input.source_turn.polyline.points.size() - source->apex_index - 1);
    for (size_t span = 1; span <= maximum_span; ++span) {
        const Point &span_start = input.source_turn.polyline.points[source->apex_index - span];
        const Point &span_finish = input.source_turn.polyline.points[source->apex_index + span];
        const double start_projection =
            (to_mm(span_start) - attachment).dot(source_tangent);
        const double finish_projection =
            (to_mm(span_finish) - attachment).dot(source_tangent);
        if (start_projection * finish_projection >= -1e-12)
            continue;
        const double half_width = std::min(std::abs(start_projection), std::abs(finish_projection));
        if (2. * half_width + 1e-9 < input.extrusion_width ||
            half_width > maximum_cap_radius + 1e-9)
            continue;

        // Keep the propagating end a real U-turn. Merely projecting the ends
        // of a source-arc subsection onto the attachment tangent distorts the
        // subsection, so it can no longer close into a printable terminal ring.
        const Vec2d cap_center = attachment - half_width * source->outward_direction;
        const int cap_steps = std::max(4, int(2 * span));
        PathSegment candidate_cap;
        candidate_cap.kind = SegmentKind::Turn;
        candidate_cap.polyline.points.reserve(size_t(cap_steps) + 1);
        for (int step = 0; step <= cap_steps; ++step) {
            const double angle = -0.5 * PI + PI * double(step) / double(cap_steps);
            candidate_cap.polyline.points.emplace_back(from_mm(
                cap_center + half_width * (std::cos(angle) * source->outward_direction +
                                           std::sin(angle) * source_tangent)));
        }

        Polyline candidate_loop;
        candidate_loop.points.emplace_back(source->apex);
        candidate_loop.points.emplace_back(candidate_cap.polyline.points.front());
        candidate_loop.points.insert(candidate_loop.points.end(),
            candidate_cap.polyline.points.begin() + 1, candidate_cap.polyline.points.end());
        if (candidate_loop.points.back() != source->apex)
            candidate_loop.points.emplace_back(source->apex);
        const double base_support_ratio = supported_area_ratio(
            extrusion_footprint(candidate_loop, input.extrusion_width), source_footprint);
        if (base_support_ratio + 1e-9 >= minimum_support_ratio) {
            source_cap = std::move(candidate_cap);
            base_loop = std::move(candidate_loop);
        }
    }
    if (source_cap.polyline.points.size() < 3)
        return fail(MacroBranchGeometryFailureReason::PrintabilityLimited, input.birth_layer + 1);
    plan.source_cap = source_cap;

    const Point rail_start = source_cap.polyline.points.front();
    const Point rail_finish = source_cap.polyline.points.back();
    const auto translated_turn = [&](double distance) {
        PathSegment turn = source_cap;
        turn.polyline.translate(from_mm(distance * growth_direction));
        return turn;
    };
    const auto make_detour = [&](double distance, const PathSegment &turn) {
        Polyline detour;
        const Point shift = from_mm(distance * growth_direction);
        const Point shifted_start = rail_start + shift;
        const Point shifted_finish = rail_finish + shift;
        detour.points.emplace_back(source->apex);
        if (rail_start != detour.points.back())
            detour.points.emplace_back(rail_start);
        if (shifted_start != detour.points.back())
            detour.points.emplace_back(shifted_start);
        size_t begin = !turn.polyline.points.empty() && turn.polyline.points.front() == detour.points.back() ? 1 : 0;
        detour.points.insert(detour.points.end(), turn.polyline.points.begin() + begin, turn.polyline.points.end());
        if (detour.points.empty() || detour.points.back() != shifted_finish)
            detour.points.emplace_back(shifted_finish);
        if (detour.points.back() != rail_finish)
            detour.points.emplace_back(rail_finish);
        if (detour.points.back() != source->apex)
            detour.points.emplace_back(source->apex);
        return detour;
    };
    const auto make_closed_cycle = [&](const Polyline &detour) {
        Polyline cycle = detour;
        if (!cycle.points.empty() && cycle.points.back() != cycle.points.front())
            cycle.points.emplace_back(cycle.points.front());
        return cycle;
    };
    const auto collision_at = [&](size_t layer_index, const ExPolygons &footprint) {
        return layer_index < input.blocked_region_by_layer.size() &&
               !input.blocked_region_by_layer[layer_index].empty() &&
               !intersection_ex(footprint, input.blocked_region_by_layer[layer_index]).empty();
    };
    const auto layer_print_z = [&](size_t layer_index) {
        if (layer_index == input.target_layer)
            return input.target_z;
        if (layer_index < input.print_z_by_layer.size())
            return input.print_z_by_layer[layer_index];
        return std::min(input.target_z,
            input.birth_z + double(layer_index - input.birth_layer) * input.layer_height);
    };

    PathSegment previous_turn = source_cap;
    Polyline previous_detour = base_loop;
    ExPolygons previous_turn_footprint = source_footprint;
    ExPolygons previous_detour_footprint = source_footprint;
    std::vector<ImmutableBranchSegment> straight_wake;
    double frontier = 0.;
    double previous_z = input.birth_z;
    bool collision_limited = false;
    plan.layers.reserve(input.target_layer - input.birth_layer);

    for (size_t layer_index = input.birth_layer + 1; layer_index <= input.target_layer; ++layer_index) {
        const double current_z = layer_print_z(layer_index);
        const double layer_height = current_z - previous_z;
        if (layer_height <= 1e-9)
            return fail(MacroBranchGeometryFailureReason::InsufficientHeight, layer_index);

        const double remaining_distance = target_distance - frontier;
        const double remaining_height = input.target_z - previous_z;
        const double angle_limited_growth = maximum_lateral_growth(layer_height, maximum_angle);
        const double scheduled_growth = input.complete_early ? remaining_distance :
            (remaining_height > 1e-9 ? remaining_distance * layer_height / remaining_height : remaining_distance);
        const double desired_frontier = std::min(
            target_distance, frontier + std::min(scheduled_growth, angle_limited_growth));

        const auto evaluate = [&](double candidate_frontier, double *support_ratio, bool *collision) {
            const PathSegment candidate_turn = translated_turn(candidate_frontier);
            const Polyline candidate_detour = make_detour(candidate_frontier, candidate_turn);
            const ExPolygons turn_footprint = extrusion_footprint(
                candidate_turn.polyline, input.extrusion_width);
            const ExPolygons detour_footprint = extrusion_footprint(
                candidate_detour, input.extrusion_width);
            const double ratio = std::min(
                supported_area_ratio(turn_footprint, previous_turn_footprint),
                supported_area_ratio(detour_footprint, previous_detour_footprint));
            const bool intersects_model = collision_at(layer_index, detour_footprint);
            if (support_ratio != nullptr)
                *support_ratio = ratio;
            if (collision != nullptr)
                *collision = intersects_model;
            return ratio + 1e-9 >= minimum_support_ratio && !intersects_model;
        };

        double previous_ratio = 1.;
        bool previous_collision = false;
        if (!evaluate(frontier, &previous_ratio, &previous_collision)) {
            return fail(previous_collision ? MacroBranchGeometryFailureReason::Collision
                                           : MacroBranchGeometryFailureReason::PrintabilityLimited,
                        layer_index);
        }

        double safe_frontier = desired_frontier;
        double safe_ratio = previous_ratio;
        bool desired_collision = false;
        if (!evaluate(desired_frontier, &safe_ratio, &desired_collision)) {
            collision_limited = collision_limited || desired_collision;
            double lower = frontier;
            double upper = desired_frontier;
            for (int iteration = 0; iteration < 32; ++iteration) {
                const double middle = 0.5 * (lower + upper);
                if (evaluate(middle, nullptr, nullptr))
                    lower = middle;
                else
                    upper = middle;
            }
            safe_frontier = lower;
            evaluate(safe_frontier, &safe_ratio, nullptr);
        }

        const PathSegment current_turn = translated_turn(safe_frontier);
        const Polyline current_detour = make_detour(safe_frontier, current_turn);
        if (safe_frontier > frontier + 1e-9) {
            const Point previous_shift = from_mm(frontier * growth_direction);
            const Point current_shift = from_mm(safe_frontier * growth_direction);
            const std::array<Polyline, 2> new_segments {{
                Polyline(rail_start + previous_shift, rail_start + current_shift),
                Polyline(rail_finish + current_shift, rail_finish + previous_shift)
            }};
            for (const Polyline &polyline : new_segments) {
                if (polyline.points.front() == polyline.points.back())
                    continue;
                ImmutableBranchSegment segment;
                segment.id = straight_wake.size();
                segment.birth_layer = layer_index;
                segment.polyline = polyline;
                straight_wake.emplace_back(std::move(segment));
            }
        }

        ClosedMacroBranchLayer layer;
        layer.layer_index = layer_index;
        layer.frontier_distance = safe_frontier;
        layer.applied_growth = safe_frontier - frontier;
        layer.support_ratio = safe_ratio;
        layer.straight_wake = straight_wake;
        layer.active_turn = current_turn;
        layer.detour = current_detour;
        layer.closed_cycle = make_closed_cycle(current_detour);
        plan.layers.emplace_back(std::move(layer));

        frontier = safe_frontier;
        if (plan.first_target_layer == size_t(-1) && frontier >= target_distance - 1e-6)
            plan.first_target_layer = layer_index;
        previous_z = current_z;
        previous_turn = current_turn;
        previous_detour = current_detour;
        previous_turn_footprint = extrusion_footprint(previous_turn.polyline, input.extrusion_width);
        previous_detour_footprint = extrusion_footprint(previous_detour, input.extrusion_width);
    }

    plan.reached_target = frontier >= target_distance - 1e-6;
    // A branch that stopped short is still a branch. Growth is capped layer by
    // layer by the model and by printability, and that capping works: the binary
    // search above lands the frontier just before the obstacle. Discarding the
    // result because it is not full length threw away branches that had grown
    // right up to the model -- on the snug overhang fixture six such branches
    // were dropped and their three source turns went unused, leaving demand
    // uncovered that was well within reach. Shorter candidates are part of the
    // approved design; the coverage loop already ranks by the area a candidate
    // actually supports, so a short branch simply scores lower.
    if (!plan.reached_target && frontier <= 1e-9)
        return fail(collision_limited ? MacroBranchGeometryFailureReason::Collision
                                      : MacroBranchGeometryFailureReason::PrintabilityLimited,
                    input.target_layer);
    result.plan = std::move(plan);
    return result;
}

std::optional<TrunkRibExtensionPlan> plan_trunk_rib_extension(
    const TrunkRibExtensionInput &input)
{
    if (input.rib.length <= 0. || input.rib.direction.norm() <= 1e-9 ||
        input.extrusion_width <= 0. || input.bed_region.empty())
        return std::nullopt;

    PhysicalRib base_rib = input.rib;
    base_rib.direction.normalize();
    const Vec2d origin = to_mm(base_rib.origin);
    const Vec2d model_reference = to_mm(input.model_reference);
    const Vec2d negative_end = origin - 0.5 * base_rib.length * base_rib.direction;
    const Vec2d positive_end = origin + 0.5 * base_rib.length * base_rib.direction;
    const bool extend_negative =
        (negative_end - model_reference).squaredNorm() >=
        (positive_end - model_reference).squaredNorm();

    const auto extended_rib = [&](double extension) {
        PhysicalRib rib = base_rib;
        rib.length += extension;
        rib.origin = from_mm(origin + (extend_negative ? -0.5 : 0.5) * extension * rib.direction);
        return rib;
    };
    const auto valid_extension = [&](double extension) {
        const PathSegment segment = make_rib_segment(extended_rib(extension), true);
        return footprint_is_valid(
            segment.polyline, input.extrusion_width, input.bed_region, input.blocked_region);
    };

    if (!valid_extension(0.))
        return std::nullopt;

    const double requested = std::max(0., input.requested_extension);
    double applied = requested;
    if (!valid_extension(requested)) {
        double lower = 0.;
        double upper = requested;
        for (int iteration = 0; iteration < 32; ++iteration) {
            const double middle = 0.5 * (lower + upper);
            if (valid_extension(middle))
                lower = middle;
            else
                upper = middle;
        }
        applied = lower;
    }

    TrunkRibExtensionPlan result;
    result.rib = extended_rib(applied);
    result.applied_extension = applied;
    result.extended_negative_end = extend_negative;
    return result;
}

std::optional<TerminalRingPlan> plan_terminal_ring(
    const PathSegment &source_turn, size_t base_layer, size_t tree_start_layer)
{
    if (source_turn.kind != SegmentKind::Turn || source_turn.polyline.points.size() < 3 ||
        tree_start_layer < base_layer)
        return std::nullopt;

    const Vec2d start = to_mm(source_turn.polyline.points.front());
    const Vec2d finish = to_mm(source_turn.polyline.points.back());
    const Vec2d center = 0.5 * (start + finish);
    const double radius = 0.5 * (finish - start).norm();
    if (radius <= 1e-6)
        return std::nullopt;

    double signed_sweep = 0.;
    const double radial_tolerance = std::max(0.02, 0.05 * radius);
    for (size_t index = 0; index < source_turn.polyline.points.size(); ++index) {
        const Vec2d radial = to_mm(source_turn.polyline.points[index]) - center;
        if (std::abs(radial.norm() - radius) > radial_tolerance)
            return std::nullopt;
        if (index > 0) {
            const Vec2d previous = to_mm(source_turn.polyline.points[index - 1]) - center;
            signed_sweep += std::atan2(
                previous.x() * radial.y() - previous.y() * radial.x(), previous.dot(radial));
        }
    }
    if (std::abs(std::abs(signed_sweep) - PI) > 0.1)
        return std::nullopt;

    TerminalRingPlan result;
    result.center = from_mm(center);
    result.radius = radius;
    result.base_layer = base_layer;
    result.tree_start_layer = tree_start_layer;

    const double direction = signed_sweep >= 0. ? 1. : -1.;
    const double finish_angle = std::atan2(finish.y() - center.y(), finish.x() - center.x());
    const int steps = std::max(8, int(source_turn.polyline.points.size()) - 1);
    result.base_complement.points.reserve(size_t(steps) + 1);
    result.base_complement.points.emplace_back(source_turn.polyline.points.back());
    for (int step = 1; step < steps; ++step) {
        const double angle = finish_angle + direction * PI * double(step) / double(steps);
        result.base_complement.points.emplace_back(from_mm(
            center + radius * Vec2d(std::cos(angle), std::sin(angle))));
    }
    result.base_complement.points.emplace_back(source_turn.polyline.points.front());

    result.vertical_ring = source_turn.polyline;
    result.vertical_ring.points.insert(
        result.vertical_ring.points.end(), result.base_complement.points.begin() + 1,
        result.base_complement.points.end());
    return result;
}

std::optional<std::vector<Point>> sample_target_contact_points(
    const SupportTargetSpec &target, double spacing, size_t maximum_points)
{
    const coord_t sample_spacing = scale_(spacing);
    if (target.region.empty() || sample_spacing <= 0 || maximum_points == 0)
        return std::nullopt;

    std::vector<Point> contact_points;
    std::set<std::pair<coord_t, coord_t>> contact_keys;
    bool contact_limit_exceeded = false;
    const auto add_contact = [&target, maximum_points, &contact_points, &contact_keys,
                              &contact_limit_exceeded](const Point &point) {
        const bool inside = std::any_of(
            target.region.begin(), target.region.end(), [&point](const ExPolygon &region) {
                return region.contains(point, true);
            });
        if (inside && contact_keys.emplace(point.x(), point.y()).second) {
            if (contact_points.size() >= maximum_points) {
                contact_limit_exceeded = true;
                return;
            }
            contact_points.emplace_back(point);
        }
    };

    // Reuse Orca's rectilinear fill as the area-coverage owner. Tree support
    // uses the same Fill abstraction before resampling its support-head lines.
    std::unique_ptr<Fill> filler(Fill::new_from_type(ipRectilinear));
    if (!filler)
        return std::nullopt;
    filler->set_bounding_box(get_extents(target.region));
    filler->layer_id = target.layer_index;
    filler->spacing = spacing;
    filler->angle = 0.f;
    filler->fixed_angle = true;
    FillParams fill_params;
    fill_params.density = 1.f;
    fill_params.dont_adjust = true;

    for (const ExPolygon &region : target.region) {
        for (const Point &point : region.contour.equally_spaced_points(double(sample_spacing))) {
            add_contact(point);
            if (contact_limit_exceeded)
                break;
        }
        if (contact_limit_exceeded)
            break;
        for (const Polygon &hole : region.holes) {
            for (const Point &point : hole.equally_spaced_points(double(sample_spacing))) {
                add_contact(point);
                if (contact_limit_exceeded)
                    break;
            }
            if (contact_limit_exceeded)
                break;
        }
        if (contact_limit_exceeded)
            break;

        Surface surface(stInternal, region);
        Polylines coverage_lines;
        try {
            coverage_lines = filler->fill_surface(&surface, fill_params);
        } catch (InfillFailedException &) {
            return std::nullopt;
        }
        for (const Polyline &line : coverage_lines) {
            for (const Point &point : line.equally_spaced_points(double(sample_spacing))) {
                add_contact(point);
                if (contact_limit_exceeded)
                    break;
            }
            if (contact_limit_exceeded)
                break;
        }
        if (contact_limit_exceeded)
            break;
    }

    // Degenerate regions may be narrower than one fill line. Keep the target's
    // deterministic interior anchor without inventing a second sampling grid.
    if (contact_points.empty())
        add_contact(target.center);

    std::sort(contact_points.begin(), contact_points.end(), [](const Point &left, const Point &right) {
        return std::tie(left.x(), left.y()) < std::tie(right.x(), right.y());
    });
    contact_points.erase(std::unique(contact_points.begin(), contact_points.end()), contact_points.end());
    if (contact_points.empty() || contact_limit_exceeded)
        return std::nullopt;
    return contact_points;
}

// Lateral distance a micro tree gains between two layers, accumulated to the
// target. Shared with the coverage loop so that what a branch is credited with
// and what its micro tree can actually reach are computed the same way.
double micro_tree_reach_from(size_t start_layer, size_t target_layer, double target_print_z,
                             const std::vector<double> &print_z_by_layer, double branch_angle,
                             double extrusion_width, double support_ratio)
{
    if (start_layer >= print_z_by_layer.size() || target_layer >= print_z_by_layer.size() ||
        start_layer >= target_layer)
        return -1.;
    const double maximum_angle = std::clamp(branch_angle, 0., 89.);
    const double ratio = std::clamp(support_ratio, 0., 1.);
    double reach = 0.;
    double previous_z = print_z_by_layer[start_layer];
    for (size_t layer_index = start_layer + 1; layer_index <= target_layer; ++layer_index) {
        const double current_z = layer_index == target_layer
            ? target_print_z : print_z_by_layer[layer_index];
        if (current_z <= previous_z + 1e-9)
            return -1.;
        reach += std::min(maximum_lateral_growth(current_z - previous_z, maximum_angle),
                          extrusion_width * (1. - ratio));
        previous_z = current_z;
    }
    return reach;
}

// Lowest layer a tree may start on: the terminal ring needs its riser first.
// Reach is greatest from this layer, so it also bounds what the branch below can
// be credited with covering.
size_t micro_tree_first_start_layer(size_t base_layer, size_t target_layer,
                                    const std::vector<double> &print_z_by_layer,
                                    double extrusion_width)
{
    if (base_layer >= print_z_by_layer.size())
        return size_t(-1);
    const double minimum_riser_height = std::max(1., 2. * extrusion_width);
    const double base_z = print_z_by_layer[base_layer];
    for (size_t candidate = base_layer + 1;
         candidate < target_layer && candidate < print_z_by_layer.size(); ++candidate)
        if (print_z_by_layer[candidate] - base_z + 1e-9 >= minimum_riser_height)
            return candidate;
    return size_t(-1);
}

std::optional<SeededMicroTreePlan> plan_seeded_micro_tree(const SeededMicroTreeInput &input)
{
    if (input.target.region.empty() || input.target.layer_index <= input.base_layer + 1 ||
        input.extrusion_width <= 0. || input.branch_distance <= 0. ||
        input.print_z_by_layer.size() <= input.target.layer_index)
        return std::nullopt;

    const std::optional<TerminalRingPlan> provisional_ring =
        plan_terminal_ring(input.source_turn, input.base_layer, input.base_layer);
    if (!provisional_ring)
        return std::nullopt;

    const std::optional<std::vector<Point>> sampled_contacts = sample_target_contact_points(
        input.target, std::max(input.branch_distance, input.tip_diameter));
    // A large target is not a single micro branch. Refuse it deterministically
    // so coverage planning can assign it to multiple terminal rings.
    if (!sampled_contacts)
        return std::nullopt;
    const std::vector<Point> &contact_points = *sampled_contacts;

    const double support_ratio_limit = std::clamp(input.minimum_layer_support_ratio, 0., 1.);
    const Vec2d seed_center = to_mm(provisional_ring->center);
    double maximum_contact_distance = 0.;
    for (const Point &contact : contact_points)
        maximum_contact_distance = std::max(maximum_contact_distance, (to_mm(contact) - seed_center).norm());

    // Reach shrinks as the start layer rises, so the first layer clearing the
    // riser is the only one that can succeed.
    const size_t tree_start_layer = micro_tree_first_start_layer(
        input.base_layer, input.target.layer_index, input.print_z_by_layer, input.extrusion_width);
    if (tree_start_layer == size_t(-1))
        return std::nullopt;
    const double tree_reach = micro_tree_reach_from(
        tree_start_layer, input.target.layer_index, input.target.print_z, input.print_z_by_layer,
        input.branch_angle, input.extrusion_width, support_ratio_limit);
    if (tree_reach + 1e-9 < maximum_contact_distance)
        return std::nullopt;

    std::optional<TerminalRingPlan> seed_ring =
        plan_terminal_ring(input.source_turn, input.base_layer, tree_start_layer);
    if (!seed_ring)
        return std::nullopt;

    SeededMicroTreePlan result;
    result.target_id = input.target_id;
    result.seed_ring = std::move(*seed_ring);
    result.contact_points = contact_points;
    result.layers.reserve(input.target.layer_index - tree_start_layer);

    const double start_z = input.print_z_by_layer[tree_start_layer];
    const double available_height = input.target.print_z - start_z;
    if (available_height <= 1e-9)
        return std::nullopt;
    const double seed_outer_radius = result.seed_ring.radius + 0.5 * input.extrusion_width;
    const double tip_outer_radius = std::max(0.5 * input.tip_diameter, 0.5 * input.extrusion_width);
    ExPolygons previous_footprint = extrusion_footprint(
        result.seed_ring.vertical_ring, input.extrusion_width);

    for (size_t layer_index = tree_start_layer + 1; layer_index <= input.target.layer_index; ++layer_index) {
        const double current_z = layer_index == input.target.layer_index
            ? input.target.print_z : input.print_z_by_layer[layer_index];
        const double fraction = std::clamp((current_z - start_z) / available_height, 0., 1.);
        const double outer_radius = seed_outer_radius + fraction * (tip_outer_radius - seed_outer_radius);

        std::vector<Vec2d> desired_centers;
        desired_centers.reserve(contact_points.size());
        for (const Point &contact : contact_points)
            desired_centers.emplace_back(seed_center + fraction * (to_mm(contact) - seed_center));

        SeededMicroTreeLayer layer;
        layer.layer_index = layer_index;
        Polygons branch_disks;
        branch_disks.reserve(desired_centers.size());
        for (const Vec2d &desired_center : desired_centers) {
            const Point center = from_mm(desired_center);
            layer.branch_centers.emplace_back(center);
            Polygon circle = make_circle(
                scale_(outer_radius),
                std::min<double>(scale_(0.02), 0.25 * scale_(outer_radius)));
            circle.translate(center);
            branch_disks.emplace_back(std::move(circle));
        }
        std::sort(layer.branch_centers.begin(), layer.branch_centers.end(), [](const Point &left, const Point &right) {
            return std::tie(left.x(), left.y()) < std::tie(right.x(), right.y());
        });

        ExPolygons centerline_regions = offset_ex(
            union_ex(branch_disks), -float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        for (const ExPolygon &region : centerline_regions) {
            const auto append_boundary = [&layer](const Polygon &boundary) {
                if (boundary.points.size() < 3)
                    return;
                Polyline path;
                path.points = boundary.points;
                path.points.emplace_back(boundary.points.front());
                layer.paths.emplace_back(std::move(path));
            };
            append_boundary(region.contour);
            for (const Polygon &hole : region.holes)
                append_boundary(hole);
        }
        if (layer.paths.empty())
            return std::nullopt;
        std::sort(layer.paths.begin(), layer.paths.end(), [](const Polyline &left, const Polyline &right) {
            return std::tie(left.points.front().x(), left.points.front().y()) <
                   std::tie(right.points.front().x(), right.points.front().y());
        });

        ExPolygons footprint;
        for (const Polyline &path : layer.paths)
            expolygons_append(footprint, extrusion_footprint(path, input.extrusion_width));
        footprint = union_ex(footprint);
        layer.support_ratio = supported_area_ratio(footprint, previous_footprint);
        if (layer.support_ratio + 1e-9 < support_ratio_limit)
            return std::nullopt;
        if (layer_index < input.blocked_region_by_layer.size() &&
            !input.blocked_region_by_layer[layer_index].empty() &&
            !intersection_ex(footprint, input.blocked_region_by_layer[layer_index]).empty())
            return std::nullopt;
        previous_footprint = std::move(footprint);
        result.layers.emplace_back(std::move(layer));
    }

    result.reached_target = !result.layers.empty() &&
        result.layers.back().layer_index == input.target.layer_index &&
        result.layers.back().branch_centers.size() == result.contact_points.size();
    return result.reached_target ? std::optional<SeededMicroTreePlan>(std::move(result)) : std::nullopt;
}

std::optional<TargetInterfacePlan> plan_target_interface(const TargetInterfaceInput &input)
{
    if (input.interface_layer_count == 0 || input.target.region.empty() ||
        input.target.layer_index < input.interface_layer_count ||
        input.lower_support_footprint.empty() || input.maximum_bridge_distance < 0.)
        return std::nullopt;

    const ExPolygons target_regions = union_ex(input.target.region);
    if (target_regions.empty())
        return std::nullopt;

    ExPolygons bridgeable_region = union_ex(input.lower_support_footprint);
    if (input.maximum_bridge_distance > 0.)
        bridgeable_region = offset_ex(
            bridgeable_region,
            float(scale_(input.maximum_bridge_distance) + SCALED_EPSILON),
            ClipperLib::jtRound);
    // Refuse an incomplete interface instead of silently dropping remote parts
    // of a target. A caller may retry with more micro-tree tips or fall back.
    if (!diff_ex(target_regions, bridgeable_region).empty())
        return std::nullopt;

    TargetInterfacePlan result;
    result.target_id = input.target.id;
    result.support_base_layer = input.target.layer_index - input.interface_layer_count;
    result.layers.reserve(input.interface_layer_count);
    for (size_t offset = 1; offset <= input.interface_layer_count; ++offset) {
        const size_t layer_index = result.support_base_layer + offset;
        if (layer_index < input.blocked_region_by_layer.size() &&
            !input.blocked_region_by_layer[layer_index].empty() &&
            !intersection_ex(target_regions, input.blocked_region_by_layer[layer_index]).empty())
            return std::nullopt;

        TargetInterfaceLayer layer;
        layer.layer_index = layer_index;
        layer.interface_number = input.target.layer_index - layer_index + 1;
        layer.regions = target_regions;
        result.layers.emplace_back(std::move(layer));
    }
    return result;
}

Point VirtualRibField::rib_origin(int index) const
{
    return from_mm(to_mm(origin) + guide_direction * (phase + double(index) * spacing));
}

int VirtualRibField::nearest_index(const Point &point) const
{
    if (spacing <= 0.)
        return 0;
    const double projected = (to_mm(point) - to_mm(origin)).dot(guide_direction) - phase;
    return int(std::llround(projected / spacing));
}

StraightBranchPlan plan_straight_branch(const StraightBranchInput &input)
{
    StraightBranchPlan result;
    Vec2d guide = to_mm(input.target) - to_mm(input.root);
    const double target_distance = guide.norm();
    guide = target_distance > 1e-9 ? guide / target_distance : Vec2d(1., 0.);

    const double extrusion_width = std::max(input.extrusion_width, 1e-6);
    const double minimum_turn_radius = input.minimum_turn_radius > 0.
        ? input.minimum_turn_radius : 0.5 * extrusion_width;
    const double actual_spacing = std::max({input.rib_spacing, 2. * minimum_turn_radius, 1e-6});
    const double activation_threshold = input.activation_threshold > 0.
        ? input.activation_threshold : std::max(input.minimum_physical_rib_length, 2. * extrusion_width);
    const double retention_threshold = input.retention_threshold > 0.
        ? std::min(input.retention_threshold, activation_threshold) : 0.75 * activation_threshold;
    const double minimum_anchor_length = input.minimum_anchor_length > 0.
        ? input.minimum_anchor_length : std::max(2. * extrusion_width, actual_spacing);
    const double minimum_support_ratio = std::clamp(input.minimum_layer_support_ratio, 0., 1.);
    const double maximum_printable_growth = std::min(
        maximum_lateral_growth(input.layer_height, input.branch_angle),
        extrusion_width * (1. - minimum_support_ratio));

    result.rib_field.origin = input.root;
    result.rib_field.guide_direction = guide;
    result.rib_field.rib_direction = Vec2d(-guide.y(), guide.x());
    result.rib_field.spacing = actual_spacing;
    result.rib_field.phase = 0.;
    result.layers.reserve(input.layer_count);

    const size_t trunk_layers = size_t(std::ceil(std::max(0., input.trunk_height) / std::max(input.layer_height, 1e-6)));
    const double target_growth_per_layer = maximum_lateral_growth(
        input.layer_height, input.branch_angle);
    const double initial_frontier = std::min(target_distance, result.rib_field.spacing);
    double frontier = initial_frontier;
    std::map<int, PhysicalRib> rib_registry;

    const auto available_length = [&input](size_t layer_idx) {
        return input.available_rib_length_by_layer.empty()
            ? input.rib_length
            : input.available_rib_length_by_layer[std::min(layer_idx, input.available_rib_length_by_layer.size() - 1)];
    };

    for (size_t layer_idx = 0; layer_idx < input.layer_count; ++layer_idx) {
        LayerPlan layer;
        layer.layer_index = layer_idx;
        const double available = available_length(layer_idx);

        for (auto &[index, rib] : rib_registry) {
            (void) index;
            if (rib.active && available + 1e-9 < retention_threshold)
                rib.active = false;
            else if (!rib.active && available + 1e-9 >= activation_threshold)
                rib.active = true;
        }

        const auto activate_reached_ribs = [&](int last_reached) {
            for (int index = 0; index <= last_reached; ++index) {
                auto found = rib_registry.find(index);
                if (found == rib_registry.end()) {
                    if (available + 1e-9 < activation_threshold)
                        continue;
                    PhysicalRib rib;
                    rib.id = size_t(index);
                    rib.virtual_rib_index = index;
                    rib.origin = result.rib_field.rib_origin(index);
                    rib.direction = result.rib_field.rib_direction;
                    rib.length = available;
                    rib.birth_layer = layer_idx;
                    rib_registry.emplace(index, rib);
                }
            }
        };

        activate_reached_ribs(std::max(0, int(std::floor((frontier + 1e-9) / actual_spacing))));

        if (layer_idx >= trunk_layers && frontier < target_distance - 1e-9) {
            const double desired_frontier = std::min(
                target_distance,
                initial_frontier + double(layer_idx - trunk_layers + 1) * target_growth_per_layer);
            const int anchor_index = std::max(0, int(std::floor((frontier + 1e-9) / actual_spacing)));
            const auto anchor = rib_registry.find(anchor_index);
            const bool anchor_ready = anchor != rib_registry.end() && anchor->second.active &&
                                      anchor->second.length + 1e-9 >= minimum_anchor_length;
            if (anchor_ready) {
                layer.applied_growth = std::min(
                    std::max(0., desired_frontier - frontier), maximum_printable_growth);
                frontier += layer.applied_growth;
                layer.estimated_support_ratio = std::clamp(1. - layer.applied_growth / extrusion_width, 0., 1.);
                activate_reached_ribs(std::max(0, int(std::floor((frontier + 1e-9) / actual_spacing))));
            }
        }

        int last_contiguous_rib = -1;
        for (int index = 0;; ++index) {
            const auto found = rib_registry.find(index);
            if (found == rib_registry.end() || !found->second.active)
                break;
            last_contiguous_rib = index;
            layer.physical_ribs.emplace_back(found->second);
        }

        if (last_contiguous_rib >= 0) {
            for (int index = 0; index <= last_contiguous_rib; ++index) {
                const bool forward = (index % 2 == 0);
                PathSegment rib = make_rib_segment(layer.physical_ribs[size_t(index)], forward);
                append_segment(layer.path, rib);
                layer.segments.emplace_back(std::move(rib));

                const double turn_start = double(index) * result.rib_field.spacing;
                const double turn_fraction = std::clamp((frontier - turn_start) / result.rib_field.spacing, 0., 1.);
                if (turn_fraction > 1e-9 && turn_start < target_distance - 1e-9) {
                    PathSegment turn = make_turn_segment(result.rib_field, layer.physical_ribs[size_t(index)], turn_fraction, forward);
                    append_segment(layer.path, turn);
                    layer.segments.emplace_back(std::move(turn));
                    const auto next_rib = rib_registry.find(index + 1);
                    if (index == last_contiguous_rib &&
                        (turn_fraction < 1. - 1e-9 || next_rib == rib_registry.end() || !next_rib->second.active))
                        layer.has_active_turn = true;
                }
            }
        }

        result.layers.emplace_back(std::move(layer));
    }
    result.reached_target = frontier >= target_distance - 1e-9;
    return result;
}

std::vector<TrunkTurnCandidate> collect_trunk_turn_candidates(
    const StraightBranchPlan &trunk, const std::vector<double> &print_z_by_layer)
{
    std::vector<TrunkTurnCandidate> result;
    const size_t layer_count = std::min(trunk.layers.size(), print_z_by_layer.size());
    for (size_t layer_index = 0; layer_index < layer_count; ++layer_index) {
        const LayerPlan &layer = trunk.layers[layer_index];
        size_t active_turn_segment = layer.segments.size();
        if (layer.has_active_turn) {
            for (size_t index = layer.segments.size(); index > 0; --index) {
                if (layer.segments[index - 1].kind == SegmentKind::Turn) {
                    active_turn_segment = index - 1;
                    break;
                }
            }
        }

        for (size_t segment_index = 0; segment_index < layer.segments.size(); ++segment_index) {
            const PathSegment &segment = layer.segments[segment_index];
            if (segment.kind != SegmentKind::Turn || segment_index == active_turn_segment ||
                segment.polyline.points.size() < 3)
                continue;
            const std::optional<CompletedTurnGeometry> geometry = analyze_completed_turn(segment);
            if (!geometry)
                continue;

            TrunkTurnCandidate candidate;
            candidate.id = result.size();
            candidate.layer_index = layer_index;
            candidate.source_segment_index = segment_index;
            candidate.print_z = print_z_by_layer[layer_index];
            candidate.center = geometry->apex;
            candidate.outward_direction = geometry->outward_direction;
            candidate.convex = true;
            candidate.printable = true;
            result.emplace_back(std::move(candidate));
        }
    }
    return result;
}

std::optional<RootCandidate> select_root_candidate(const RootSelectionInput &input)
{
    if (input.bed_region.empty() || input.maximum_xy_distance < 0. || input.rib_spacing <= 0. ||
        input.rib_length <= 0. || input.extrusion_width <= 0.)
        return std::nullopt;

    const double maximum_area = input.maximum_bed_contact_area > 0.
        ? input.maximum_bed_contact_area : std::numeric_limits<double>::max();
    // Bed area already claimed by earlier roots constrains only where this root
    // may place extrusion, never the contour it follows: the contour is derived
    // from the model footprint below and must not move because a neighbour exists.
    // Without this, two roots following the same model outline overlap by exactly
    // the padding target_contour_span adds at each end of an arc -- measured at
    // 12.78 degrees per seam on the hollow gear, whose two arcs are otherwise
    // already complementary at 192.78 degrees each.
    RootSelectionInput validity_input = input;
    if (!input.reserved_region.empty()) {
        expolygons_append(validity_input.blocked_region, input.reserved_region);
        validity_input.blocked_region = union_ex(validity_input.blocked_region);
    }
    std::optional<RootCandidate> best;
    // PERMANENT DIAGNOSTIC -- root selection had no observable of any kind, and
    // three separate failures (zero-angle, multi-column, the ipadstand timeout)
    // all dead-end here. Nine mechanisms were proposed for undersized roots in
    // one session and every one was refuted, because each was reasoned out
    // rather than read. Report what the search actually did. Do not delete.
    size_t probe_direct_candidates = 0;
    size_t probe_contour_regions = 0;
    size_t probe_contour_far = 0;
    size_t probe_contour_candidates = 0;
    const auto report_root_selection = [&](const char *path) {
        // Quiet when the search found a root that spans its target; a healthy
        // selection needs no line. Anything less is the interesting case.
        if (best && best->target_coverage >= 0.999)
            return;
        BOOST_LOG_TRIVIAL(warning)
            << "Tsunami root selection:"
            << " path=" << path
            << " direct_candidates=" << probe_direct_candidates
            << " contour_regions=" << probe_contour_regions
            << " contour_too_far=" << probe_contour_far
            << " contour_candidates=" << probe_contour_candidates
            << " rib_length_mm=" << input.rib_length
            << " max_xy_distance_mm=" << input.maximum_xy_distance
            << " chosen_ribs=" << (best ? best->physical_ribs.size() : size_t(0))
            << " chosen_coverage=" << (best ? best->target_coverage : -1.)
            << " chosen_bed_mm2=" << (best ? best->bed_contact_area : -1.)
            << " chosen_score=" << (best ? best->score : -1.);
    };
    if (input.allow_direct_projection) {
        for (RootCandidate &candidate : make_direct_root_candidates(validity_input, maximum_area)) {
            ++probe_direct_candidates;
            if (!best || candidate.score > best->score + 1e-9 ||
                (std::abs(candidate.score - best->score) <= 1e-9 &&
                 std::tie(candidate.position.x(), candidate.position.y()) <
                     std::tie(best->position.x(), best->position.y())))
                best = std::move(candidate);
        }
        if (best) {
            report_root_selection("direct");
            return best;
        }
    }
    if (input.blocked_region.empty()) {
        report_root_selection("no_blocked_region");
        return std::nullopt;
    }

    const ExPolygons centerline_forbidden = offset_ex(
        input.blocked_region,
        float(scale_(0.5 * input.extrusion_width + root_turn_radius(input) + 0.01)),
        ClipperLib::jtRound);
    const double minimum_depth = std::min(input.rib_length,
        std::max(2. * input.extrusion_width, input.rib_spacing));
    const double depth_step = std::max(input.extrusion_width, 0.25);
    const int depth_steps = std::max(1, int(std::ceil((input.rib_length - minimum_depth) / depth_step)) + 1);
    for (const ExPolygon &region : centerline_forbidden) {
        ++probe_contour_regions;
        const double distance = std::sqrt(locate_on_contour(region.contour, input.target).squared_distance);
        if (distance > input.maximum_xy_distance + 1e-9) {
            ++probe_contour_far;
            continue;
        }
        for (int depth_index = 0; depth_index < depth_steps; ++depth_index) {
            const double depth = depth_index + 1 == depth_steps ? input.rib_length
                : std::min(input.rib_length, minimum_depth + double(depth_index) * depth_step);
            std::vector<RootCandidate> candidates = make_contour_root_candidates(
                validity_input, region.contour, depth, maximum_area);
            for (RootCandidate &candidate : candidates) {
                ++probe_contour_candidates;
                if (!best || candidate.score > best->score + 1e-9 ||
                    (std::abs(candidate.score - best->score) <= 1e-9 &&
                     std::tie(candidate.position.x(), candidate.position.y()) <
                         std::tie(best->position.x(), best->position.y())))
                    best = std::move(candidate);
            }
        }
    }
    report_root_selection("contour");
    return best;
}

std::optional<SharedTrunkPlan> plan_shared_trunk(const SharedTrunkInput &input)
{
    if (input.targets.empty() || input.bed_region.empty() || input.layer_height <= 0. ||
        input.rib_spacing <= 0. || input.rib_length <= 0. || input.extrusion_width <= 0.)
        return std::nullopt;

    const std::vector<const SupportTargetSpec *> targets = ordered_targets(input.targets);
    const double branch_angle = std::clamp(input.branch_angle, 0., 89.);
    const bool targets_span_z = targets.size() > 1 &&
        (targets.front()->layer_index != targets.back()->layer_index ||
         std::abs(targets.front()->print_z - targets.back()->print_z) > 1e-9);
    const double support_ratio = std::clamp(input.minimum_layer_support_ratio, 0., 1.);
    const double printable_growth_per_layer = std::min(
        maximum_lateral_growth(input.layer_height, branch_angle),
        input.extrusion_width * (1. - support_ratio));

    const auto maximum_reach = [&](const SupportTargetSpec &target) {
        const double trunk_top = std::max(0., input.trunk_height);
        if (input.print_z_by_layer.empty()) {
            const double available_height = std::max(0., target.print_z - trunk_top);
            const size_t growth_layers = size_t(std::floor(available_height / input.layer_height + 1e-9));
            return input.rib_spacing + double(growth_layers) * printable_growth_per_layer;
        }

        double reach = input.rib_spacing;
        double previous_z = trunk_top;
        for (double print_z : input.print_z_by_layer) {
            const double current_z = std::min(print_z, target.print_z);
            if (current_z > previous_z + 1e-9) {
                reach += std::min(
                    maximum_lateral_growth(current_z - previous_z, branch_angle),
                    input.extrusion_width * (1. - support_ratio));
                previous_z = current_z;
            }
            if (print_z >= target.print_z - 1e-9)
                break;
        }
        if (target.print_z > previous_z + 1e-9)
            reach += std::min(
                maximum_lateral_growth(target.print_z - previous_z, branch_angle),
                input.extrusion_width * (1. - support_ratio));
        return reach;
    };

    std::vector<SupportTargetSpec> root_seeds = input.targets;
    ExPolygons common_projection = union_ex(targets.front()->region);
    for (size_t index = 1; index < targets.size() && !common_projection.empty(); ++index)
        common_projection = intersection_ex(common_projection, targets[index]->region);
    if (!common_projection.empty()) {
        SupportTargetSpec common_seed;
        common_seed.region = std::move(common_projection);
        common_seed.layer_index = targets.front()->layer_index;
        common_seed.print_z = targets.front()->print_z;
        for (const SupportTargetSpec *target : targets) {
            if (target->print_z < common_seed.print_z) {
                common_seed.layer_index = target->layer_index;
                common_seed.print_z = target->print_z;
            }
        }
        const BoundingBox bounds = get_extents(common_seed.region);
        common_seed.center = bounds.center();
        if (std::none_of(common_seed.region.begin(), common_seed.region.end(), [&common_seed](const ExPolygon &region) {
                return region.contains(common_seed.center, true);
            }))
            common_seed.center = common_seed.region.front().contour.points.front();
        root_seeds.emplace_back(std::move(common_seed));
    }

    std::optional<SharedTrunkPlan> best;
    double best_reach_utilization = std::numeric_limits<double>::max();
    for (const SupportTargetSpec &seed : root_seeds) {
        RootSelectionInput root_input;
        root_input.bed_region = input.bed_region;
        root_input.blocked_region = input.blocked_region;
        root_input.reserved_region = input.reserved_region;
        root_input.shares_contour = input.shares_contour;
        root_input.target_region = seed.region;
        root_input.target = seed.center;
        // For targets at different Z, an all-direct shared trunk either
        // pierces the lower target/interface or forces body/interface overlap.
        // Use an exterior root so every target leaves a convex trunk U-turn.
        // At zero angle, lateral branching is forbidden and direct projection
        // remains the only valid shared solution.
        const double direct_clearance = 0.5 * input.extrusion_width + 0.01;
        ExPolygons direct_centerlines = offset_ex(
            seed.region, float(-scale_(direct_clearance)), ClipperLib::jtRound);
        if (direct_centerlines.empty())
            direct_centerlines = seed.region;
        const bool full_vertical_projection_is_clear = input.blocked_region.empty() ||
            intersection_ex(
                direct_centerlines,
                offset_ex(input.blocked_region, float(scale_(direct_clearance)), ClipperLib::jtRound)).empty();
        root_input.allow_direct_projection = input.allow_direct_projection &&
            full_vertical_projection_is_clear && !(targets_span_z && branch_angle > 1e-9);
        root_input.maximum_xy_distance = maximum_reach(seed);
        root_input.rib_spacing = input.rib_spacing;
        root_input.rib_length = input.rib_length;
        root_input.extrusion_width = input.extrusion_width;
        root_input.minimum_bed_contact_area = input.minimum_bed_contact_area;
        root_input.maximum_bed_contact_area = input.maximum_bed_contact_area;
        std::optional<RootCandidate> candidate = select_root_candidate(root_input);
        if (!candidate)
            continue;

        size_t highest_target_layer = 0;
        for (const SupportTargetSpec *target : targets)
            highest_target_layer = std::max(highest_target_layer, target->layer_index);
        const ExPolygons root_footprint = extrusion_footprint(
            candidate->path, input.extrusion_width);
        bool trunk_collision = false;
        // The support is meant to meet the model on highest_target_layer.
        // Only layers strictly below that contact are collision obstacles.
        const size_t collision_layer_count = std::min(
            highest_target_layer, input.blocked_region_by_layer.size());
        for (size_t layer_index = 0; layer_index < collision_layer_count; ++layer_index) {
            if (!input.blocked_region_by_layer[layer_index].empty() &&
                !intersection_ex(root_footprint, input.blocked_region_by_layer[layer_index]).empty()) {
                trunk_collision = true;
                break;
            }
        }
        if (trunk_collision)
            continue;

        double maximum_required_angle = 0.;
        double reach_utilization = 0.;
        bool all_direct = true;
        bool reachable = true;
        std::vector<SharedTrunkTargetPlan> target_plans;
        target_plans.reserve(targets.size());
        for (const SupportTargetSpec *target : targets) {
            Point branch_target = closest_region_point(
                candidate->approach_point, target->region, target->center);
            double distance = (to_mm(branch_target) - to_mm(candidate->approach_point)).norm();
            // A contour root may lie inside the expanded contact polygon while
            // the actual demand is still vertically blocked. It must grow
            // toward the target field instead of being classified as direct.
            if (!candidate->direct_projection && distance <= 1e-9) {
                // The approach point already lies under the target field, but the
                // vertical projection is blocked, so the trunk must still grow
                // outward. The growth this coarse pre-pass may demand is the
                // distance to the target boundary, not to its centroid: section
                // 9.4 of the contract scopes this stage to rejecting clearly
                // unreachable roots before heavy routing, and covering a large
                // target is owned later by plan_runtime_trunk's multi-branch
                // coverage loop. Demanding the centroid rejected roots that were
                // already standing underneath the target they had to serve.
                branch_target = closest_region_point(
                    candidate->approach_point, target->region, target->center, false);
                distance = (to_mm(branch_target) - to_mm(candidate->approach_point)).norm();
            }
            const bool direct_projection = candidate->direct_projection && distance <= 1e-9;
            all_direct = all_direct && direct_projection;
            const double height = target->print_z - std::max(0., input.trunk_height);
            const double required_angle = distance <= 1e-9 ? 0.
                : (height > 1e-9 ? std::atan2(distance, height) * 180. / PI
                                 : std::numeric_limits<double>::infinity());
            const double reach = maximum_reach(*target);
            if (required_angle > branch_angle + 1e-9 || distance > reach + 1e-9) {
                reachable = false;
                break;
            }
            maximum_required_angle = std::max(maximum_required_angle, required_angle);
            reach_utilization = std::max(reach_utilization, distance / std::max(reach, 1e-9));
            target_plans.push_back({ target->id, branch_target, distance, required_angle, direct_projection });
        }
        if (!reachable)
            continue;

        SharedTrunkPlan plan;
        plan.root = std::move(*candidate);
        plan.maximum_required_angle = maximum_required_angle;
        plan.all_targets_directly_above = all_direct;
        plan.target_plans = std::move(target_plans);
        plan.target_ids.reserve(targets.size());
        for (const SupportTargetSpec *target : targets) {
            plan.target_ids.emplace_back(target->id);
            if (target->print_z > plan.top_z + 1e-9 ||
                (std::abs(target->print_z - plan.top_z) <= 1e-9 && target->layer_index > plan.top_layer)) {
                plan.top_z = target->print_z;
                plan.top_layer = target->layer_index;
            }
        }

        const bool better = !best || reach_utilization < best_reach_utilization - 1e-9 ||
            (std::abs(reach_utilization - best_reach_utilization) <= 1e-9 &&
             (plan.maximum_required_angle < best->maximum_required_angle - 1e-9 ||
              (std::abs(plan.maximum_required_angle - best->maximum_required_angle) <= 1e-9 &&
               std::tie(plan.root.position.x(), plan.root.position.y()) <
                   std::tie(best->root.position.x(), best->root.position.y()))));
        if (better) {
            best_reach_utilization = reach_utilization;
            best = std::move(plan);
        }
    }
    return best;
}

SharedTrunkForestPlan plan_shared_trunks(const SharedTrunkInput &input)
{
    SharedTrunkForestPlan result;
    struct PlannedGroup {
        std::vector<SupportTargetSpec> targets;
        SharedTrunkPlan trunk;
    };
    std::vector<PlannedGroup> groups;

    for (const SupportTargetSpec *target : ordered_targets(input.targets)) {
        SharedTrunkInput single_input = input;
        single_input.targets = { *target };
        std::optional<SharedTrunkPlan> trunk = plan_shared_trunk(single_input);
        if (!trunk) {
            result.unassigned_target_ids.emplace_back(target->id);
            continue;
        }
        groups.push_back({ std::move(single_input.targets), std::move(*trunk) });
    }

    for (;;) {
        size_t best_left = groups.size();
        size_t best_right = groups.size();
        size_t best_target_count = 0;
        std::optional<PlannedGroup> best_merge;
        for (size_t left = 0; left < groups.size(); ++left) {
            for (size_t right = left + 1; right < groups.size(); ++right) {
                SharedTrunkInput merged_input = input;
                merged_input.targets = groups[left].targets;
                merged_input.targets.insert(merged_input.targets.end(),
                                            groups[right].targets.begin(), groups[right].targets.end());
                std::optional<SharedTrunkPlan> trunk = plan_shared_trunk(merged_input);
                if (!trunk)
                    continue;

                const size_t target_count = merged_input.targets.size();
                const bool better = !best_merge || target_count > best_target_count ||
                    (target_count == best_target_count &&
                     (trunk->maximum_required_angle < best_merge->trunk.maximum_required_angle - 1e-9 ||
                      (std::abs(trunk->maximum_required_angle - best_merge->trunk.maximum_required_angle) <= 1e-9 &&
                       std::tie(trunk->target_ids, trunk->root.position.x(), trunk->root.position.y()) <
                           std::tie(best_merge->trunk.target_ids, best_merge->trunk.root.position.x(),
                                    best_merge->trunk.root.position.y()))));
                if (better) {
                    best_left = left;
                    best_right = right;
                    best_target_count = target_count;
                    best_merge = PlannedGroup { std::move(merged_input.targets), std::move(*trunk) };
                }
            }
        }
        if (!best_merge)
            break;

        groups[best_left] = std::move(*best_merge);
        groups.erase(groups.begin() + best_right);
        std::sort(groups.begin(), groups.end(), [](const PlannedGroup &left, const PlannedGroup &right) {
            return left.trunk.target_ids < right.trunk.target_ids;
        });
    }

    std::sort(groups.begin(), groups.end(), [](const PlannedGroup &left, const PlannedGroup &right) {
        return left.trunk.target_ids < right.trunk.target_ids;
    });
    result.trunks.reserve(groups.size());
    for (PlannedGroup &group : groups)
        result.trunks.emplace_back(std::move(group.trunk));
    std::sort(result.unassigned_target_ids.begin(), result.unassigned_target_ids.end());
    return result;
}

StraightBranchPlan plan_contour_branch(const RootCandidate &root, const StraightBranchInput &input)
{
    StraightBranchPlan result;
    if (root.path.points.size() < 2 || root.physical_ribs.empty() || input.layer_count == 0)
        return result;

    if (root.frontier_rib_index >= root.physical_ribs.size())
        return result;
    PhysicalRib anchor = root.physical_ribs[root.frontier_rib_index];
    anchor.id = 0;
    anchor.virtual_rib_index = 0;
    anchor.birth_layer = 0;

    Vec2d start = to_mm(anchor.origin);
    const Vec2d finish = to_mm(input.target);
    Vec2d target_direction = finish - start;
    const double direct_distance = target_direction.norm();
    if (root.direct_projection || direct_distance <= 1e-9) {
        result.layers.resize(input.layer_count);
        for (size_t layer_index = 0; layer_index < input.layer_count; ++layer_index) {
            result.layers[layer_index].layer_index = layer_index;
            result.layers[layer_index].physical_ribs = root.physical_ribs;
            result.layers[layer_index].segments = root.segments;
            result.layers[layer_index].path = root.path;
        }
        result.reached_target = true;
        return result;
    }
    target_direction /= direct_distance;

    Vec2d start_tangent = root.frontier_tangent.normalized();
    if (start_tangent.dot(target_direction) < 0.)
        start_tangent = -start_tangent;
    const double handle_length = std::min(0.4 * direct_distance,
        std::max(2. * input.rib_spacing, anchor.length));
    const Vec2d control1 = start + handle_length * start_tangent;
    const Vec2d control2 = finish - 0.5 * handle_length * target_direction;
    const auto bezier_point = [&](double t) {
        const double inverse = 1. - t;
        return inverse * inverse * inverse * start
             + 3. * inverse * inverse * t * control1
             + 3. * inverse * t * t * control2
             + t * t * t * finish;
    };
    const auto bezier_tangent = [&](double t) {
        const double inverse = 1. - t;
        Vec2d tangent = 3. * inverse * inverse * (control1 - start)
                      + 6. * inverse * t * (control2 - control1)
                      + 3. * t * t * (finish - control2);
        return tangent.norm() > 1e-9 ? tangent.normalized() : target_direction;
    };

    constexpr int guide_resolution = 256;
    std::array<Vec2d, guide_resolution + 1> dense_points;
    std::array<double, guide_resolution + 1> dense_lengths {};
    dense_points[0] = start;
    for (int index = 1; index <= guide_resolution; ++index) {
        dense_points[size_t(index)] = bezier_point(double(index) / double(guide_resolution));
        dense_lengths[size_t(index)] = dense_lengths[size_t(index - 1)]
            + (dense_points[size_t(index)] - dense_points[size_t(index - 1)]).norm();
    }
    const double guide_length = dense_lengths.back();
    const double actual_spacing = std::max(input.rib_spacing, input.extrusion_width);

    struct GuidedRib {
        double path_distance { 0. };
        Vec2d tangent { 1., 0. };
        PhysicalRib rib;
    };
    std::vector<GuidedRib> virtual_ribs;
    const int full_intervals = std::max(1, int(std::floor(guide_length / actual_spacing)));
    virtual_ribs.reserve(size_t(full_intervals) + 2);
    const auto append_virtual_rib = [&](double distance, size_t virtual_index, std::vector<GuidedRib> &ribs) {
        const auto upper = std::lower_bound(dense_lengths.begin(), dense_lengths.end(), distance);
        const size_t upper_index = size_t(std::distance(dense_lengths.begin(), upper));
        const size_t lower_index = upper_index == 0 ? 0 : upper_index - 1;
        const double segment_length = dense_lengths[upper_index] - dense_lengths[lower_index];
        const double fraction = segment_length > 1e-9
            ? (distance - dense_lengths[lower_index]) / segment_length : 0.;
        const double t = (double(lower_index) + fraction) / double(guide_resolution);
        Vec2d rib_direction(-bezier_tangent(t).y(), bezier_tangent(t).x());
        if (!ribs.empty() && rib_direction.dot(ribs.back().rib.direction) < 0.)
            rib_direction = -rib_direction;
        GuidedRib guided;
        guided.path_distance = distance;
        guided.tangent = bezier_tangent(t);
        guided.rib.id = virtual_index;
        guided.rib.virtual_rib_index = int(virtual_index);
        guided.rib.origin = from_mm(bezier_point(t));
        guided.rib.direction = rib_direction;
        guided.rib.length = anchor.length;
        ribs.emplace_back(std::move(guided));
    };

    GuidedRib initial;
    initial.path_distance = 0.;
    initial.tangent = start_tangent;
    initial.rib = anchor;
    virtual_ribs.emplace_back(std::move(initial));
    for (int index = 1; index <= full_intervals; ++index)
        append_virtual_rib(std::min(guide_length, double(index) * actual_spacing), size_t(index), virtual_ribs);
    if (virtual_ribs.back().path_distance < guide_length - 1e-6)
        append_virtual_rib(guide_length, virtual_ribs.size(), virtual_ribs);

    result.rib_field.origin = anchor.origin;
    result.rib_field.guide_direction = start_tangent;
    result.rib_field.rib_direction = anchor.direction;
    result.rib_field.spacing = actual_spacing;
    result.layers.reserve(input.layer_count);

    const size_t root_layers = std::min(input.layer_count, std::max<size_t>(1,
        size_t(std::ceil(std::max(0., input.trunk_height) / std::max(input.layer_height, 1e-6)))));
    const double desired_growth = maximum_lateral_growth(
        input.layer_height, input.branch_angle);
    const double minimum_support_ratio = std::clamp(input.minimum_layer_support_ratio, 0., 1.);
    const double maximum_growth = std::min(
        desired_growth,
        std::max(input.extrusion_width, 1e-6) * (1. - minimum_support_ratio));
    const double minimum_anchor_length = input.minimum_anchor_length > 0.
        ? input.minimum_anchor_length : std::max(2. * input.extrusion_width, actual_spacing);
    double frontier = 0.;
    std::vector<size_t> rib_birth_layers(virtual_ribs.size(), std::numeric_limits<size_t>::max());
    rib_birth_layers.front() = 0;

    const auto make_guided_turn = [](const GuidedRib &from, const GuidedRib &to, double fraction, bool upper) {
        PathSegment segment;
        segment.kind = SegmentKind::Turn;
        fraction = std::clamp(fraction, 0., 1.);
        const double sign = upper ? 1. : -1.;
        const Vec2d from_point = to_mm(from.rib.origin) + sign * 0.5 * from.rib.length * from.rib.direction;
        const Vec2d to_point = to_mm(to.rib.origin) + sign * 0.5 * to.rib.length * to.rib.direction;
        const double distance = std::max(1e-6, to.path_distance - from.path_distance);
        const int steps = std::max(1, int(std::ceil(8. * fraction)));
        segment.polyline.points.reserve(size_t(steps) + 1);
        for (int step = 0; step <= steps; ++step) {
            const double t = fraction * double(step) / double(steps);
            const double t2 = t * t;
            const double t3 = t2 * t;
            const Vec2d point = (2. * t3 - 3. * t2 + 1.) * from_point
                              + (t3 - 2. * t2 + t) * distance * from.tangent
                              + (-2. * t3 + 3. * t2) * to_point
                              + (t3 - t2) * distance * to.tangent;
            segment.polyline.points.emplace_back(from_mm(point));
        }
        return segment;
    };

    const auto active_rib_count_at = [&virtual_ribs](double candidate_frontier) {
        return size_t(std::upper_bound(
            virtual_ribs.begin(), virtual_ribs.end(), candidate_frontier + 1e-9,
            [](double distance, const GuidedRib &rib) { return distance < rib.path_distance; }) - virtual_ribs.begin());
    };
    const auto build_guided_layer = [&](size_t layer_index, double candidate_frontier) {
        LayerPlan layer;
        layer.layer_index = layer_index;
        const size_t active_rib_count = std::max<size_t>(1, active_rib_count_at(candidate_frontier));
        for (size_t index = 0; index < active_rib_count; ++index) {
            const bool forward = (index % 2 == 1);
            PhysicalRib rib_state = virtual_ribs[index].rib;
            rib_state.birth_layer = rib_birth_layers[index] == std::numeric_limits<size_t>::max()
                ? layer_index : rib_birth_layers[index];
            layer.physical_ribs.emplace_back(rib_state);
            PathSegment rib = make_rib_segment(rib_state, forward);
            if (index == 0)
                rib.polyline.points.back() = root.frontier_point;
            append_segment(layer.path, rib);
            layer.segments.emplace_back(std::move(rib));
            if (index + 1 < virtual_ribs.size()) {
                const double interval = virtual_ribs[index + 1].path_distance - virtual_ribs[index].path_distance;
                const double fraction = std::clamp(
                    (candidate_frontier - virtual_ribs[index].path_distance) / std::max(interval, 1e-9), 0., 1.);
                if (fraction > 1e-9) {
                    PathSegment turn = make_guided_turn(
                        virtual_ribs[index], virtual_ribs[index + 1], fraction, forward);
                    if (index == 0)
                        turn.polyline.points.front() = root.frontier_point;
                    append_segment(layer.path, turn);
                    layer.segments.emplace_back(std::move(turn));
                    if (index + 1 == active_rib_count && fraction < 1. - 1e-9)
                        layer.has_active_turn = true;
                }
            }
        }
        return layer;
    };
    const auto collides_with_model = [&](const LayerPlan &layer, size_t layer_index) {
        if (layer.path.points.size() < 2 || layer_index >= input.blocked_region_by_layer.size() ||
            input.blocked_region_by_layer[layer_index].empty())
            return false;
        const Polygons footprint = offset(
            layer.path, float(scale_(0.5 * input.extrusion_width)), ClipperLib::jtRound);
        return !intersection_ex(footprint, input.blocked_region_by_layer[layer_index]).empty();
    };

    for (size_t layer_index = 0; layer_index < input.layer_count; ++layer_index) {
        LayerPlan layer;
        layer.layer_index = layer_index;
        if (layer_index < root_layers) {
            layer.physical_ribs = root.physical_ribs;
            layer.segments = root.segments;
            layer.path = root.path;
            if (collides_with_model(layer, layer_index)) {
                result.layers.clear();
                return result;
            }
            result.layers.emplace_back(std::move(layer));
            continue;
        }

        const double previous_frontier = frontier;
        if (anchor.length + 1e-9 >= minimum_anchor_length && frontier < guide_length - 1e-9) {
            const double target_frontier = std::min(
                guide_length, double(layer_index - root_layers + 1) * desired_growth);
            frontier = std::min(target_frontier, previous_frontier + maximum_growth);
        }

        LayerPlan candidate = build_guided_layer(layer_index, frontier);
        if (collides_with_model(candidate, layer_index)) {
            LayerPlan supported = build_guided_layer(layer_index, previous_frontier);
            if (collides_with_model(supported, layer_index)) {
                result.layers.clear();
                return result;
            }
            double safe_frontier = previous_frontier;
            double blocked_frontier = frontier;
            for (int iteration = 0; iteration < 12; ++iteration) {
                const double midpoint = 0.5 * (safe_frontier + blocked_frontier);
                LayerPlan midpoint_layer = build_guided_layer(layer_index, midpoint);
                if (collides_with_model(midpoint_layer, layer_index))
                    blocked_frontier = midpoint;
                else
                    safe_frontier = midpoint;
            }
            frontier = safe_frontier;
            candidate = build_guided_layer(layer_index, frontier);
        }

        const size_t accepted_rib_count = std::max<size_t>(1, active_rib_count_at(frontier));
        for (size_t index = 1; index < accepted_rib_count; ++index)
            if (rib_birth_layers[index] == std::numeric_limits<size_t>::max())
                rib_birth_layers[index] = layer_index;
        candidate = build_guided_layer(layer_index, frontier);
        candidate.applied_growth = frontier - previous_frontier;
        candidate.estimated_support_ratio = std::clamp(
            1. - candidate.applied_growth / std::max(input.extrusion_width, 1e-6), 0., 1.);
        result.layers.emplace_back(std::move(candidate));
    }
    result.reached_target = frontier >= guide_length - 1e-9;
    return result;
}

} // namespace Tsunami

namespace {

struct TsunamiTarget {
    size_t layer_count { 0 };
    size_t object_layer_above { 0 };
    double print_z { 0. };
    Point center;
    BoundingBox bounds;
    double area { 0. };
    ExPolygon region;
};

Point target_anchor(const ExPolygon &region, const BoundingBox &bounds)
{
    const Point bounds_center = bounds.center();
    if (region.contains(bounds_center, false))
        return bounds_center;

    const coord_t extension = std::max(bounds.size().x(), bounds.size().y()) + scale_(1.);
    const std::array<Polyline, 2> probes {{
        Polyline(Point(bounds.min.x() - extension, bounds_center.y()),
                 Point(bounds.max.x() + extension, bounds_center.y())),
        Polyline(Point(bounds_center.x(), bounds.min.y() - extension),
                 Point(bounds_center.x(), bounds.max.y() + extension))
    }};
    Point best_point = region.contour.points.front();
    double best_length = -1.;
    double best_center_distance = std::numeric_limits<double>::max();
    for (const Polyline &probe : probes) {
        for (const Polyline &piece : intersection_pl(Polylines { probe }, ExPolygons { region })) {
            if (piece.points.size() < 2)
                continue;
            const Vec2d first(unscale<double>(piece.points.front().x()),
                              unscale<double>(piece.points.front().y()));
            const Vec2d last(unscale<double>(piece.points.back().x()),
                             unscale<double>(piece.points.back().y()));
            const double length = (last - first).norm();
            if (length <= 1e-9)
                continue;
            const Vec2d midpoint_mm = 0.5 * (first + last);
            const Point midpoint(scale_(midpoint_mm.x()), scale_(midpoint_mm.y()));
            const Vec2d bounds_center_mm(unscale<double>(bounds_center.x()),
                                         unscale<double>(bounds_center.y()));
            const double center_distance = (midpoint_mm - bounds_center_mm).squaredNorm();
            if (length > best_length + 1e-9 ||
                (std::abs(length - best_length) <= 1e-9 && center_distance < best_center_distance - 1e-9) ||
                (std::abs(length - best_length) <= 1e-9 &&
                 std::abs(center_distance - best_center_distance) <= 1e-9 &&
                 std::tie(midpoint.x(), midpoint.y()) < std::tie(best_point.x(), best_point.y()))) {
                best_point = midpoint;
                best_length = length;
                best_center_distance = center_distance;
            }
        }
    }
    return best_point;
}

void append_support_targets(size_t layer_count, size_t object_layer_above, double print_z, ExPolygon region,
                            std::vector<TsunamiTarget> &targets, size_t split_depth = 0)
{
    if (region.contour.points.size() < 3 || std::abs(region.area()) <= 0.)
        return;
    const BoundingBox region_bounds = get_extents(region);

    if (!region.holes.empty() && split_depth < 8) {
        const auto largest_hole = std::max_element(
            region.holes.begin(), region.holes.end(), [](const Polygon &left, const Polygon &right) {
                return std::abs(left.area()) < std::abs(right.area());
            });
        const BoundingBox region_bounds = get_extents(region);
        const BoundingBox hole_bounds = get_extents(*largest_hole);
        const coord_t split_x = hole_bounds.center().x();
        const coord_t extension = std::max(region_bounds.size().x(), region_bounds.size().y()) + scale_(1.);
        const Polygons left_half { Polygon {
            Point(region_bounds.min.x() - extension, region_bounds.min.y() - extension),
            Point(split_x, region_bounds.min.y() - extension),
            Point(split_x, region_bounds.max.y() + extension),
            Point(region_bounds.min.x() - extension, region_bounds.max.y() + extension)
        } };
        const Polygons right_half { Polygon {
            Point(split_x, region_bounds.min.y() - extension),
            Point(region_bounds.max.x() + extension, region_bounds.min.y() - extension),
            Point(region_bounds.max.x() + extension, region_bounds.max.y() + extension),
            Point(split_x, region_bounds.max.y() + extension)
        } };
        ExPolygons pieces = intersection_ex(ExPolygons { region }, left_half);
        ExPolygons right_pieces = intersection_ex(ExPolygons { region }, right_half);
        pieces.insert(pieces.end(), std::make_move_iterator(right_pieces.begin()),
                      std::make_move_iterator(right_pieces.end()));
        size_t remaining_holes = 0;
        for (const ExPolygon &piece : pieces)
            remaining_holes += piece.holes.size();
        if (pieces.size() > 1 && remaining_holes < region.holes.size()) {
            for (ExPolygon &piece : pieces)
                append_support_targets(
                    layer_count, object_layer_above, print_z, std::move(piece), targets, split_depth + 1);
            return;
        }
    }

    TsunamiTarget target;
    target.layer_count = layer_count;
    target.object_layer_above = object_layer_above;
    target.print_z = print_z;
    target.bounds = region_bounds;
    target.center = target_anchor(region, target.bounds);
    target.area = std::abs(region.area());
    target.region = std::move(region);
    targets.emplace_back(std::move(target));
}

std::vector<TsunamiTarget> find_support_targets(
    const PrintObject &object, const SupportGeneratorLayersPtr &top_contacts)
{
    std::vector<TsunamiTarget> targets;
    for (const SupportGeneratorLayer *top_contact : top_contacts) {
        if (top_contact == nullptr || top_contact->idx_object_layer_above == size_t(-1) ||
            top_contact->idx_object_layer_above >= object.layers().size())
            continue;

        const ExPolygons &object_slice =
            object.layers()[top_contact->idx_object_layer_above]->lslices;
        ExPolygons target_regions;
        if (top_contact->overhang_polygons != nullptr) {
            // Normal support preserves overhang_polygons as the snug support
            // demand, while contact_polygons/polygons are expanded into its
            // propagation grid. Routing Tsunami from that grid creates target
            // fragments unrelated to independent overhang islands.
            target_regions = intersection_ex(
                union_ex(*top_contact->overhang_polygons), object_slice);
        } else if (!top_contact->polygons.empty()) {
            // Defensive compatibility for externally constructed contact
            // layers that predate the preserved snug-demand field.
            target_regions = intersection_ex(
                union_ex(top_contact->polygons), object_slice);
        }

        const size_t layer_count = size_t(std::upper_bound(
            object.layers().begin(), object.layers().end(), top_contact->print_z + EPSILON,
            [](double print_z, const Layer *layer) { return print_z < layer->print_z; }) - object.layers().begin());
        if (layer_count == 0)
            continue;
        for (ExPolygon &region : target_regions)
            append_support_targets(layer_count, top_contact->idx_object_layer_above,
                                   top_contact->print_z, std::move(region), targets);
    }
    std::sort(targets.begin(), targets.end(), [](const TsunamiTarget &left, const TsunamiTarget &right) {
        if (std::abs(left.print_z - right.print_z) > 1e-9)
            return left.print_z < right.print_z;
        if (left.object_layer_above != right.object_layer_above)
            return left.object_layer_above < right.object_layer_above;
        if (std::abs(left.area - right.area) > 1e-9)
            return left.area > right.area;
        return std::tie(left.center.x(), left.center.y()) < std::tie(right.center.x(), right.center.y());
    });
    return targets;
}

struct RuntimeTsunamiTrunk {
    struct TerminalRing {
        Tsunami::TargetId target_id { 0 };
        Tsunami::BranchId branch_id { 0 };
        Tsunami::SeededMicroTreePlan tree;
    };

    Tsunami::SharedTrunkPlan trunk;
    std::vector<Tsunami::ClosedMacroBranchPlan> branches;
    std::map<Tsunami::BranchId, ExPolygons> branch_target_regions;
    std::vector<Tsunami::TargetId> direct_target_ids;
    std::vector<TerminalRing> terminal_rings;
    std::vector<Tsunami::TargetInterfacePlan> interfaces;
};

enum class RuntimePlanFailureCode {
    None,
    InvalidRoot,
    MissingTarget,
    InterfaceLayerUnderflow,
    MissingLayerZ,
    EmptySupportEnds,
    NoPrintableTurn,
    TopologyAssignment,
    InvalidTurnAssignment,
    BranchGeometry,
    BranchAttachment,
    MissingTerminalSource,
    MicroTreeGeometry,
    TerminalRingCollision,
    SharedTrunkCrossesInterface,
    MissingSupportFootprint,
    IncompleteTargetCoverage,
    InterfaceGeometry,
    RepeatedRejectedAssignment,
    ExhaustedAssignments
};

struct RuntimePlanFailure {
    RuntimePlanFailureCode code { RuntimePlanFailureCode::None };
    Tsunami::TargetId target_id { size_t(-1) };
    Tsunami::TurnId turn_id { size_t(-1) };
    size_t layer_index { size_t(-1) };
    std::optional<Tsunami::MacroBranchGeometryFailureReason> branch_geometry_reason;
};

struct RuntimeTsunamiTrunkResult {
    std::optional<RuntimeTsunamiTrunk> plan;
    RuntimePlanFailure failure;
};

const char *runtime_failure_name(RuntimePlanFailureCode code)
{
    switch (code) {
    case RuntimePlanFailureCode::None: return "none";
    case RuntimePlanFailureCode::InvalidRoot: return "invalid_root";
    case RuntimePlanFailureCode::MissingTarget: return "missing_target";
    case RuntimePlanFailureCode::InterfaceLayerUnderflow: return "interface_layer_underflow";
    case RuntimePlanFailureCode::MissingLayerZ: return "missing_layer_z";
    case RuntimePlanFailureCode::EmptySupportEnds: return "empty_support_ends";
    case RuntimePlanFailureCode::NoPrintableTurn: return "no_printable_turn";
    case RuntimePlanFailureCode::TopologyAssignment: return "topology_assignment";
    case RuntimePlanFailureCode::InvalidTurnAssignment: return "invalid_turn_assignment";
    case RuntimePlanFailureCode::BranchGeometry: return "branch_geometry";
    case RuntimePlanFailureCode::BranchAttachment: return "branch_attachment";
    case RuntimePlanFailureCode::MissingTerminalSource: return "missing_terminal_source";
    case RuntimePlanFailureCode::MicroTreeGeometry: return "micro_tree_geometry";
    case RuntimePlanFailureCode::TerminalRingCollision: return "terminal_ring_collision";
    case RuntimePlanFailureCode::SharedTrunkCrossesInterface: return "shared_trunk_crosses_interface";
    case RuntimePlanFailureCode::MissingSupportFootprint: return "missing_support_footprint";
    case RuntimePlanFailureCode::IncompleteTargetCoverage: return "incomplete_target_coverage";
    case RuntimePlanFailureCode::InterfaceGeometry: return "interface_geometry";
    case RuntimePlanFailureCode::RepeatedRejectedAssignment: return "repeated_rejected_assignment";
    case RuntimePlanFailureCode::ExhaustedAssignments: return "exhausted_assignments";
    }
    return "unknown";
}

const char *macro_branch_geometry_failure_name(Tsunami::MacroBranchGeometryFailureReason reason)
{
    switch (reason) {
    case Tsunami::MacroBranchGeometryFailureReason::InvalidSourceTurn: return "invalid_source_turn";
    case Tsunami::MacroBranchGeometryFailureReason::InsufficientAnchor: return "insufficient_anchor";
    case Tsunami::MacroBranchGeometryFailureReason::TargetOutsideConvexSide: return "target_outside_convex_side";
    case Tsunami::MacroBranchGeometryFailureReason::InsufficientHeight: return "insufficient_height";
    case Tsunami::MacroBranchGeometryFailureReason::BranchAngleExceeded: return "branch_angle_exceeded";
    case Tsunami::MacroBranchGeometryFailureReason::Collision: return "collision";
    case Tsunami::MacroBranchGeometryFailureReason::PrintabilityLimited: return "printability_limited";
    }
    return "unknown";
}

RuntimeTsunamiTrunkResult runtime_failure(
    RuntimePlanFailureCode code, Tsunami::TargetId target_id = size_t(-1),
    Tsunami::TurnId turn_id = size_t(-1), size_t layer_index = size_t(-1),
    std::optional<Tsunami::MacroBranchGeometryFailureReason> branch_geometry_reason = std::nullopt)
{
    return { std::nullopt, { code, target_id, turn_id, layer_index, branch_geometry_reason } };
}

ExPolygons runtime_path_footprint(const Polyline &path, double extrusion_width)
{
    if (path.points.size() < 2 || extrusion_width <= 0.)
        return {};
    const ClipperLib::EndType end_type = path.points.front() == path.points.back()
        ? ClipperLib::etClosedLine : ClipperLib::etOpenRound;
    return union_ex(offset(
        path, float(scale_(0.5 * extrusion_width)), ClipperLib::jtRound,
        DefaultLineMiterLimit, end_type));
}

ExPolygons runtime_paths_footprint(const Polylines &paths, double extrusion_width)
{
    ExPolygons result;
    for (const Polyline &path : paths)
        expolygons_append(result, runtime_path_footprint(path, extrusion_width));
    return union_ex(result);
}

Point runtime_from_mm(const Vec2d &point)
{
    return Point(scale_(point.x()), scale_(point.y()));
}

Vec2d runtime_to_mm(const Point &point)
{
    return Vec2d(unscale<double>(point.x()), unscale<double>(point.y()));
}

const Tsunami::SupportTargetSpec *find_target_spec(
    const std::vector<Tsunami::SupportTargetSpec> &targets, Tsunami::TargetId target_id)
{
    const auto target = std::find_if(targets.begin(), targets.end(), [target_id](const Tsunami::SupportTargetSpec &item) {
        return item.id == target_id;
    });
    return target == targets.end() ? nullptr : &*target;
}

Tsunami::StraightBranchPlan make_vertical_trunk_plan(const Tsunami::SharedTrunkPlan &trunk)
{
    Tsunami::StraightBranchPlan result;
    result.layers.reserve(trunk.top_layer + 1);
    for (size_t layer_index = 0; layer_index <= trunk.top_layer; ++layer_index) {
        Tsunami::LayerPlan layer;
        layer.layer_index = layer_index;
        layer.physical_ribs = trunk.root.physical_ribs;
        layer.segments = trunk.root.segments;
        layer.path = trunk.root.path;
        result.layers.emplace_back(std::move(layer));
    }
    result.reached_target = !result.layers.empty();
    return result;
}

RuntimeTsunamiTrunkResult plan_runtime_trunk(
    const Tsunami::SharedTrunkPlan &trunk,
    const std::vector<Tsunami::SupportTargetSpec> &targets,
    const std::vector<ExPolygons> &model_by_layer,
    const std::vector<ExPolygons> &blocked_by_layer,
    const std::vector<double> &print_z_by_layer,
    double nominal_layer_height,
    double branch_angle,
    double trunk_height,
    double extrusion_width,
    double rib_spacing,
    double minimum_branch_spacing,
    size_t interface_layer_count,
    bool micro_branch_enabled,
    double micro_branch_angle,
    double micro_branch_distance,
    double micro_tip_diameter)
{
    if (trunk.root.path.points.size() < 2 || trunk.root.segments.empty() || trunk.root.physical_ribs.empty())
        return runtime_failure(RuntimePlanFailureCode::InvalidRoot);

    RuntimeTsunamiTrunk result;
    result.trunk = trunk;
    std::vector<Tsunami::SupportTargetSpec> support_end_targets;
    support_end_targets.reserve(trunk.target_plans.size());
    for (const Tsunami::SharedTrunkTargetPlan &target_plan : trunk.target_plans) {
        const Tsunami::SupportTargetSpec *target = find_target_spec(targets, target_plan.target_id);
        if (target == nullptr)
            return runtime_failure(RuntimePlanFailureCode::MissingTarget, target_plan.target_id);
        Tsunami::SupportTargetSpec support_end = *target;
        if (interface_layer_count > 0) {
            if (support_end.layer_index < interface_layer_count)
                return runtime_failure(
                    RuntimePlanFailureCode::InterfaceLayerUnderflow, target->id,
                    size_t(-1), support_end.layer_index);
            support_end.layer_index -= interface_layer_count;
            if (support_end.layer_index >= print_z_by_layer.size())
                return runtime_failure(
                    RuntimePlanFailureCode::MissingLayerZ, target->id,
                    size_t(-1), support_end.layer_index);
            support_end.print_z = print_z_by_layer[support_end.layer_index];
        }
        support_end_targets.emplace_back(support_end);
    }

    if (support_end_targets.empty())
        return runtime_failure(RuntimePlanFailureCode::EmptySupportEnds);
    const auto highest_support_end = std::max_element(
        support_end_targets.begin(), support_end_targets.end(),
        [](const Tsunami::SupportTargetSpec &left, const Tsunami::SupportTargetSpec &right) {
            return left.layer_index < right.layer_index;
        });
    result.trunk.top_layer = highest_support_end->layer_index;
    result.trunk.top_z = highest_support_end->print_z;

    const Tsunami::StraightBranchPlan vertical_trunk = make_vertical_trunk_plan(result.trunk);
    std::vector<Tsunami::TrunkTurnCandidate> candidates =
        Tsunami::collect_trunk_turn_candidates(vertical_trunk, print_z_by_layer);
    for (Tsunami::TrunkTurnCandidate &candidate : candidates)
        candidate.printable = candidate.print_z + 1e-9 >= std::max(0., trunk_height);
    const ExPolygons trunk_footprint = runtime_path_footprint(
        result.trunk.root.path, extrusion_width);
    // One constant used to answer three different questions, which is why
    // widening it under micro changed the problem rather than the solution.
    // They are separated here.
    //
    // How much of the target the model already holds up. Our support strategy
    // cannot change that, so this never widens under micro. It used to, because
    // one constant answered this and the question below at once, and shrinking
    // the demand this way is what made micro look like it solved the snug
    // overhang while covering less of it.
    const double model_clearance = 0.5 * rib_spacing + 0.5 * extrusion_width;
    // How far a support footprint carries, for crediting coverage and for
    // spacing the contact sampling. This one does widen under micro, and
    // measurement is the reason: a micro tree genuinely extends what its branch
    // holds up. The reach disc bounds how far a tree grows from its ring; this
    // is how far the overhang spans from what the tree leaves behind.
    //
    // Measured 2026-08-15 on the snug overhang against an independent analytic
    // sector, uncovered mm2, macro-only being 1.84:
    //
    //   model wide,   credit wide     6.04    terminal ring passes
    //   model narrow, credit narrow   2.59    terminal ring fails
    //   model wide,   credit narrow   6.76    terminal ring fails
    //   model narrow, credit wide     0.78    terminal ring passes
    //
    // The two act in opposite directions on the two fixtures, which is why one
    // constant could not serve both and why splitting them was the fix rather
    // than picking a better single value.
    const double maximum_bridge_distance =
        (micro_branch_enabled
             ? 0.75 * std::max(micro_branch_distance, micro_tip_diameter)
             : 0.5 * rib_spacing) +
        0.5 * extrusion_width;
    const auto model_supported = [model_clearance](const ExPolygons &footprint) {
        return model_clearance > 0.
            ? offset_ex(
                footprint,
                float(scale_(model_clearance) + SCALED_EPSILON),
                ClipperLib::jtRound)
            : footprint;
    };
    const auto bridgeable = [maximum_bridge_distance](const ExPolygons &footprint) {
        return maximum_bridge_distance > 0.
            ? offset_ex(
                footprint,
                float(scale_(maximum_bridge_distance) + SCALED_EPSILON),
                ClipperLib::jtRound)
            : footprint;
    };
    const double minimum_resolvable_span = 0.01 * extrusion_width;
    const double minimum_resolvable_area =
        double(scale_(minimum_resolvable_span)) * double(scale_(minimum_resolvable_span));
    const auto remove_numerical_slivers = [minimum_resolvable_area](ExPolygons &regions) {
        remove_small_and_small_holes(regions, minimum_resolvable_area);
    };
    const auto layer_for = [](const Tsunami::ClosedMacroBranchPlan &branch, size_t layer_index) {
        const auto layer = std::lower_bound(
            branch.layers.begin(), branch.layers.end(), layer_index,
            [](const Tsunami::ClosedMacroBranchLayer &item, size_t index) {
                return item.layer_index < index;
            });
        return layer != branch.layers.end() && layer->layer_index == layer_index ? &*layer : nullptr;
    };

    // Everything a micro tree grown from this branch's terminal ring can touch on
    // the target layer. A tree only has the height above the ring, so this is a
    // disc of that reach about the ring centre. Two callers share it so that what
    // a branch is credited with and what its residue is judged against cannot
    // drift apart: the coverage loop clips credit to it, and the residue gate
    // asks whether the leftover falls inside it.
    const auto micro_reach_disc = [&](const Tsunami::ClosedMacroBranchPlan &branch,
                                      size_t target_layer, double target_print_z)
        -> std::optional<ExPolygon> {
        if (branch.first_target_layer == size_t(-1))
            return std::nullopt;
        const Tsunami::ClosedMacroBranchLayer *ring_layer =
            layer_for(branch, branch.first_target_layer);
        if (ring_layer == nullptr)
            return std::nullopt;
        const std::optional<Tsunami::TerminalRingPlan> ring = Tsunami::plan_terminal_ring(
            ring_layer->active_turn, branch.first_target_layer, branch.first_target_layer);
        if (!ring)
            return std::nullopt;
        const size_t start_layer = Tsunami::micro_tree_first_start_layer(
            branch.first_target_layer, target_layer, print_z_by_layer, extrusion_width);
        if (start_layer == size_t(-1))
            return std::nullopt;
        const double reach = Tsunami::micro_tree_reach_from(
            start_layer, target_layer, target_print_z, print_z_by_layer,
            micro_branch_angle, extrusion_width, 0.5);
        if (reach <= 0.)
            return std::nullopt;
        Polygon disc = make_circle_num_segments(scale_(reach), 64);
        disc.translate(ring->center);
        return ExPolygon(disc);
    };

    struct CoverageCandidate {
        size_t source_segment_index { 0 };
        Point endpoint;
        Tsunami::ClosedMacroBranchPlan branch;
        ExPolygons bridgeable_region;
    };
    // One branch per source turn leaves a source occupied by whichever branch won
    // it, and the winner is usually a long one that only completes high up. Its
    // terminal ring is then born late and has little height left for a micro tree,
    // which measured as residue 4.19 mm from the nearest ring against a 3.8 mm
    // reach. A second branch on the same turn completes lower and puts a ring
    // where the tree can still grow: on the hollow gear it takes the residue
    // outside micro reach from 1.50 mm2 to zero, and a third changes nothing.
    //
    // Only when micro is on. The payoff is the lower ring, so with micro off the
    // extra branch has no job -- measured, it adds two branches to hollow gear
    // target 1 and moves the uncovered area by nothing at all.
    const size_t maximum_branches_per_source_turn = micro_branch_enabled ? 2 : 1;
    std::map<size_t, size_t> source_turn_use_count;
    Tsunami::BranchId next_branch_id = 1;
    // Base and tip of each accepted branch, keyed by source segment so adjacent
    // modules can be paired. Used by the fan-gap acceptance in the coverage loop.
    struct BranchSpan {
        size_t source_segment_index { 0 };
        Point base;
        Point tip;
    };
    std::vector<BranchSpan> branch_spans;
    // Targets whose remaining demand is inter-branch fan gaps rather than demand
    // the branches never reached. Read by the final coverage gate further down.
    std::set<Tsunami::TargetId> fan_gap_accepted_targets;

    for (const Tsunami::SharedTrunkTargetPlan &target_plan : result.trunk.target_plans) {
        const Tsunami::SupportTargetSpec *target = find_target_spec(targets, target_plan.target_id);
        const Tsunami::SupportTargetSpec *support_end =
            find_target_spec(support_end_targets, target_plan.target_id);
        if (target == nullptr || support_end == nullptr)
            return runtime_failure(RuntimePlanFailureCode::MissingTarget, target_plan.target_id);

        const ExPolygons model_coverage = support_end->layer_index < model_by_layer.size()
            ? model_supported(model_by_layer[support_end->layer_index]) : ExPolygons {};
        Tsunami::SupportTargetSpec support_demand = *support_end;
        support_demand.region = diff_ex(target->region, model_coverage);
        remove_numerical_slivers(support_demand.region);
        ExPolygons uncovered = diff_ex(support_demand.region, bridgeable(trunk_footprint));
        remove_numerical_slivers(uncovered);
        // Spans belong to the target currently being covered.
        branch_spans.clear();
        if (uncovered.empty()) {
            result.direct_target_ids.emplace_back(target->id);
            continue;
        }
        if (std::none_of(candidates.begin(), candidates.end(), [](const Tsunami::TrunkTurnCandidate &candidate) {
                return candidate.printable;
            }))
            return runtime_failure(RuntimePlanFailureCode::NoPrintableTurn, target->id);

        const std::optional<std::vector<Point>> sampled = Tsunami::sample_target_contact_points(
            support_demand, std::max(extrusion_width, maximum_bridge_distance), 512);
        if (!sampled) {
            // PERMANENT DIAGNOSTIC -- this path and the final gate below both
            // returned IncompleteTargetCoverage with no log, so a target that
            // could not be sampled was indistinguishable from branches that
            // could not cover it. That is why the zero-angle fixture sat
            // undiagnosed for the whole session. Do not delete.
            BOOST_LOG_TRIVIAL(warning)
                << "Tsunami contact sampling failed:"
                << " target=" << target->id
                << " demand_mm2=" << std::abs(area(support_demand.region)) * SCALING_FACTOR * SCALING_FACTOR
                << " spacing_mm=" << std::max(extrusion_width, maximum_bridge_distance)
                << " -- the target could not be sampled, which is not the same"
                   " as branches failing to cover it.";
            return runtime_failure(RuntimePlanFailureCode::IncompleteTargetCoverage, target->id);
        }

        struct RankedEndpoint {
            Point point;
            ExPolygons estimated_region;
            double estimated_coverage { 0. };
            double lateral_error { 0. };
            double forward_distance { 0. };
        };
        struct SourceCoverageSet {
            size_t source_segment_index { 0 };
            std::vector<RankedEndpoint> endpoints;
        };

        std::vector<SourceCoverageSet> source_sets;
        std::set<size_t> source_segments;
        for (const Tsunami::TrunkTurnCandidate &candidate : candidates)
            if (candidate.printable &&
                source_turn_use_count[candidate.source_segment_index] < maximum_branches_per_source_turn)
                source_segments.emplace(candidate.source_segment_index);

        for (size_t source_segment_index : source_segments) {
            if (source_segment_index >= trunk.root.segments.size())
                continue;
            const auto source = std::find_if(
                candidates.begin(), candidates.end(), [source_segment_index](const Tsunami::TrunkTurnCandidate &item) {
                    return item.source_segment_index == source_segment_index;
                });
            if (source == candidates.end())
                continue;

            SourceCoverageSet source_set;
            source_set.source_segment_index = source_segment_index;
            source_set.endpoints.reserve(sampled->size());
            Vec2d outward = source->outward_direction;
            if (outward.norm() <= 1e-9)
                continue;
            outward.normalize();
            // A branch module grows along its source rib direction and never
            // rotates toward the target, so a demand point belongs to the segment
            // whose normal passes through it, and only points inside the module's
            // own served width are its business.
            //
            // Half the rib pitch, even though the module is measurably narrower
            // than that: its rails are carved from the source turn and bounded by
            // the 0.5 support ratio at birth, giving a 0.65-0.84 mm gap and a
            // ~1.21 mm footprint against the 1.5 mm pitch. Narrowing the claim to
            // match was tried on 2026-08-15 and is **rejected by measurement**:
            // at `0.5 * extrusion_width + maximum_bridge_distance` the hollow gear
            // dropped from 14 branches and 55.81 mm2 uncovered to 2 branches and
            // 294.13 mm2, because points just outside the narrow band then belong
            // to no segment at all. The generous claim lets neighbouring modules
            // and bridging finish what one module starts.
            const double served_half_width = 0.5 * rib_spacing + 0.5 * extrusion_width;
            for (const Point &point : *sampled) {
                const Vec2d delta = runtime_to_mm(point) - runtime_to_mm(source->center);
                const double forward_distance = delta.dot(outward);
                if (delta.norm() <= 1e-9 || forward_distance < -1e-9)
                    continue;
                const double lateral_error = std::abs(
                    delta.x() * outward.y() - delta.y() * outward.x());
                if (lateral_error > served_half_width)
                    continue;
                // Estimate the module that will actually be built: along the fixed
                // direction, stopping at the point's forward projection.
                Polyline centerline;
                centerline.points = { source->center,
                    runtime_from_mm(runtime_to_mm(source->center) + forward_distance * outward) };
                if (centerline.points.front() == centerline.points.back())
                    continue;
                ExPolygons estimated_region = intersection_ex(
                    support_demand.region,
                    bridgeable(runtime_path_footprint(centerline, extrusion_width)));
                const double estimated_coverage = std::abs(area(estimated_region));
                if (estimated_coverage > 0.)
                    source_set.endpoints.push_back({
                        point, std::move(estimated_region), estimated_coverage,
                        lateral_error, forward_distance });
            }
            std::sort(source_set.endpoints.begin(), source_set.endpoints.end(),
                [](const RankedEndpoint &left, const RankedEndpoint &right) {
                if (std::abs(left.estimated_coverage - right.estimated_coverage) > 0.5)
                    return left.estimated_coverage > right.estimated_coverage;
                if (std::abs(left.lateral_error - right.lateral_error) > 1e-9)
                    return left.lateral_error < right.lateral_error;
                if (std::abs(left.forward_distance - right.forward_distance) > 1e-9)
                    return left.forward_distance > right.forward_distance;
                return std::tie(left.point.x(), left.point.y()) <
                       std::tie(right.point.x(), right.point.y());
            });
            if (!source_set.endpoints.empty())
                source_sets.emplace_back(std::move(source_set));
        }

        using CoverageCacheKey = std::tuple<size_t, coord_t, coord_t>;
        std::map<CoverageCacheKey, std::optional<CoverageCandidate>> coverage_cache;
        // PERMANENT DIAGNOSTIC -- do not delete when clearing temporary probes.
        // A coverage stall drops the whole object to Normal support, and the
        // existing failure log reports only the target, which says nothing about
        // why the trunk could not reach it. These counters cost a few increments
        // on the success path and are emitted only on the failure path, next to
        // that log. Rebuilding them by hand has already cost three separate
        // sessions; the numbers they print are the ones the handoff quotes.
        size_t coverage_evaluated = 0;
        size_t coverage_no_turn = 0;
        size_t coverage_no_plan = 0;
        // PERMANENT DIAGNOSTIC -- no_plan lumped every geometric refusal into one
        // number, so a stall where the branch planner never succeeds looked the
        // same whatever the reason. ClosedMacroBranchResult already carries the
        // reason, so count it: on the zero-angle fixture all 244 refusals turned
        // out to be a single reason, which no amount of staring at no_plan could
        // have said. Do not delete.
        std::map<Tsunami::MacroBranchGeometryFailureReason, size_t> coverage_no_plan_reasons;
        const auto branch_failure_name = [](Tsunami::MacroBranchGeometryFailureReason reason) {
            switch (reason) {
            case Tsunami::MacroBranchGeometryFailureReason::InvalidSourceTurn:       return "invalid_source_turn";
            case Tsunami::MacroBranchGeometryFailureReason::InsufficientAnchor:      return "insufficient_anchor";
            case Tsunami::MacroBranchGeometryFailureReason::TargetOutsideConvexSide: return "target_outside_convex_side";
            case Tsunami::MacroBranchGeometryFailureReason::InsufficientHeight:      return "insufficient_height";
            case Tsunami::MacroBranchGeometryFailureReason::BranchAngleExceeded:     return "branch_angle_exceeded";
            case Tsunami::MacroBranchGeometryFailureReason::Collision:               return "collision";
            case Tsunami::MacroBranchGeometryFailureReason::PrintabilityLimited:     return "printability_limited";
            }
            return "unknown";
        };
        size_t coverage_no_top_layer = 0;
        size_t coverage_support_empty = 0;
        // Whether a single trunk turn can serve an endpoint within the branch
        // angle. Shared so the endpoint ranking below and the precise planner
        // apply exactly the same reachability test.
        const auto turn_reaches_endpoint = [&](const Tsunami::TrunkTurnCandidate &candidate,
                                               const Point &endpoint) {
            if (!candidate.printable || candidate.layer_index >= support_end->layer_index)
                return false;
            const double available_height = support_end->print_z - candidate.print_z;
            if (available_height <= 1e-9)
                return false;
            const double distance = (runtime_to_mm(endpoint) -
                                     runtime_to_mm(candidate.center)).norm();
            return std::atan2(distance, available_height) * 180. / std::acos(-1.) <=
                   std::clamp(branch_angle, 0., 89.) + 1e-9;
        };
        const auto endpoint_is_reachable = [&](size_t source_segment_index, const Point &endpoint) {
            for (const Tsunami::TrunkTurnCandidate &candidate : candidates)
                if (candidate.source_segment_index == source_segment_index &&
                    turn_reaches_endpoint(candidate, endpoint))
                    return true;
            return false;
        };
        const auto plan_candidate = [&](size_t source_segment_index, const Point &endpoint)
            -> CoverageCandidate * {
                const CoverageCacheKey key {
                    source_segment_index, endpoint.x(), endpoint.y() };
                auto [cached, inserted] = coverage_cache.try_emplace(key);
                if (!inserted)
                    return cached->second ? &*cached->second : nullptr;
                ++coverage_evaluated;
                std::vector<const Tsunami::TrunkTurnCandidate *> turn_candidates;
                for (const Tsunami::TrunkTurnCandidate &candidate : candidates) {
                    if (!candidate.printable || candidate.source_segment_index != source_segment_index ||
                        candidate.layer_index >= support_end->layer_index)
                        continue;
                    const double available_height = support_end->print_z - candidate.print_z;
                    const double distance = (runtime_to_mm(endpoint) -
                                             runtime_to_mm(candidate.center)).norm();
                    if (available_height <= 1e-9 ||
                        std::atan2(distance, available_height) * 180. / std::acos(-1.) >
                            std::clamp(branch_angle, 0., 89.) + 1e-9)
                        continue;
                    turn_candidates.emplace_back(&candidate);
                }
                std::sort(turn_candidates.begin(), turn_candidates.end(),
                    [micro_branch_enabled](const Tsunami::TrunkTurnCandidate *left,
                                           const Tsunami::TrunkTurnCandidate *right) {
                        if (left->layer_index != right->layer_index)
                            return micro_branch_enabled ? left->layer_index < right->layer_index
                                                        : left->layer_index > right->layer_index;
                        return left->id < right->id;
                    });
                if (turn_candidates.size() > 1)
                    turn_candidates.resize(1);
                if (turn_candidates.empty())
                    ++coverage_no_turn;

                std::optional<Tsunami::ClosedMacroBranchPlan> planned;
                for (const Tsunami::TrunkTurnCandidate *candidate : turn_candidates) {
                    if (candidate->layer_index >= vertical_trunk.layers.size() ||
                        candidate->source_segment_index >=
                            vertical_trunk.layers[candidate->layer_index].segments.size())
                        continue;
                    Tsunami::ClosedMacroBranchInput branch_input;
                    branch_input.target_id = target->id;
                    branch_input.source_turn = vertical_trunk.layers[candidate->layer_index]
                                                   .segments[candidate->source_segment_index];
                    branch_input.target = endpoint;
                    branch_input.birth_layer = candidate->layer_index;
                    branch_input.target_layer = support_end->layer_index;
                    branch_input.birth_z = candidate->print_z;
                    branch_input.target_z = support_end->print_z;
                    branch_input.layer_height = nominal_layer_height;
                    branch_input.branch_angle = branch_angle;
                    branch_input.extrusion_width = extrusion_width;
                    branch_input.minimum_layer_support_ratio = 0.5;
                    branch_input.anchor_length = trunk.root.rib_length;
                    branch_input.minimum_anchor_length = std::max(2. * extrusion_width, rib_spacing);
                    branch_input.minimum_branch_spacing = minimum_branch_spacing;
                    branch_input.complete_early = micro_branch_enabled;
                    branch_input.blocked_region_by_layer = blocked_by_layer;
                    branch_input.print_z_by_layer = print_z_by_layer;
                    Tsunami::ClosedMacroBranchResult branch_result =
                        Tsunami::plan_closed_macro_branch(branch_input);
                    // Accept shorter branches too: what matters downstream is the
                    // demand the branch actually supports, which is measured below.
                    if (!branch_result.plan && branch_result.failure)
                        ++coverage_no_plan_reasons[*branch_result.failure];
                    if (branch_result.plan) {
                        planned = std::move(*branch_result.plan);
                        break;
                    }
                }
                if (!planned) {
                    if (!turn_candidates.empty())
                        ++coverage_no_plan;
                    return nullptr;
                }
                const Tsunami::ClosedMacroBranchLayer *top_layer =
                    layer_for(*planned, support_end->layer_index);
                if (top_layer == nullptr) {
                    ++coverage_no_top_layer;
                    return nullptr;
                }
                ExPolygons supported = bridgeable(runtime_path_footprint(
                    top_layer->detour, extrusion_width));
                supported = intersection_ex(support_demand.region, supported);
                // bridgeable() credits by distance alone, but the micro tree that
                // has to span the gap grows from this branch's terminal ring and
                // only has the height above it to do so. Credit no more than that
                // tree can actually reach, or the branch is assigned demand its
                // own micro stage will reject.
                if (micro_branch_enabled && !supported.empty()) {
                    const std::optional<ExPolygon> reach_disc = micro_reach_disc(
                        *planned, support_end->layer_index, support_end->print_z);
                    if (!reach_disc)
                        supported.clear();
                    else
                        supported = intersection_ex(supported, ExPolygons { *reach_disc });
                }
                if (supported.empty()) {
                    ++coverage_support_empty;
                    return nullptr;
                }
                cached->second = CoverageCandidate {
                    source_segment_index, endpoint, std::move(*planned), std::move(supported) };
                return &*cached->second;
        };
        while (!uncovered.empty()) {
            CoverageCandidate *best = nullptr;
            ExPolygons best_gain_region;
            double best_gain = 0.;
            for (const SourceCoverageSet &source_set : source_sets) {
                if (source_turn_use_count[source_set.source_segment_index] >= maximum_branches_per_source_turn)
                    continue;
                std::vector<std::pair<double, const RankedEndpoint *>> ranked_for_uncovered;
                ranked_for_uncovered.reserve(source_set.endpoints.size());
                for (const RankedEndpoint &endpoint : source_set.endpoints) {
                    // Spend the attempt budget only on endpoints this source can
                    // actually reach. The gain estimate rewards long centerlines,
                    // which are exactly the ones that exceed the branch angle, so
                    // the ranking used to be anti-correlated with feasibility.
                    // Measured on the hollow gear: at the stall every one of the 18
                    // unused source segments had reachable endpoints, and in every
                    // case the first of them ranked 5th or worse, so all four
                    // attempts were always consumed by endpoints out of reach.
                    // Testing reachability first also skips the intersection below
                    // for those endpoints.
                    if (!endpoint_is_reachable(source_set.source_segment_index, endpoint.point))
                        continue;
                    const double estimated_gain = std::abs(area(
                        intersection_ex(uncovered, endpoint.estimated_region)));
                    if (estimated_gain > 0.)
                        ranked_for_uncovered.emplace_back(estimated_gain, &endpoint);
                }
                std::sort(ranked_for_uncovered.begin(), ranked_for_uncovered.end(),
                    [](const auto &left, const auto &right) {
                        if (std::abs(left.first - right.first) > 0.5)
                            return left.first > right.first;
                        return std::tie(left.second->point.x(), left.second->point.y()) <
                               std::tie(right.second->point.x(), right.second->point.y());
                    });
                constexpr size_t maximum_precise_attempts_per_source = 4;
                size_t attempts = 0;
                for (const auto &[estimated_gain, endpoint] : ranked_for_uncovered) {
                    (void) estimated_gain;
                    if (attempts++ >= maximum_precise_attempts_per_source)
                        break;
                    CoverageCandidate *candidate = plan_candidate(
                        source_set.source_segment_index, endpoint->point);
                    if (!candidate)
                        continue;
                    ExPolygons gain_region = intersection_ex(
                        uncovered, candidate->bridgeable_region);
                    const double gain = std::abs(area(gain_region));
                    const bool better = gain > best_gain + 0.5 ||
                        (gain > 0. && std::abs(gain - best_gain) <= 0.5 &&
                         (best == nullptr ||
                          std::tie(candidate->source_segment_index, candidate->endpoint.x(), candidate->endpoint.y()) <
                              std::tie(best->source_segment_index, best->endpoint.x(), best->endpoint.y())));
                    if (better) {
                        best = candidate;
                        best_gain = gain;
                        best_gain_region = std::move(gain_region);
                    }
                    break;
                }
            }
            // A branch is a structure spanning every layer from its source turn
            // up. Requiring only a positive gain is not a threshold at all: the
            // comparison is in scaled area, where 1 mm2 is about 1e12, so a gain
            // of 5e-13 mm2 passed. Measured at two branches per source turn, that
            // built two whole branches on hollow gear target 1 for 2.05e-05 and
            // 1.23e-06 mm2, which is why the uncovered area did not visibly move.
            //
            // One extrusion square is the floor of meaning -- below the contact a
            // single deposited segment leaves, a branch cannot change whether the
            // overhang prints. It is a lower bound on meaningful, not a judgement
            // about optimal. The branches micro genuinely needs clear it: the
            // smallest kept gain measured is 0.247 mm2 against a 0.176 mm2 floor,
            // while the rejected ones sit four orders of magnitude below.
            const double minimum_useful_gain =
                double(scale_(extrusion_width)) * double(scale_(extrusion_width));
            if (best == nullptr || best_gain < minimum_useful_gain) {
                // Every branch has grown as far as its source turn allows. What is
                // left between adjacent radial modules is the fan gap: their angular
                // pitch grows with radius while the module width does not, so
                // strictly parallel branches cannot tile an annular target. Closing
                // those gaps is Micro Branch's job in the contract and is not
                // implemented yet.
                //
                // Until it is, a plan with at least two branches is accepted and the
                // residue is reported split into the part between the branches and
                // the part beyond their tips. Note what this does NOT do: with micro
                // off, the split is logged but not enforced, so two branches are
                // enough to pass whatever the split says. That is the temporary
                // relaxation, and it is why removing it is Micro Branch's exit
                // criterion rather than a cleanup.
                // With micro enabled the claim "Micro Branch will fill this" is
                // checkable, so check it instead of counting branches: the residue
                // has to lie inside the reach of some branch's terminal ring. This
                // is the same predicate the coverage loop clips credit with. A
                // plan that scraped together two branches on a bad root leaves its
                // residue far outside every disc and is now rejected, which lets
                // root selection retry instead of rubber-stamping the plan.
                ExPolygons beyond_reach;
                bool accept_residue = branch_spans.size() >= 2;
                if (accept_residue && micro_branch_enabled) {
                    ExPolygons reach_region;
                    for (const Tsunami::ClosedMacroBranchPlan &branch : result.branches) {
                        if (branch.target_id != target->id)
                            continue;
                        if (std::optional<ExPolygon> disc = micro_reach_disc(
                                branch, support_end->layer_index, support_end->print_z))
                            reach_region.emplace_back(std::move(*disc));
                    }
                    beyond_reach = diff_ex(uncovered, union_ex(reach_region));
                    remove_numerical_slivers(beyond_reach);
                    accept_residue = beyond_reach.empty();
                }
                if (accept_residue) {
                    std::vector<BranchSpan> ordered = branch_spans;
                    std::sort(ordered.begin(), ordered.end(),
                        [](const BranchSpan &left, const BranchSpan &right) {
                            return left.source_segment_index < right.source_segment_index;
                        });
                    Polygons fan;
                    for (size_t index = 0; index + 1 < ordered.size(); ++index) {
                        Polygon quad;
                        quad.points = { ordered[index].base, ordered[index].tip,
                                        ordered[index + 1].tip, ordered[index + 1].base };
                        if (quad.area() < 0.)
                            quad.reverse();
                        fan.emplace_back(std::move(quad));
                    }
                    ExPolygons envelope = union_ex(fan);
                    for (const Tsunami::ClosedMacroBranchPlan &branch : result.branches) {
                        if (branch.target_id != target->id)
                            continue;
                        const Tsunami::ClosedMacroBranchLayer *top =
                            layer_for(branch, support_end->layer_index);
                        if (top != nullptr)
                            expolygons_append(envelope,
                                runtime_path_footprint(top->detour, extrusion_width));
                    }
                    envelope = union_ex(envelope);
                    ExPolygons beyond_tips = diff_ex(uncovered, bridgeable(envelope));
                    remove_numerical_slivers(beyond_tips);
                    fan_gap_accepted_targets.insert(target->id);
                    const double to_mm2 = SCALING_FACTOR * SCALING_FACTOR;
                    const double uncovered_mm2 = std::abs(area(uncovered)) * to_mm2;
                    const double beyond_mm2 = std::abs(area(beyond_tips)) * to_mm2;
                    BOOST_LOG_TRIVIAL(warning)
                        << "Tsunami fan-gap acceptance:"
                        << " target=" << target->id
                        << " branches=" << result.branches.size()
                        << " demand_mm2=" << std::abs(area(support_demand.region)) * to_mm2
                        << " uncovered_mm2=" << uncovered_mm2
                        << " fan_gap_mm2=" << uncovered_mm2 - beyond_mm2
                        << " beyond_tips_mm2=" << beyond_mm2
                        << " -- branches exhausted their source turns; residue accepted"
                           " pending Micro Branch. NOT a coverage guarantee.";
                }
                // PERMANENT DIAGNOSTIC -- the rejection is as informative as the
                // acceptance: it says the residue is out of every terminal ring's
                // micro reach, so this root cannot be rescued by Micro Branch and
                // the caller should try another one. Do not delete.
                if (micro_branch_enabled && !accept_residue && branch_spans.size() >= 2) {
                    const double to_mm2 = SCALING_FACTOR * SCALING_FACTOR;
                    // The largest piece says which kind of failure this is, and
                    // guessing it cost a build cycle once. A long thin piece between
                    // two rings is a fan gap the micro angle can still close; a
                    // compact blob whose half-width is well inside the reach is a
                    // sector with no branch at all, which belongs to the branch
                    // stage and cannot be fixed by anything micro does.
                    const ExPolygon *largest = nullptr;
                    for (const ExPolygon &piece : beyond_reach)
                        if (largest == nullptr ||
                            std::abs(piece.area()) > std::abs(largest->area()))
                            largest = &piece;
                    const Vec2d centroid = largest == nullptr
                        ? Vec2d::Zero() : runtime_to_mm(largest->contour.centroid());
                    const BoundingBox bounds = largest == nullptr
                        ? BoundingBox() : largest->contour.bounding_box();
                    BOOST_LOG_TRIVIAL(warning)
                        << "Tsunami micro reach rejection:"
                        << " target=" << target->id
                        << " branches=" << result.branches.size()
                        << " uncovered_mm2=" << std::abs(area(uncovered)) * to_mm2
                        << " beyond_reach_mm2=" << std::abs(area(beyond_reach)) * to_mm2
                        << " pieces=" << beyond_reach.size()
                        << " largest_mm2="
                        << (largest == nullptr ? 0. : std::abs(largest->area()) * to_mm2)
                        << " largest_centroid_mm=(" << centroid.x() << "," << centroid.y() << ")"
                        << " largest_size_mm=(" << unscale<double>(bounds.size().x())
                        << "," << unscale<double>(bounds.size().y()) << ")"
                        << " -- residue lies outside every terminal ring's micro reach;"
                           " rejecting so root selection can retry.";
                }
                // PERMANENT DIAGNOSTIC -- see the counter declarations above.
                const double to_mm2 = SCALING_FACTOR * SCALING_FACTOR;
                // `branches` counts every branch this trunk has placed so far,
                // across targets, while `uncovered` is this target's remainder
                // alone. Reading the pair as if both described one target invites
                // the conclusion that branches were added without changing the
                // outcome, so report the per-target count next to it.
                const size_t branches_for_target = size_t(std::count_if(
                    result.branches.begin(), result.branches.end(),
                    [&](const Tsunami::ClosedMacroBranchPlan &branch) {
                        return branch.target_id == target->id;
                    }));
                BOOST_LOG_TRIVIAL(warning)
                    << "Tsunami coverage stall:"
                    << " target=" << target->id
                    << " root_ribs=" << trunk.root.physical_ribs.size()
                    << " root_rib_length_mm=" << trunk.root.rib_length
                    << " root_bed_area_mm2=" << trunk.root.bed_contact_area
                    << " branches=" << result.branches.size()
                    << " branches_for_target=" << branches_for_target
                    << " demand_mm2=" << std::abs(area(support_demand.region)) * to_mm2
                    << " uncovered_mm2=" << std::abs(area(uncovered)) * to_mm2
                    << " evaluated=" << coverage_evaluated
                    << " no_turn=" << coverage_no_turn
                    << " no_plan=" << coverage_no_plan
                    << " no_plan_reasons=[" << [&]() {
                           std::string out;
                           for (const auto &[reason, count] : coverage_no_plan_reasons) {
                               if (!out.empty()) out += " ";
                               out += branch_failure_name(reason);
                               out += "=";
                               out += std::to_string(count);
                           }
                           return out;
                       }() << "]"
                    << " no_top_layer=" << coverage_no_top_layer
                    << " support_empty=" << coverage_support_empty;
                if (fan_gap_accepted_targets.count(target->id) != 0)
                    break;
                return runtime_failure(RuntimePlanFailureCode::IncompleteTargetCoverage, target->id);
            }

            const size_t selected_source_segment = best->source_segment_index;
            Tsunami::ClosedMacroBranchPlan selected_branch = best->branch;
            selected_branch.branch_id = next_branch_id++;
            result.branch_target_regions.emplace(selected_branch.branch_id, std::move(best_gain_region));
            ++source_turn_use_count[selected_source_segment];
            {
                const Tsunami::ClosedMacroBranchLayer *top =
                    layer_for(selected_branch, support_end->layer_index);
                const double frontier = top != nullptr ? top->frontier_distance : 0.;
                branch_spans.push_back({ selected_source_segment, selected_branch.attachment,
                    runtime_from_mm(runtime_to_mm(selected_branch.attachment) +
                                    frontier * selected_branch.growth_direction) });
            }
            uncovered = diff_ex(uncovered, best->bridgeable_region);
            remove_numerical_slivers(uncovered);
            result.branches.emplace_back(std::move(selected_branch));
        }
    }

    const bool paths_attach = std::all_of(
        result.branches.begin(), result.branches.end(), [&trunk](const Tsunami::ClosedMacroBranchPlan &branch) {
            return std::find(trunk.root.path.points.begin(), trunk.root.path.points.end(), branch.attachment) !=
                       trunk.root.path.points.end() &&
                   std::all_of(branch.layers.begin(), branch.layers.end(), [&branch](const Tsunami::ClosedMacroBranchLayer &layer) {
                       return layer.detour.points.size() >= 3 &&
                              layer.detour.points.front() == branch.attachment &&
                              layer.detour.points.back() == branch.attachment;
                   });
        });
    if (!paths_attach)
        return runtime_failure(RuntimePlanFailureCode::BranchAttachment);

    if (micro_branch_enabled) {
        for (const Tsunami::ClosedMacroBranchPlan &branch : result.branches) {
            const auto assigned = result.branch_target_regions.find(branch.branch_id);
            const Tsunami::SupportTargetSpec *support_end =
                find_target_spec(support_end_targets, branch.target_id);
            const auto base_layer = std::lower_bound(
                branch.layers.begin(), branch.layers.end(), branch.first_target_layer,
                [](const Tsunami::ClosedMacroBranchLayer &layer, size_t index) {
                    return layer.layer_index < index;
                });
            if (assigned == result.branch_target_regions.end() || support_end == nullptr ||
                branch.first_target_layer == size_t(-1) || base_layer == branch.layers.end() ||
                base_layer->layer_index != branch.first_target_layer)
                return runtime_failure(
                    RuntimePlanFailureCode::MissingTerminalSource, branch.target_id,
                    size_t(-1), branch.first_target_layer);

            Tsunami::SeededMicroTreeInput micro_input;
            micro_input.target_id = branch.target_id;
            micro_input.source_turn = base_layer->active_turn;
            micro_input.base_layer = branch.first_target_layer;
            micro_input.target = *support_end;
            micro_input.target.region = assigned->second;
            const std::optional<std::vector<Point>> anchors = Tsunami::sample_target_contact_points(
                micro_input.target, std::max(micro_branch_distance, micro_tip_diameter));
            if (!anchors) {
                return runtime_failure(
                    RuntimePlanFailureCode::MicroTreeGeometry, branch.target_id,
                    size_t(-1), branch.first_target_layer);
            }
            micro_input.target.center = anchors->front();
            micro_input.branch_angle = micro_branch_angle;
            micro_input.branch_distance = micro_branch_distance;
            micro_input.tip_diameter = micro_tip_diameter;
            micro_input.extrusion_width = extrusion_width;
            micro_input.minimum_layer_support_ratio = 0.5;
            micro_input.blocked_region_by_layer = blocked_by_layer;
            micro_input.print_z_by_layer = print_z_by_layer;
            std::optional<Tsunami::SeededMicroTreePlan> micro_tree =
                Tsunami::plan_seeded_micro_tree(micro_input);
            if (!micro_tree) {
                return runtime_failure(
                    RuntimePlanFailureCode::MicroTreeGeometry, branch.target_id,
                    size_t(-1), branch.first_target_layer);
            }

            const ExPolygons ring_footprint = runtime_path_footprint(
                micro_tree->seed_ring.vertical_ring, extrusion_width);
            for (size_t layer_index = micro_tree->seed_ring.base_layer;
                 layer_index <= micro_tree->seed_ring.tree_start_layer && layer_index < blocked_by_layer.size();
                 ++layer_index) {
                if (!blocked_by_layer[layer_index].empty() &&
                    !intersection_ex(ring_footprint, blocked_by_layer[layer_index]).empty())
                    return runtime_failure(
                        RuntimePlanFailureCode::TerminalRingCollision, branch.target_id,
                        size_t(-1), layer_index);
            }
            result.terminal_rings.push_back({
                branch.target_id, branch.branch_id, std::move(*micro_tree) });
        }
    }

    for (const Tsunami::SharedTrunkTargetPlan &target_plan : result.trunk.target_plans) {
        const Tsunami::SupportTargetSpec *target = find_target_spec(targets, target_plan.target_id);
        const Tsunami::SupportTargetSpec *support_end =
            find_target_spec(support_end_targets, target_plan.target_id);
        if (target == nullptr || support_end == nullptr)
            return runtime_failure(RuntimePlanFailureCode::MissingTarget, target_plan.target_id);
        if (interface_layer_count > 0 && result.trunk.top_layer > support_end->layer_index &&
            !intersection_ex(target->region, trunk_footprint).empty())
            return runtime_failure(
                RuntimePlanFailureCode::SharedTrunkCrossesInterface,
                target_plan.target_id, size_t(-1), support_end->layer_index);

        ExPolygons lower_support_footprint = trunk_footprint;
        for (const Tsunami::ClosedMacroBranchPlan &branch : result.branches) {
            if (branch.target_id != target->id)
                continue;
            if (micro_branch_enabled) {
                const auto ring = std::find_if(
                    result.terminal_rings.begin(), result.terminal_rings.end(),
                    [&branch](const RuntimeTsunamiTrunk::TerminalRing &item) {
                        return item.branch_id == branch.branch_id;
                    });
                if (ring == result.terminal_rings.end() || ring->tree.layers.empty() ||
                    ring->tree.layers.back().layer_index != support_end->layer_index)
                    return runtime_failure(
                        RuntimePlanFailureCode::MissingSupportFootprint,
                        target->id, size_t(-1), support_end->layer_index);
                expolygons_append(lower_support_footprint, runtime_paths_footprint(
                    ring->tree.layers.back().paths, extrusion_width));
            } else {
                const Tsunami::ClosedMacroBranchLayer *layer = layer_for(branch, support_end->layer_index);
                if (layer == nullptr)
                    return runtime_failure(
                        RuntimePlanFailureCode::MissingSupportFootprint,
                        target->id, size_t(-1), support_end->layer_index);
                expolygons_append(lower_support_footprint,
                                  runtime_path_footprint(layer->detour, extrusion_width));
            }
        }
        lower_support_footprint = union_ex(lower_support_footprint);
        ExPolygons load_bearing_footprint = lower_support_footprint;
        if (support_end->layer_index < model_by_layer.size())
            expolygons_append(load_bearing_footprint, model_by_layer[support_end->layer_index]);
        load_bearing_footprint = union_ex(load_bearing_footprint);
        ExPolygons final_uncovered = diff_ex(
            target->region, bridgeable(load_bearing_footprint));
        remove_numerical_slivers(final_uncovered);
        if (!final_uncovered.empty() && fan_gap_accepted_targets.count(target->id) == 0)
        {
            // PERMANENT DIAGNOSTIC -- see the sampling failure above. Do not delete.
            BOOST_LOG_TRIVIAL(warning)
                << "Tsunami final coverage gate:"
                << " target=" << target->id
                << " target_mm2=" << std::abs(area(target->region)) * SCALING_FACTOR * SCALING_FACTOR
                << " uncovered_mm2=" << std::abs(area(final_uncovered)) * SCALING_FACTOR * SCALING_FACTOR
                << " -- planning completed but the built support does not reach"
                   " all of the target.";
            return runtime_failure(
                RuntimePlanFailureCode::IncompleteTargetCoverage,
                target->id, size_t(-1), support_end->layer_index);
        }

        if (interface_layer_count > 0) {
            Tsunami::TargetInterfaceInput interface_input;
            interface_input.target = *target;
            interface_input.interface_layer_count = interface_layer_count;
            interface_input.lower_support_footprint = std::move(lower_support_footprint);
            interface_input.maximum_bridge_distance = maximum_bridge_distance;
            interface_input.blocked_region_by_layer = blocked_by_layer;
            std::optional<Tsunami::TargetInterfacePlan> interface_plan =
                Tsunami::plan_target_interface(interface_input);
            if (!interface_plan)
                return runtime_failure(
                    RuntimePlanFailureCode::InterfaceGeometry,
                    target->id, size_t(-1), support_end->layer_index);
            result.interfaces.emplace_back(std::move(*interface_plan));
        }
    }
    return { std::move(result), {} };
}

const Tsunami::ClosedMacroBranchLayer *branch_layer_at(
    const Tsunami::ClosedMacroBranchPlan &branch, size_t layer_index)
{
    const auto layer = std::lower_bound(
        branch.layers.begin(), branch.layers.end(), layer_index,
        [](const Tsunami::ClosedMacroBranchLayer &item, size_t index) { return item.layer_index < index; });
    return layer != branch.layers.end() && layer->layer_index == layer_index ? &*layer : nullptr;
}

const RuntimeTsunamiTrunk::TerminalRing *terminal_ring_for_branch(
    const RuntimeTsunamiTrunk &runtime, Tsunami::BranchId branch_id)
{
    const auto ring = std::find_if(
        runtime.terminal_rings.begin(), runtime.terminal_rings.end(),
        [branch_id](const RuntimeTsunamiTrunk::TerminalRing &item) { return item.branch_id == branch_id; });
    return ring == runtime.terminal_rings.end() ? nullptr : &*ring;
}

const Tsunami::SeededMicroTreeLayer *micro_tree_layer_at(
    const Tsunami::SeededMicroTreePlan &tree, size_t layer_index)
{
    const auto layer = std::lower_bound(
        tree.layers.begin(), tree.layers.end(), layer_index,
        [](const Tsunami::SeededMicroTreeLayer &item, size_t index) { return item.layer_index < index; });
    return layer != tree.layers.end() && layer->layer_index == layer_index ? &*layer : nullptr;
}

struct RuntimeInterfaceEmission {
    Tsunami::TargetId target_id { 0 };
    size_t layer_index { 0 };
    size_t interface_number { 1 };
    ExtrusionRole role { erSupportMaterialInterface };
    Polylines paths;
};

std::optional<RuntimeInterfaceEmission> make_runtime_interface_emission(
    Tsunami::TargetId target_id,
    const Tsunami::TargetInterfaceLayer &layer,
    ExPolygons regions,
    size_t interface_total,
    const PrintObjectConfig &config,
    const SupportParameters &support_params,
    const Layer &object_layer)
{
    if (regions.empty())
        return RuntimeInterfaceEmission { target_id, layer.layer_index, layer.interface_number };

    const bool use_sublayer_pattern = support_interface_sublayer_selected(
        config.support_interface_sublayer_pattern.value,
        config.support_interface_sublayer_start_layer.value,
        config.support_interface_sublayer_end_layer.value,
        int(layer.interface_number), int(interface_total));
    const SupportMaterialInterfacePattern effective_pattern = use_sublayer_pattern
        ? config.support_interface_sublayer_pattern_type.value
        : config.support_interface_pattern.value;
    std::unique_ptr<Fill> filler(Fill::new_from_type(
        interface_pattern_to_fill_pattern(effective_pattern, support_params)));
    if (!filler)
        return std::nullopt;

    const Flow flow = support_params.support_material_interface_flow.with_height(float(object_layer.height));
    filler->set_bounding_box(get_extents(regions));
    filler->layer_id = layer.layer_index;
    // Match the established support-interface phase contract: keep nominal
    // spacing constant even when the synchronized support layer height varies.
    filler->spacing = support_params.support_material_interface_flow.spacing();
    filler->angle = use_sublayer_pattern
        ? Geometry::deg2rad(float(config.support_interface_sublayer_angle.value))
        : support_params.interface_angle;
    if (!use_sublayer_pattern && effective_pattern == smipRectilinearInterlaced) {
        filler->fixed_angle = true;
        filler->angle = support_params.base_angle +
            ((layer.interface_number - 1) & 1 ? float(M_PI_2) : 0.f);
    }

    RuntimeInterfaceEmission result;
    result.target_id = target_id;
    result.layer_index = layer.layer_index;
    result.interface_number = layer.interface_number;
    result.role = use_sublayer_pattern
        ? erSupportMaterialInterfaceSublayer
        : erSupportMaterialInterface;
    FillParams fill_params;
    fill_params.density = float(support_params.top_interface_density);
    fill_params.dont_adjust = true;
    fill_params.dont_sort = effective_pattern == smipGrid ||
                            effective_pattern == smipTriangles ||
                            effective_pattern == smipRectilinearInterlaced;
    fill_params.flow = flow;
    fill_params.extrusion_role = result.role;
    if (effective_pattern == smipTriangles) {
        fill_params.density_per_direction = true;
        fill_params.anchor_length = 0.f;
        fill_params.anchor_length_max = 0.f;
    }

    for (ExPolygon &region : regions) {
        Surface surface(stInternal, std::move(region));
        try {
            Polylines paths = filler->fill_surface(&surface, fill_params);
            result.paths.insert(result.paths.end(),
                std::make_move_iterator(paths.begin()), std::make_move_iterator(paths.end()));
        } catch (InfillFailedException &) {
            return std::nullopt;
        }
    }
    if (result.paths.empty())
        return std::nullopt;
    return result;
}

bool splice_closed_detour(Polyline &path, const Tsunami::ClosedMacroBranchPlan &branch,
                          const Tsunami::ClosedMacroBranchLayer &layer)
{
    if (layer.detour.points.size() < 3 || layer.detour.points.front() != branch.attachment ||
        layer.detour.points.back() != branch.attachment)
        return false;
    const auto attachment = std::find(path.points.begin(), path.points.end(), branch.attachment);
    if (attachment == path.points.end())
        return false;
    path.points.insert(attachment + 1, layer.detour.points.begin() + 1, layer.detour.points.end());
    return true;
}

Polylines split_at_semantic_anchors(const Polyline &path, const std::vector<Point> &anchors)
{
    if (path.points.size() < 2 || anchors.empty())
        return { path };

    std::set<std::pair<coord_t, coord_t>> anchor_coordinates;
    for (const Point &anchor : anchors)
        anchor_coordinates.emplace(anchor.x(), anchor.y());

    Polylines result;
    size_t begin = 0;
    for (size_t index = 1; index < path.points.size(); ++index) {
        const Point &point = path.points[index];
        const bool is_anchor = anchor_coordinates.count({ point.x(), point.y() }) != 0;
        if (!is_anchor && index + 1 < path.points.size())
            continue;

        Polyline section;
        section.points.assign(path.points.begin() + begin, path.points.begin() + index + 1);
        if (section.points.size() >= 2 && section.points.front() != section.points.back())
            result.emplace_back(std::move(section));
        begin = index;
    }
    return result;
}

void append_segment_anchors(std::vector<Point> &anchors, const Tsunami::PathSegment &segment)
{
    if (segment.polyline.points.size() < 2)
        return;
    anchors.emplace_back(segment.polyline.points.front());
    anchors.emplace_back(segment.polyline.points.back());
}

void append_branch_anchors(std::vector<Point> &anchors,
                           const Tsunami::ClosedMacroBranchPlan &branch,
                           const Tsunami::ClosedMacroBranchLayer &layer)
{
    anchors.emplace_back(branch.attachment);
    append_segment_anchors(anchors, branch.source_cap);
    append_segment_anchors(anchors, layer.active_turn);
}

std::optional<ExPolygons> runtime_body_footprint_at(
    const RuntimeTsunamiTrunk &runtime, size_t layer_index, double extrusion_width)
{
    if (layer_index > runtime.trunk.top_layer)
        return ExPolygons {};

    Polyline complete_path = runtime.trunk.root.path;
    ExPolygons branch_footprints;
    for (const Tsunami::ClosedMacroBranchPlan &branch : runtime.branches) {
        const Tsunami::ClosedMacroBranchLayer *layer = branch_layer_at(branch, layer_index);
        if (layer == nullptr)
            continue;
        const RuntimeTsunamiTrunk::TerminalRing *ring = terminal_ring_for_branch(runtime, branch.branch_id);
        ExPolygons current_branch;
        if (ring != nullptr && layer_index > ring->tree.seed_ring.base_layer) {
            if (ring->tree.seed_ring.emits_vertical_ring(layer_index))
                current_branch = runtime_path_footprint(
                    ring->tree.seed_ring.vertical_ring, extrusion_width);
            else if (const Tsunami::SeededMicroTreeLayer *tree_layer =
                         micro_tree_layer_at(ring->tree, layer_index))
                current_branch = runtime_paths_footprint(tree_layer->paths, extrusion_width);
        } else {
            current_branch = runtime_path_footprint(layer->detour, extrusion_width);
            if (ring != nullptr && ring->tree.seed_ring.emits_base_complement(layer_index)) {
                expolygons_append(current_branch, runtime_path_footprint(
                    ring->tree.seed_ring.base_complement, extrusion_width));
                current_branch = union_ex(current_branch);
            }
            if (!splice_closed_detour(complete_path, branch, *layer)) {
                return std::nullopt;
            }
        }
        if (current_branch.empty())
            continue;
        expolygons_append(branch_footprints, std::move(current_branch));
    }

    ExPolygons footprint = runtime_path_footprint(complete_path, extrusion_width);
    expolygons_append(footprint, branch_footprints);
    return union_ex(footprint);
}

bool runtime_trunks_are_disjoint(
    const std::vector<RuntimeTsunamiTrunk> &trunks, double extrusion_width)
{
    size_t layer_count = 0;
    for (const RuntimeTsunamiTrunk &runtime : trunks)
        layer_count = std::max(layer_count, runtime.trunk.top_layer + 1);

    for (size_t layer_index = 0; layer_index < layer_count; ++layer_index) {
        std::vector<ExPolygons> footprints;
        for (const RuntimeTsunamiTrunk &runtime : trunks) {
            std::optional<ExPolygons> footprint = runtime_body_footprint_at(
                runtime, layer_index, extrusion_width);
            if (!footprint)
                return false;
            if (footprint->empty())
                continue;
            for (const ExPolygons &previous : footprints) {
                const ExPolygons overlap = intersection_ex(previous, *footprint);
                if (!overlap.empty()) {
                    return false;
                }
            }
            footprints.emplace_back(std::move(*footprint));
        }
    }
    return true;
}

enum class GenerationFailureStage {
    UnsupportedLayerSchedule,
    NoTargets,
    InvalidTargetLayer,
    UnassignedTarget,
    RuntimePlanning,
    MissingTarget,
    IndependentRootPlanning,
    EmptyRuntimePlan,
    OverlappingTrunks,
    InvalidInterfaceLayer,
    InterfaceEmission,
    BodyInterfaceCollision
};

const char *generation_failure_name(GenerationFailureStage stage)
{
    switch (stage) {
    case GenerationFailureStage::UnsupportedLayerSchedule: return "unsupported_layer_schedule";
    case GenerationFailureStage::NoTargets: return "no_targets";
    case GenerationFailureStage::InvalidTargetLayer: return "invalid_target_layer";
    case GenerationFailureStage::UnassignedTarget: return "unassigned_target";
    case GenerationFailureStage::RuntimePlanning: return "runtime_planning";
    case GenerationFailureStage::MissingTarget: return "missing_target";
    case GenerationFailureStage::IndependentRootPlanning: return "independent_root_planning";
    case GenerationFailureStage::EmptyRuntimePlan: return "empty_runtime_plan";
    case GenerationFailureStage::OverlappingTrunks: return "overlapping_trunks";
    case GenerationFailureStage::InvalidInterfaceLayer: return "invalid_interface_layer";
    case GenerationFailureStage::InterfaceEmission: return "interface_emission";
    case GenerationFailureStage::BodyInterfaceCollision: return "body_interface_collision";
    }
    return "unknown";
}

void log_generation_failure(
    GenerationFailureStage stage, Tsunami::TargetId target_id = size_t(-1),
    size_t layer_index = size_t(-1), const RuntimePlanFailure *runtime_failure = nullptr,
    const Tsunami::SupportTargetSpec *target = nullptr)
{
    std::string message = std::string("Tsunami planning failed: stage=") + generation_failure_name(stage);
    if (target_id != size_t(-1))
        message += " target=" + std::to_string(target_id);
    if (layer_index != size_t(-1))
        message += " layer=" + std::to_string(layer_index);
    if (runtime_failure != nullptr) {
        message += std::string(" reason=") + runtime_failure_name(runtime_failure->code);
        if (runtime_failure->target_id != size_t(-1))
            message += " runtime_target=" + std::to_string(runtime_failure->target_id);
        if (runtime_failure->turn_id != size_t(-1))
            message += " turn=" + std::to_string(runtime_failure->turn_id);
        if (runtime_failure->layer_index != size_t(-1))
            message += " runtime_layer=" + std::to_string(runtime_failure->layer_index);
        if (runtime_failure->branch_geometry_reason)
            message += std::string(" geometry_reason=") +
                       macro_branch_geometry_failure_name(*runtime_failure->branch_geometry_reason);
    }
    if (target != nullptr) {
        const BoundingBox bounds = get_extents(target->region);
        const double area_mm2 = std::abs(area(target->region)) * SCALING_FACTOR * SCALING_FACTOR;
        message += " target_center_mm=(" + std::to_string(unscale<double>(target->center.x())) +
                   "," + std::to_string(unscale<double>(target->center.y())) + ")";
        message += " target_bbox_mm=(" + std::to_string(unscale<double>(bounds.min.x())) +
                   "," + std::to_string(unscale<double>(bounds.min.y())) + ")-(" +
                   std::to_string(unscale<double>(bounds.max.x())) + "," +
                   std::to_string(unscale<double>(bounds.max.y())) + ")";
        message += " target_area_mm2=" + std::to_string(area_mm2);
    }
    BOOST_LOG_TRIVIAL(warning) << message;
}

} // namespace

void TsunamiSupport::generate()
{
    std::vector<Tsunami::SupportTargetSpec> target_specs;
    const auto generate_normal_fallback = [this, &target_specs](
        GenerationFailureStage stage, Tsunami::TargetId target_id = size_t(-1),
        size_t layer_index = size_t(-1), const RuntimePlanFailure *runtime_failure = nullptr) {
        const Tsunami::TargetId diagnostic_target_id =
            runtime_failure != nullptr && runtime_failure->target_id != size_t(-1)
                ? runtime_failure->target_id : target_id;
        log_generation_failure(
            stage, target_id, layer_index, runtime_failure,
            find_target_spec(target_specs, diagnostic_target_id));
        PrintObjectSupportMaterial fallback(&m_object, m_slicing_params);
        fallback.generate(m_object);
    };

    // Runtime Tsunami paths currently use object-layer indices as their Z
    // clock. Independent support heights require a separate support-layer
    // schedule; using object Z values would silently move contact geometry.
    if (m_object.print()->config().independent_support_layer_height.value) {
        generate_normal_fallback(GenerationFailureStage::UnsupportedLayerSchedule);
        return;
    }

    const PrintObjectConfig &config = m_object.config();
    const SupportParameters support_params(m_object);
    const double extrusion_width = support_params.support_material_flow.width();

    SupportGeneratorLayerStorage contact_layer_storage;
    PrintObjectSupportMaterial contact_detector(&m_object, m_slicing_params);
    // Tsunami's build-plate-only contract constrains roots to the bed. It must
    // not discard targets whose vertical projection is blocked, because the
    // branch planner is specifically responsible for reaching around them.
    const SupportGeneratorLayersPtr top_contacts =
        contact_detector.detect_top_contact_layers(contact_layer_storage, false);
    const std::vector<TsunamiTarget> targets = find_support_targets(m_object, top_contacts);
    if (targets.empty()) {
        generate_normal_fallback(GenerationFailureStage::NoTargets);
        return;
    }

    throw_on_cancel();
    const double spacing = config.tsunami_rib_spacing.value;
    const double actual_spacing = std::max(spacing, extrusion_width);
    const double nominal_layer_height = std::max(m_slicing_params.layer_height, 1e-6);
    const size_t interface_count = size_t(std::max(0, config.support_interface_top_layers.value));
    const double micro_branch_size = std::max(
        config.tsunami_micro_branch_size.value, extrusion_width);
    const double micro_tip_diameter = std::max(
        2. * extrusion_width, 0.4 * micro_branch_size);

    Polygon bed_region = get_bed_shape_with_excluded_area(m_object.print()->config());
    const Vec3d plate_offset = m_object.print()->get_plate_origin();
    bed_region.translate(Point(scale_(plate_offset.x()), scale_(plate_offset.y())) - m_object.instances().front().shift);

    std::vector<ExPolygons> model_by_layer;
    model_by_layer.reserve(m_object.layers().size());
    std::vector<ExPolygons> blocked_by_layer;
    blocked_by_layer.reserve(m_object.layers().size());
    for (const Layer *layer : m_object.layers()) {
        model_by_layer.emplace_back(layer->lslices);
        blocked_by_layer.emplace_back(offset_ex(
            layer->lslices, scale_(config.support_object_xy_distance.value)));
    }
    std::vector<double> print_z_by_layer;
    print_z_by_layer.reserve(m_object.layers().size());
    for (const Layer *layer : m_object.layers())
        print_z_by_layer.emplace_back(layer->print_z);

    target_specs.reserve(targets.size());
    // The user picks the trunk's wave pitch (tsunami_rib_spacing) and the total
    // radial width of its band (tsunami_trunk_thickness). Straight rib depth is
    // derived from those two, because it is the band minus what the U-turns at
    // each end already occupy: the turns bulge inward and outward by one turn
    // radius each, and root_turn_radius() is max(0.5 * extrusion width,
    // 0.25 * pitch). Mirrored here because that helper is scoped to the planner.
    //
    // Rib depth used to come from the largest target's bounding box, which had
    // nothing to do with bed anchoring: on the hollow gear it asked for 44 mm
    // ribs, pushed the root's first-layer footprint past
    // tsunami_max_bed_contact_area, and made select_root_candidate's shrink loop
    // trim the trunk *arc* instead of the depth, leaving the target only
    // partially covered.
    const double trunk_turn_allowance = 2. * std::max(0.5 * extrusion_width, 0.25 * actual_spacing);
    const double rib_length = std::max(
        config.tsunami_trunk_thickness.value - trunk_turn_allowance, extrusion_width);
    for (size_t target_index = 0; target_index < targets.size(); ++target_index) {
        const TsunamiTarget &target = targets[target_index];
        if (target.layer_count == 0 || target.layer_count > m_object.layers().size()) {
            generate_normal_fallback(
                GenerationFailureStage::InvalidTargetLayer, target_index, target.layer_count);
            return;
        }
        Tsunami::SupportTargetSpec spec;
        spec.id = target_index;
        spec.layer_index = target.layer_count - 1;
        spec.print_z = target.print_z;
        spec.center = target.center;
        spec.region = ExPolygons { target.region };
        target_specs.emplace_back(std::move(spec));
    }


    Tsunami::SharedTrunkInput shared_input;
    shared_input.bed_region = bed_region;
    shared_input.blocked_region = blocked_by_layer.front();
    shared_input.blocked_region_by_layer = blocked_by_layer;
    shared_input.print_z_by_layer = print_z_by_layer;
    shared_input.targets = target_specs;
    shared_input.layer_height = nominal_layer_height;
    shared_input.branch_angle = config.tsunami_branch_angle.value;
    shared_input.trunk_height = config.tsunami_trunk_height.value;
    shared_input.rib_spacing = actual_spacing;
    shared_input.rib_length = rib_length;
    shared_input.extrusion_width = extrusion_width;
    shared_input.minimum_layer_support_ratio = 0.5;
    shared_input.minimum_bed_contact_area = config.tsunami_min_bed_contact_area.value;
    shared_input.maximum_bed_contact_area = config.tsunami_max_bed_contact_area.value;
    shared_input.allow_direct_projection = !config.tsunami_micro_branch_enabled.value;

    const Tsunami::SharedTrunkForestPlan forest = Tsunami::plan_shared_trunks(shared_input);
    if (!forest.unassigned_target_ids.empty()) {
        generate_normal_fallback(
            GenerationFailureStage::UnassignedTarget, forest.unassigned_target_ids.front());
        return;
    }

    std::vector<RuntimeTsunamiTrunk> runtime_trunks;
    // Bed footprint of every root accepted so far, fed back into the next
    // independent trunk's root selection so trunks do not claim the same bed.
    ExPolygons reserved_root_regions;
    for (const Tsunami::SharedTrunkPlan &trunk : forest.trunks) {
        throw_on_cancel();
        RuntimeTsunamiTrunkResult runtime = plan_runtime_trunk(
            trunk, target_specs, model_by_layer, blocked_by_layer, print_z_by_layer, nominal_layer_height,
            config.tsunami_branch_angle.value, config.tsunami_trunk_height.value,
            extrusion_width, actual_spacing, config.tsunami_branch_minimum_spacing.value, interface_count,
            config.tsunami_micro_branch_enabled.value,
            config.tsunami_micro_branch_angle.value, micro_branch_size,
            micro_tip_diameter);
        if (runtime.plan) {
            runtime_trunks.emplace_back(std::move(*runtime.plan));
            continue;
        }

        // A shared root may be reachable in the coarse pass but have no valid
        // target/turn pairing. Preserve isolation by retrying its targets as
        // independent Tsunami trunks before considering the safe Normal fallback.
        if (trunk.target_ids.size() <= 1) {
            generate_normal_fallback(
                GenerationFailureStage::RuntimePlanning,
                trunk.target_ids.empty() ? size_t(-1) : trunk.target_ids.front(),
                size_t(-1), &runtime.failure);
            return;
        }
        for (Tsunami::TargetId target_id : trunk.target_ids) {
            const Tsunami::SupportTargetSpec *target = find_target_spec(target_specs, target_id);
            if (target == nullptr) {
                generate_normal_fallback(GenerationFailureStage::MissingTarget, target_id);
                return;
            }
            Tsunami::SharedTrunkInput single_input = shared_input;
            single_input.targets = { *target };
            // Independent trunks are planned one after another against the same
            // model outline, so each must keep clear of the bed the previous ones
            // already claimed. runtime_trunks_are_disjoint rejects the whole object
            // otherwise, and on the hollow gear that rejection was the only thing
            // left standing between the two half-ring trunks and a valid plan.
            single_input.reserved_region = reserved_root_regions;
            // Every trunk in this retry follows the same model outline, so none may
            // pad its arc past its own target's angular extent: that margin is
            // where the neighbours go. Suppressing it keeps the arcs complementary
            // instead of letting the reservation trim whichever trunk is planned
            // last, which cost that trunk 6 of its 31 ribs.
            single_input.shares_contour = trunk.target_ids.size() > 1;
            std::optional<Tsunami::SharedTrunkPlan> single_trunk = Tsunami::plan_shared_trunk(single_input);
            if (!single_trunk) {
                generate_normal_fallback(
                    GenerationFailureStage::IndependentRootPlanning, target_id);
                return;
            }
            runtime = plan_runtime_trunk(
                *single_trunk, target_specs, model_by_layer, blocked_by_layer, print_z_by_layer, nominal_layer_height,
                config.tsunami_branch_angle.value, config.tsunami_trunk_height.value,
                extrusion_width, actual_spacing, config.tsunami_branch_minimum_spacing.value, interface_count,
                config.tsunami_micro_branch_enabled.value,
                config.tsunami_micro_branch_angle.value, micro_branch_size,
                micro_tip_diameter);
            if (!runtime.plan) {
                generate_normal_fallback(
                    GenerationFailureStage::RuntimePlanning, target_id,
                    size_t(-1), &runtime.failure);
                return;
            }
            expolygons_append(reserved_root_regions,
                runtime_path_footprint(runtime.plan->trunk.root.path, extrusion_width));
            reserved_root_regions = union_ex(reserved_root_regions);
            runtime_trunks.emplace_back(std::move(*runtime.plan));
        }
    }
    if (runtime_trunks.empty()) {
        generate_normal_fallback(GenerationFailureStage::EmptyRuntimePlan);
        return;
    }
    if (!runtime_trunks_are_disjoint(runtime_trunks, extrusion_width)) {
        generate_normal_fallback(GenerationFailureStage::OverlappingTrunks);
        return;
    }

    struct InterfaceLayerRef {
        Tsunami::TargetId target_id { 0 };
        const Tsunami::TargetInterfaceLayer *layer { nullptr };
    };
    std::vector<InterfaceLayerRef> interface_layer_refs;
    for (const RuntimeTsunamiTrunk &runtime : runtime_trunks)
        for (const Tsunami::TargetInterfacePlan &interface_plan : runtime.interfaces)
            for (const Tsunami::TargetInterfaceLayer &layer : interface_plan.layers)
                interface_layer_refs.push_back({ interface_plan.target_id, &layer });
    std::sort(interface_layer_refs.begin(), interface_layer_refs.end(),
        [](const InterfaceLayerRef &left, const InterfaceLayerRef &right) {
            return std::tie(left.layer->layer_index, left.layer->interface_number, left.target_id) <
                   std::tie(right.layer->layer_index, right.layer->interface_number, right.target_id);
        });

    std::vector<ExPolygons> occupied_interface_regions(m_object.layers().size());
    std::vector<RuntimeInterfaceEmission> interface_emissions;
    for (const InterfaceLayerRef &ref : interface_layer_refs) {
        if (ref.layer == nullptr || ref.layer->layer_index >= m_object.layers().size()) {
            generate_normal_fallback(
                GenerationFailureStage::InvalidInterfaceLayer, ref.target_id,
                ref.layer == nullptr ? size_t(-1) : ref.layer->layer_index);
            return;
        }
        ExPolygons regions = occupied_interface_regions[ref.layer->layer_index].empty()
            ? ref.layer->regions
            : diff_ex(ref.layer->regions, occupied_interface_regions[ref.layer->layer_index]);
        if (regions.empty())
            continue;
        ExPolygons claimed = regions;
        std::optional<RuntimeInterfaceEmission> emission = make_runtime_interface_emission(
            ref.target_id, *ref.layer, std::move(regions), interface_count,
            config, support_params, *m_object.layers()[ref.layer->layer_index]);
        if (!emission) {
            generate_normal_fallback(
                GenerationFailureStage::InterfaceEmission, ref.target_id, ref.layer->layer_index);
            return;
        }
        expolygons_append(occupied_interface_regions[ref.layer->layer_index], claimed);
        occupied_interface_regions[ref.layer->layer_index] =
            union_ex(occupied_interface_regions[ref.layer->layer_index]);
        interface_emissions.emplace_back(std::move(*emission));
    }
    for (size_t layer_index = 0; layer_index < occupied_interface_regions.size(); ++layer_index) {
        if (occupied_interface_regions[layer_index].empty())
            continue;
        for (const RuntimeTsunamiTrunk &runtime : runtime_trunks) {
            std::optional<ExPolygons> body_footprint = runtime_body_footprint_at(
                runtime, layer_index, extrusion_width);
            if (!body_footprint ||
                !intersection_ex(*body_footprint, occupied_interface_regions[layer_index]).empty()) {
                generate_normal_fallback(
                    GenerationFailureStage::BodyInterfaceCollision, size_t(-1), layer_index);
                return;
            }
        }
    }

    m_object.clear_support_layers();
    size_t layer_count = std::max_element(runtime_trunks.begin(), runtime_trunks.end(),
        [](const RuntimeTsunamiTrunk &left, const RuntimeTsunamiTrunk &right) {
            return left.trunk.top_layer < right.trunk.top_layer;
        })->trunk.top_layer + 1;
    for (const RuntimeInterfaceEmission &emission : interface_emissions)
        layer_count = std::max(layer_count, emission.layer_index + 1);
    const auto append_path = [&support_params](SupportLayer *support_layer, Polyline polyline,
                                               const std::vector<Point> &semantic_anchors,
                                               ExtrusionRole role, const Layer &object_layer) {
        const bool interface_path = role == erSupportMaterialInterface ||
                                    role == erSupportMaterialInterfaceSublayer;
        Flow flow = (interface_path ? support_params.support_material_interface_flow
                                    : support_params.support_material_flow)
                        .with_height(float(object_layer.height));
        const Polylines sections = split_at_semantic_anchors(polyline, semantic_anchors);
        if (sections.size() <= 1) {
            auto *path = new ExtrusionPath(role,
                                           flow.mm3_per_mm(), float(flow.width()), float(flow.height()));
            path->polyline = Polyline3(std::move(polyline));
            support_layer->support_fills.entities.emplace_back(path);
            return;
        }

        auto *multipath = new ExtrusionMultiPath;
        multipath->paths.reserve(sections.size());
        for (const Polyline &section : sections) {
            ExtrusionPath path(role, flow.mm3_per_mm(), float(flow.width()), float(flow.height()));
            path.polyline = Polyline3(section);
            multipath->paths.emplace_back(std::move(path));
        }
        support_layer->support_fills.entities.emplace_back(multipath);
    };

    for (size_t index = 0; index < layer_count; ++index) {
        throw_on_cancel();
        const Layer *object_layer = m_object.layers()[index];
        bool has_interface = false;
        struct EmissionPath {
            Polyline polyline;
            ExtrusionRole role { erSupportMaterial };
            std::vector<Point> semantic_anchors;
        };
        std::vector<EmissionPath> emission_paths;
        for (const RuntimeTsunamiTrunk &runtime : runtime_trunks) {
            if (index > runtime.trunk.top_layer)
                continue;
            Polyline trunk_path = runtime.trunk.root.path;
            std::vector<Point> trunk_anchors;
            for (const Tsunami::PathSegment &segment : runtime.trunk.root.segments)
                append_segment_anchors(trunk_anchors, segment);
            std::vector<EmissionPath> detached_branches;
            for (const Tsunami::ClosedMacroBranchPlan &branch : runtime.branches) {
                const Tsunami::ClosedMacroBranchLayer *branch_layer = branch_layer_at(branch, index);
                if (branch_layer == nullptr)
                    continue;
                const RuntimeTsunamiTrunk::TerminalRing *ring =
                    terminal_ring_for_branch(runtime, branch.branch_id);
                if (ring != nullptr && index > ring->tree.seed_ring.base_layer) {
                    if (ring->tree.seed_ring.emits_vertical_ring(index)) {
                        detached_branches.push_back({
                            ring->tree.seed_ring.vertical_ring, erSupportMaterial, {} });
                    } else if (const Tsunami::SeededMicroTreeLayer *tree_layer =
                                   micro_tree_layer_at(ring->tree, index)) {
                        for (const Polyline &path : tree_layer->paths)
                            detached_branches.push_back({ path, erSupportMaterial, {} });
                    }
                    continue;
                }
                const bool spliced = splice_closed_detour(trunk_path, branch, *branch_layer);
                std::vector<Point> branch_anchors;
                append_branch_anchors(branch_anchors, branch, *branch_layer);
                if (spliced)
                    trunk_anchors.insert(trunk_anchors.end(), branch_anchors.begin(), branch_anchors.end());
                else
                    detached_branches.push_back({
                        branch_layer->detour, erSupportMaterial, std::move(branch_anchors) });
                if (ring != nullptr && ring->tree.seed_ring.emits_base_complement(index))
                    detached_branches.push_back({
                        ring->tree.seed_ring.base_complement, erSupportMaterial, {} });
            }
            emission_paths.push_back({
                std::move(trunk_path), erSupportMaterial, std::move(trunk_anchors) });
            emission_paths.insert(emission_paths.end(),
                                  std::make_move_iterator(detached_branches.begin()),
                                  std::make_move_iterator(detached_branches.end()));
        }

        for (const RuntimeInterfaceEmission &interface_emission : interface_emissions) {
            if (interface_emission.layer_index != index)
                continue;
            for (const Polyline &path : interface_emission.paths)
                emission_paths.push_back({ path, interface_emission.role, {} });
        }

        for (EmissionPath &emission : emission_paths) {
            if (emission.polyline.points.size() < 2)
                continue;
            if (index % 2 == 1)
                emission.polyline.reverse();
            has_interface = has_interface || emission.role == erSupportMaterialInterface ||
                                             emission.role == erSupportMaterialInterfaceSublayer;
        }
        SupportLayer *support_layer = m_object.add_support_layer(
            int(index), has_interface ? int(index) : -1, object_layer->height, object_layer->print_z);
        for (EmissionPath &emission : emission_paths) {
            if (emission.polyline.points.size() >= 2)
                append_path(support_layer, std::move(emission.polyline), emission.semantic_anchors,
                            emission.role, *object_layer);
        }
        support_layer->support_fills.no_sort = true;
        support_layer->support_islands = union_ex(support_layer->support_fills.polygons_covered_by_spacing(float(SCALED_EPSILON)));
        support_layer->lslices = support_layer->support_islands;
    }
}

} // namespace Slic3r
