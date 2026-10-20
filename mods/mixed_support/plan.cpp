#include "plan.hpp"

#include <algorithm>

namespace mixed {

bool any_polygons(const std::vector<Polygons> &layers)
{
    return std::any_of(layers.begin(), layers.end(), [](const Polygons &p) { return !p.empty(); });
}

bool Plan::has_normal() const { return any_polygons(normal_mask); }
bool Plan::has_tree() const { return any_polygons(tree_mask); }

Plan plan(const std::vector<Polygons> &demand, const std::vector<Polygons> &buildplate_shadow, double coverage_threshold_percent,
          bool selective_merge, const std::vector<Polygons> *painted_normal, const std::vector<Polygons> *painted_tree)
{
    Plan         out;
    const size_t layer_count = std::max(demand.size(), buildplate_shadow.size());
    out.normal_mask.resize(layer_count);
    out.tree_mask.resize(layer_count);
    out.normal_paint_fallback.resize(layer_count);
    const double threshold = std::clamp(coverage_threshold_percent, 0., 100.); // percent

    for (size_t layer_id = 0; layer_id < demand.size(); ++layer_id) {
        ExPolygons islands = union_ex(demand[layer_id]);
        // A fixed order, so the decisions list is deterministic.
        std::sort(islands.begin(), islands.end(), [](const ExPolygon &a, const ExPolygon &b) {
            const BoundingBox ba = get_extents(a), bb = get_extents(b);
            if (ba.min.y() != bb.min.y()) return ba.min.y() < bb.min.y();
            if (ba.min.x() != bb.min.x()) return ba.min.x() < bb.min.x();
            if (ba.max.y() != bb.max.y()) return ba.max.y() < bb.max.y();
            if (ba.max.x() != bb.max.x()) return ba.max.x() < bb.max.x();
            if (a.contour.points.size() != b.contour.points.size()) return a.contour.points.size() < b.contour.points.size();
            return std::lexicographical_compare(a.contour.points.begin(), a.contour.points.end(), b.contour.points.begin(),
                                                b.contour.points.end());
        });
        const Polygons empty;
        const Polygons &shadow = layer_id < buildplate_shadow.size() ? buildplate_shadow[layer_id] : empty;
        for (ExPolygon &island : islands) {
            const long double demand_area = std::max<long double>(0., area(island)); // scaled area
            ExPolygons        reachable;
            if (shadow.empty() || !get_extents(shadow).overlap(get_extents(island)))
                reachable = {island};
            else
                reachable = diff_ex(to_polygons(island), shadow);
            const long double reachable_area = std::max<long double>(0., area(reachable)); // scaled area
            // Relative tolerance on scaled areas, so equal areas compare equal after rounding.
            const long double tolerance     = std::max<long double>(1.L, demand_area) * 1e-12L;
            const long double blocked_area  = std::max<long double>(0., demand_area - reachable_area);
            const bool        has_reachable = reachable_area > tolerance;
            const bool        has_blocked   = blocked_area > tolerance;
            Channel           channel;
            if (selective_merge)
                channel = has_reachable && has_blocked ? Channel::Split : has_reachable ? Channel::Normal : Channel::Tree;
            else
                // Inclusive: an island exactly at the threshold goes to normal support.
                channel = reachable_area * 100.L + tolerance >= static_cast<long double>(threshold) * demand_area ? Channel::Normal :
                                                                                                                 Channel::Tree;
            out.decisions.push_back(channel);
            if (channel == Channel::Split) {
                append(out.normal_mask[layer_id], to_polygons(reachable));
                append(out.tree_mask[layer_id], to_polygons(diff_ex(to_polygons(island), to_polygons(reachable))));
            } else {
                append(channel == Channel::Normal ? out.normal_mask[layer_id] : out.tree_mask[layer_id], to_polygons(island));
            }
        }
    }
    const bool painted = painted_normal != nullptr || painted_tree != nullptr;
    for (size_t layer_id = 0; painted && layer_id < demand.size(); ++layer_id) {
        if (demand[layer_id].empty())
            continue;
        const Polygons  empty;
        const Polygons  layer_demand = union_(demand[layer_id]);
        const Polygons &pn           = painted_normal != nullptr && layer_id < painted_normal->size() ? (*painted_normal)[layer_id] : empty;
        const Polygons &pt           = painted_tree != nullptr && layer_id < painted_tree->size() ? (*painted_tree)[layer_id] : empty;
        const Polygons  normal_paint = intersection(layer_demand, pn);
        const Polygons  tree_paint   = intersection(layer_demand, pt);
        // Projections of facets painted for different channels can overlap; tree painting wins there,
        // so a tree request is never silently replaced by normal support.
        const Polygons normal_only = tree_paint.empty() ? normal_paint : diff(normal_paint, tree_paint);
        Polygons       all_paint   = normal_only;
        append(all_paint, tree_paint);
        if (!all_paint.empty()) {
            all_paint                 = union_(all_paint);
            out.normal_mask[layer_id] = diff(out.normal_mask[layer_id], all_paint);
            out.tree_mask[layer_id]   = diff(out.tree_mask[layer_id], all_paint);
        }
        const Polygons &shadow           = layer_id < buildplate_shadow.size() ? buildplate_shadow[layer_id] : empty;
        const Polygons  reachable_normal = shadow.empty() ? normal_only : diff(normal_only, shadow);
        out.normal_paint_fallback[layer_id] = normal_only.empty() ? Polygons{} : diff(normal_only, reachable_normal);
        append(out.normal_mask[layer_id], reachable_normal);
        append(out.tree_mask[layer_id], tree_paint);
        append(out.tree_mask[layer_id], out.normal_paint_fallback[layer_id]);
    }
    for (size_t layer_id = 0; layer_id < layer_count; ++layer_id) {
        if (!out.normal_mask[layer_id].empty())
            out.normal_mask[layer_id] = union_(out.normal_mask[layer_id]);
        if (!out.tree_mask[layer_id].empty())
            out.tree_mask[layer_id] = union_(out.tree_mask[layer_id]);
    }
    return out;
}

} // namespace mixed
