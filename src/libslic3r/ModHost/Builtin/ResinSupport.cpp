// Resin-style support: PrusaSlicer 2.9.6's SLA support-point generator and support trees (Default
// and Branching), adapted to FFF printing. The SLA library lives in libslic3r/SLA; this module
// prepares its inputs from the sliced object, filters the points with Orca's support settings and
// painting, builds the tree, and hands its cross-sections and head contacts to Orca's normal-support
// generator, which prints them with its own interface, gap, raft and toolpath settings.

#include "ResinSupport.hpp"

#include "libslic3r/AABBMesh.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/Exception.hpp"
#include "libslic3r/Flow.hpp"
#include "libslic3r/Geometry.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/SLA/SupportIslands/SampleConfigFactory.hpp"
#include "libslic3r/SLA/SupportPointGenerator.hpp"
#include "libslic3r/SLA/SupportTree.hpp"
#include "libslic3r/SLA/SupportTreeSlicer.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace Slic3r {
namespace ModHost {
namespace ResinSupport {

namespace {

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
// point, or the nearest layer in Z when none contains it.
size_t point_layer_index(const PrintObject &object, const std::vector<float> &heights, const sla::SupportPoint &support_point)
{
    const Point point   = Point::new_scale(support_point.pos.x(), support_point.pos.y());
    size_t      best    = heights.size();
    float       best_dz = std::numeric_limits<float>::max();
    for (size_t layer_id = 0; layer_id < heights.size(); ++layer_id) {
        if (!contains_point(object.layers()[layer_id]->lslices, point))
            continue;
        const float dz = std::abs(heights[layer_id] - support_point.pos.z());
        if (dz < best_dz) {
            best    = layer_id;
            best_dz = dz;
        }
    }
    return best < heights.size() ? best : nearest_height_index(heights, support_point.pos.z());
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

sla::PillarConnectionMode connection_mode(const std::string &mode)
{
    if (mode == "zigzag")
        return sla::PillarConnectionMode::zigzag;
    if (mode == "cross")
        return sla::PillarConnectionMode::cross;
    return sla::PillarConnectionMode::dynamic;
}

} // namespace

ModSettings read_settings(const PrintObject &object)
{
    const DynamicPrintConfig &config = object.print()->full_print_config();
    auto value = [&config](const std::string &name) { return param_value(config, "resin_support." + name); };
    auto num   = [&value](const std::string &name, double &dst) {
        if (auto v = value(name); v && std::holds_alternative<double>(*v))
            dst = std::get<double>(*v);
    };
    auto flag = [&value](const std::string &name, bool &dst) {
        if (auto v = value(name); v && std::holds_alternative<bool>(*v))
            dst = std::get<bool>(*v);
    };
    auto text = [&value](const std::string &name, std::string &dst) {
        if (auto v = value(name); v && std::holds_alternative<std::string>(*v))
            dst = std::get<std::string>(*v);
    };

    ModSettings s;
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

PointFilter::PointFilter(const Polygons &enforcers, const Polygons &blockers, const Polygons *threshold, bool enforcers_only)
    : m_enforcers(union_ex(enforcers))
    , m_blockers(union_ex(blockers))
    , m_threshold(threshold ? union_ex(*threshold) : ExPolygons{})
    , m_threshold_enabled(threshold != nullptr)
    , m_enforcers_only(enforcers_only)
{}

bool PointFilter::accepts(const Point &point) const
{
    const bool enforced         = contains_point(m_enforcers, point);
    const bool blocked          = contains_point(m_blockers, point);
    const bool passes_threshold = !m_threshold_enabled || contains_point(m_threshold, point);
    return enforced || (!blocked && !m_enforcers_only && passes_threshold);
}

void generate(PrintObject &object)
{
    const PrintObjectConfig &cfg = object.config();
    if (object.layers().empty())
        return;
    if (object.print()->config().belt_printer.value)
        throw Slic3r::SlicingError("Resin support does not support belt printers yet.");

    const ModSettings        s              = read_settings(object);
    const StrategySettings  &st             = s.strategy;
    const SlicingParameters &slicing_params = object.slicing_parameters();
    sla::ThrowOnCancel       cancel         = [&object]() {
        if (object.print()->canceled())
            throw CanceledException();
    };
    cancel();

    // The object mesh in the frame the layers were sliced in, moved to its print Z (raft and
    // elevation included).
    TriangleMesh mesh = object.model_object()->raw_mesh();
    mesh.transform(object.trafo_centered(), true);
    mesh.translate(0.f, 0.f, float(slicing_params.object_print_z_min));
    if (mesh.empty())
        return;

    std::vector<ExPolygons> model_slices;
    std::vector<float>      heights;
    model_slices.reserve(object.layer_count());
    heights.reserve(object.layer_count());
    for (const Layer *layer : object.layers()) {
        model_slices.emplace_back(layer->lslices);
        heights.emplace_back(float(layer->slice_z + slicing_params.object_print_z_min));
    }

    // A non-zero support_threshold_angle limits the automatic points to areas the layer below cannot
    // carry at that angle. Zero keeps PrusaSlicer's own island and peninsula detection.
    std::vector<Polygons> threshold_demand;
    if (cfg.support_threshold_angle.value > 0) {
        threshold_demand.resize(model_slices.size());
        threshold_demand.front() = to_polygons(model_slices.front());
        for (size_t layer_id = 1; layer_id < model_slices.size(); ++layer_id) {
            const coord_t reach     = threshold_reach(object.layers()[layer_id - 1]->height, cfg.support_threshold_angle.value);
            const Polygons supported = offset(to_polygons(model_slices[layer_id - 1]), float(reach), jtSquare, 0.);
            threshold_demand[layer_id] = diff(to_polygons(model_slices[layer_id]), supported);
        }
    }

    sla::SupportPointGeneratorData generator_data = sla::prepare_generator_data(std::move(model_slices), heights, sla::PrepareSupportConfig{},
                                                                                cancel);

    // Heads and pillars thinner than one support line cannot be printed; widen them to it.
    const Flow   support_flow       = support_material_flow(&object);
    const double printable_diameter = std::max(double(support_flow.nozzle_diameter()), double(support_flow.width())); // mm

    sla::SupportPointGeneratorConfig point_cfg;
    point_cfg.density_relative     = float(s.points_density_percent / 100.);
    point_cfg.head_diameter        = float(std::max(st.head_front_diameter_mm, printable_diameter));
    point_cfg.island_configuration = sla::SampleConfigFactory::apply_density(sla::SampleConfigFactory::create(point_cfg.head_diameter),
                                                                             point_cfg.density_relative);

    sla::LayerSupportPoints layer_points = sla::generate_support_points(generator_data, point_cfg, cancel);
    // How far a point may move to reach the mesh surface: one layer height (mm).
    const double allowed_move = heights.size() > 1 ? double(heights[1] - heights[0]) + std::numeric_limits<float>::epsilon() :
                                                     std::max(0.01, cfg.layer_height.value);
    AABBMesh           indexed_mesh(mesh);
    sla::SupportPoints points = sla::move_on_mesh_surface(layer_points, indexed_mesh, allowed_move, cancel);

    // Like PrusaSlicer without elevation: points on the bed-facing bottom are not needed when the
    // object sits on the bed (or the raft).
    if (cfg.object_elevation.value < EPSILON) {
        const float ground = float(mesh.bounding_box().min.z() + EPSILON);
        points.erase(std::remove_if(points.begin(), points.end(), [ground](const sla::SupportPoint &p) { return p.pos.z() <= ground; }),
                     points.end());
    }

    // Support enforcers and blockers, modelled and painted, and the threshold mask decide which of
    // the automatic points stay.
    std::vector<Polygons> enforcers = object.slice_support_enforcers();
    std::vector<Polygons> blockers  = object.slice_support_blockers();
    enforcers.resize(heights.size());
    blockers.resize(heights.size());
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);

    std::vector<PointFilter> filters;
    filters.reserve(heights.size());
    for (size_t layer_id = 0; layer_id < heights.size(); ++layer_id) {
        cancel();
        filters.emplace_back(enforcers[layer_id], blockers[layer_id], threshold_demand.empty() ? nullptr : &threshold_demand[layer_id],
                             s.enforcers_only);
    }
    enforcers.clear();
    blockers.clear();
    threshold_demand.clear();

    sla::SupportPoints kept;
    std::vector<size_t> kept_layer;
    kept.reserve(points.size());
    for (sla::SupportPoint &support_point : points) {
        const size_t layer_id = point_layer_index(object, heights, support_point);
        if (filters[layer_id].accepts(Point::new_scale(support_point.pos.x(), support_point.pos.y()))) {
            kept.emplace_back(std::move(support_point));
            kept_layer.push_back(layer_id);
        }
    }

    sla::SupportTreeConfig tree_cfg;
    tree_cfg.enabled                 = true;
    tree_cfg.tree_type               = s.branching ? sla::SupportTreeType::Branching : sla::SupportTreeType::Default;
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
    tree_cfg.object_elevation_mm             = slicing_params.object_print_z_min - slicing_params.raft_interface_top_z;
    tree_cfg.max_bridges_on_pillar           = unsigned(std::max(0, st.max_bridges_on_pillar));
    tree_cfg.max_weight_on_model_support     = st.max_weight_on_model;

    sla::SupportableMesh supportable(mesh.its, kept, tree_cfg);
    supportable.zoffset = mesh.bounding_box().min.z();

    sla::JobController controller;
    controller.stopcondition = [&object]() { return object.print()->canceled(); };
    controller.cancelfn      = cancel;
    auto [tree_mesh, tree_output] = sla::create_support_tree(supportable, controller);
    (void) tree_mesh;

    // Contact demand: a printable disc around each kept head, on the head's layer. Orca's interface,
    // Z gap, X/Y gap and raft settings then apply to these contacts.
    std::vector<Polygons> contact_demand(heights.size());
    const double          contact_radius = scale_(0.5 * std::max(st.head_front_diameter_mm, printable_diameter));
    // Circle approximation error: 0.02 mm, scaled.
    const double circle_error = std::max(1., scale_(0.02));
    for (size_t i = 0; i < kept.size(); ++i) {
        Polygon disc = make_circle(contact_radius, circle_error);
        disc.translate(Point::new_scale(kept[i].pos.x(), kept[i].pos.y()));
        contact_demand[kept_layer[i]].emplace_back(std::move(disc));
    }
    for (Polygons &layer_demand : contact_demand)
        if (!layer_demand.empty())
            layer_demand = union_(layer_demand);

    SupportMaterialOverrides overrides;
    overrides.contact_demand = &contact_demand;
    overrides.body_at_z      = [&tree_output](coordf_t z) { return sla::slice_support_tree_at_height(tree_output, float(z)); };
    overrides.keep_narrow_islands = true;
    PrintObjectSupportMaterial support(&object, slicing_params, overrides);
    support.generate(object);

    BOOST_LOG_TRIVIAL(info) << "Resin support: " << kept.size() << " contact points";
}

} // namespace ResinSupport
} // namespace ModHost
} // namespace Slic3r
