// CuraEngine's normal ("area") support, ported from CuraEngine src/support.cpp at commit 47b0388
// (2026-10-15, AGPL-3.0, like OrcaSlicer). The support, roof and bottom areas follow CuraEngine's
// AreaSupport::generateSupportAreasForMesh, generateSupportRoof and generateSupportBottom; the
// areas are then printed with Orca's support toolpaths. Cura works in micrometres; every Cura
// constant below is converted to Orca's scaled units at its declaration.

#include "CuraSupport.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportCommon.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/SupportParameters.hpp"

#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>

namespace Slic3r {
namespace ModHost {
namespace CuraSupport {

namespace {

// Cura micrometres in Orca's scaled units (SCALING_FACTOR is mm per scaled unit; 1 um = 1e-3 mm).
inline coord_t um(double micrometres) { return coord_t(std::llround(micrometres * 1e-3 / SCALING_FACTOR)); }

// Cura's Shape::offset defaults: mitre joins with a mitre limit of 1.2.
Polygons cura_offset(const Polygons &polygons, coord_t distance, JoinType join = jtMiter)
{
    if (distance == 0)
        return polygons;
    return offset(polygons, float(distance), join, join == jtMiter ? 1.2 : DefaultMiterLimit);
}

// Shape::createTubeShape(inner, outer): the band from `inner` inside to `outer` outside the outline.
Polygons tube(const Polygons &polygons, coord_t inner, coord_t outer)
{
    return diff(cura_offset(polygons, outer), cura_offset(polygons, -inner));
}

// Shape::removeSmallAreas without hole removal: drop islands (with their holes) under `min_area_mm2`.
Polygons remove_small_areas(const Polygons &polygons, double min_area_mm2)
{
    if (min_area_mm2 <= 0. || polygons.empty())
        return polygons;
    // Scaled area of one square millimetre.
    const double min_area = min_area_mm2 / (SCALING_FACTOR * SCALING_FACTOR);
    ExPolygons islands = union_ex(polygons);
    islands.erase(std::remove_if(islands.begin(), islands.end(), [min_area](const ExPolygon &e) { return std::abs(e.contour.area()) < min_area; }),
                  islands.end());
    return to_polygons(islands);
}

double bounded_tan(double angle_rad)
{
    // Cura's boundedTan: tan clamped away from +-90 degrees.
    constexpr double limit = M_PI / 2. - 0.0001;
    return std::tan(std::clamp(angle_rad, -limit, limit));
}

template<class Fn> void parallel_layers(size_t begin, size_t end, const PrintObject &object, Fn &&fn)
{
    tbb::parallel_for(tbb::blocked_range<size_t>(begin, end), [&](const tbb::blocked_range<size_t> &range) {
        for (size_t i = range.begin(); i < range.end(); ++i) {
            if (object.print()->canceled()) throw CanceledException();
            fn(i);
        }
    });
}

struct Inputs
{
    // Orca settings in Cura's terms.
    coord_t layer_thickness;       // layer_height
    coord_t z_distance_top;        // support_top_distance
    coord_t z_distance_bottom;     // support_bottom_distance
    coord_t xy_distance;           // support_xy_distance
    coord_t support_line_width;    // support_line_width
    coord_t extension_offset;      // support_offset
    double  support_angle_rad;     // support_angle, from vertical
    bool    automatic;             // support_type auto (not enforcers only)
    bool    platform_only;         // support_type == PLATFORM_ONLY
    size_t  roof_layer_count;      // support_roof_height / layer_height
    size_t  bottom_layer_count;    // support_bottom_height / layer_height
    coord_t roof_line_width;
    coord_t bottom_line_width;
    Polygon machine_border;        // printable area, for conical support

    std::vector<Polygons> outlines;  // storage.getLayerOutlines per layer
    std::vector<Polygons> enforcers; // force_overhang
    std::vector<Polygons> blockers;  // anti_overhang
};

Inputs gather_inputs(const PrintObject &object, const ModSettings &settings)
{
    const PrintObjectConfig &config = object.config();
    Inputs in;
    in.layer_thickness   = scale_(config.layer_height.value);
    in.z_distance_top    = scale_(config.support_top_z_distance.value);
    in.z_distance_bottom = scale_(config.support_bottom_z_distance.value);
    in.xy_distance       = scale_(config.support_object_xy_distance.value);
    in.extension_offset  = scale_(config.support_expansion.value);
    // Orca's threshold is measured from the horizontal and 0 means automatic; Cura's support_angle
    // is measured from the vertical. Cura's own default (50 degrees) stands in for automatic.
    const int threshold  = config.support_threshold_angle.value;
    in.support_angle_rad = Geometry::deg2rad(threshold > 0 ? 90. - double(std::min(threshold, 89)) : 50.);
    in.automatic         = is_auto(config.support_type.value);
    in.platform_only     = config.support_on_build_plate_only.value;

    const Flow support_flow   = support_material_flow(&object);
    const Flow interface_flow = support_material_interface_flow(&object);
    in.support_line_width = coord_t(support_flow.scaled_width());
    in.roof_line_width    = coord_t(interface_flow.scaled_width());
    in.bottom_line_width  = in.roof_line_width;

    const double layer_height = config.layer_height.value;
    in.roof_layer_count   = size_t(std::max(0, config.support_interface_top_layers.value));
    const int bottom      = config.support_interface_bottom_layers.value;
    in.bottom_layer_count = size_t(std::max(0, bottom < 0 ? config.support_interface_top_layers.value : bottom));
    (void) layer_height;

    const auto &area = object.print()->config().printable_area.values;
    for (const Vec2d &p : area)
        in.machine_border.points.emplace_back(scale_(p.x()), scale_(p.y()));
    // The printable area is in bed coordinates; the object's layers are in object coordinates.
    in.machine_border.translate(-object.center_offset());

    const size_t layer_count = object.layer_count();
    in.outlines.resize(layer_count);
    for (size_t i = 0; i < layer_count; ++i)
        in.outlines[i] = to_polygons(object.get_layer(int(i))->lslices);

    in.enforcers = object.slice_support_enforcers();
    in.blockers  = object.slice_support_blockers();
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, in.enforcers);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, in.blockers);
    in.enforcers.resize(layer_count);
    in.blockers.resize(layer_count);
    (void) settings;
    return in;
}

// AreaSupport::computeBasicAndFullOverhang.
std::pair<Polygons, Polygons> basic_and_full_overhang(const Inputs &in, size_t layer_idx, double layer_height_mm)
{
    const Polygons &outlines = in.outlines[layer_idx];
    // Overhang is averaged over 0.4 mm of height so textures on vertical walls get no support.
    constexpr double smooth_height_mm = 0.4;
    const size_t     layers_below     = size_t(std::lround(smooth_height_mm / layer_height_mm));
    const coord_t    max_dist         = coord_t(bounded_tan(in.support_angle_rad) * double(in.layer_thickness));

    Polygons outlines_below = cura_offset(in.outlines[layer_idx - 1], max_dist);
    for (size_t k = 2; k <= layers_below && k <= layer_idx; ++k)
        outlines_below = union_(outlines_below, cura_offset(in.outlines[layer_idx - k], max_dist * coord_t(k)));

    Polygons basic = in.automatic ? diff(outlines, outlines_below) : Polygons{};
    if (!in.blockers[layer_idx].empty())
        basic = diff(basic, union_(in.blockers[layer_idx]));
    if (!in.enforcers[layer_idx].empty())
        basic = union_(basic, in.enforcers[layer_idx]);

    // +0.1 mm so it joins the support from the layer above more easily.
    const Polygons extended = cura_offset(basic, max_dist * coord_t(layers_below) + um(100.));
    return {basic, intersection(extended, outlines)};
}

// AreaSupport::generateVaryingXYDisallowedArea: the X/Y distance shrinks towards
// support_distance * tan(support_angle) where the wall below is sloped.
Polygons varying_xy_disallowed_area(const Inputs &in, size_t layer_idx, const ModSettings &)
{
    const coord_t close_dist = um(20.);
    auto closed = [&](size_t idx) { return polygons_simplify(cura_offset(cura_offset(in.outlines[idx], -close_dist), close_dist), um(25.)); };
    const Polygons layer_current = closed(layer_idx);

    struct Delta { double support_distance; double delta_z; Polygons layer; };
    std::vector<Delta> deltas;
    if (layer_idx > 0)
        deltas.push_back({double(in.z_distance_bottom), -double(in.layer_thickness), closed(layer_idx - 1)});
    if (layer_idx + 1 < in.outlines.size())
        deltas.push_back({double(in.z_distance_top), double(in.layer_thickness), closed(layer_idx + 1)});

    std::vector<std::pair<size_t, double>> acc;
    for (const Polygon &poly : layer_current)
        acc.resize(acc.size() + poly.size(), {0, 0.});
    for (const Delta &d : deltas) {
        const double natural = d.support_distance * bounded_tan(in.support_angle_rad);
        size_t       i       = 0;
        for (const Polygon &poly : layer_current)
            for (const Point &p : poly.points) {
                double min_dist2 = std::numeric_limits<double>::max();
                for (const Polygon &dp : d.layer)
                    for (size_t k = 0; k < dp.size(); ++k) {
                        const Line l(dp.points[k], dp.points[(k + 1) % dp.size()]);
                        min_dist2 = std::min(min_dist2, l.distance_to_squared(p));
                    }
                const double slope     = std::sqrt(min_dist2) / d.delta_z;
                const double wall      = std::atan(std::abs(slope));
                const double ratio     = std::clamp(wall / in.support_angle_rad, 0., 1.);
                const double varying   = double(in.xy_distance) + (natural - double(in.xy_distance)) * ratio;
                acc[i].first += 1;
                acc[i].second += varying;
                ++i;
            }
    }

    // Shape::offsetMulti: each edge moves out by the distances at its two ends.
    Polygons moved;
    size_t   i = 0;
    for (const Polygon &poly : layer_current) {
        if (poly.empty())
            continue;
        Polygon out;
        const size_t n         = poly.size();
        auto         dist_at   = [&](size_t j) { return acc[i + j].first == 0 ? double(in.xy_distance) : acc[i + j].second / double(acc[i + j].first); };
        Point        prev_p    = poly.points.back();
        double       prev_dist = dist_at(n - 1);
        for (size_t j = 0; j < n; ++j) {
            const Point  p    = poly.points[j];
            const double dist = dist_at(j);
            const Vec2d  dir  = (prev_p - p).cast<double>();
            // Edges shorter than 10 um are skipped, as Cura does.
            if (dir.squaredNorm() > double(um(10.)) * double(um(10.))) {
                const Vec2d unit = dir.normalized();
                const Vec2d ccw(-unit.y(), unit.x());
                out.points.emplace_back((prev_p.cast<double>() + ccw * prev_dist).cast<coord_t>());
                out.points.emplace_back((p.cast<double>() + ccw * dist).cast<coord_t>());
            }
            prev_p    = p;
            prev_dist = dist;
        }
        moved.emplace_back(std::move(out));
        i += n;
    }
    // Clipper's positive fill unions the self-overlapping offset rings, as SimplifyPolygons does in Cura.
    moved = union_(moved, pftPositive);
    const coord_t smooth = in.xy_distance / 2;
    return cura_offset(cura_offset(moved, smooth), -smooth);
}

// AreaSupport::join.
Polygons join(const Inputs &in, const ModSettings &s, const Polygons &layer_up, const Polygons &layer_this, coord_t half_min_feature)
{
    Polygons joined;
    const double conical_angle = Geometry::deg2rad(s.conical_angle_deg);
    if (s.conical_enabled && conical_angle != 0.) {
        const coord_t conical_offset = conical_angle > 0 ? -coord_t(bounded_tan(conical_angle) * double(in.layer_thickness))
                                                         : coord_t(bounded_tan(-conical_angle) * double(in.layer_thickness));
        const coord_t smallest = scale_(s.conical_min_width_mm);
        const Polygons insetted    = cura_offset(layer_up, -smallest / 2);
        const Polygons small_parts = diff(layer_up, cura_offset(insetted, smallest / 2 + um(20.)));
        Polygons border{in.machine_border};
        joined = intersection(union_(union_(layer_this, cura_offset(layer_up, conical_offset)), small_parts), border);
    } else {
        joined = union_(layer_this, layer_up);
    }
    const coord_t join_distance = scale_(s.join_distance_mm);
    if (join_distance > 0) {
        joined = union_(cura_offset(cura_offset(cura_offset(joined, -half_min_feature), join_distance + half_min_feature, jtRound),
                                    -join_distance, jtRound),
                        joined);
    }
    return polygons_simplify(joined, um(25.));
}

// AreaSupport::moveUpFromModel.
void move_up_from_model(const Inputs &in, Polygons &stair_removal, const Polygons &sloped_areas, Polygons &support_areas, size_t layer_idx,
                        size_t bottom_empty_layer_count, size_t stair_step_layer_count, coord_t stair_step_width)
{
    if (layer_idx < bottom_empty_layer_count)
        return;
    if (bottom_empty_layer_count == 0 && stair_step_layer_count <= 1)
        return;
    const size_t    bottom_layer_nr = layer_idx - bottom_empty_layer_count;
    const Polygons &bottom_outline  = in.outlines[bottom_layer_nr];
    Polygons        to_be_removed;
    if (stair_step_layer_count <= 1) {
        to_be_removed = bottom_outline;
    } else {
        to_be_removed = union_(stair_removal, bottom_outline);
        if (layer_idx % stair_step_layer_count == 0) {
            const Polygons supporting_bottom = bottom_layer_nr > 0 ? in.outlines[bottom_layer_nr - 1] : Polygons{};
            const Polygons allowed_step      = intersection(cura_offset(supporting_bottom, stair_step_width), sloped_areas);
            const long long step_bottom      = (long long) bottom_layer_nr - (long long) stair_step_layer_count + 1;
            stair_removal = step_bottom >= 0 ? intersection(in.outlines[size_t(step_bottom)], allowed_step) : allowed_step;
        }
    }
    support_areas = diff(support_areas, to_be_removed);
}

// AreaSupport::handleWallStruts.
void handle_wall_struts(const ModSettings &s, Polygons &layer_this)
{
    const double max_supported = scale_(s.tower_max_supported_mm);
    const coord_t half_tower   = scale_(s.tower_diameter_mm) / 2;
    Polygons struts;
    for (const Polygon &poly : layer_this) {
        if (poly.size() >= 6)
            continue;
        double best_length2 = -1.;
        size_t best         = 0;
        for (size_t i = 0; i < poly.size(); ++i) {
            const double l2 = (poly.points[i] - poly.points[(i + 1) % poly.size()]).cast<double>().squaredNorm();
            if (l2 > best_length2) {
                best_length2 = l2;
                best         = i;
            }
        }
        if (best_length2 < max_supported * max_supported)
            break; // a small area, not a wall (Cura stops looking here too)
        const double area  = poly.area();
        const double width = std::sqrt(area * area / best_length2);
        if (width < max_supported) {
            const Point mid = (poly.points[best] + poly.points[(best + 1) % poly.size()]) / 2;
            Polygon strut;
            strut.points = {mid + Point(half_tower, half_tower), mid + Point(-half_tower, half_tower), mid + Point(-half_tower, -half_tower),
                            mid + Point(half_tower, -half_tower)};
            struts.emplace_back(std::move(strut));
        }
    }
    if (!struts.empty())
        layer_this = union_(layer_this, struts);
}

// AreaSupport::handleTowers.
void handle_towers(const Inputs &in, const ModSettings &s, const Polygons &xy_disallowed, Polygons &layer_this, std::vector<Polygons> &tower_roofs,
                   std::vector<std::vector<Polygons>> &overhang_points, size_t layer_idx, size_t layer_count)
{
    const size_t layer_overhang_point = layer_idx + 1;
    if (layer_overhang_point + 1 >= layer_count)
        return;
    std::vector<Polygons> &here = overhang_points[layer_overhang_point];
    if (!here.empty()) {
        if (!overhang_points[layer_overhang_point - 1].empty()) {
            const coord_t max_supported = scale_(s.tower_max_supported_mm);
            for (Polygons &poly_here : here)
                for (const Polygons &poly_below : overhang_points[layer_overhang_point - 1])
                    poly_here = diff(poly_here, cura_offset(poly_below, max_supported * 2));
        }
        for (Polygons &poly : here)
            if (!poly.empty())
                tower_roofs.push_back(poly);
    }
    const double  roof_angle    = Geometry::deg2rad(s.tower_roof_angle_deg);
    const coord_t tower_diam    = scale_(s.tower_diameter_mm);
    const double  tower_area    = double(tower_diam) * double(tower_diam);
    const coord_t expansion     = roof_angle == 0. ? tower_diam / 2 : coord_t(double(in.layer_thickness) / bounded_tan(roof_angle));
    for (Polygons &roof : tower_roofs) {
        if (roof.empty())
            continue;
        layer_this = union_(layer_this, roof);
        if (area(roof) < tower_area) {
            Polygons      model_outline = xy_disallowed;
            const coord_t step          = std::max(in.support_line_width / 2, expansion);
            for (coord_t done = 0; done <= expansion; done += step) {
                roof          = cura_offset(roof, step, jtRound);
                model_outline = cura_offset(diff(model_outline, roof), step, jtRound);
                roof          = diff(roof, model_outline);
                if (roof.empty())
                    break;
                if (area(roof) >= tower_area) {
                    layer_this = union_(layer_this, roof);
                    roof.clear();
                    break;
                }
            }
        } else {
            roof.clear();
        }
    }
}

// AreaSupport::generateSupportInterfaceLayer.
Polygons interface_layer(Polygons &support_areas, const Polygons &colliding, coord_t safety_offset, coord_t outline_offset, double min_area_mm2)
{
    const Polygons model = union_(colliding);
    Polygons interface   = intersection(cura_offset(support_areas, safety_offset / 2), model);
    interface            = intersection(cura_offset(interface, safety_offset), support_areas);
    if (outline_offset != 0) {
        interface = cura_offset(interface, outline_offset);
        if (outline_offset > 0)
            interface = intersection(interface, support_areas);
    }
    interface     = remove_small_areas(interface, min_area_mm2);
    support_areas = diff(support_areas, interface);
    return interface;
}

} // namespace

ModSettings read_settings(const PrintObject &object)
{
    const DynamicPrintConfig &config = object.print()->full_print_config();
    ModSettings s;
    auto num = [&config](const char *name, double &dst) {
        if (auto v = param_value(config, std::string("cura_support.") + name); v && std::holds_alternative<double>(*v))
            dst = std::get<double>(*v);
    };
    auto flag = [&config](const char *name, bool &dst) {
        if (auto v = param_value(config, std::string("cura_support.") + name); v && std::holds_alternative<bool>(*v))
            dst = std::get<bool>(*v);
    };
    num("join_distance", s.join_distance_mm);
    num("min_feature_width", s.min_feature_width_mm);
    flag("z_overrides_xy", s.z_overrides_xy);
    num("xy_distance_overhang", s.xy_distance_overhang_mm);
    num("stair_step_height", s.stair_step_height_mm);
    num("stair_step_width", s.stair_step_width_mm);
    num("stair_step_min_slope", s.stair_step_min_slope_deg);
    num("minimum_support_area", s.minimum_support_area_mm2);
    num("minimum_roof_area", s.minimum_roof_area_mm2);
    num("minimum_bottom_area", s.minimum_bottom_area_mm2);
    num("roof_offset", s.roof_offset_mm);
    num("bottom_offset", s.bottom_offset_mm);
    flag("use_towers", s.use_towers);
    num("tower_diameter", s.tower_diameter_mm);
    num("tower_max_supported_diameter", s.tower_max_supported_mm);
    num("tower_roof_angle", s.tower_roof_angle_deg);
    flag("conical_enabled", s.conical_enabled);
    num("conical_angle", s.conical_angle_deg);
    num("conical_min_width", s.conical_min_width_mm);
    return s;
}

Areas compute_areas(const PrintObject &object, const ModSettings &s)
{
    const Inputs in          = gather_inputs(object, s);
    const size_t layer_count = in.outlines.size();
    Areas        out;
    out.support.assign(layer_count, Polygons{});
    out.roofs.assign(layer_count, Polygons{});
    out.bottoms.assign(layer_count, Polygons{});
    if (layer_count < 2)
        return out;

    const double  layer_height_mm      = object.config().layer_height.value;
    const size_t  layer_z_distance_top = size_t(in.z_distance_top / in.layer_thickness) + 1;
    if (layer_z_distance_top + 1 > layer_count)
        return out;

    // Overhangs (AreaSupport::generateOverhangAreasForMesh).
    std::vector<Polygons> overhang(layer_count), full_overhang(layer_count);
    parallel_layers(1, layer_count, object, [&](size_t i) {
        auto [basic, full] = basic_and_full_overhang(in, i, layer_height_mm);
        overhang[i]        = std::move(basic);
        full_overhang[i]   = std::move(full);
    });
    if (std::all_of(full_overhang.begin(), full_overhang.end(), [](const Polygons &p) { return p.empty(); }))
        return out;

    // Overhang points for towers (AreaSupport::detectOverhangPoints).
    std::vector<std::vector<Polygons>> overhang_points(layer_count);
    const coord_t max_tower_supported = scale_(s.tower_max_supported_mm);
    const bool    use_towers          = s.use_towers && max_tower_supported > 0;
    if (use_towers) {
        const double max_area = double(max_tower_supported) * double(max_tower_supported);
        for (size_t i = 1; i < layer_count; ++i)
            for (const ExPolygon &part : object.get_layer(int(i))->lslices) {
                if (part.contour.area() >= max_area)
                    continue;
                if (!intersection(in.outlines[i - 1], to_polygons(part)).empty())
                    continue;
                Polygons point = diff(to_polygons(part), in.blockers[i]);
                if (!point.empty())
                    overhang_points[i].push_back(std::move(point));
            }
    }

    // X/Y disallowed and sloped areas (generateSupportAreasForMesh, first parallel loop).
    const coord_t half_min_feature = (s.min_feature_width_mm > 0. ? scale_(s.min_feature_width_mm) : coord_t(0.85 * double(in.support_line_width))) + um(10.);
    const double  sloped_angle     = Geometry::deg2rad(s.stair_step_min_slope_deg);
    const coord_t sloped_width     = um(10.) + coord_t(double(in.layer_thickness) / bounded_tan(sloped_angle)) / 2;
    std::vector<Polygons> xy_disallowed(layer_count), sloped(layer_count);
    xy_disallowed[0] = cura_offset(in.outlines[0], in.xy_distance);
    parallel_layers(1, layer_count, object, [&](size_t i) {
        sloped[i] = cura_offset(cura_offset(intersection(tube(in.outlines[i - 1], sloped_width, um(10.)), tube(in.outlines[i], um(10.), sloped_width)), -um(10.)),
                                um(10.) + sloped_width);
        if (s.z_overrides_xy)
            xy_disallowed[i] = union_(cura_offset(in.outlines[i], scale_(s.xy_distance_overhang_mm)), varying_xy_disallowed_area(in, i, s));
        else
            xy_disallowed[i] = cura_offset(in.outlines[i], in.xy_distance);
    });

    const coord_t stair_step_width        = std::max<coord_t>(0, scale_(s.stair_step_width_mm));
    const size_t  bottom_empty_layer_count = size_t((in.z_distance_bottom + in.layer_thickness - 1) / in.layer_thickness);
    const coord_t stair_step_height       = std::max<coord_t>(0, scale_(s.stair_step_height_mm));
    const size_t  stair_step_layer_count  = size_t(stair_step_height / in.layer_thickness) + 1;
    if (stair_step_layer_count > 1)
        for (size_t i = 1; i < layer_count; ++i)
            if (i % stair_step_layer_count != 1)
                sloped[i] = union_(sloped[i], sloped[i - 1]);

    // Main loop, top to bottom.
    std::vector<Polygons> support(layer_count);
    std::vector<Polygons> tower_roofs;
    Polygons              stair_removal;
    for (size_t idx = layer_count - 1 - layer_z_distance_top + 1; idx-- > 0;) {
        if (object.print()->canceled()) throw CanceledException();
        Polygons layer_this = full_overhang[idx + layer_z_distance_top];
        if (in.extension_offset > 0) {
            // Grow the support and the model outline in turns, so the expansion does not fold around the model.
            Polygons      model_outline = in.outlines[idx];
            const coord_t step          = std::max<coord_t>(1, in.support_line_width / 2);
            Polygons      expansion     = layer_this;
            for (coord_t done = 0; done <= in.extension_offset; done += step) {
                expansion     = cura_offset(expansion, step);
                model_outline = cura_offset(diff(model_outline, expansion), step);
                expansion     = diff(expansion, model_outline);
            }
            layer_this = union_(layer_this, expansion);
        }
        if (use_towers) {
            handle_wall_struts(s, layer_this);
            handle_towers(in, s, xy_disallowed[idx], layer_this, tower_roofs, overhang_points, idx, layer_count);
        }
        if (idx + 1 < layer_count) {
            const Polygons model_on_layer = idx > 0 ? in.outlines[idx] : Polygons{};
            layer_this = diff(join(in, s, support[idx + 1], layer_this, half_min_feature), model_on_layer);
        }
        if (use_towers) {
            for (const ExPolygon &part : union_ex(layer_this)) {
                const Polygons p = cura_offset(cura_offset(diff(to_polygons(part), xy_disallowed[idx]), -half_min_feature), half_min_feature);
                const double part_area = area(p);
                if (part_area == 0. || part_area > double(max_tower_supported) * double(max_tower_supported))
                    continue;
                constexpr size_t tower_top_layer_count = 6; // layers after which a tiny support area needs a tower
                if (idx + tower_top_layer_count < layer_count && idx >= tower_top_layer_count + bottom_empty_layer_count)
                    tower_roofs.push_back(p);
            }
        }
        move_up_from_model(in, stair_removal, sloped[idx], layer_this, idx, bottom_empty_layer_count, stair_step_layer_count, stair_step_width);
        support[idx] = std::move(layer_this);
    }

    // X/Y distance, printability, minimum area.
    parallel_layers(0, layer_count, object, [&](size_t i) {
        Polygons &layer = support[i];
        if (!layer.empty() && !xy_disallowed[i].empty())
            layer = diff(layer, xy_disallowed[i]);
        layer = cura_offset(cura_offset(layer, -half_min_feature), half_min_feature);
        layer = remove_small_areas(layer, s.minimum_support_area_mm2);
    });

    if (in.platform_only) {
        Polygons touching = support[0];
        const double conical_angle = Geometry::deg2rad(s.conical_angle_deg);
        const bool   conical       = s.conical_enabled && conical_angle != 0.;
        const coord_t conical_offset = conical_angle > 0 ? -coord_t(bounded_tan(conical_angle) * double(in.layer_thickness))
                                                         : coord_t(bounded_tan(-conical_angle) * double(in.layer_thickness));
        for (size_t i = 1; i < layer_count; ++i) {
            if (conical)
                touching = offset(touching, float(std::abs(conical_offset) + um(10.)), jtMiter, 10.);
            touching   = intersection(support[i], touching);
            support[i] = touching;
        }
    }

    // Enforce the top Z distance.
    if (layer_z_distance_top > 1) {
        const size_t max_check = std::max<long long>(0, (long long) layer_count - (long long) (layer_z_distance_top - 1));
        parallel_layers(0, max_check, object, [&](size_t i) { support[i] = diff(support[i], in.outlines[i + layer_z_distance_top - 1]); });
    }

    // Remove floating support.
    for (size_t i = 1; i + 1 < layer_count; ++i)
        if (!support[i].empty())
            support[i] = intersection(support[i], union_(support[i + 1], support[i - 1]));

    // Roofs (AreaSupport::generateSupportRoof).
    if (in.roof_layer_count > 0) {
        const size_t z_top = size_t((in.z_distance_top + in.layer_thickness - 1) / in.layer_thickness);
        for (long long idx = (long long) layer_count - (long long) z_top - 1; idx >= 0; --idx) {
            const long long top_above = (long long) std::min(layer_count - 1, size_t(idx) + in.roof_layer_count + z_top);
            Polygons        outlines;
            for (long long above = top_above; above > idx + (long long) z_top - 1; --above)
                append(outlines, in.outlines[size_t(above)]);
            out.roofs[size_t(idx)] = interface_layer(support[size_t(idx)], outlines, in.roof_line_width, scale_(s.roof_offset_mm),
                                                    s.minimum_roof_area_mm2);
        }
        // Remove support between a roof and the model above it.
        for (size_t idx = 1; idx + z_top < layer_count; ++idx) {
            if (out.roofs[idx].empty())
                continue;
            const size_t upper = std::min(idx + in.roof_layer_count + z_top + 5, layer_count - 1);
            for (size_t k = idx; k < upper; ++k)
                support[k] = diff(support[k], out.roofs[idx]);
        }
    }

    // Bottoms (AreaSupport::generateSupportBottom).
    if (in.bottom_layer_count > 0) {
        const size_t z_bottom = size_t((in.z_distance_bottom + in.layer_thickness - 1) / in.layer_thickness);
        for (long long idx = (long long) layer_count - 1; idx >= (long long) z_bottom; --idx) {
            const size_t below_from = size_t(std::max<long long>(0, idx - (long long) in.bottom_layer_count - (long long) z_bottom));
            Polygons     outlines;
            for (size_t below = below_from; below < size_t(idx) - z_bottom + 1; ++below)
                append(outlines, in.outlines[below]);
            out.bottoms[size_t(idx)] = interface_layer(support[size_t(idx)], outlines, in.bottom_line_width, scale_(s.bottom_offset_mm),
                                                      s.minimum_bottom_area_mm2);
        }
    }

    out.support = std::move(support);
    return out;
}

void generate(PrintObject &object)
{
    if (object.layer_count() == 0)
        return;
    if (object.print()->config().belt_printer.value)
        throw Slic3r::SlicingError("Cura support does not support belt printers yet.");

    const ModSettings s     = read_settings(object);
    Areas             areas = compute_areas(object, s);

    SupportParameters            support_params(object);
    SupportGeneratorLayerStorage storage;
    SupportGeneratorLayersPtr    base_layers, interface_layers, empty;
    auto make_layer = [&](size_t idx, SupporLayerType type, Polygons &&polygons) {
        const Layer          &object_layer = *object.get_layer(int(idx));
        SupportGeneratorLayer &layer       = storage.allocate(type);
        layer.print_z  = object_layer.print_z;
        layer.bottom_z = object_layer.bottom_z();
        layer.height   = object_layer.height;
        layer.polygons = std::move(polygons);
        return &layer;
    };
    for (size_t i = 0; i < areas.support.size(); ++i) {
        if (!areas.support[i].empty())
            base_layers.push_back(make_layer(i, SupporLayerType::Base, std::move(areas.support[i])));
        if (!areas.roofs[i].empty())
            interface_layers.push_back(make_layer(i, SupporLayerType::TopInterface, std::move(areas.roofs[i])));
        if (!areas.bottoms[i].empty())
            interface_layers.push_back(make_layer(i, SupporLayerType::BottomInterface, std::move(areas.bottoms[i])));
    }
    if (base_layers.empty() && interface_layers.empty())
        return;
    std::sort(interface_layers.begin(), interface_layers.end(), [](const SupportGeneratorLayer *a, const SupportGeneratorLayer *b) { return *a < *b; });

    SupportGeneratorLayersPtr raft_layers = generate_raft_base(object, support_params, object.slicing_parameters(), empty, interface_layers, empty,
                                                               base_layers, storage);
    if (object.print()->canceled()) throw CanceledException();
    generate_support_layers(object, raft_layers, empty, empty, base_layers, interface_layers, empty);
    if (object.print()->canceled()) throw CanceledException();
    generate_support_toolpaths(object.support_layers(), object.config(), support_params, object.slicing_parameters(), raft_layers, empty, empty,
                               base_layers, interface_layers, empty);
}

} // namespace CuraSupport
} // namespace ModHost
} // namespace Slic3r
