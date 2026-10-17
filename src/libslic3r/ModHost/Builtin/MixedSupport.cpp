// Mixed support: one object supported by a normal generator (Orca's or Cura's) where support can
// stand on the bed, and by a tree generator where it cannot. Ported from Magpie line 1
// (MixedSupportPlan and PrintObject::_generate_support_material). The plan splits the normal
// generator's own support demand into two masks; each generator runs on its mask; the tree
// generator avoids the normal support's plastic; the two sets of layers are merged by Z, with the
// normal support owning any overlap.

#include "MixedSupport.hpp"

#include "CuraSupport.hpp"

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ClipperUtils.hpp"
#include "libslic3r/ExPolygon.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/I18N.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/ModHost/ModSet.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/Support/SupportLayer.hpp"
#include "libslic3r/Support/SupportMaterial.hpp"
#include "libslic3r/Support/TreeSupport.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <unordered_map>

namespace Slic3r {
namespace ModHost {
namespace MixedSupport {

namespace {

bool any_polygons(const std::vector<Polygons> &layers)
{
    return std::any_of(layers.begin(), layers.end(), [](const Polygons &p) { return !p.empty(); });
}

SupportMaterialStyle tree_style(const std::string &name)
{
    if (name == "tree_slim")
        return smsTreeSlim;
    if (name == "tree_strong")
        return smsTreeStrong;
    if (name == "tree_hybrid")
        return smsTreeHybrid;
    return smsTreeOrganic;
}

void cancel_check(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

// Per object layer, the plastic of the normal support layers that overlap it in Z: the tree
// branches keep clear of it. Only extruded plastic counts; support_islands may span unfilled gaps.
std::vector<Polygons> tree_obstacles(const PrintObject &object, const SupportLayerPtrs &normal_layers)
{
    std::vector<Polygons> obstacles(object.layer_count());
    const coordf_t        raft_top = object.slicing_parameters().has_raft() ? object.slicing_parameters().raft_contact_top_z : 0.; // mm
    for (const SupportLayer *support_layer : normal_layers) {
        if (support_layer->print_z <= raft_top + EPSILON)
            continue;
        Polygons footprint = support_layer->support_fills.polygons_covered_by_width(float(SCALED_EPSILON));
        if (footprint.empty())
            continue;
        footprint = union_(footprint);
        const coordf_t bottom_z = support_layer->bottom_z();
        for (size_t layer_id = 0; layer_id < object.layer_count(); ++layer_id) {
            const Layer *object_layer = object.layers()[layer_id];
            if (object_layer->print_z + EPSILON < bottom_z)
                continue;
            if (object_layer->bottom_z() > support_layer->print_z + EPSILON)
                break;
            append(obstacles[layer_id], footprint);
        }
    }
    for (Polygons &layer : obstacles)
        if (!layer.empty())
            layer = union_(layer);
    return obstacles;
}

// Appends the parts of a tree path that do not run over the normal support's plastic. Both paths
// keep their full width: the owner coverage already includes the owner's width, and the cut is
// grown by this path's half width.
void append_residual_path(const ExtrusionPath &path, const ExPolygons &owner_coverage, std::unordered_map<coord_t, ExPolygons> &cuts,
                          ExtrusionEntityCollection &destination)
{
    if (owner_coverage.empty()) {
        destination.append(path);
        return;
    }
    // Half the residual path's width (scaled), at least SCALED_EPSILON.
    const coord_t half_width = std::max<coord_t>(SCALED_EPSILON, path.width > 0.f ? coord_t(scale_(0.5 * path.width)) : coord_t(0));
    auto [it, inserted]      = cuts.try_emplace(half_width);
    if (inserted)
        it->second = offset_ex(owner_coverage, float(half_width));
    path.subtract_expolygons(it->second, &destination);
}

void append_residual(const ExtrusionEntity &entity, const ExPolygons &owner_coverage, std::unordered_map<coord_t, ExPolygons> &cuts,
                     ExtrusionEntityCollection &destination)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const ExtrusionEntity *child : collection->entities)
            append_residual(*child, owner_coverage, cuts, destination);
    } else if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity)) {
        append_residual_path(*path, owner_coverage, cuts, destination);
    } else if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity)) {
        for (const ExtrusionPath &p : multipath->paths)
            append_residual_path(p, owner_coverage, cuts, destination);
    } else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity)) {
        for (const ExtrusionPath &p : loop->paths)
            append_residual_path(p, owner_coverage, cuts, destination);
    } else {
        throw Slic3r::SlicingError(_u8L("Mixed support met an extrusion it cannot merge."));
    }
}

// Merges the two generators' layers by print Z into the object's support layers. Layers at the
// same Z (within EPSILON) become one mixed layer owned by the normal layer.
void merge_layers(PrintObject &object, SupportLayerPtrs normal_layers, SupportLayerPtrs tree_layers)
{
    SupportLayerPtrs &out = object.support_layers();
    const auto by_z = [](const SupportLayer *a, const SupportLayer *b) { return a->print_z < b->print_z; };
    std::stable_sort(normal_layers.begin(), normal_layers.end(), by_z);
    std::stable_sort(tree_layers.begin(), tree_layers.end(), by_z);
    size_t n = 0, t = 0;
    while (n < normal_layers.size() || t < tree_layers.size()) {
        if (t >= tree_layers.size() || (n < normal_layers.size() && normal_layers[n]->print_z + EPSILON < tree_layers[t]->print_z)) {
            out.push_back(normal_layers[n++]);
            continue;
        }
        if (n >= normal_layers.size() || tree_layers[t]->print_z + EPSILON < normal_layers[n]->print_z) {
            out.push_back(tree_layers[t++]);
            continue;
        }
        SupportLayer *normal = normal_layers[n++];
        SupportLayer *tree   = tree_layers[t++];
        normal->height       = std::min(normal->height, tree->height);
        normal->print_z      = 0.5 * (normal->print_z + tree->print_z);
        normal->support_type = stInnerMixed;
        // H/2 support: the merged layer keeps both channels' source events.
        append(normal->half_layer_parent_bands, tree->half_layer_parent_bands);
        // The normal support owns overlaps: the plan gave the tree only what normal support cannot
        // reach, and letting a many-walled tree own them can erase the normal support entirely.
        const ExPolygons owner_coverage = union_ex(normal->support_fills.polygons_covered_by_width(float(SCALED_EPSILON)));
        std::unordered_map<coord_t, ExPolygons> cuts;
        for (const ExtrusionEntity *entity : tree->support_fills.entities)
            append_residual(*entity, owner_coverage, cuts, normal->support_fills);
        normal->support_fills.no_sort = normal->support_fills.no_sort || tree->support_fills.no_sort;
        append(normal->support_islands, std::move(tree->support_islands));
        if (!normal->support_islands.empty())
            normal->support_islands = union_ex(normal->support_islands);
        append(normal->lslices, std::move(tree->lslices));
        if (!normal->lslices.empty())
            normal->lslices = union_ex(normal->lslices);
        append(normal->base_areas, std::move(tree->base_areas));
        if (!normal->base_areas.empty())
            normal->base_areas = union_ex(normal->base_areas);
        PrintObject::delete_support_layer(tree);
        out.push_back(normal);
    }
    for (size_t i = 0; i < out.size(); ++i)
        out[i]->set_id(i);
}

} // namespace

bool Plan::has_normal() const { return any_polygons(normal_mask); }
bool Plan::has_tree() const { return any_polygons(tree_mask); }

ModSettings read_settings(const PrintObject &object)
{
    const DynamicPrintConfig &config = object.print()->full_print_config();
    auto value = [&config](const char *name) { return param_value(config, std::string("mixed_support.") + name); };
    ModSettings s;
    if (auto v = value("normal_generator"); v && std::holds_alternative<std::string>(*v))
        s.normal_generator = std::get<std::string>(*v);
    if (auto v = value("tree_style"); v && std::holds_alternative<std::string>(*v))
        s.tree_style = std::get<std::string>(*v);
    if (auto v = value("coverage_threshold"); v && std::holds_alternative<double>(*v))
        s.coverage_threshold = std::get<double>(*v);
    if (auto v = value("selective_merge"); v && std::holds_alternative<bool>(*v))
        s.selective_merge = std::get<bool>(*v);
    return s;
}

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
            const long double demand_area = std::max<long double>(0., island.area()); // scaled area
            ExPolygons        reachable;
            if (shadow.empty() || !get_extents(shadow).overlap(get_extents(island)))
                reachable = {island};
            else
                reachable = diff_ex(island, shadow);
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
                append(out.tree_mask[layer_id], to_polygons(diff_ex(island, reachable)));
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

void painted_channels(const PrintObject &object, std::vector<Polygons> &normal, std::vector<Polygons> &tree)
{
    normal.clear();
    tree.clear();
    const auto &volumes = object.model_object()->volumes;
    if (std::none_of(volumes.begin(), volumes.end(), [](const ModelVolume *v) { return v->is_model_part() && !v->support_channel_facets.empty(); }))
        return;
    normal.resize(object.layer_count());
    tree.resize(object.layer_count());
    std::vector<Polygons> enforced(object.layer_count());
    object.project_and_append_support_channel_facets(EnforcerBlockerType::ENFORCER, normal);
    object.project_and_append_support_channel_facets(EnforcerBlockerType::BLOCKER, tree);
    object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforced);
    for (size_t i = 0; i < object.layer_count(); ++i) {
        // A channel means something only where the surface is still painted as a support enforcer, so
        // stale channel painting (old or hand-edited files) does nothing.
        normal[i] = i < enforced.size() ? intersection(normal[i], enforced[i]) : Polygons{};
        tree[i]   = i < enforced.size() ? intersection(tree[i], enforced[i]) : Polygons{};
    }
}

std::vector<Polygons> detect_demand(const PrintObject &object, const ModSettings &s)
{
    if (s.normal_generator == "cura")
        return CuraSupport::overhang_demand(object, CuraSupport::read_settings(object));

    std::vector<Polygons>        demand(object.layer_count());
    SupportGeneratorLayerStorage storage;
    SupportMaterialOverrides     overrides;
    overrides.ignore_support_type = true;
    PrintObjectSupportMaterial detector(&object, object.slicing_parameters(), overrides);
    for (const SupportGeneratorLayer *contact : detector.detect_top_contact_layers(object, storage)) {
        if (contact == nullptr || contact->idx_object_layer_above >= demand.size())
            continue;
        if (contact->overhang_polygons && !contact->overhang_polygons->empty())
            append(demand[contact->idx_object_layer_above], *contact->overhang_polygons);
        else
            append(demand[contact->idx_object_layer_above], contact->polygons);
    }
    for (Polygons &layer : demand)
        if (!layer.empty())
            layer = union_(layer);
    return demand;
}

void generate(PrintObject &object)
{
    if (object.layer_count() == 0)
        return;
    if (object.print()->config().belt_printer.value)
        throw Slic3r::SlicingError(_u8L("Mixed support does not support belt printers yet."));

    const ModSettings s = read_settings(object);
    SupportLayerPtrs  normal_layers, tree_layers;
    auto              free_layers = [](SupportLayerPtrs &layers) {
        for (SupportLayer *layer : layers)
            PrintObject::delete_support_layer(layer);
        layers.clear();
    };
    try {
        object.print()->set_status(51, _u8L("Mixed support: analyzing overhangs"));
        const std::vector<Polygons> demand = detect_demand(object, s);
        cancel_check(object);
        object.print()->set_status(52, _u8L("Mixed support: classifying normal and tree regions"));
        std::vector<Polygons> painted_normal, painted_tree;
        painted_channels(object, painted_normal, painted_tree);
        const bool painted = !painted_normal.empty();
        const Plan p       = plan(demand, buildplate_covered_by_object(object), s.coverage_threshold, s.selective_merge,
                                  painted ? &painted_normal : nullptr, painted ? &painted_tree : nullptr);
        if (any_polygons(p.normal_paint_fallback))
            object.add_module_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
                                      _u8L("Some areas painted for normal support are not reachable from the build plate and were assigned to tree support."));
        cancel_check(object);

        auto run_normal = [&object, &s](const std::vector<Polygons> &mask) {
            object.clear_support_layers();
            if (s.normal_generator == "cura") {
                CuraSupport::generate(object, &mask);
            } else {
                SupportMaterialOverrides overrides;
                overrides.contact_mask        = &mask;
                overrides.ignore_support_type = true;
                PrintObjectSupportMaterial(&object, object.slicing_parameters(), overrides).generate(object);
            }
            SupportLayerPtrs layers = object.take_support_layers();
            for (SupportLayer *layer : layers)
                layer->support_type = stInnerNormal;
            return layers;
        };

        if (p.has_normal() || object.slicing_parameters().has_raft()) {
            object.print()->set_status(53, _u8L("Mixed support: generating normal support"));
            normal_layers = run_normal(p.normal_mask);
        }
        cancel_check(object);

        if (p.has_tree()) {
            object.print()->set_status(56, _u8L("Mixed support: generating tree support"));
            const SupportMaterialStyle  style     = tree_style(s.tree_style);
            const std::vector<Polygons> obstacles = tree_obstacles(object, normal_layers);
            TreeSupportOverrides        overrides;
            overrides.demand_mask = &p.tree_mask;
            overrides.style       = style;
            // Organic trees can lose every branch when sparse support lines become hard obstacles;
            // their overlap with normal support is settled in the merge instead.
            overrides.extra_obstacles = style == smsTreeOrganic ? nullptr : &obstacles;
            object.clear_support_layers();
            TreeSupport tree(object, object.slicing_parameters(), overrides);
            tree.throw_on_cancel = [&object]() { cancel_check(object); };
            tree.generate();
            tree_layers = object.take_support_layers();
            for (SupportLayer *layer : tree_layers)
                layer->support_type = stInnerTree;
            if (tree_layers.empty()) {
                // The tree generator found no printable branch for its demand. Give that demand to
                // the normal generator too, so it is supported wherever normal support can reach.
                object.clear_tree_support_preview_cache();
                std::vector<Polygons> fallback = p.normal_mask;
                fallback.resize(std::max(fallback.size(), p.tree_mask.size()));
                for (size_t i = 0; i < p.tree_mask.size(); ++i)
                    if (!p.tree_mask[i].empty())
                        fallback[i] = union_(fallback[i], p.tree_mask[i]);
                free_layers(normal_layers);
                normal_layers = run_normal(fallback);
                if (normal_layers.empty())
                    throw Slic3r::SlicingError(_u8L("Mixed support could not generate a printable route for the assigned support demand."));
                // PERMANENT DIAGNOSTIC: layers from the fallback may hold only the original normal
                // demand; they do not prove the tree's demand got printable support.
                object.add_module_warning(PrintStateBase::WarningLevel::CRITICAL,
                                               _u8L("Tree support could not route the assigned Mixed support demand. Normal support was "
                                                    "retried, but some areas may remain unsupported. Check the preview before printing."));
            }
        }
        cancel_check(object);

        object.print()->set_status(69, _u8L("Mixed support: merging support channels"));
        object.clear_support_layers();
        merge_layers(object, std::move(normal_layers), std::move(tree_layers));
        normal_layers.clear();
        tree_layers.clear();
    } catch (...) {
        free_layers(normal_layers);
        free_layers(tree_layers);
        object.clear_support_layers();
        object.clear_tree_support_preview_cache();
        throw;
    }
}

} // namespace MixedSupport
} // namespace ModHost
} // namespace Slic3r
