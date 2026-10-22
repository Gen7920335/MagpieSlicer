// CuraEngine's normal ("area") support, ported from CuraEngine src/support.cpp at commit 47b0388
// (2026-10-15, AGPL-3.0, like OrcaSlicer). The support, roof and bottom areas follow CuraEngine's
// AreaSupport::generateSupportAreasForMesh, generateSupportRoof and generateSupportBottom; the core
// prints them with Orca's support toolpaths (slot support.areas, magpie_support.h). Cura works in
// micrometres; every Cura constant below is converted to integer units at its declaration.
//
// Every polygon operation runs in the core (geometry.hpp), and the arithmetic around them is written
// exactly as it was inside the core, so the areas are the same bit for bit.

#include "geometry.hpp"

#include <magpie_support.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <type_traits>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#define CURA_MOD_ID "cura_support"
#define CURA_MOD_VERSION "1.3.0"

MAGPIE_MOD_DEFINE(CURA_MOD_ID, CURA_MOD_VERSION)

namespace cura {

namespace {

struct Cancelled {};

// Cura micrometres in integer units (1 um = 1e-3 mm).
inline coord_t um(double micrometres) { return coord_t(std::llround(micrometres * 1e-3 / scaling_factor())); }

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
    // Integer-unit area of min_area_mm2 square millimetres.
    const double min_area = min_area_mm2 / (scaling_factor() * scaling_factor());
    ExPolygons islands = union_ex(polygons);
    islands.erase(std::remove_if(islands.begin(), islands.end(), [min_area](const ExPolygon &e) { return std::abs(e.contour.area()) < min_area; }),
                  islands.end());
    return to_polygons(islands);
}

double bounded_tan(double angle_rad)
{
    // Cura's boundedTan: tan clamped away from +-90 degrees.
    constexpr double limit = PI / 2. - 0.0001;
    return std::tan(std::clamp(angle_rad, -limit, limit));
}

// Runs fn(i) for i in [begin, end) on the core's thread pool. Exceptions stay on this side of the
// boundary: the first is rethrown here once the loop is over.
template<class Fn> void parallel_layers(mp_ctx *ctx, size_t begin, size_t end, Fn &&fn)
{
    if (end <= begin)
        return;
    struct Job
    {
        std::remove_reference_t<Fn> *fn;
        size_t            begin;
        mp_ctx           *ctx;
        std::atomic<bool> failed{false};
        bool              cancelled = false;
        std::mutex        mutex;
        std::string       error;
    } job;
    job.fn    = &fn;
    job.begin = begin;
    job.ctx   = ctx;
    magpie::host()->parallel_for(ctx, uint32_t(end - begin), [](void *user, uint32_t i) {
        Job &j = *static_cast<Job *>(user);
        if (j.failed.load())
            return;
        try {
            if (magpie::host()->cancelled(j.ctx))
                throw Cancelled{};
            (*j.fn)(j.begin + i);
        } catch (const Cancelled &) {
            std::lock_guard<std::mutex> lock(j.mutex);
            j.cancelled = true;
            j.failed    = true;
        } catch (const std::exception &e) {
            std::lock_guard<std::mutex> lock(j.mutex);
            if (j.error.empty())
                j.error = e.what();
            j.failed = true;
        } catch (...) {
            std::lock_guard<std::mutex> lock(j.mutex);
            if (j.error.empty())
                j.error = "unknown exception";
            j.failed = true;
        }
    }, &job);
    if (job.cancelled)
        throw Cancelled{};
    if (job.failed)
        throw std::runtime_error(job.error);
}

// Settings of CuraEngine's normal support that Orca has no setting for: this mod's parameters, in
// Cura's own terms. The defaults are the manifest's.
struct ModSettings
{
    double join_distance_mm         = 2.0;  // support_join_distance
    double min_feature_width_mm     = 0.;   // min_even_wall_line_width; 0 = Cura's default, 0.85 x nozzle size
    bool   z_overrides_xy           = true; // support_xy_overrides_z == z_overrides_xy
    double xy_distance_overhang_mm  = 0.2;  // support_xy_distance_overhang
    double stair_step_height_mm     = 0.3;  // support_bottom_stair_step_height
    double stair_step_width_mm      = 5.0;  // support_bottom_stair_step_width
    double stair_step_min_slope_deg = 10.;  // support_bottom_stair_step_min_slope
    double minimum_support_area_mm2 = 0.;   // minimum_support_area
    double minimum_roof_area_mm2    = 1.;   // minimum_roof_area
    double minimum_bottom_area_mm2  = 1.;   // minimum_bottom_area
    double roof_offset_mm           = 0.;   // support_roof_offset
    double bottom_offset_mm         = 0.;   // support_bottom_offset
    bool   use_towers               = false; // support_use_towers
    double tower_diameter_mm        = 3.0;  // support_tower_diameter
    double tower_max_supported_mm   = 3.0;  // support_tower_maximum_supported_diameter
    double tower_roof_angle_deg     = 65.;  // support_tower_roof_angle
    bool   conical_enabled          = false; // support_conical_enabled
    double conical_angle_deg        = 30.;  // support_conical_angle
    double conical_min_width_mm     = 5.0;  // support_conical_min_width
    bool   solid_first_layer        = false; // Magpie line 1's cura_solid_support_raft
};

ModSettings read_settings(mp_ctx *ctx)
{
    ModSettings s;
    auto num = [ctx](const char *name, double &dst) {
        if (auto v = magpie::param_f64(ctx, name))
            dst = *v;
    };
    auto flag = [ctx](const char *name, bool &dst) {
        if (auto v = magpie::param_f64(ctx, name))
            dst = *v != 0.;
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
    flag("solid_first_layer", s.solid_first_layer);
    return s;
}

double config_f64(mp_ctx *ctx, const char *key)
{
    double v = 0.;
    if (magpie::host()->config_f64(ctx, key, -1, &v) != MP_OK)
        throw std::runtime_error(std::string("setting ") + key + " could not be read");
    return v;
}

std::string config_str(mp_ctx *ctx, const char *key)
{
    char buf[256] = {};
    if (magpie::host()->config_str(ctx, key, -1, buf, uint32_t(sizeof(buf))) != MP_OK)
        throw std::runtime_error(std::string("setting ") + key + " could not be read");
    return buf;
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
    coord_t adhesion_size;         // room the brim, skirt or raft takes inside the printable area
    coord_t support_nozzle;        // machine_nozzle_size of support_infill_extruder_nr
    coord_t object_nozzle;         // machine_nozzle_size of the mesh's extruder
    double  layer_height_mm;

    std::vector<ExPolygons> slices;    // the object's islands per layer
    std::vector<Polygons>   outlines;  // storage.getLayerOutlines per layer
    std::vector<Polygons>   enforcers; // force_overhang
    std::vector<Polygons>   blockers;  // anti_overhang
    std::vector<Polygons>   mask;      // empty, or per layer the only places support may go
};

Inputs gather_inputs(mp_ctx *ctx, const mp_support_input_v1 &c)
{
    Inputs in;
    in.layer_thickness   = scale_(config_f64(ctx, "layer_height"));
    in.z_distance_top    = scale_(config_f64(ctx, "support_top_z_distance"));
    in.z_distance_bottom = scale_(config_f64(ctx, "support_bottom_z_distance"));
    in.xy_distance       = scale_(config_f64(ctx, "support_object_xy_distance"));
    in.extension_offset  = scale_(config_f64(ctx, "support_expansion"));
    // Orca's threshold is measured from the horizontal and 0 means automatic; Cura's support_angle
    // is measured from the vertical. Cura's own default (50 degrees) stands in for automatic.
    const int threshold  = int(config_f64(ctx, "support_threshold_angle"));
    in.support_angle_rad = deg2rad(threshold > 0 ? 90. - double(std::min(threshold, 89)) : 50.);
    // is_auto(): normal(auto) and tree(auto).
    const std::string type = config_str(ctx, "support_type");
    in.automatic         = type.size() >= 6 && type.compare(type.size() - 6, 6, "(auto)") == 0;
    in.platform_only     = config_f64(ctx, "support_on_build_plate_only") != 0.;

    in.support_line_width = coord_t(c.support_line_width);
    in.roof_line_width    = coord_t(c.interface_line_width);
    in.bottom_line_width  = in.roof_line_width;

    in.layer_height_mm    = config_f64(ctx, "layer_height");
    const int top_layers  = int(config_f64(ctx, "support_interface_top_layers"));
    in.roof_layer_count   = size_t(std::max(0, top_layers));
    const int bottom      = int(config_f64(ctx, "support_interface_bottom_layers"));
    in.bottom_layer_count = size_t(std::max(0, bottom < 0 ? top_layers : bottom));

    in.adhesion_size  = coord_t(c.adhesion_margin);
    in.support_nozzle = coord_t(c.support_nozzle_diameter);
    in.object_nozzle  = coord_t(c.object_nozzle_diameter);
    in.machine_border.points.assign(reinterpret_cast<const Point *>(c.printable_area.pts),
                                    reinterpret_cast<const Point *>(c.printable_area.pts) + c.printable_area.n);

    const size_t layer_count = c.layer_count;
    in.slices.resize(layer_count);
    in.outlines.resize(layer_count);
    in.enforcers.resize(layer_count);
    in.blockers.resize(layer_count);
    for (size_t i = 0; i < layer_count; ++i) {
        for (uint32_t k = 0; k < c.slices[i].n; ++k)
            in.slices[i].push_back(to_expolygon(c.slices[i].items[k]));
        in.outlines[i]  = to_polygons(in.slices[i]);
        in.enforcers[i] = to_polygons(c.enforcers[i]);
        in.blockers[i]  = to_polygons(c.blockers[i]);
    }
    if (c.mask != nullptr) {
        in.mask.resize(layer_count);
        for (size_t i = 0; i < layer_count; ++i)
            in.mask[i] = to_polygons(c.mask[i]);
    }
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

    const Polygons &original_outlines_below = in.outlines[layer_idx - 1];
    Polygons        outlines_below          = cura_offset(original_outlines_below, max_dist);
    for (size_t k = 2; k <= layers_below && k <= layer_idx; ++k)
        outlines_below = union_(outlines_below, cura_offset(in.outlines[layer_idx - k], max_dist * coord_t(k)));

    Polygons basic = in.automatic ? diff(outlines, outlines_below) : Polygons{};
    if (!in.blockers[layer_idx].empty())
        basic = diff(basic, union_(in.blockers[layer_idx]));
    // Forced support only where the layer below leaves something to hold up: none on vertical or top faces
    // (CuraEngine 5.14, CURA-13349). Cura's EPSILON is 5 um.
    if (!in.enforcers[layer_idx].empty())
        basic = union_(basic, intersection(in.enforcers[layer_idx], diff(outlines, cura_offset(original_outlines_below, um(5.)))));

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

    std::vector<Point> vertices;
    for (const Polygon &poly : layer_current)
        vertices.insert(vertices.end(), poly.points.begin(), poly.points.end());
    std::vector<std::pair<size_t, double>> acc(vertices.size(), {0, 0.});
    for (const Delta &d : deltas) {
        const double natural = d.support_distance * bounded_tan(in.support_angle_rad);
        // The nearest edge of the neighbouring layer for every vertex, the brute-force minimum bit for
        // bit (the core's distance service).
        const std::vector<double> min_dist2s = min_dist2(vertices, d.layer);
        for (size_t i = 0; i < vertices.size(); ++i) {
            const double min_dist2 = min_dist2s[i];
            const double slope     = std::sqrt(min_dist2) / d.delta_z;
            const double wall      = std::atan(std::abs(slope));
            const double ratio     = std::clamp(wall / in.support_angle_rad, 0., 1.);
            const double varying   = double(in.xy_distance) + (natural - double(in.xy_distance)) * ratio;
            acc[i].first += 1;
            acc[i].second += varying;
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
            // Vec2d dir = (prev_p - p).cast<double>()
            const double dir_x = double(prev_p.x() - p.x());
            const double dir_y = double(prev_p.y() - p.y());
            // Edges shorter than 10 um are skipped, as Cura does.
            if (dir_x * dir_x + dir_y * dir_y > double(um(10.)) * double(um(10.))) {
                // Eigen's normalized(): each coordinate divided by the norm.
                const double norm   = std::sqrt(dir_x * dir_x + dir_y * dir_y);
                const double unit_x = dir_x / norm;
                const double unit_y = dir_y / norm;
                const double ccw_x  = -unit_y;
                const double ccw_y  = unit_x;
                out.points.emplace_back(coord_t(double(prev_p.x()) + ccw_x * prev_dist), coord_t(double(prev_p.y()) + ccw_y * prev_dist));
                out.points.emplace_back(coord_t(double(p.x()) + ccw_x * dist), coord_t(double(p.y()) + ccw_y * dist));
            }
            prev_p    = p;
            prev_dist = dist;
        }
        moved.emplace_back(std::move(out));
        i += n;
    }
    // Clipper's positive fill unions the self-overlapping offset rings, as SimplifyPolygons does in Cura.
    moved = union_positive(moved);
    const coord_t smooth = in.xy_distance / 2;
    return cura_offset(cura_offset(moved, smooth), -smooth);
}

// Cura's half_min_feature_width, min_even_wall_line_width + 10 um, for the settings of the extruder with nozzle
// `nozzle`. min_even_wall_line_width defaults to min_wall_line_width, machine_nozzle_size * 0.85, which the
// frontend hands the engine in whole micrometres.
coord_t half_min_feature_width(const ModSettings &s, coord_t nozzle)
{
    if (s.min_feature_width_mm > 0.)
        return coord_t(scale_(s.min_feature_width_mm) + um(10.));
    const double nozzle_um = double(nozzle) * scaling_factor() * 1000.;
    return um(std::round(0.85 * nozzle_um)) + um(10.);
}

// AreaSupport::join.
Polygons join(const Inputs &in, const ModSettings &s, const Polygons &layer_up, const Polygons &layer_this)
{
    Polygons joined;
    const double conical_angle = deg2rad(s.conical_angle_deg);
    if (s.conical_enabled && conical_angle != 0.) {
        const coord_t conical_offset = conical_angle > 0 ? -coord_t(bounded_tan(conical_angle) * double(in.layer_thickness))
                                                         : coord_t(bounded_tan(-conical_angle) * double(in.layer_thickness));
        const coord_t smallest = scale_(s.conical_min_width_mm);
        const Polygons insetted    = cura_offset(layer_up, -smallest / 2);
        const Polygons small_parts = diff(layer_up, cura_offset(insetted, smallest / 2 + um(20.)));
        // Room for the platform adhesion around the support.
        const Polygons border = cura_offset(Polygons{in.machine_border}, -in.adhesion_size);
        joined = intersection(union_(union_(layer_this, cura_offset(layer_up, conical_offset)), small_parts), border);
    } else {
        joined = union_(layer_this, layer_up);
    }
    const coord_t join_distance = scale_(s.join_distance_mm);
    if (join_distance > 0) {
        // Read from the support infill extruder's settings.
        const coord_t half_min_feature = half_min_feature_width(s, in.support_nozzle);
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
    const coord_t half_tower   = coord_t(scale_(s.tower_diameter_mm) / 2);
    Polygons struts;
    for (const Polygon &poly : layer_this) {
        if (poly.size() >= 6)
            continue;
        double best_length2 = -1.;
        size_t best         = 0;
        for (size_t i = 0; i < poly.size(); ++i) {
            // (a - b).cast<double>().squaredNorm()
            const Point  d  = poly.points[i] - poly.points[(i + 1) % poly.size()];
            const double dx = double(d.x()), dy = double(d.y());
            const double l2 = dx * dx + dy * dy;
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
    const double  roof_angle    = deg2rad(s.tower_roof_angle_deg);
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

// Basic and full overhang per layer (AreaSupport::generateOverhangAreasForMesh), clipped to the mask.
void detect_overhangs(mp_ctx *ctx, const Inputs &in, std::vector<Polygons> &overhang, std::vector<Polygons> &full_overhang)
{
    const size_t layer_count     = in.outlines.size();
    const double layer_height_mm = in.layer_height_mm;
    overhang.assign(layer_count, Polygons{});
    full_overhang.assign(layer_count, Polygons{});
    parallel_layers(ctx, 1, layer_count, [&](size_t i) {
        auto [basic, full] = basic_and_full_overhang(in, i, layer_height_mm);
        if (!in.mask.empty()) {
            const Polygons &allowed = in.mask[i];
            basic = intersection(basic, allowed);
            full  = intersection(full, allowed);
        }
        overhang[i]      = std::move(basic);
        full_overhang[i] = std::move(full);
    });
}

struct Areas
{
    std::vector<Polygons> support; // body, with roofs and bottoms taken out
    std::vector<Polygons> roofs;
    std::vector<Polygons> bottoms;
    // Parts of support and roofs printed fractional_drop lower (support_fractional_roof and the fractional
    // support infill parts of CuraEngine), taken out of support and roofs; empty when the drop is 0.
    std::vector<Polygons> fractional_support;
    std::vector<Polygons> fractional_roofs;
    coord_t               fractional_drop = 0;
};

std::vector<Polygons> overhang_demand(mp_ctx *ctx, const Inputs &in)
{
    std::vector<Polygons> overhang, full_overhang;
    if (in.outlines.size() >= 2)
        detect_overhangs(ctx, in, overhang, full_overhang);
    else
        full_overhang.assign(in.outlines.size(), Polygons{});
    return full_overhang;
}

Areas compute_areas(mp_ctx *ctx, const Inputs &in, const ModSettings &s)
{
    const size_t layer_count = in.outlines.size();
    Areas        out;
    out.support.assign(layer_count, Polygons{});
    out.roofs.assign(layer_count, Polygons{});
    out.bottoms.assign(layer_count, Polygons{});
    if (layer_count < 2)
        return out;

    const size_t  layer_z_distance_top = size_t(in.z_distance_top / in.layer_thickness) + 1;
    if (layer_z_distance_top + 1 > layer_count)
        return out;

    // Overhangs (AreaSupport::generateOverhangAreasForMesh).
    std::vector<Polygons> overhang, full_overhang;
    detect_overhangs(ctx, in, overhang, full_overhang);
    if (std::all_of(full_overhang.begin(), full_overhang.end(), [](const Polygons &p) { return p.empty(); }))
        return out;

    // Overhang points for towers (AreaSupport::detectOverhangPoints).
    std::vector<std::vector<Polygons>> overhang_points(layer_count);
    const coord_t max_tower_supported = scale_(s.tower_max_supported_mm);
    const bool    use_towers          = s.use_towers && max_tower_supported > 0;
    if (use_towers) {
        const double max_area = double(max_tower_supported) * double(max_tower_supported);
        for (size_t i = 1; i < layer_count; ++i)
            for (const ExPolygon &part : in.slices[i]) {
                if (part.contour.area() >= max_area)
                    continue;
                if (!intersection(in.outlines[i - 1], to_polygons(part)).empty())
                    continue;
                Polygons point = diff(to_polygons(part), in.blockers[i]);
                if (!in.mask.empty())
                    point = intersection(point, in.mask[i]);
                if (!point.empty())
                    overhang_points[i].push_back(std::move(point));
            }
    }

    // X/Y disallowed and sloped areas (generateSupportAreasForMesh, first parallel loop).
    // Read from the mesh's settings.
    const coord_t half_min_feature = half_min_feature_width(s, in.object_nozzle);
    const double  sloped_angle     = deg2rad(s.stair_step_min_slope_deg);
    const coord_t sloped_width     = um(10.) + coord_t(double(in.layer_thickness) / bounded_tan(sloped_angle)) / 2;
    std::vector<Polygons> xy_disallowed(layer_count), sloped(layer_count);
    xy_disallowed[0] = cura_offset(in.outlines[0], in.xy_distance);
    parallel_layers(ctx, 1, layer_count, [&](size_t i) {
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
        if (magpie::host()->cancelled(ctx)) throw Cancelled{};
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
            layer_this = diff(join(in, s, support[idx + 1], layer_this), model_on_layer);
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
    parallel_layers(ctx, 0, layer_count, [&](size_t i) {
        Polygons &layer = support[i];
        if (!layer.empty() && !xy_disallowed[i].empty())
            layer = diff(layer, xy_disallowed[i]);
        layer = cura_offset(cura_offset(layer, -half_min_feature), half_min_feature);
        layer = remove_small_areas(layer, s.minimum_support_area_mm2);
    });

    if (in.platform_only) {
        Polygons touching = support[0];
        const double conical_angle = deg2rad(s.conical_angle_deg);
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
        parallel_layers(ctx, 0, max_check, [&](size_t i) { support[i] = diff(support[i], in.outlines[i + layer_z_distance_top - 1]); });
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

    // The top Z distance left over past whole layers (PathConfigStorage leftover_support_distance). CuraEngine
    // prints the parts right under the model that much lower: the roof a layer has and the next layer up has not
    // (generateSupportRoof's support_fractional_roof), and the support under the overhang the main loop
    // supported from this layer (SupportLayer::fillInfillParts).
    const coord_t leftover = in.z_distance_top % in.layer_thickness;
    if (leftover > 0) {
        out.fractional_drop = leftover;
        out.fractional_support.assign(layer_count, Polygons{});
        out.fractional_roofs.assign(layer_count, Polygons{});
        for (size_t idx = 1; idx + 1 < layer_count; ++idx) {
            if (!out.roofs[idx].empty())
                out.fractional_roofs[idx] = diff(out.roofs[idx], out.roofs[idx + 1]);
            if (!support[idx].empty() && idx + layer_z_distance_top < layer_count)
                out.fractional_support[idx] = intersection(support[idx], overhang[idx + layer_z_distance_top]);
        }
        for (size_t idx = 1; idx + 1 < layer_count; ++idx) {
            if (!out.fractional_roofs[idx].empty())
                out.roofs[idx] = diff(out.roofs[idx], out.fractional_roofs[idx]);
            if (!out.fractional_support[idx].empty())
                support[idx] = diff(support[idx], out.fractional_support[idx]);
        }
    }

    out.support = std::move(support);
    return out;
}

// ---------------------------------------------------------------------------------------------------
// support.areas v1

// What one call hands back: the areas in the C layout, owned here until release.
struct Output
{
    std::vector<Polygons>             layers[5]; // body, roof, bottom, fractional body, fractional roof
    std::vector<std::vector<mp_ring>> rings[5];
    std::vector<mp_rings>             lists[5];
};

void fill_output(std::unique_ptr<Output> data, mp_support_output_v1 *out, coord_t fractional_drop = 0)
{
    // A host older than the fractional fields gets the whole layers (the gap rounds down to whole layers): the
    // lowered parts go back into their layers before the rings are built from them (review 2026-10-22).
    const bool host_takes_fractional = out->size >= offsetof(mp_support_output_v1, fractional_drop) + sizeof(out->fractional_drop) && fractional_drop > 0;
    if (!host_takes_fractional)
        for (int k = 3; k < 5; ++k)
            for (size_t i = 0; i < data->layers[k].size() && i < data->layers[k - 3].size(); ++i)
                append(data->layers[k - 3][i], data->layers[k][i]);
    for (int k = 0; k < 5; ++k) {
        const size_t n = data->layers[k].size();
        data->rings[k].resize(n);
        data->lists[k].resize(n);
        for (size_t i = 0; i < n; ++i) {
            for (const Polygon &p : data->layers[k][i])
                data->rings[k][i].push_back({reinterpret_cast<const mp_point *>(p.points.data()), uint32_t(p.points.size())});
            data->lists[k][i] = {data->rings[k][i].data(), uint32_t(data->rings[k][i].size())};
        }
    }
    out->layer_count = uint32_t(data->layers[0].size());
    out->body        = data->lists[0].data();
    out->roof        = data->lists[1].empty() ? nullptr : data->lists[1].data();
    out->bottom      = data->lists[2].empty() ? nullptr : data->lists[2].data();
    if (host_takes_fractional) {
        out->fractional_body = data->lists[3].data();
        out->fractional_roof = data->lists[4].data();
        out->fractional_drop = fractional_drop;
    }
    out->mod_data    = data.release();
}

template<class Fn> mp_status run(Fn &&fn)
{
    static thread_local std::string message;
    try {
        fn();
        return {MP_OK, nullptr};
    } catch (const Cancelled &) {
        return {MP_ERR_CANCELLED, "cancelled"};
    } catch (const std::exception &e) {
        message = e.what();
    } catch (...) {
        message = "unknown exception";
    }
    return {MP_ERR_FAILED, message.c_str()};
}

mp_status compute(void *, mp_ctx *ctx, const mp_support_input_v1 *in, mp_support_output_v1 *out)
{
    return run([&] {
        if (in == nullptr || out == nullptr || in->size < sizeof(mp_support_input_v1))
            throw std::runtime_error("bad arguments");
        if (in->layer_count == 0) {
            out->layer_count = 0;
            return;
        }
        if (config_f64(ctx, "belt_printer") != 0.)
            throw std::runtime_error("Cura support does not support belt printers yet.");
        const ModSettings s      = read_settings(ctx);
        const Inputs      inputs = gather_inputs(ctx, *in);
        Areas             areas  = compute_areas(ctx, inputs, s);
        auto data       = std::make_unique<Output>();
        data->layers[0] = std::move(areas.support);
        data->layers[1] = std::move(areas.roofs);
        data->layers[2] = std::move(areas.bottoms);
        data->layers[3] = std::move(areas.fractional_support);
        data->layers[4] = std::move(areas.fractional_roofs);
        // Cura prints the first support layer with the same lines as the rest (support_initial_layer_line_distance
        // defaults to the support line distance) and without widening it, unless asked for a solid first layer.
        out->first_layer_dense = s.solid_first_layer ? 1 : 0;
        fill_output(std::move(data), out, areas.fractional_drop);
    });
}

mp_status demand(void *, mp_ctx *ctx, const mp_support_input_v1 *in, mp_support_output_v1 *out)
{
    return run([&] {
        if (in == nullptr || out == nullptr || in->size < sizeof(mp_support_input_v1))
            throw std::runtime_error("bad arguments");
        const Inputs inputs = gather_inputs(ctx, *in);
        auto data       = std::make_unique<Output>();
        data->layers[0] = overhang_demand(ctx, inputs);
        fill_output(std::move(data), out);
    });
}

void release(void *, mp_support_output_v1 *out)
{
    if (out == nullptr)
        return;
    delete static_cast<Output *>(out->mod_data);
    out->mod_data    = nullptr;
    out->layer_count = 0;
    out->body = out->roof = out->bottom = nullptr;
    if (out->size >= offsetof(mp_support_output_v1, fractional_drop) + sizeof(out->fractional_drop)) {
        out->fractional_body = out->fractional_roof = nullptr;
        out->fractional_drop = 0;
    }
}

const mp_support_areas_v1 table = {sizeof(mp_support_areas_v1), compute, demand, release};

} // namespace
} // namespace cura

int32_t magpie::mod_init(const MagpieHostApi &host, MagpieRegistrar &registrar)
{
    uint32_t size = 0;
    cura::geometry() = static_cast<const mp_geometry_v1 *>(registrar.import_service(&registrar, MP_GEOMETRY_SERVICE, MP_GEOMETRY_VERSION, &size, nullptr));
    if (cura::geometry() == nullptr || size < sizeof(mp_geometry_v1)) {
        host.log(MP_LOG_ERROR, CURA_MOD_ID, "the application offers no core.geometry v1 service");
        return MP_ERR_NOT_FOUND;
    }
    if (int rc = registrar.register_slot(&registrar, MP_SUPPORT_AREAS_SLOT, MP_SUPPORT_AREAS_VERSION, "cura", &cura::table, sizeof(cura::table), nullptr);
        rc != MP_OK)
        return rc;
    // The same table for other mods (Mixed support once it is a mod).
    return registrar.export_service(&registrar, "cura.support_areas", 1, &cura::table, sizeof(cura::table), nullptr);
}

void magpie::mod_shutdown() { cura::geometry() = nullptr; }
