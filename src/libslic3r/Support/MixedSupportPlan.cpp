#include "MixedSupportPlan.hpp"

#include "../Layer.hpp"
#include "../Print.hpp"
#include "CuraStyleSupport.hpp"
#include "SupportMaterial.hpp"
#include "SupportParameters.hpp"
#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"

#include <boost/geometry.hpp>
#include <boost/geometry/index/rtree.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <numeric>
#include <unordered_map>

namespace Slic3r {
namespace {

namespace bg  = boost::geometry;
namespace bgi = boost::geometry::index;

using SpatialPoint = bg::model::point<double, 2, bg::cs::cartesian>;
using SpatialBox   = bg::model::box<SpatialPoint>;
using SpatialEntry = std::pair<SpatialBox, size_t>;
using SpatialIndex = bgi::rtree<SpatialEntry, bgi::rstar<16, 4>>;

SpatialBox spatial_box(const BoundingBox &bbox)
{
    return SpatialBox(
        SpatialPoint(double(bbox.min.x()), double(bbox.min.y())),
        SpatialPoint(double(bbox.max.x()), double(bbox.max.y())));
}

class DisjointSet
{
public:
    explicit DisjointSet(size_t count) : m_parent(count), m_rank(count, 0)
    {
        std::iota(m_parent.begin(), m_parent.end(), size_t(0));
    }

    size_t find(size_t value)
    {
        while (m_parent[value] != value) {
            m_parent[value] = m_parent[m_parent[value]];
            value = m_parent[value];
        }
        return value;
    }

    void unite(size_t lhs, size_t rhs)
    {
        lhs = find(lhs);
        rhs = find(rhs);
        if (lhs == rhs)
            return;
        if (m_rank[lhs] < m_rank[rhs])
            std::swap(lhs, rhs);
        m_parent[rhs] = lhs;
        if (m_rank[lhs] == m_rank[rhs])
            ++m_rank[lhs];
    }

private:
    std::vector<size_t>   m_parent;
    std::vector<unsigned> m_rank;
};

struct DemandPolygon {
    size_t      layer_id { 0 };
    ExPolygon   polygon;
    ExPolygons  connected_area;
    BoundingBox bbox;
    long double demand_area { 0. };
    long double reachable_area { 0. };
    ExPolygons  reachable;
};

bool any_polygons(const std::vector<Polygons> &layers)
{
    return std::any_of(layers.begin(), layers.end(), [](const Polygons &polygons) { return !polygons.empty(); });
}

} // namespace

std::vector<Polygons> detect_mixed_support_demand(const PrintObject &object)
{
    std::vector<Polygons> demand(object.layer_count());
    if (object.config().mixed_normal_support_generator.value == mnsgCura) {
        CuraStyleSupportGenerator detector(&object, object.slicing_parameters());
        return detector.detect_support_demand();
    }

    SupportGeneratorLayerStorage storage;
    PrintObjectSupportMaterial detector(&object, object.slicing_parameters());
    const SupportGeneratorLayersPtr contacts = detector.detect_top_contact_layers(storage, false);
    for (const SupportGeneratorLayer *contact : contacts) {
        if (contact == nullptr || contact->idx_object_layer_above >= demand.size())
            continue;
        if (contact->overhang_polygons && !contact->overhang_polygons->empty())
            append(demand[contact->idx_object_layer_above], *contact->overhang_polygons);
        else if (!contact->polygons.empty())
            append(demand[contact->idx_object_layer_above], contact->polygons);
    }
    for (Polygons &layer : demand)
        if (!layer.empty())
            layer = union_(layer);
    return demand;
}

MixedSupportPlan MixedSupportPlan::build(const PrintObject &object, const std::vector<Polygons> &support_demand)
{
    const SupportParameters support_parameters(object);
    // Component connection dilation: effective support extrusion width in scaled XY coordinates.
    const coord_t connection_offset = std::max<coord_t>(
        SCALED_EPSILON, scale_(support_parameters.support_extrusion_width));
    std::vector<Polygons> painted_normal(object.layer_count());
    std::vector<Polygons> painted_tree(object.layer_count());
    std::vector<Polygons> painted_support(object.layer_count());
    object.project_and_append_mixed_support_facets(EnforcerBlockerType::ENFORCER, painted_normal);
    object.project_and_append_mixed_support_facets(EnforcerBlockerType::BLOCKER, painted_tree);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, painted_support);
    for (size_t layer_id = 0; layer_id < painted_support.size(); ++layer_id) {
        // A channel annotation is meaningful only while the same surface is
        // still explicitly painted as a support enforcer. This also makes old
        // or externally edited files with stale channel data harmless.
        painted_normal[layer_id] = to_polygons(intersection_ex(painted_normal[layer_id], painted_support[layer_id]));
        painted_tree[layer_id] = to_polygons(intersection_ex(painted_tree[layer_id], painted_support[layer_id]));
    }
    return build_for_geometry(
        support_demand, buildplate_covered_by_object(object), connection_offset,
        object.config().mixed_normal_coverage_threshold.value,
        object.config().mixed_selective_merge.value,
        &painted_normal, &painted_tree);
}

MixedSupportPlan MixedSupportPlan::build_for_geometry(
    const std::vector<Polygons> &support_demand,
    const std::vector<Polygons> &buildplate_shadow,
    coord_t connection_offset,
    double normal_coverage_threshold_percent,
    bool selective_merge,
    const std::vector<Polygons> *painted_normal,
    const std::vector<Polygons> *painted_tree)
{
    MixedSupportPlan plan;
    const size_t layer_count = std::max(support_demand.size(), buildplate_shadow.size());
    plan.m_normal_mask.resize(layer_count);
    plan.m_tree_mask.resize(layer_count);
    plan.m_buildplate_shadow = buildplate_shadow;
    plan.m_buildplate_shadow.resize(layer_count);
    plan.m_normal_paint_fallback.resize(layer_count);
    connection_offset = std::max<coord_t>(SCALED_EPSILON, connection_offset);

    std::vector<DemandPolygon> polygons;
    for (size_t layer_id = 0; layer_id < std::min(layer_count, support_demand.size()); ++layer_id) {
        ExPolygons normalized = union_ex(support_demand[layer_id]);
        std::sort(normalized.begin(), normalized.end(), [](const ExPolygon &lhs, const ExPolygon &rhs) {
            const BoundingBox lhs_bbox = get_extents(lhs);
            const BoundingBox rhs_bbox = get_extents(rhs);
            if (lhs_bbox.min.y() != rhs_bbox.min.y()) return lhs_bbox.min.y() < rhs_bbox.min.y();
            if (lhs_bbox.min.x() != rhs_bbox.min.x()) return lhs_bbox.min.x() < rhs_bbox.min.x();
            if (lhs_bbox.max.y() != rhs_bbox.max.y()) return lhs_bbox.max.y() < rhs_bbox.max.y();
            if (lhs_bbox.max.x() != rhs_bbox.max.x()) return lhs_bbox.max.x() < rhs_bbox.max.x();
            if (lhs.contour.points.size() != rhs.contour.points.size())
                return lhs.contour.points.size() < rhs.contour.points.size();
            return std::lexicographical_compare(
                lhs.contour.points.begin(), lhs.contour.points.end(),
                rhs.contour.points.begin(), rhs.contour.points.end());
        });
        for (ExPolygon &polygon : normalized) {
            DemandPolygon item;
            item.layer_id = layer_id;
            item.polygon = std::move(polygon);
            item.connected_area = offset_ex(item.polygon, connection_offset);
            if (item.connected_area.empty())
                item.connected_area = { item.polygon };
            item.bbox = get_extents(item.connected_area);
            item.demand_area = std::max<long double>(0., item.polygon.area());
            polygons.emplace_back(std::move(item));
        }
    }

    if (polygons.empty())
        return plan;

    std::vector<SpatialIndex> indices(layer_count);
    DisjointSet components(polygons.size());
    for (size_t polygon_id = 0; polygon_id < polygons.size(); ++polygon_id) {
        const DemandPolygon &polygon = polygons[polygon_id];
        const size_t first_layer = polygon.layer_id == 0 ? 0 : polygon.layer_id - 1;
        for (size_t layer_id = first_layer; layer_id <= polygon.layer_id; ++layer_id) {
            std::vector<SpatialEntry> candidates;
            indices[layer_id].query(bgi::intersects(spatial_box(polygon.bbox)), std::back_inserter(candidates));
            std::sort(candidates.begin(), candidates.end(), [](const SpatialEntry &lhs, const SpatialEntry &rhs) {
                return lhs.second < rhs.second;
            });
            for (const SpatialEntry &candidate : candidates)
                if (overlaps(polygon.connected_area, polygons[candidate.second].connected_area))
                    components.unite(polygon_id, candidate.second);
        }
        indices[polygon.layer_id].insert({ spatial_box(polygon.bbox), polygon_id });
    }

    const double threshold = std::clamp(normal_coverage_threshold_percent, 0., 100.);
    for (DemandPolygon &polygon : polygons) {
        if (threshold == 0.) {
            polygon.reachable_area = polygon.demand_area;
            polygon.reachable = { polygon.polygon };
            continue;
        }
        const Polygons &shadow = plan.m_buildplate_shadow[polygon.layer_id];
        if (shadow.empty() || !get_extents(shadow).overlap(get_extents(polygon.polygon))) {
            polygon.reachable_area = polygon.demand_area;
            polygon.reachable = { polygon.polygon };
        } else {
            polygon.reachable = diff_ex(polygon.polygon, shadow);
            polygon.reachable_area = std::max<long double>(0., area(polygon.reachable));
        }
    }

    std::unordered_map<size_t, size_t> decision_by_root;
    for (size_t polygon_id = 0; polygon_id < polygons.size(); ++polygon_id) {
        const size_t root = components.find(polygon_id);
        auto [it, inserted] = decision_by_root.emplace(root, plan.m_decisions.size());
        if (inserted) {
            MixedSupportComponentDecision decision;
            decision.id = plan.m_decisions.size();
            plan.m_decisions.emplace_back(std::move(decision));
        }
        MixedSupportComponentDecision &decision = plan.m_decisions[it->second];
        decision.polygon_ids.push_back(polygon_id);
        decision.demand_area += polygons[polygon_id].demand_area;
        decision.reachable_area += polygons[polygon_id].reachable_area;
    }

    for (MixedSupportComponentDecision &decision : plan.m_decisions) {
        decision.coverage_percent = decision.demand_area > 0. ?
            double(100.L * decision.reachable_area / decision.demand_area) : 100.;
        const long double lhs = decision.reachable_area * 100.L;
        const long double rhs = static_cast<long double>(threshold) * decision.demand_area;
        // Relative tolerance in scaled area-percent units. It is applied only at the final channel boundary.
        const long double tolerance = std::max<long double>(1.L, decision.demand_area) * 1e-12L;
        decision.channel = lhs + tolerance >= rhs ? MixedSupportChannel::Normal : MixedSupportChannel::Tree;
        decision.selectively_split = selective_merge && decision.channel == MixedSupportChannel::Tree &&
            decision.reachable_area > tolerance;
        for (size_t polygon_id : decision.polygon_ids) {
            const DemandPolygon &polygon = polygons[polygon_id];
            if (decision.selectively_split) {
                append(plan.m_normal_mask[polygon.layer_id], to_polygons(polygon.reachable));
                append(plan.m_tree_mask[polygon.layer_id],
                    to_polygons(diff_ex(polygon.polygon, polygon.reachable)));
            } else {
                Polygons &destination = decision.channel == MixedSupportChannel::Normal ?
                    plan.m_normal_mask[polygon.layer_id] : plan.m_tree_mask[polygon.layer_id];
                append(destination, to_polygons(polygon.polygon));
            }
        }
        if (decision.selectively_split)
            decision.channel = MixedSupportChannel::Mixed;
        const long double scaled_area_to_mm2 =
            static_cast<long double>(SCALING_FACTOR) * static_cast<long double>(SCALING_FACTOR);
        BOOST_LOG_TRIVIAL(debug)
            << "Mixed support component " << decision.id
            << ": demand_mm2=" << double(decision.demand_area * scaled_area_to_mm2)
            << " reachable_mm2=" << double(decision.reachable_area * scaled_area_to_mm2)
            << " coverage_percent=" << decision.coverage_percent
            << " threshold_percent=" << threshold
            << " channel=" << (decision.channel == MixedSupportChannel::Normal ? "normal" :
                                 decision.channel == MixedSupportChannel::Tree ? "tree" : "selective-mixed");
    }

    for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
        const Polygons demand = layer_id < support_demand.size() ? union_(support_demand[layer_id]) : Polygons{};
        if (!demand.empty()) {
            const Polygons normal_paint = painted_normal != nullptr && layer_id < painted_normal->size() ?
                to_polygons(intersection_ex(demand, (*painted_normal)[layer_id])) : Polygons{};
            const Polygons tree_paint = painted_tree != nullptr && layer_id < painted_tree->size() ?
                to_polygons(intersection_ex(demand, (*painted_tree)[layer_id])) : Polygons{};

            // Projected facet masks may overlap even though each source facet owns one channel.
            // Tree wins that ambiguous projection because it preserves the build-plate-only
            // invariant of the normal channel and can route around model geometry.
            const Polygons normal_only = tree_paint.empty() ? normal_paint :
                to_polygons(diff_ex(normal_paint, tree_paint));
            Polygons painted = normal_only;
            append(painted, tree_paint);
            if (!painted.empty())
                painted = union_(painted);

            if (!painted.empty()) {
                plan.m_normal_mask[layer_id] = to_polygons(diff_ex(plan.m_normal_mask[layer_id], painted));
                plan.m_tree_mask[layer_id] = to_polygons(diff_ex(plan.m_tree_mask[layer_id], painted));
            }

            const Polygons reachable_normal = plan.m_buildplate_shadow[layer_id].empty() ? normal_only :
                to_polygons(diff_ex(normal_only, plan.m_buildplate_shadow[layer_id]));
            plan.m_normal_paint_fallback[layer_id] = normal_only.empty() ? Polygons{} :
                to_polygons(diff_ex(normal_only, reachable_normal));

            append(plan.m_normal_mask[layer_id], reachable_normal);
            append(plan.m_tree_mask[layer_id], tree_paint);
            append(plan.m_tree_mask[layer_id], plan.m_normal_paint_fallback[layer_id]);
        }
        if (!plan.m_normal_mask[layer_id].empty())
            plan.m_normal_mask[layer_id] = union_(plan.m_normal_mask[layer_id]);
        if (!plan.m_tree_mask[layer_id].empty())
            plan.m_tree_mask[layer_id] = union_(plan.m_tree_mask[layer_id]);
    }
    return plan;
}

bool MixedSupportPlan::has_normal_demand() const { return any_polygons(m_normal_mask); }
bool MixedSupportPlan::has_tree_demand() const { return any_polygons(m_tree_mask); }
bool MixedSupportPlan::has_normal_paint_fallback() const { return any_polygons(m_normal_paint_fallback); }

} // namespace Slic3r
