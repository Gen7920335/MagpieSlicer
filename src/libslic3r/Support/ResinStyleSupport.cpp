#include "ResinStyleSupport.hpp"

#include "../ClipperUtils.hpp"
#include "../Exception.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "SupportCommon.hpp"
#include "SupportMaterial.hpp"
#include "SupportTiming.hpp"
#include "../SLA/SupportIslands/SampleConfigFactory.hpp"
#include "../SLA/SupportPointGenerator.hpp"
#include "../SLA/SupportTree.hpp"
#include "../SLA/SupportTreeSlicer.hpp"

#include <algorithm>
#include <limits>

#include <boost/log/trivial.hpp>

namespace Slic3r {
namespace {

struct ResinStrategyValues
{
    double head_front_diameter;
    double head_width;
    double pillar_diameter;
    double small_pillar_percent;
    int    max_bridges;
    double max_weight_on_model;
    SLAPillarConnectionMode connection_mode;
    bool   buildplate_only;
    double widening_factor;
    double base_diameter;
    double base_height;
    double base_safety_distance;
    double critical_angle;
    double max_bridge_length;
    double max_pillar_link_distance;
    double object_elevation;
};

ResinStrategyValues strategy_values(const PrintObjectConfig &cfg)
{
    if (cfg.resin_support_tree_type.value == rstBranching) {
        return {
            cfg.resin_branching_support_head_front_diameter.value,
            cfg.resin_branching_support_head_width.value,
            cfg.resin_branching_support_pillar_diameter.value,
            cfg.resin_branching_support_small_pillar_diameter_percent.value,
            cfg.resin_branching_support_max_bridges_on_pillar.value,
            cfg.resin_branching_support_max_weight_on_model.value,
            cfg.resin_branching_support_pillar_connection_mode.value,
            cfg.resin_branching_support_buildplate_only.value,
            cfg.resin_branching_support_pillar_widening_factor.value,
            cfg.resin_branching_support_base_diameter.value,
            cfg.resin_branching_support_base_height.value,
            cfg.resin_branching_support_base_safety_distance.value,
            cfg.resin_branching_support_critical_angle.value,
            cfg.resin_branching_support_max_bridge_length.value,
            cfg.resin_branching_support_max_pillar_link_distance.value,
            cfg.resin_branching_support_object_elevation.value
        };
    }

    return {
        cfg.resin_support_head_front_diameter.value,
        cfg.resin_support_head_width.value,
        cfg.resin_support_pillar_diameter.value,
        cfg.resin_support_small_pillar_diameter_percent.value,
        cfg.resin_support_max_bridges_on_pillar.value,
        cfg.resin_support_max_weight_on_model.value,
        cfg.resin_support_pillar_connection_mode.value,
        cfg.resin_support_buildplate_only.value,
        cfg.resin_support_pillar_widening_factor.value,
        cfg.resin_support_base_diameter.value,
        cfg.resin_support_base_height.value,
        cfg.resin_support_base_safety_distance.value,
        cfg.resin_support_critical_angle.value,
        cfg.resin_support_max_bridge_length.value,
        cfg.resin_support_max_pillar_link_distance.value,
        cfg.resin_support_object_elevation.value
    };
}

sla::PillarConnectionMode connection_mode(SLAPillarConnectionMode mode)
{
    switch (mode) {
    case slapcmZigZag: return sla::PillarConnectionMode::zigzag;
    case slapcmCross:  return sla::PillarConnectionMode::cross;
    case slapcmDynamic:
    default:           return sla::PillarConnectionMode::dynamic;
    }
}

bool contains_point(const ExPolygons &polygons, const Point &point)
{
    return std::any_of(polygons.begin(), polygons.end(),
        [&point](const ExPolygon &polygon) { return polygon.contains(point); });
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

size_t support_point_layer_index(
    const PrintObject &object, const std::vector<float> &heights,
    const sla::SupportPoint &support_point)
{
    const Point point = Point::new_scale(support_point.pos.x(), support_point.pos.y());
    size_t best = heights.size();
    float best_distance = std::numeric_limits<float>::max();
    for (size_t layer_id = 0; layer_id < heights.size(); ++layer_id) {
        if (!contains_point(object.layers()[layer_id]->lslices, point))
            continue;
        const float distance = std::abs(heights[layer_id] - support_point.pos.z());
        if (distance < best_distance) {
            best = layer_id;
            best_distance = distance;
        }
    }
    return best < heights.size() ? best : nearest_height_index(heights, support_point.pos.z());
}

} // namespace

ResinSupportPointFilter::ResinSupportPointFilter(const Polygons &enforcers, const Polygons &blockers,
                                               const Polygons *threshold, bool enforcers_only)
    : m_enforcers(union_ex(enforcers)), m_blockers(union_ex(blockers)),
      m_threshold(threshold ? union_ex(*threshold) : ExPolygons{}),
      m_threshold_enabled(threshold != nullptr), m_enforcers_only(enforcers_only)
{}

bool ResinSupportPointFilter::accepts(const Point &point) const
{
    const bool enforced = contains_point(m_enforcers, point);
    const bool blocked = contains_point(m_blockers, point);
    const bool passes_threshold = !m_threshold_enabled || contains_point(m_threshold, point);
    return enforced || (!blocked && !m_enforcers_only && passes_threshold);
}

void ResinStyleSupport::generate()
{
    SupportProfileStage total_timing("support-resin", "Generate complete resin style support", m_object.layer_count());
    const PrintObjectConfig &cfg = m_object.config();
    if (!cfg.enable_support.value || !is_resin(cfg.support_type.value) || m_object.layers().empty())
        return;

    if (m_object.print()->canceled())
        throw CanceledException();
    SupportProfileStage mesh_timing("support-resin", "Prepare resin support mesh and slices", m_object.layer_count());
    const ResinStrategyValues values = strategy_values(cfg);

    // Build the same centered object-space mesh used by FFF slicing, then move
    // it to its actual print Z (raft plus independent resin elevation).
    TriangleMesh mesh = m_object.model_object()->raw_mesh();
    mesh.transform(m_object.trafo_centered(), true);
    mesh.translate(0.f, 0.f, float(m_slicing_parameters.object_print_z_min));
    if (mesh.empty())
        return;

    std::vector<ExPolygons> model_slices;
    std::vector<float> heights;
    model_slices.reserve(m_object.layer_count());
    heights.reserve(m_object.layer_count());
    for (const Layer *layer : m_object.layers()) {
        model_slices.emplace_back(layer->lslices);
        heights.emplace_back(float(layer->slice_z + m_slicing_parameters.object_print_z_min));
    }
    mesh_timing.finish(model_slices.size());

    // A non-zero FFF threshold narrows the SLA generator's automatic point
    // candidates to areas that the preceding layer cannot support at that
    // angle. Zero deliberately preserves the native SLA island/peninsula
    // detector for backward compatibility.
    std::vector<Polygons> threshold_demand;
    SupportProfileStage demand_timing("support-resin", "Build resin threshold demand", model_slices.size());
    if (cfg.support_threshold_angle.value > 0) {
        threshold_demand.resize(model_slices.size());
        threshold_demand.front() = to_polygons(model_slices.front());
        for (size_t layer_id = 1; layer_id < model_slices.size(); ++layer_id) {
            SupportProfileStage layer_timing("support-resin-demand", "Build resin threshold layer", model_slices[layer_id].size());
            const coord_t supported_offset = support_overhang_offset_from_threshold(
                m_object.layers()[layer_id - 1]->height,
                cfg.support_threshold_angle.value);
            Polygons supported = offset(
                to_polygons(model_slices[layer_id - 1]), float(supported_offset),
                ClipperLib::jtSquare, 0.);
            threshold_demand[layer_id] = diff(to_polygons(model_slices[layer_id]), supported);
            layer_timing.finish(threshold_demand[layer_id].size(), "object_layer=" + std::to_string(layer_id));
        }
    }
    demand_timing.finish(threshold_demand.size());

    sla::ThrowOnCancel cancel = [this]() {
        if (m_object.print()->canceled())
            throw CanceledException();
    };
    SupportProfileStage generator_data_timing("support-resin", "Prepare resin support point generator", model_slices.size());
    sla::SupportPointGeneratorData generator_data = sla::prepare_generator_data(
        std::move(model_slices), heights, sla::PrepareSupportConfig{}, cancel);
    generator_data_timing.finish(heights.size());

    const PrintConfig &print_cfg = m_object.print()->config();
    const Flow support_flow = support_material_flow(&m_object);
    const double nozzle = support_flow.nozzle_diameter();
    const double configured_width = support_flow.width();
    const double printable_diameter = std::max(nozzle, configured_width);

    sla::SupportPointGeneratorConfig point_cfg;
    point_cfg.density_relative = float(cfg.resin_support_points_density_relative.value / 100.0);
    point_cfg.head_diameter = float(std::max(values.head_front_diameter, printable_diameter));
    point_cfg.island_configuration = sla::SampleConfigFactory::apply_density(
        sla::SampleConfigFactory::create(point_cfg.head_diameter), point_cfg.density_relative);

    SupportProfileStage points_timing("support-resin", "Generate resin support contact points", heights.size());
    sla::LayerSupportPoints layer_points = sla::generate_support_points(
        generator_data, point_cfg, cancel);
    const double allowed_move = heights.size() > 1 ?
        double(heights[1] - heights[0]) + std::numeric_limits<float>::epsilon() :
        std::max(0.01, cfg.layer_height.value);
    AABBMesh indexed_mesh(mesh);
    sla::SupportPoints points = sla::move_on_mesh_surface(
        layer_points, indexed_mesh, allowed_move, cancel);
    points_timing.finish(points.size());

    // Match PrusaSlicer's zero-elevation behavior: points on the bed-facing
    // bottom are redundant when the model itself is not floating.
    if (values.object_elevation < EPSILON) {
        const float ground = float(mesh.bounding_box().min.z() + EPSILON);
        points.erase(std::remove_if(points.begin(), points.end(),
            [ground](const sla::SupportPoint &point) { return point.pos.z() <= ground; }),
            points.end());
    }

    // Apply FFF support volumes and painted enforcer/blocker facets to the
    // latest automatic point set before building the tree.
    SupportProfileStage painting_timing("support-resin", "Apply resin enforcers blockers and threshold", points.size());
    std::vector<Polygons> enforcers = m_object.slice_support_enforcers();
    std::vector<Polygons> blockers  = m_object.slice_support_blockers();
    enforcers.resize(heights.size());
    blockers.resize(heights.size());
    m_object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
    m_object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);

    std::vector<ResinSupportPointFilter> point_filters;
    point_filters.reserve(heights.size());
    {
        SupportProfileStage masks_timing("support-resin", "Normalize resin point masks", heights.size());
        for (size_t layer_id = 0; layer_id < heights.size(); ++layer_id) {
            cancel();
            // Reconstruct contour/hole topology once per layer, never per point.
            point_filters.emplace_back(enforcers[layer_id], blockers[layer_id],
                threshold_demand.empty() ? nullptr : &threshold_demand[layer_id], cfg.resin_support_enforcers_only.value);
            enforcers[layer_id].clear();
            blockers[layer_id].clear();
            if (!threshold_demand.empty()) threshold_demand[layer_id].clear();
        }
        masks_timing.finish(point_filters.size());
    }

    sla::SupportPoints filtered_points;
    filtered_points.reserve(points.size());
    for (sla::SupportPoint &support_point : points) {
        const size_t layer_id = support_point_layer_index(m_object, heights, support_point);
        const Point point = Point::new_scale(support_point.pos.x(), support_point.pos.y());
        if (point_filters[layer_id].accepts(point))
            filtered_points.emplace_back(std::move(support_point));
    }
    painting_timing.finish(filtered_points.size());

    sla::SupportTreeConfig tree_cfg;
    tree_cfg.enabled = true;
    tree_cfg.tree_type = cfg.resin_support_tree_type.value == rstBranching ?
        sla::SupportTreeType::Branching : sla::SupportTreeType::Default;
    tree_cfg.head_front_radius_mm = 0.5 * std::max(values.head_front_diameter, printable_diameter);
    const double pillar_radius = 0.5 * std::max(values.pillar_diameter, printable_diameter);
    tree_cfg.head_back_radius_mm = pillar_radius;
    tree_cfg.head_fallback_radius_mm = std::max(
        0.5 * printable_diameter,
        0.01 * values.small_pillar_percent * pillar_radius);
    tree_cfg.head_penetration_mm = 0.;
    tree_cfg.head_width_mm = values.head_width;
    tree_cfg.pillar_connection_mode = connection_mode(values.connection_mode);
    tree_cfg.ground_facing_only = values.buildplate_only;
    tree_cfg.pillar_widening_factor = values.widening_factor;
    tree_cfg.base_radius_mm = 0.5 * std::max(values.base_diameter, printable_diameter);
    tree_cfg.base_height_mm = values.base_height;
    tree_cfg.pillar_base_safety_distance_mm = values.base_safety_distance;
    tree_cfg.bridge_slope = values.critical_angle * PI / 180.;
    tree_cfg.max_bridge_length_mm = values.max_bridge_length;
    tree_cfg.max_pillar_link_distance_mm = values.max_pillar_link_distance;
    tree_cfg.object_elevation_mm = values.object_elevation;
    tree_cfg.max_bridges_on_pillar = unsigned(std::max(0, values.max_bridges));
    tree_cfg.max_weight_on_model_support = values.max_weight_on_model;

    sla::SupportableMesh supportable(mesh.its, filtered_points, tree_cfg);
    supportable.zoffset = mesh.bounding_box().min.z();

    sla::JobController controller;
    controller.stopcondition = [this]() { return m_object.print()->canceled(); };
    controller.cancelfn = cancel;
    SupportProfileStage tree_timing("support-resin", "Build resin support tree", filtered_points.size());
    auto [tree_mesh, tree_output] = sla::create_support_tree(supportable, controller);
    (void) tree_mesh;
    tree_timing.finish(filtered_points.size());

    // Restrict native FFF contact patches to printable discs around the actual
    // resin support heads. Existing interface material, Z gaps, XY clearance,
    // patterns, raft and toolpath settings remain authoritative downstream.
    std::vector<Polygons> demand_mask(heights.size());
    const coord_t contact_radius = coord_t(scale_(0.5 * std::max(values.head_front_diameter, printable_diameter)));
    const coord_t circle_error = std::max<coord_t>(1, coord_t(scale_(0.02)));
    SupportProfileStage mask_timing("support-resin", "Build resin FFF contact mask", filtered_points.size());
    for (const sla::SupportPoint &support_point : filtered_points) {
        const size_t layer_id = support_point_layer_index(m_object, heights, support_point);
        Polygon disc = make_circle(contact_radius, circle_error);
        disc.translate(Point::new_scale(support_point.pos.x(), support_point.pos.y()));
        demand_mask[layer_id].emplace_back(std::move(disc));
    }
    for (Polygons &layer_mask : demand_mask)
        if (!layer_mask.empty())
            layer_mask = union_(layer_mask);
    mask_timing.finish(demand_mask.size());

    SupportProfileStage fff_timing("support-resin", "Convert resin tree into FFF support", demand_mask.size());
    PrintObjectSupportMaterial fff_support(
        &m_object, m_slicing_parameters, &demand_mask, false);
    fff_support.generate_from_resin_body(
        m_object,
        [&tree_output](coordf_t z) {
            return sla::slice_support_tree_at_height(tree_output, float(z));
        });
    fff_timing.finish(m_object.support_layer_count());

    BOOST_LOG_TRIVIAL(info) << "Resin style support generated "
                            << filtered_points.size() << " contact points";
    total_timing.finish(m_object.support_layer_count());
}

} // namespace Slic3r
