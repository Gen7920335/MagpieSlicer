// Resin-style support: PrusaSlicer 2.9.6's SLA support-point generator and support trees (Default and
// Branching), adapted to FFF printing. The SLA library is this mod's own copy (prusa/libslic3r/SLA,
// BranchingTree), compiled with the core's geometry library linked in statically, so the points, the
// tree and its cross-sections are computed by the same code as when they were part of the core.
//
// Slot support.contacts (magpie_slot_support_contacts.h): the core hands over the sliced object and its
// mesh; this mod filters the points with Orca's support settings and painting, builds the tree, and
// returns discs around the tree heads as the contacts and the tree's cross-sections as the body. Orca's
// normal-support generator prints them with its own interface, gap, raft and toolpath settings.
// Slot object.z_offset (magpie_slot_object_z_offset.h): the object elevation of the selected strategy.

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Polygon.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/SLA/SupportIslands/SampleConfigFactory.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include "libslic3r/SLA/SupportTree.hpp"
#include "libslic3r/SLA/SupportTreeSlicer.hpp"

#include <magpie_mod.hpp>
#include <magpie_slot_object_z_offset.h>
#include <magpie_slot_support_contacts.h>

#include <boost/log/core.hpp>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#define RESIN_MOD_ID "resin_support"
#define RESIN_MOD_VERSION "2.1.0"

MAGPIE_MOD_DEFINE(RESIN_MOD_ID, RESIN_MOD_VERSION)

namespace resin {

using namespace Slic3r;

namespace {

struct Cancelled {};

// One of PrusaSlicer's two SLA support-tree strategies, with the settings PrusaSlicer keeps per
// strategy. The parameters hold one set for each ("default_*" and "branching_*").
struct StrategySettings
{
    double      head_front_diameter_mm      = 0.4;
    double      head_width_mm               = 1.0;
    double      pillar_diameter_mm          = 1.0;
    double      small_pillar_percent        = 50.;
    int         max_bridges_on_pillar       = 3;
    double      max_weight_on_model         = 10.;
    std::string pillar_connection_mode      = "dynamic"; // dynamic, zigzag or cross
    bool        buildplate_only             = false;
    double      pillar_widening_factor      = 0.5;
    double      base_diameter_mm            = 4.0;
    double      base_height_mm              = 1.0;
    double      base_safety_distance_mm     = 1.0;
    double      critical_angle_deg          = 45.;
    double      max_bridge_length_mm        = 15.;
    double      max_pillar_link_distance_mm = 10.;
    double      object_elevation_mm         = 5.0;
};

struct ModSettings
{
    bool             enable                 = false;
    bool             branching              = false; // tree_type == "branching"
    double           points_density_percent = 100.;
    bool             enforcers_only         = false;
    StrategySettings strategy; // the settings of the selected strategy
};

ModSettings read_settings(mp_ctx *ctx)
{
    auto num = [ctx](const std::string &name, double &dst) {
        if (auto v = magpie::param_f64(ctx, name.c_str()))
            dst = *v;
    };
    auto flag = [ctx](const std::string &name, bool &dst) {
        if (auto v = magpie::param_i64(ctx, name.c_str()))
            dst = *v != 0;
    };
    auto text = [ctx](const std::string &name, std::string &dst) {
        if (auto v = magpie::param_str(ctx, name.c_str()))
            dst = *v;
    };

    ModSettings s;
    flag("enable", s.enable);
    std::string tree_type = "default";
    text("tree_type", tree_type);
    s.branching = tree_type == "branching";
    num("points_density", s.points_density_percent);
    flag("enforcers_only", s.enforcers_only);

    // PrusaSlicer's per-strategy defaults differ only in these two values.
    StrategySettings &st = s.strategy;
    if (s.branching) {
        st.max_bridges_on_pillar = 2;
        st.max_bridge_length_mm  = 5.;
    }
    const std::string p = s.branching ? "branching_" : "default_";
    num(p + "head_front_diameter", st.head_front_diameter_mm);
    num(p + "head_width", st.head_width_mm);
    num(p + "pillar_diameter", st.pillar_diameter_mm);
    num(p + "small_pillar_diameter_percent", st.small_pillar_percent);
    double max_bridges = st.max_bridges_on_pillar;
    num(p + "max_bridges_on_pillar", max_bridges);
    st.max_bridges_on_pillar = int(std::lround(max_bridges));
    num(p + "max_weight_on_model", st.max_weight_on_model);
    text(p + "pillar_connection_mode", st.pillar_connection_mode);
    flag(p + "buildplate_only", st.buildplate_only);
    num(p + "pillar_widening_factor", st.pillar_widening_factor);
    num(p + "base_diameter", st.base_diameter_mm);
    num(p + "base_height", st.base_height_mm);
    num(p + "base_safety_distance", st.base_safety_distance_mm);
    num(p + "critical_angle", st.critical_angle_deg);
    num(p + "max_bridge_length", st.max_bridge_length_mm);
    num(p + "max_pillar_link_distance", st.max_pillar_link_distance_mm);
    num(p + "object_elevation", st.object_elevation_mm);
    return s;
}

double config_f64(mp_ctx *ctx, const char *key)
{
    double v = 0.;
    if (magpie::host()->config_f64(ctx, key, -1, &v) != MP_OK)
        throw std::runtime_error(std::string("cannot read the setting ") + key);
    return v;
}

std::string config_str(mp_ctx *ctx, const char *key)
{
    char buf[256] = {};
    if (magpie::host()->config_str(ctx, key, -1, buf, uint32_t(sizeof(buf))) != MP_OK)
        throw std::runtime_error(std::string("cannot read the setting ") + key);
    return buf;
}

// The lift the object.z_offset slot reports: the selected strategy's elevation, only for an object
// whose support this mod generates. Another mod's support (Mixed is picked over resin support when
// both are on; the host gives the picked name as "support_generator") does not fill the gap under a lifted object, and with enforcers only the object's
// bottom gets no support points, so in both cases the object stays on the bed.
double object_lift_mm(mp_ctx *ctx, const ModSettings &s)
{
    if (!s.enable || s.enforcers_only || config_str(ctx, "support_generator") != "resin")
        return 0.;
    return s.strategy.object_elevation_mm;
}

bool contains_point(const ExPolygons &polygons, const Point &point)
{
    return std::any_of(polygons.begin(), polygons.end(), [&point](const ExPolygon &polygon) { return polygon.contains(point); });
}

size_t nearest_height_index(const std::vector<float> &heights, float z)
{
    auto it = std::lower_bound(heights.begin(), heights.end(), z);
    if (it == heights.begin())
        return 0;
    if (it == heights.end())
        return heights.size() - 1;
    const size_t upper = size_t(it - heights.begin());
    return z - heights[upper - 1] <= heights[upper] - z ? upper - 1 : upper;
}

// The object layer a support point belongs to: the layer nearest in Z whose slices contain the
// point (of two equally near, the lower), or the nearest layer in Z when none contains it.
// The layers are visited from the point's height outwards in order of their distance, so the
// containment test runs only until the first containing layer instead of on every layer; heights
// rise with the layer index, so the distances on each side grow monotonically. Layers at exactly
// that distance are all looked at, and the lowest containing one wins, as a scan of every layer
// keeping the first strictly nearer one would choose.
size_t point_layer_index(const std::vector<ExPolygons> &slices, const std::vector<float> &heights, const resin_sla::SupportPoint &support_point)
{
    const Point point = Point::new_scale(support_point.pos.x(), support_point.pos.y());
    const float z     = support_point.pos.z();
    auto        dz    = [&heights, z](size_t layer_id) { return std::abs(heights[layer_id] - z); }; // mm
    const size_t n     = heights.size();
    size_t       above = size_t(std::lower_bound(heights.begin(), heights.end(), z) - heights.begin()); // next to visit upwards
    size_t       below = above;                                                                     // one past the next to visit downwards
    size_t       best  = n;
    float        best_dz = 0.f;
    while (below > 0 || above < n) {
        size_t layer_id;
        if (below == 0)
            layer_id = above++;
        else if (above == n)
            layer_id = --below;
        else if (dz(below - 1) <= dz(above))
            layer_id = --below;
        else
            layer_id = above++;
        const float d = dz(layer_id);
        if (best < n && d > best_dz)
            break;
        if (contains_point(slices[layer_id], point) && (best == n || layer_id < best)) {
            best    = layer_id;
            best_dz = d;
        }
    }
    return best < n ? best : nearest_height_index(heights, z);
}

// How far, in scaled units, a layer may stick out past the layer below before it needs support at
// the given threshold angle (degrees from the horizontal). The same reach Orca's detect_overhangs()
// uses: the layer height over tan(threshold + 1 degree), capped at 89 degrees to stay finite.
coord_t threshold_reach(double layer_height_mm, double threshold_angle_deg)
{
    if (layer_height_mm <= 0. || threshold_angle_deg <= 0.)
        return 0;
    const double inclusive_angle_deg = std::min(threshold_angle_deg + 1., 89.);
    return coord_t(scale_(layer_height_mm / std::tan(Geometry::deg2rad(inclusive_angle_deg))));
}

resin_sla::PillarConnectionMode connection_mode(const std::string &mode)
{
    if (mode == "zigzag")
        return resin_sla::PillarConnectionMode::zigzag;
    if (mode == "cross")
        return resin_sla::PillarConnectionMode::cross;
    return resin_sla::PillarConnectionMode::dynamic;
}

// Decides whether an automatic SLA support point stays: inside a support enforcer it always stays;
// otherwise it must be outside every blocker, enforcers-only must be off, and, when the threshold
// mask is enabled, it must lie inside it. An enabled but empty threshold mask rejects every point.
class PointFilter
{
public:
    PointFilter(const Polygons &enforcers, const Polygons &blockers, const Polygons *threshold, bool enforcers_only)
        : m_enforcers(union_ex(enforcers))
        , m_blockers(union_ex(blockers))
        , m_threshold(threshold ? union_ex(*threshold) : ExPolygons{})
        , m_threshold_enabled(threshold != nullptr)
        , m_enforcers_only(enforcers_only)
    {}
    bool accepts(const Point &point) const
    {
        const bool enforced         = contains_point(m_enforcers, point);
        const bool blocked          = contains_point(m_blockers, point);
        const bool passes_threshold = !m_threshold_enabled || contains_point(m_threshold, point);
        return enforced || (!blocked && !m_enforcers_only && passes_threshold);
    }

private:
    ExPolygons m_enforcers, m_blockers, m_threshold;
    bool       m_threshold_enabled;
    bool       m_enforcers_only;
};

Polygons polygons_of(const mp_rings &rings)
{
    Polygons out;
    out.reserve(rings.n);
    for (uint32_t r = 0; r < rings.n; ++r) {
        Polygon p;
        p.points.reserve(rings.rings[r].n);
        for (uint32_t k = 0; k < rings.rings[r].n; ++k)
            p.points.emplace_back(rings.rings[r].pts[k].x, rings.rings[r].pts[k].y);
        out.push_back(std::move(p));
    }
    return out;
}

ExPolygons expolygons_of(const mp_expolys &in)
{
    ExPolygons out;
    out.reserve(in.n);
    for (uint32_t i = 0; i < in.n; ++i) {
        ExPolygon e;
        for (uint32_t r = 0; r < in.items[i].n; ++r) {
            const mp_ring &ring = in.items[i].rings[r];
            Polygon        p;
            p.points.reserve(ring.n);
            for (uint32_t k = 0; k < ring.n; ++k)
                p.points.emplace_back(ring.pts[k].x, ring.pts[k].y);
            if (r == 0)
                e.contour = std::move(p);
            else
                e.holes.push_back(std::move(p));
        }
        out.push_back(std::move(e));
    }
    return out;
}

// What compute hands back, in the C layout, owned here until release: the contacts and the tree.
struct Output
{
    std::vector<Polygons>             contacts;
    std::vector<std::vector<mp_ring>> rings;
    std::vector<mp_rings>             lists;
    resin_sla::SupportTreeOutput      tree;
    // The last body call's islands and their C view.
    std::vector<ExPolygons>             body;
    std::vector<std::vector<std::vector<mp_ring>>> body_rings;
    std::vector<std::vector<mp_expoly>> body_items;
};

void compute_contacts(mp_ctx *ctx, const mp_support_contacts_input_v1 &in, Output &result, bool &empty)
{
    empty = true;
    if (in.layer_count == 0)
        return;
    if (config_f64(ctx, "belt_printer") != 0.)
        throw std::runtime_error("Resin support does not support belt printers yet.");

    const ModSettings       s      = read_settings(ctx);
    const StrategySettings &st     = s.strategy;
    resin_sla::ThrowOnCancel cancel = [ctx]() {
        if (magpie::host()->cancelled(ctx))
            throw Cancelled();
    };
    cancel();

    // The object mesh in the frame the layers were sliced in, at its print Z (raft and elevation included).
    indexed_triangle_set its;
    its.vertices.reserve(in.mesh.vertex_count);
    for (uint32_t i = 0; i < in.mesh.vertex_count; ++i)
        its.vertices.emplace_back(in.mesh.vertices[3 * i], in.mesh.vertices[3 * i + 1], in.mesh.vertices[3 * i + 2]);
    its.indices.reserve(in.mesh.triangle_count);
    for (uint32_t i = 0; i < in.mesh.triangle_count; ++i)
        its.indices.emplace_back(in.mesh.indices[3 * i], in.mesh.indices[3 * i + 1], in.mesh.indices[3 * i + 2]);
    TriangleMesh mesh(std::move(its));
    if (mesh.empty())
        return;

    std::vector<ExPolygons> slices;
    std::vector<float>      heights;
    slices.reserve(in.layer_count);
    heights.reserve(in.layer_count);
    for (uint32_t i = 0; i < in.layer_count; ++i) {
        slices.emplace_back(expolygons_of(in.slices[i]));
        heights.emplace_back(float(in.slice_z[i] + in.print_z_min));
    }
    std::vector<ExPolygons> model_slices = slices;

    // A non-zero support_threshold_angle limits the automatic points to areas the layer below cannot
    // carry at that angle. Zero keeps PrusaSlicer's own island and peninsula detection.
    const double          threshold_angle = config_f64(ctx, "support_threshold_angle");
    std::vector<Polygons> threshold_demand;
    if (int(threshold_angle) > 0) {
        threshold_demand.resize(model_slices.size());
        threshold_demand.front() = to_polygons(model_slices.front());
        for (size_t layer_id = 1; layer_id < model_slices.size(); ++layer_id) {
            const coord_t  reach     = threshold_reach(in.layer_height[layer_id - 1], int(threshold_angle));
            const Polygons supported = offset(to_polygons(model_slices[layer_id - 1]), float(reach), jtSquare, 0.);
            threshold_demand[layer_id] = diff(to_polygons(model_slices[layer_id]), supported);
        }
    }

    resin_sla::SupportPointGeneratorData generator_data = resin_sla::prepare_generator_data(std::move(model_slices), heights,
                                                                                          resin_sla::PrepareSupportConfig{}, cancel);

    // Heads and pillars thinner than one support line cannot be printed; widen them to it.
    const double printable_diameter = std::max(in.support_nozzle_diameter, in.support_line_width); // mm

    resin_sla::SupportPointGeneratorConfig point_cfg;
    point_cfg.density_relative     = float(s.points_density_percent / 100.);
    point_cfg.head_diameter        = float(std::max(st.head_front_diameter_mm, printable_diameter));
    point_cfg.island_configuration = resin_sla::SampleConfigFactory::apply_density(resin_sla::SampleConfigFactory::create(point_cfg.head_diameter),
                                                                                   point_cfg.density_relative);

    resin_sla::LayerSupportPoints layer_points = resin_sla::generate_support_points(generator_data, point_cfg, cancel);
    // How far a point may move to reach the mesh surface: one layer height (mm).
    const double allowed_move = heights.size() > 1 ? double(heights[1] - heights[0]) + std::numeric_limits<float>::epsilon() :
                                                     std::max(0.01, config_f64(ctx, "layer_height"));
    AABBMesh                 indexed_mesh(mesh);
    resin_sla::SupportPoints points = resin_sla::move_on_mesh_surface(layer_points, indexed_mesh, allowed_move, cancel);

    // Like PrusaSlicer without elevation: points on the bed-facing bottom are not needed when the
    // object sits on the bed (or the raft).
    if (object_lift_mm(ctx, s) < EPSILON) {
        const float ground = float(mesh.bounding_box().min.z() + EPSILON);
        points.erase(std::remove_if(points.begin(), points.end(), [ground](const resin_sla::SupportPoint &p) { return p.pos.z() <= ground; }),
                     points.end());
    }

    // Support enforcers and blockers, modelled and painted, and the threshold mask decide which of
    // the automatic points stay.
    std::vector<PointFilter> filters;
    filters.reserve(heights.size());
    for (size_t layer_id = 0; layer_id < heights.size(); ++layer_id) {
        cancel();
        const Polygons enforcers = polygons_of(in.enforcers[layer_id]);
        const Polygons blockers  = polygons_of(in.blockers[layer_id]);
        filters.emplace_back(enforcers, blockers, threshold_demand.empty() ? nullptr : &threshold_demand[layer_id], s.enforcers_only);
    }
    threshold_demand.clear();

    resin_sla::SupportPoints kept;
    std::vector<size_t>      kept_layer;
    kept.reserve(points.size());
    for (resin_sla::SupportPoint &support_point : points) {
        const size_t layer_id = point_layer_index(slices, heights, support_point);
        if (filters[layer_id].accepts(Point::new_scale(support_point.pos.x(), support_point.pos.y()))) {
            kept.emplace_back(std::move(support_point));
            kept_layer.push_back(layer_id);
        }
    }

    resin_sla::SupportTreeConfig tree_cfg;
    tree_cfg.enabled                 = true;
    tree_cfg.tree_type               = s.branching ? resin_sla::SupportTreeType::Branching : resin_sla::SupportTreeType::Default;
    tree_cfg.head_front_radius_mm    = 0.5 * std::max(st.head_front_diameter_mm, printable_diameter);
    const double pillar_radius_mm    = 0.5 * std::max(st.pillar_diameter_mm, printable_diameter);
    tree_cfg.head_back_radius_mm     = pillar_radius_mm;
    tree_cfg.head_fallback_radius_mm = std::max(0.5 * printable_diameter, 0.01 * st.small_pillar_percent * pillar_radius_mm);
    // The heads touch the surface; FFF cannot print into the object.
    tree_cfg.head_penetration_mm             = 0.;
    tree_cfg.head_width_mm                   = st.head_width_mm;
    tree_cfg.pillar_connection_mode          = connection_mode(st.pillar_connection_mode);
    tree_cfg.ground_facing_only              = st.buildplate_only;
    tree_cfg.pillar_widening_factor          = st.pillar_widening_factor;
    tree_cfg.base_radius_mm                  = 0.5 * std::max(st.base_diameter_mm, printable_diameter);
    tree_cfg.base_height_mm                  = st.base_height_mm;
    tree_cfg.pillar_base_safety_distance_mm  = st.base_safety_distance_mm;
    tree_cfg.bridge_slope                    = Geometry::deg2rad(st.critical_angle_deg);
    tree_cfg.max_bridge_length_mm            = st.max_bridge_length_mm;
    tree_cfg.max_pillar_link_distance_mm     = st.max_pillar_link_distance_mm;
    // The pillars stand on the raft interface (or the bed), not on the gap below the object.
    tree_cfg.object_elevation_mm             = in.print_z_min - in.raft_interface_top_z;
    tree_cfg.max_bridges_on_pillar           = unsigned(std::max(0, st.max_bridges_on_pillar));
    tree_cfg.max_weight_on_model_support     = st.max_weight_on_model;

    resin_sla::SupportableMesh supportable(mesh.its, kept, tree_cfg);
    supportable.zoffset = mesh.bounding_box().min.z();

    resin_sla::JobController controller;
    controller.stopcondition = [ctx]() { return magpie::host()->cancelled(ctx) != 0; };
    controller.cancelfn      = cancel;
    auto [tree_mesh, tree_output] = resin_sla::create_support_tree(supportable, controller);
    (void) tree_mesh;
    result.tree = std::move(tree_output);

    // Contact demand: a printable disc around each kept head, on the head's layer. Orca's interface,
    // Z gap, X/Y gap and raft settings then apply to these contacts.
    result.contacts.assign(heights.size(), Polygons{});
    const double contact_radius = scale_(0.5 * std::max(st.head_front_diameter_mm, printable_diameter));
    // Circle approximation error: 0.02 mm, scaled.
    const double circle_error = std::max(1., scale_(0.02));
    for (size_t i = 0; i < kept.size(); ++i) {
        Polygon disc = make_circle(contact_radius, circle_error);
        disc.translate(Point::new_scale(kept[i].pos.x(), kept[i].pos.y()));
        result.contacts[kept_layer[i]].emplace_back(std::move(disc));
    }
    for (Polygons &layer_demand : result.contacts)
        if (!layer_demand.empty())
            layer_demand = union_(layer_demand);

    magpie::log(MP_LOG_INFO, RESIN_MOD_ID, "Resin support: " + std::to_string(kept.size()) + " contact points");
    empty = false;
}

template<class Fn> mp_status run(Fn &&fn)
{
    static thread_local std::string message;
    try {
        // The core's scaling factor, which this mod's copy of the geometry library uses: the
        // application changes it for very large printers, never while a slice runs, so calls running
        // at the same time never write different values.
        if (::SCALING_FACTOR != magpie::host()->scaling_factor_mm)
            ::SCALING_FACTOR = magpie::host()->scaling_factor_mm;
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

mp_status compute(void *, mp_ctx *ctx, const mp_support_contacts_input_v1 *in, mp_support_contacts_output_v1 *out)
{
    return run([&] {
        if (in == nullptr || out == nullptr || in->size < sizeof(mp_support_contacts_input_v1))
            throw std::runtime_error("bad arguments");
        auto data  = std::make_unique<Output>();
        bool empty = true;
        compute_contacts(ctx, *in, *data, empty);
        if (empty) {
            out->layer_count = 0;
            out->contacts    = nullptr;
            out->mod_data    = nullptr;
            return;
        }
        const size_t n = data->contacts.size();
        data->rings.resize(n);
        data->lists.resize(n);
        for (size_t i = 0; i < n; ++i) {
            for (const Polygon &p : data->contacts[i])
                data->rings[i].push_back({reinterpret_cast<const mp_point *>(p.points.data()), uint32_t(p.points.size())});
            data->lists[i] = {data->rings[i].data(), uint32_t(data->rings[i].size())};
        }
        out->layer_count         = uint32_t(n);
        out->contacts            = data->lists.data();
        out->keep_narrow_islands = 1;
        out->mod_data            = data.release();
    });
}

mp_status body(void *, mp_ctx *, const mp_support_contacts_output_v1 *of, uint32_t n, const double *z, mp_expolys *out)
{
    return run([&] {
        if (of == nullptr || of->mod_data == nullptr || (n > 0 && (z == nullptr || out == nullptr)))
            throw std::runtime_error("bad arguments");
        Output &data = *static_cast<Output *>(of->mod_data);
        data.body.assign(n, ExPolygons{});
        // Each height is cut on its own from the finished tree (a read-only walk over its parts), so the
        // heights run in parallel and every slot of the result is written by one task.
        tbb::parallel_for(tbb::blocked_range<uint32_t>(0, n), [&data, z](const tbb::blocked_range<uint32_t> &range) {
            for (uint32_t i = range.begin(); i < range.end(); ++i)
                data.body[i] = resin_sla::slice_support_tree_at_height(data.tree, float(z[i]));
        });
        data.body_rings.assign(n, {});
        data.body_items.assign(n, {});
        for (uint32_t i = 0; i < n; ++i) {
            data.body_rings[i].resize(data.body[i].size());
            for (size_t k = 0; k < data.body[i].size(); ++k) {
                const ExPolygon      &e     = data.body[i][k];
                std::vector<mp_ring> &rings = data.body_rings[i][k];
                rings.push_back({reinterpret_cast<const mp_point *>(e.contour.points.data()), uint32_t(e.contour.points.size())});
                for (const Polygon &h : e.holes)
                    rings.push_back({reinterpret_cast<const mp_point *>(h.points.data()), uint32_t(h.points.size())});
                data.body_items[i].push_back({rings.data(), uint32_t(rings.size())});
            }
            out[i] = {data.body_items[i].data(), uint32_t(data.body_items[i].size())};
        }
    });
}

void release(void *, mp_support_contacts_output_v1 *out)
{
    if (out == nullptr)
        return;
    delete static_cast<Output *>(out->mod_data);
    out->mod_data    = nullptr;
    out->layer_count = 0;
    out->contacts    = nullptr;
}

mp_status z_offset(void *, mp_ctx *ctx, double *lift_mm)
{
    return run([&] {
        if (lift_mm == nullptr)
            throw std::runtime_error("bad arguments");
        *lift_mm = object_lift_mm(ctx, read_settings(ctx));
    });
}

const mp_support_contacts_v1 contacts_table = {sizeof(mp_support_contacts_v1), compute, body, release};
const mp_object_z_offset_v1  z_offset_table = {sizeof(mp_object_z_offset_v1), z_offset};

} // namespace
} // namespace resin

int32_t magpie::mod_init(const MagpieHostApi &, MagpieRegistrar &registrar)
{
    // The SLA code logs through Boost.Log, which in this library is a copy of its own with no sink set
    // up: it would print every line to the console. The mod reports through host->log instead.
    boost::log::core::get()->set_logging_enabled(false);
    if (int rc = registrar.register_slot(&registrar, MP_SUPPORT_CONTACTS_SLOT, MP_SUPPORT_CONTACTS_VERSION, "resin", &resin::contacts_table,
                                         sizeof(resin::contacts_table), nullptr);
        rc != MP_OK)
        return rc;
    return registrar.register_slot(&registrar, MP_OBJECT_Z_OFFSET_SLOT, MP_OBJECT_Z_OFFSET_VERSION, "resin", &resin::z_offset_table,
                                   sizeof(resin::z_offset_table), nullptr);
}

void magpie::mod_shutdown() {}
