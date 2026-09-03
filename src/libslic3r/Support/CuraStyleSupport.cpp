#include "CuraStyleSupport.hpp"
#include "../Gpu/CudaSlicer.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportMaterial.hpp"
#include "SupportParameters.hpp"
#include "SupportTiming.hpp"

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <chrono>
#include <iterator>
#include <tbb/blocked_range.h>
#include <tbb/parallel_for.h>
#include <boost/log/trivial.hpp>

namespace Slic3r {

#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

static void throw_if_canceled(const PrintObject &object)
{
    if (object.print()->canceled())
        throw CanceledException();
}

template<class Function>
static void parallel_for_layers(size_t begin_idx, size_t end_idx, Function &&function,
                                const std::function<void()> &throw_on_cancel = {})
{
    if (begin_idx >= end_idx)
        return;

    tbb::parallel_for(
        tbb::blocked_range<size_t>(begin_idx, end_idx, 4),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx) {
                if (throw_on_cancel)
                    throw_on_cancel();
                function(layer_idx);
            }
        });
}

static bool bounding_boxes_may_overlap(const BoundingBox &first, const BoundingBox &second)
{
    return !first.defined || !second.defined || first.overlap(second);
}

static bool polygons_may_overlap(const Polygons &first, const Polygons &second)
{
    if (first.empty() || second.empty())
        return false;
    return bounding_boxes_may_overlap(get_extents(first), get_extents(second));
}

static bool polygons_may_overlap(const Polygons &polygons, const BoundingBox &bounds)
{
    return !polygons.empty() && bounding_boxes_may_overlap(get_extents(polygons), bounds);
}

static bool polygons_may_overlap_after_offset(const Polygons &first, const Polygons &second, coord_t second_offset)
{
    if (first.empty() || second.empty())
        return false;
    BoundingBox second_bounds = get_extents(second);
    if (second_bounds.defined && second_offset > 0)
        second_bounds.offset(second_offset);
    return bounding_boxes_may_overlap(get_extents(first), second_bounds);
}

CuraStyleSupportGenerator::CuraStyleSupportGenerator(
    const PrintObject *object, const SlicingParameters &slicing_params,
    const std::vector<Polygons> *demand_mask, bool force_buildplate_only,
    const CuraSupportDemand *precomputed_demand)
    : m_object(object)
    , m_slicing_params(slicing_params)
    , m_demand_mask(demand_mask)
    , m_force_buildplate_only(force_buildplate_only)
    , m_precomputed_demand(precomputed_demand)
{
}

struct CuraLayerGeometry
{
    CuraLayerGeometry(const PrintObject &object, coord_t xy_gap)
        : outlines(object.layer_count())
        , outline_bounds(object.layer_count())
        , outlines_with_xy_gap(object.layer_count())
        , xy_gap_bounds(object.layer_count())
        , xy_gap_ready(object.layer_count(), uint8_t(0))
        , print_z(object.layer_count())
        , bottom_z(object.layer_count())
        , xy_gap(xy_gap)
    {
        parallel_for_layers(0, object.layer_count(), [&](size_t layer_idx) {
            const Layer &layer = *object.layers()[layer_idx];
            outlines[layer_idx] = to_polygons(layer.lslices);
            if (!outlines[layer_idx].empty())
                outline_bounds[layer_idx] = get_extents(outlines[layer_idx]);
            print_z[layer_idx] = layer.print_z;
            bottom_z[layer_idx] = layer.bottom_z();
        }, [&object]() { throw_if_canceled(object); });
    }

    const Polygons &with_xy_gap(size_t layer_idx) const
    {
        if (!xy_gap_ready[layer_idx]) {
            if (!outlines[layer_idx].empty())
                outlines_with_xy_gap[layer_idx] = offset(
                    outlines[layer_idx], float(xy_gap), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            if (!outlines_with_xy_gap[layer_idx].empty())
                xy_gap_bounds[layer_idx] = get_extents(outlines_with_xy_gap[layer_idx]);
            xy_gap_ready[layer_idx] = uint8_t(1);
        }
        return outlines_with_xy_gap[layer_idx];
    }

    bool may_overlap_with_xy_gap(size_t layer_idx, const BoundingBox &other_bounds) const
    {
        if (!other_bounds.defined || layer_idx >= outline_bounds.size() || !outline_bounds[layer_idx].defined)
            return false;

        BoundingBox conservative_bounds = outline_bounds[layer_idx];
        if (xy_gap > 0)
            conservative_bounds.offset(xy_gap);
        return conservative_bounds.overlap(other_bounds);
    }

    std::vector<Polygons> outlines;
    std::vector<BoundingBox> outline_bounds;
    mutable std::vector<Polygons> outlines_with_xy_gap;
    mutable std::vector<BoundingBox> xy_gap_bounds;
    mutable std::vector<uint8_t> xy_gap_ready;
    std::vector<coordf_t> print_z;
    std::vector<coordf_t> bottom_z;
    coord_t xy_gap;
};

struct CuraSupportAnnotations
{
    explicit CuraSupportAnnotations(const PrintObject &object)
        : enforcers(object.slice_support_enforcers())
        , blockers(object.slice_support_blockers())
    {
        enforcers.resize(object.layer_count());
        blockers.resize(object.layer_count());
        object.project_and_append_custom_facets(false, EnforcerBlockerType::ENFORCER, enforcers);
        object.project_and_append_custom_facets(false, EnforcerBlockerType::BLOCKER, blockers);

        parallel_for_layers(0, enforcers.size(), [&](size_t layer_idx) {
            Polygons &enforcer = enforcers[layer_idx];
            if (!enforcer.empty())
                enforcer = union_(enforcer);

            Polygons &blocker = blockers[layer_idx];
            if (!blocker.empty())
                blocker = offset(union_(blocker), float(SCALED_EPSILON), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        }, [&object]() { throw_if_canceled(object); });
    }

    std::vector<Polygons> enforcers;
    std::vector<Polygons> blockers;
};

std::vector<Polygons> CuraSupportDemand::combined(const std::function<void()> &throw_on_cancel) const
{
    std::vector<Polygons> result(automatic.size());
    parallel_for_layers(0, result.size(), [&](size_t layer_idx) {
        if (automatic[layer_idx].empty())
            result[layer_idx] = enforced[layer_idx];
        else if (enforced[layer_idx].empty())
            result[layer_idx] = automatic[layer_idx];
        else
            result[layer_idx] = union_(automatic[layer_idx], enforced[layer_idx]);
    }, throw_on_cancel);
    return result;
}

static CuraSupportDemand compute_cura_style_full_overhangs(
    const PrintObject &object,
    bool               automatic_support,
    bool               apply_buildplate_only)
{
    CuraSupportDemand overhangs{
        std::vector<Polygons>(object.layer_count()),
        std::vector<Polygons>(object.layer_count())
    };

    // The normal and Cura body generators must agree on which object surfaces
    // need support. Reuse Orca's native detector all the way through contact
    // classification instead of maintaining a second angle approximation here.
    // This preserves Orca's per-region external-perimeter width, threshold
    // overlap fallback, blockers/enforcers, bridge suppression, sharp-tail
    // handling and inclusive threshold-angle convention.
    SupportGeneratorLayerStorage layer_storage;
    PrintObjectSupportMaterial detector(&object, object.slicing_parameters());
    const SupportGeneratorLayersPtr contacts =
        detector.detect_top_contact_layers(layer_storage, apply_buildplate_only);
    for (const SupportGeneratorLayer *contact : contacts) {
        throw_if_canceled(object);
        if (contact == nullptr || contact->idx_object_layer_above >= overhangs.automatic.size())
            continue;

        const size_t layer_idx = contact->idx_object_layer_above;
        const Polygons &all_contact =
            contact->overhang_polygons && !contact->overhang_polygons->empty() ?
                *contact->overhang_polygons : contact->polygons;
        if (all_contact.empty())
            continue;

        if (!automatic_support) {
            append(overhangs.enforced[layer_idx], all_contact);
            continue;
        }

        if (contact->enforcer_polygons && !contact->enforcer_polygons->empty()) {
            append(overhangs.enforced[layer_idx], *contact->enforcer_polygons);
            append(overhangs.automatic[layer_idx],
                   diff(all_contact, *contact->enforcer_polygons));
        } else {
            append(overhangs.automatic[layer_idx], all_contact);
        }
    }
    parallel_for_layers(0, overhangs.automatic.size(), [&](size_t layer_idx) {
        if (!overhangs.automatic[layer_idx].empty())
            overhangs.automatic[layer_idx] = union_(overhangs.automatic[layer_idx]);
        if (!overhangs.enforced[layer_idx].empty())
            overhangs.enforced[layer_idx] = union_(overhangs.enforced[layer_idx]);
    }, [&object]() { throw_if_canceled(object); });

    return overhangs;
}

static void remove_unprintable_support_parts(std::vector<Polygons> &support_by_layer, const SupportParameters &support_params)
{
    const coord_t half_min_feature = std::max<coord_t>(support_params.support_material_flow.scaled_width() / 2, scale_(0.05));

    for (Polygons &support : support_by_layer) {
        if (support.empty())
            continue;

        support = offset(support, -float(half_min_feature), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        if (!support.empty())
            support = offset(support, float(half_min_feature), SUPPORT_SURFACES_OFFSET_PARAMETERS);
    }
}

static void keep_buildplate_connected_support(std::vector<Polygons> &support_by_layer, const PrintObject &object)
{
    if (support_by_layer.empty())
        return;

    Polygons touching = support_by_layer.front();
    for (size_t layer_idx = 1; layer_idx < support_by_layer.size(); ++layer_idx) {
        throw_if_canceled(object);
        if (support_by_layer[layer_idx].empty() || touching.empty()) {
            support_by_layer[layer_idx].clear();
            touching.clear();
            continue;
        }

        support_by_layer[layer_idx] = intersection(support_by_layer[layer_idx], touching);
        touching = support_by_layer[layer_idx];
    }
}

static Polygons close_unprintable_parts(const Polygons &polygons, coord_t half_min_feature)
{
    if (polygons.empty())
        return {};

    Polygons closed = offset(polygons, -float(half_min_feature), SUPPORT_SURFACES_OFFSET_PARAMETERS);
    return closed.empty() ? Polygons{} : offset(closed, float(half_min_feature), SUPPORT_SURFACES_OFFSET_PARAMETERS);
}

static Polygons close_interface_footprint(const Polygons &polygons, coord_t close_distance)
{
    if (polygons.empty())
        return {};

    Polygons closed = offset(polygons, float(close_distance), SUPPORT_SURFACES_OFFSET_PARAMETERS);
    return closed.empty() ? Polygons{} : offset(closed, -float(close_distance), SUPPORT_SURFACES_OFFSET_PARAMETERS);
}

Polygons join_cura_style_support_regions(
    const Polygons &current,
    const Polygons &inherited,
    coord_t         join_distance,
    coord_t         half_min_feature)
{
    if (join_distance <= 0) {
        if (current.empty())
            return inherited;
        if (inherited.empty())
            return current;
    }

    Polygons joined = current;
    append(joined, inherited);
    joined = union_(joined);
    if (joined.empty() || join_distance <= 0)
        return joined;

    // Match CuraEngine's AreaSupport::join morphology: shrink away features
    // that cannot carry a line, grow the remaining cores far enough to bridge
    // nearby regions, then shrink only the added bridge. Unioning the result
    // back into `joined` is deliberate: small original support regions are not
    // removed by this operation.
    Polygons printable_cores = offset(
        joined, -float(half_min_feature), SUPPORT_SURFACES_OFFSET_PARAMETERS);
    if (printable_cores.empty())
        return joined;

    constexpr double round_arc_tolerance_mm = 0.05;
    Polygons connected = offset(
        printable_cores, float(join_distance + half_min_feature),
        ClipperLib::jtRound, scale_(round_arc_tolerance_mm));
    if (connected.empty())
        return joined;
    connected = offset(
        connected, -float(join_distance),
        ClipperLib::jtRound, scale_(round_arc_tolerance_mm));
    append(joined, connected);
    return union_(joined);
}

// Translated from CuraEngine's stepwise support horizontal expansion. Growing
// both the support and the remaining model space keeps expansion from folding
// around model walls.
static Polygons expand_support_away_from_model(
    Polygons support,
    Polygons model_outline,
    coord_t expansion,
    coord_t support_line_width)
{
    if (support.empty() || expansion <= 0)
        return support;

    const coord_t step = std::max<coord_t>(1, support_line_width / 2);
    Polygons horizontal_expansion = support;
    coord_t expanded = 0;
    while (expanded < expansion) {
        const coord_t amount = std::min(step, expansion - expanded);
        horizontal_expansion = offset(horizontal_expansion, float(amount), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        if (!model_outline.empty()) {
            if (polygons_may_overlap(model_outline, horizontal_expansion))
                model_outline = diff(model_outline, horizontal_expansion);
            model_outline = offset(model_outline, float(amount), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            if (polygons_may_overlap(horizontal_expansion, model_outline))
                horizontal_expansion = diff(horizontal_expansion, model_outline);
        }
        expanded += amount;
    }
    return union_(support, horizontal_expansion);
}

static size_t closest_sorted_layer(
    const std::vector<coordf_t> &z_values,
    size_t begin_idx,
    size_t end_idx,
    coordf_t target_z)
{
    if (begin_idx >= end_idx || end_idx > z_values.size())
        return size_t(-1);

    const auto begin = z_values.begin() + begin_idx;
    const auto end = z_values.begin() + end_idx;
    const auto upper = std::lower_bound(begin, end, target_z);
    if (upper == begin)
        return begin_idx;
    if (upper == end)
        return end_idx - 1;

    const auto lower = std::prev(upper);
    return target_z - *lower <= *upper - target_z ?
        size_t(std::distance(z_values.begin(), lower)) :
        size_t(std::distance(z_values.begin(), upper));
}

static size_t closest_top_contact_layer(const CuraLayerGeometry &geometry, size_t overhang_layer_idx, coordf_t gap)
{
    if (overhang_layer_idx == 0 || overhang_layer_idx >= geometry.outlines.size())
        return size_t(-1);

    return closest_sorted_layer(
        geometry.print_z, 0, overhang_layer_idx,
        geometry.bottom_z[overhang_layer_idx] - gap);
}

static std::vector<size_t> make_top_contact_map(
    const PrintObject &object,
    const CuraLayerGeometry &geometry,
    const std::vector<Polygons> &overhangs,
    coordf_t gap)
{
    std::vector<size_t> contact_for_overhang(overhangs.size(), size_t(-1));
    for (size_t overhang_layer_idx = 1; overhang_layer_idx < overhangs.size(); ++overhang_layer_idx) {
        throw_if_canceled(object);
        if (!overhangs[overhang_layer_idx].empty())
            contact_for_overhang[overhang_layer_idx] = closest_top_contact_layer(geometry, overhang_layer_idx, gap);
    }
    return contact_for_overhang;
}

static std::vector<Polygons> seed_overhangs_at_contacts(
    const PrintObject             &object,
    const std::vector<Polygons> &overhangs,
    const std::vector<size_t>   &contact_for_overhang,
    coord_t                      half_min_feature,
    bool                         prune_unprintable_fragments)
{
    std::vector<Polygons> seeds(overhangs.size());
    for (size_t overhang_layer_idx = 1; overhang_layer_idx < overhangs.size(); ++overhang_layer_idx) {
        throw_if_canceled(object);
        const size_t contact_idx = contact_for_overhang[overhang_layer_idx];
        if (contact_idx == size_t(-1) || overhangs[overhang_layer_idx].empty())
            continue;
        append(seeds[contact_idx], overhangs[overhang_layer_idx]);
    }
    parallel_for_layers(0, seeds.size(), [&](size_t layer_idx) {
        if (!seeds[layer_idx].empty()) {
            seeds[layer_idx] = union_(seeds[layer_idx]);
            // Mixed selective masks can cut a printable overhang into thousands
            // of sub-extrusion-width fragments.  They cannot produce a support
            // line, so remove them before the serial downward propagation rather
            // than copying and unioning the same debris through every lower layer.
            if (prune_unprintable_fragments)
                seeds[layer_idx] = close_unprintable_parts(seeds[layer_idx], half_min_feature);
        }
    }, [&object]() { throw_if_canceled(object); });
    return seeds;
}

static std::vector<Polygons> propagate_cura_style_support_channel(
    const PrintObject              &object,
    const SupportParameters        &support_params,
    const CuraSupportAnnotations   &annotations,
    const CuraLayerGeometry        &geometry,
    const std::vector<Polygons>    &overhangs,
    coordf_t                        top_gap,
    bool                            keep_buildplate_only,
    bool                            prune_unprintable_fragments)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    std::vector<Polygons> support_by_layer(layer_count, Polygons{});

    if (layer_count < 2)
        return support_by_layer;

    const coord_t half_min_feature = std::max<coord_t>(support_params.support_material_flow.scaled_width() / 2, scale_(0.05));
    const coord_t support_join_distance = scale_(config.cura_support_join_distance.value);
    const std::vector<size_t> contact_map = make_top_contact_map(object, geometry, overhangs, top_gap);
    std::vector<Polygons> seeds = seed_overhangs_at_contacts(
        object, overhangs, contact_map, half_min_feature, prune_unprintable_fragments);
    const coord_t support_expansion = scale_(config.support_expansion.value);
    const auto highest_seed = std::find_if(
        seeds.rbegin(), seeds.rend(), [](const Polygons &polygons) { return !polygons.empty(); });
    if (highest_seed == seeds.rend())
        return support_by_layer;
    const size_t highest_active_layer = size_t(std::distance(seeds.begin(), highest_seed.base())) - 1;
    const Polygons no_inherited_support;

    for (int layer_idx = int(highest_active_layer); layer_idx >= 0; --layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-propagation", "Propagate Cura support layer", 1);
        Polygons layer_this = std::move(seeds[size_t(layer_idx)]);

        const Polygons &model_on_layer = geometry.outlines[size_t(layer_idx)];
        if (!layer_this.empty() && support_expansion > 0)
            layer_this = expand_support_away_from_model(
                std::move(layer_this), model_on_layer, support_expansion,
                support_params.support_material_flow.scaled_width());
        else if (!layer_this.empty() && support_expansion < 0)
            layer_this = offset(layer_this, float(support_expansion), SUPPORT_SURFACES_OFFSET_PARAMETERS);

        const Polygons &inherited = size_t(layer_idx + 1) < layer_count ?
            support_by_layer[layer_idx + 1] : no_inherited_support;
        // The inherited channel was already joined when it was created.  Do
        // not run the same full-island close again on every seedless layer;
        // only a newly introduced seed can create a new join candidate.
        if (layer_this.empty())
            layer_this = inherited;
        else
            layer_this = join_cura_style_support_regions(
                layer_this, inherited, support_join_distance, half_min_feature);

        if (!layer_this.empty()) {
            if (!annotations.blockers[size_t(layer_idx)].empty() &&
                polygons_may_overlap(layer_this, annotations.blockers[size_t(layer_idx)]))
                layer_this = diff(layer_this, annotations.blockers[size_t(layer_idx)]);
            if (!model_on_layer.empty() &&
                polygons_may_overlap(layer_this, geometry.outline_bounds[size_t(layer_idx)]))
                layer_this = diff(layer_this, model_on_layer);
            // Keep the state handed to the next serial iteration printable and
            // bounded.  Previously this cleanup happened only after all layers
            // had inherited the fragmented geometry, causing minute-long single-
            // core runs and multi-gigabyte growth on Mixed + Selective Merge.
            if (prune_unprintable_fragments)
                layer_this = close_unprintable_parts(layer_this, half_min_feature);

            // CuraEngine simplifies the result of AreaSupport::join on every
            // layer.  Without this step, offset/difference vertices accumulate
            // through the entire downward propagation and make later joins and
            // adjacency checks super-linear.  Stay within Orca's configured
            // geometry resolution so emitted geometry retains its normal
            // accuracy contract.
            if (!layer_this.empty())
                layer_this = polygons_simplify(
                    layer_this, scale_(object.print()->config().resolution.value), true);
        }

        support_by_layer[size_t(layer_idx)] = std::move(layer_this);
        layer_timing.finish(support_by_layer[size_t(layer_idx)].size(),
            "object_layer=" + std::to_string(layer_idx));
    }
    // Immutable, corresponding layer pairs only. Never compare unrelated layers.
    std::vector<uint8_t> cuda_xy_overlap;
    if (Gpu::CudaSlicerBackend::operation_enabled("cura_support")) {
        cuda_xy_overlap.assign(highest_active_layer + 1, 2);
        std::vector<Gpu::VulkanAabb> queries, targets;
        std::vector<size_t> layer_ids;
        for (size_t i = 0; i <= highest_active_layer; ++i) {
            throw_if_canceled(object);
            if (support_by_layer[i].empty() || !geometry.outline_bounds[i].defined) continue;
            const BoundingBox q = get_extents(support_by_layer[i]);
            BoundingBox t = geometry.outline_bounds[i];
            if (!q.defined) continue;
            const coord_t margin = std::max<coord_t>(0, geometry.xy_gap);
            if (t.min.x() < std::numeric_limits<coord_t>::min()+margin || t.min.y() < std::numeric_limits<coord_t>::min()+margin ||
                t.max.x() > std::numeric_limits<coord_t>::max()-margin || t.max.y() > std::numeric_limits<coord_t>::max()-margin) continue;
            t.offset(margin);
            queries.push_back({{q.min.x(),q.min.y()},{q.max.x(),q.max.y()}});
            targets.push_back({{t.min.x(),t.min.y()},{t.max.x(),t.max.y()}});
            layer_ids.push_back(i);
        }
        const auto batch = Gpu::CudaSlicerBackend::dispatch_aabb_pairs(queries,targets);
        if (batch.resolved) for (size_t i = 0; i < layer_ids.size(); ++i) cuda_xy_overlap[layer_ids[i]] = batch.may_overlap[i];
    }
    parallel_for_layers(0, highest_active_layer + 1, [&](size_t layer_idx) {
        Polygons &support = support_by_layer[layer_idx];
        SupportProfileStage layer_timing("support-cura-trim", "Trim Cura support XY gap", support.size());
        if (support.empty())
            return;

        const BoundingBox support_bounds = get_extents(support);
        // Cura only compares a support layer with the model on the same layer.
        // A conservative per-layer bounding box is cheaper than constructing an
        // all-layer spatial batch and lets the exact XY-gap polygon stay lazy.
        const bool overlaps_model = cuda_xy_overlap.empty() || cuda_xy_overlap[layer_idx] == 2 ?
            geometry.may_overlap_with_xy_gap(layer_idx, support_bounds) : cuda_xy_overlap[layer_idx] != 0;
        if (overlaps_model) {
            const Polygons &model_with_xy_gap = geometry.with_xy_gap(layer_idx);
            if (!model_with_xy_gap.empty() &&
                bounding_boxes_may_overlap(support_bounds, geometry.xy_gap_bounds[layer_idx]))
                support = diff(support, model_with_xy_gap);
        }

        support = close_unprintable_parts(support, half_min_feature);
        layer_timing.finish(support.size(), "object_layer=" + std::to_string(layer_idx));
    }, [&object]() { throw_if_canceled(object); });
    if (keep_buildplate_only)
        keep_buildplate_connected_support(support_by_layer, object);

    for (size_t layer_idx = 1; layer_idx + 1 < layer_count; ++layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-propagation", "Validate Cura layer adjacency", support_by_layer[layer_idx].size());
        if (support_by_layer[layer_idx].empty())
            continue;

        Polygons adjacent = support_by_layer[layer_idx - 1];
        if (!support_by_layer[layer_idx + 1].empty())
            adjacent = adjacent.empty() ? support_by_layer[layer_idx + 1] : union_(adjacent, support_by_layer[layer_idx + 1]);
        if (!adjacent.empty()) {
            const coord_t adjacency_distance = support_params.support_material_flow.scaled_width();
            if (polygons_may_overlap_after_offset(support_by_layer[layer_idx], adjacent, adjacency_distance))
                support_by_layer[layer_idx] = intersection(
                    support_by_layer[layer_idx],
                    offset(adjacent, float(adjacency_distance), SUPPORT_SURFACES_OFFSET_PARAMETERS));
            else
                support_by_layer[layer_idx].clear();
        }
        layer_timing.finish(support_by_layer[layer_idx].size(),
            "object_layer=" + std::to_string(layer_idx));
    }

    return support_by_layer;
}

static std::vector<Polygons> propagate_cura_style_support(
    const PrintObject             &object,
    const SlicingParameters       &slicing_params,
    const SupportParameters       &support_params,
    const CuraSupportAnnotations  &annotations,
    const CuraLayerGeometry       &geometry,
    const CuraSupportDemand       &overhangs,
    bool                           force_buildplate_only,
    bool                           prune_unprintable_fragments)
{
    const auto has_support = [](const std::vector<Polygons> &channel) {
        return std::any_of(channel.begin(), channel.end(), [](const Polygons &polygons) { return !polygons.empty(); });
    };

    std::vector<Polygons> automatic(object.layer_count());
    if (has_support(overhangs.automatic))
        automatic = propagate_cura_style_support_channel(
            object, support_params, annotations, geometry, overhangs.automatic, slicing_params.gap_support_object,
            force_buildplate_only || object.config().support_on_build_plate_only.value,
            prune_unprintable_fragments);

    std::vector<Polygons> enforced(object.layer_count());
    if (has_support(overhangs.enforced))
        enforced = propagate_cura_style_support_channel(
            object, support_params, annotations, geometry, overhangs.enforced, slicing_params.gap_support_object,
            force_buildplate_only, prune_unprintable_fragments);

    for (size_t layer_idx = 0; layer_idx < automatic.size(); ++layer_idx) {
        throw_if_canceled(object);
        if (automatic[layer_idx].empty())
            automatic[layer_idx] = std::move(enforced[layer_idx]);
        else if (!enforced[layer_idx].empty())
            automatic[layer_idx] = union_(automatic[layer_idx], enforced[layer_idx]);
    }
    return automatic;
}

struct ContactFootprints
{
    std::vector<Polygons> top_contacts;
    std::vector<Polygons> bottom_contacts;
    std::vector<size_t> top_object_layers;
    std::vector<size_t> bottom_object_layers;
};

static size_t closest_bottom_contact_layer(const CuraLayerGeometry &geometry, size_t object_layer_idx, coordf_t gap)
{
    if (object_layer_idx + 1 >= geometry.outlines.size())
        return size_t(-1);

    return closest_sorted_layer(
        geometry.bottom_z, object_layer_idx + 1, geometry.outlines.size(),
        geometry.print_z[object_layer_idx] + gap);
}

static ContactFootprints make_cura_style_contact_footprints(
    const PrintObject           &object,
    const SlicingParameters     &slicing_params,
    const SupportParameters     &support_params,
    const CuraLayerGeometry     &geometry,
    const std::vector<Polygons> &full_overhangs,
    const std::vector<Polygons> &support_body)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    ContactFootprints out{
        std::vector<Polygons>(layer_count, Polygons{}),
        std::vector<Polygons>(layer_count, Polygons{}),
        std::vector<size_t>(layer_count, size_t(-1)),
        std::vector<size_t>(layer_count, size_t(-1))
    };

    if (layer_count < 2)
        return out;

    const std::vector<size_t> top_contact_map = make_top_contact_map(object, geometry, full_overhangs, slicing_params.gap_support_object);
    const coord_t support_expansion = scale_(config.support_expansion.value);
    double interface_margin_scaled = std::max<double>(
        support_params.support_material_interface_flow.scaled_spacing(),
        support_params.support_material_interface_flow.scaled_width());
    interface_margin_scaled = std::max<double>(interface_margin_scaled, support_params.support_material_flow.scaled_width());
    interface_margin_scaled = std::max<double>(interface_margin_scaled, support_expansion);
    interface_margin_scaled = std::max<double>(interface_margin_scaled, scale_(0.2));
    const coord_t interface_margin = coord_t(std::ceil(interface_margin_scaled));
    const coord_t close_distance = std::max<coord_t>(support_params.support_material_interface_flow.scaled_width() / 2, scale_(0.05));

    for (size_t overhang_layer_idx = 1; support_params.num_top_interface_layers > 0 && overhang_layer_idx < full_overhangs.size(); ++overhang_layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-contact", "Build Cura top interface footprint", full_overhangs[overhang_layer_idx].size());
        Polygons overhang_seed = full_overhangs[overhang_layer_idx];
        if (overhang_seed.empty())
            continue;

        const size_t contact_layer_idx = top_contact_map[overhang_layer_idx];
        if (contact_layer_idx == size_t(-1))
            continue;
        Polygons footprint = contact_layer_idx < support_body.size() ? support_body[contact_layer_idx] : Polygons{};
        if (footprint.empty())
            footprint = overhang_seed;

        // Use the actual support top as the interface footprint, but limit it to
        // the support island that belongs to this overhang. The raw overhang
        // polygon alone often leaves only a narrow boundary strip.
        if (!polygons_may_overlap_after_offset(footprint, overhang_seed, interface_margin))
            continue;
        Polygons interface_envelope = offset(overhang_seed, float(interface_margin), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        if (!interface_envelope.empty())
            footprint = intersection(footprint, interface_envelope);
        if (footprint.empty())
            continue;

        footprint = close_interface_footprint(footprint, close_distance);
        if (footprint.empty())
            continue;

        const Polygons &model_with_xy_gap = geometry.with_xy_gap(contact_layer_idx);
        if (!model_with_xy_gap.empty() &&
            polygons_may_overlap(footprint, geometry.xy_gap_bounds[contact_layer_idx]))
            footprint = diff(footprint, model_with_xy_gap);

        footprint = close_interface_footprint(footprint, close_distance);
        if (footprint.empty())
            continue;

        append(out.top_contacts[contact_layer_idx], footprint);
        if (out.top_object_layers[contact_layer_idx] == size_t(-1))
            out.top_object_layers[contact_layer_idx] = overhang_layer_idx;
        layer_timing.finish(out.top_contacts[contact_layer_idx].size(),
            "overhang_layer=" + std::to_string(overhang_layer_idx) + ";support_layer=" + std::to_string(contact_layer_idx));
    }

    for (size_t object_layer_idx = 0; support_params.num_bottom_interface_layers > 0 && object_layer_idx + 1 < layer_count; ++object_layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-contact", "Build Cura bottom interface footprint", 1);
        Polygons object_top = geometry.outlines[object_layer_idx];
        const Polygons &object_above = geometry.outlines[object_layer_idx + 1];
        const coord_t object_top_clearance = scale_(0.05);
        if (!object_above.empty() &&
            polygons_may_overlap_after_offset(object_top, object_above, object_top_clearance))
            object_top = diff(object_top, offset(object_above, float(object_top_clearance), SUPPORT_SURFACES_OFFSET_PARAMETERS));
        if (object_top.empty())
            continue;

        const size_t contact_layer_idx = closest_bottom_contact_layer(geometry, object_layer_idx, slicing_params.gap_object_support);
        if (contact_layer_idx == size_t(-1) || support_body[contact_layer_idx].empty())
            continue;

        if (!polygons_may_overlap_after_offset(support_body[contact_layer_idx], object_top, interface_margin))
            continue;
        Polygons footprint = intersection(
            support_body[contact_layer_idx],
            offset(object_top, float(interface_margin), SUPPORT_SURFACES_OFFSET_PARAMETERS));
        footprint = close_interface_footprint(footprint, close_distance);
        if (footprint.empty())
            continue;

        append(out.bottom_contacts[contact_layer_idx], footprint);
        if (out.bottom_object_layers[contact_layer_idx] == size_t(-1))
            out.bottom_object_layers[contact_layer_idx] = object_layer_idx;
        layer_timing.finish(out.bottom_contacts[contact_layer_idx].size(),
            "object_layer=" + std::to_string(object_layer_idx) + ";support_layer=" + std::to_string(contact_layer_idx));
    }

    for (Polygons &polys : out.top_contacts) {
        throw_if_canceled(object);
        if (!polys.empty())
            polys = union_(polys);
    }
    for (Polygons &polys : out.bottom_contacts) {
        throw_if_canceled(object);
        if (!polys.empty())
            polys = union_(polys);
    }

    return out;
}

static SupportGeneratorLayersPtr make_contact_layers_from_footprints(
    const PrintObject             &object,
    const std::vector<Polygons>   &footprints,
    const std::vector<size_t>     &object_layer_indices,
    SupporLayerType                layer_type,
    SupportGeneratorLayerStorage  &layer_storage)
{
    SupportGeneratorLayersPtr layers;

    for (size_t layer_idx = 0; layer_idx < footprints.size(); ++layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-layer", "Create Cura contact layer", footprints[layer_idx].size());
        if (footprints[layer_idx].empty())
            continue;

        const Layer &object_layer = *object.layers()[layer_idx];
        SupportGeneratorLayer &support_layer = layer_storage.allocate(layer_type);
        support_layer.print_z  = object_layer.print_z;
        support_layer.bottom_z = object_layer.bottom_z();
        support_layer.height   = object_layer.height;
        support_layer.polygons = footprints[layer_idx];
        if (layer_type == SupporLayerType::TopContact) {
            support_layer.idx_object_layer_above = object_layer_indices[layer_idx];
            support_layer.contact_polygons = std::make_unique<Polygons>(footprints[layer_idx]);
            support_layer.overhang_polygons = std::make_unique<Polygons>(footprints[layer_idx]);
        } else {
            support_layer.idx_object_layer_below = object_layer_indices[layer_idx];
        }
        layers.push_back(&support_layer);
        layer_timing.finish(support_layer.polygons.size(), "object_layer=" + std::to_string(layer_idx));
    }

    return layers;
}

static SupportGeneratorLayersPtr make_base_layers_from_support_body(
    const PrintObject           &object,
    std::vector<Polygons>        support_by_layer,
    const std::vector<Polygons> &non_base_support_by_layer,
    SupportGeneratorLayerStorage &layer_storage)
{
    SupportGeneratorLayersPtr base_layers;

    for (size_t layer_idx = 0; layer_idx < support_by_layer.size(); ++layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-layer", "Create Cura base layer", support_by_layer[layer_idx].size());
        Polygons base_polygons = std::move(support_by_layer[layer_idx]);
        if (base_polygons.empty())
            continue;

        if (layer_idx < non_base_support_by_layer.size() && !non_base_support_by_layer[layer_idx].empty())
            base_polygons = diff(base_polygons, non_base_support_by_layer[layer_idx]);
        if (base_polygons.empty())
            continue;

        const Layer &object_layer = *object.layers()[layer_idx];
        SupportGeneratorLayer &support_layer = layer_storage.allocate(SupporLayerType::Base);
        support_layer.print_z  = object_layer.print_z;
        support_layer.bottom_z = layer_idx > 0 ? object.layers()[layer_idx - 1]->print_z : 0.;
        support_layer.height   = support_layer.print_z - support_layer.bottom_z;
        support_layer.polygons = std::move(base_polygons);
        base_layers.push_back(&support_layer);
        layer_timing.finish(support_layer.polygons.size(), "object_layer=" + std::to_string(layer_idx));
    }

    return base_layers;
}

void CuraStyleSupportGenerator::generate(PrintObject &object)
{
    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - Start";

    if (m_object == nullptr || object.layer_count() == 0)
        return;
    throw_if_canceled(object);

    using Clock = std::chrono::steady_clock;
    const auto started_at = Clock::now();
    SupportProfileStage parameters_timing("support-cura", "Prepare parameters", object.layer_count());
    SupportParameters support_params(object);
    if (is_mixed(object.config().support_type.value))
        support_params.cura_style_support = true;
    SupportGeneratorLayerStorage layer_storage;
    parameters_timing.finish(object.layer_count());
    const auto parameters_ready_at = Clock::now();
    SupportProfileStage geometry_timing("support-cura", "Prepare layer geometry", object.layer_count());
    const CuraLayerGeometry geometry(object, scale_(support_params.gap_xy));
    geometry_timing.finish(object.layer_count());
    const auto geometry_ready_at = Clock::now();

    SupportProfileStage annotations_timing("support-cura", "Prepare annotations", object.layer_count());
    const CuraSupportAnnotations annotations(object);
    annotations_timing.finish(object.layer_count());
    const auto annotations_ready_at = Clock::now();
    const bool automatic_support = object.config().support_type.value == stNormalCuraAuto ||
                                   is_mixed(object.config().support_type.value);
    SupportProfileStage demand_timing("support-cura", "Prepare overhang demand", object.layer_count());
    CuraSupportDemand overhangs =
        m_precomputed_demand != nullptr &&
        m_precomputed_demand->automatic.size() == object.layer_count() &&
        m_precomputed_demand->enforced.size() == object.layer_count() ?
            *m_precomputed_demand :
            compute_cura_style_full_overhangs(
                object, automatic_support,
                !is_mixed(object.config().support_type.value) &&
                    (m_force_buildplate_only || object.config().support_on_build_plate_only.value));
    if (m_demand_mask != nullptr) {
        parallel_for_layers(0, object.layer_count(), [&](size_t layer_idx) {
            if (layer_idx >= m_demand_mask->size() || (*m_demand_mask)[layer_idx].empty()) {
                overhangs.automatic[layer_idx].clear();
                overhangs.enforced[layer_idx].clear();
            } else {
                overhangs.automatic[layer_idx] = intersection(overhangs.automatic[layer_idx], (*m_demand_mask)[layer_idx]);
                overhangs.enforced[layer_idx] = intersection(overhangs.enforced[layer_idx], (*m_demand_mask)[layer_idx]);
            }
        }, [&object]() { throw_if_canceled(object); });
    }
    const std::vector<Polygons> full_overhangs = overhangs.combined(
        [&object]() { throw_if_canceled(object); });
    demand_timing.finish(full_overhangs.size());
    const auto overhangs_ready_at = Clock::now();
    object.print()->set_status(54, _u8L("Cura support: propagating support regions"));
    SupportProfileStage propagation_timing("support-cura", "Propagate support regions", object.layer_count());
    std::vector<Polygons> support_body = propagate_cura_style_support(
        object, m_slicing_params, support_params, annotations, geometry, overhangs,
        m_force_buildplate_only, m_demand_mask != nullptr);
    propagation_timing.finish(support_body.size());
    const auto propagation_ready_at = Clock::now();
    SupportProfileStage contacts_timing("support-cura", "Build contact footprints", support_body.size());
    ContactFootprints contact_footprints = make_cura_style_contact_footprints(
        object, m_slicing_params, support_params, geometry, full_overhangs, support_body);
    contacts_timing.finish(contact_footprints.top_contacts.size() + contact_footprints.bottom_contacts.size());
    const auto contacts_ready_at = Clock::now();
    SupportProfileStage layers_timing("support-cura", "Assemble support layers", support_body.size());
    std::vector<Polygons> non_base_support_by_layer(support_body.size(), Polygons{});

    for (size_t layer_idx = 0; layer_idx < support_body.size(); ++layer_idx) {
        throw_if_canceled(object);
        SupportProfileStage layer_timing("support-cura-layer", "Merge Cura contact and base regions", support_body[layer_idx].size());
        if (!contact_footprints.top_contacts[layer_idx].empty()) {
            support_body[layer_idx] = support_body[layer_idx].empty() ?
                contact_footprints.top_contacts[layer_idx] :
                union_(support_body[layer_idx], contact_footprints.top_contacts[layer_idx]);
            non_base_support_by_layer[layer_idx] = contact_footprints.top_contacts[layer_idx];
        }
        if (!contact_footprints.bottom_contacts[layer_idx].empty()) {
            support_body[layer_idx] = support_body[layer_idx].empty() ?
                contact_footprints.bottom_contacts[layer_idx] :
                union_(support_body[layer_idx], contact_footprints.bottom_contacts[layer_idx]);
            non_base_support_by_layer[layer_idx] = non_base_support_by_layer[layer_idx].empty() ?
                contact_footprints.bottom_contacts[layer_idx] :
                union_(non_base_support_by_layer[layer_idx], contact_footprints.bottom_contacts[layer_idx]);
        }
        layer_timing.finish(support_body[layer_idx].size(), "object_layer=" + std::to_string(layer_idx));
    }

    SupportGeneratorLayersPtr top_contacts = make_contact_layers_from_footprints(
        object, contact_footprints.top_contacts, contact_footprints.top_object_layers,
        SupporLayerType::TopContact, layer_storage);
    SupportGeneratorLayersPtr bottom_contacts = make_contact_layers_from_footprints(
        object, contact_footprints.bottom_contacts, contact_footprints.bottom_object_layers,
        SupporLayerType::BottomContact, layer_storage);
    SupportGeneratorLayersPtr base_layers = make_base_layers_from_support_body(
        object, std::move(support_body), non_base_support_by_layer, layer_storage);
    layers_timing.finish(base_layers.size() + top_contacts.size() + bottom_contacts.size());
    const auto layers_ready_at = Clock::now();
    const auto milliseconds = [](const Clock::time_point &begin, const Clock::time_point &end) {
        return std::chrono::duration<double, std::milli>(end - begin).count();
    };
    const auto log_timings = [&](const Clock::time_point &finished_at, const char *status) {
        BOOST_LOG_TRIVIAL(info)
            << "Cura-style support timings [" << status << "]: parameters="
            << milliseconds(started_at, parameters_ready_at) << "ms geometry="
            << milliseconds(parameters_ready_at, geometry_ready_at) << "ms annotations="
            << milliseconds(geometry_ready_at, annotations_ready_at) << "ms overhangs="
            << milliseconds(annotations_ready_at, overhangs_ready_at) << "ms propagation="
            << milliseconds(overhangs_ready_at, propagation_ready_at) << "ms contacts="
            << milliseconds(propagation_ready_at, contacts_ready_at) << "ms layers="
            << milliseconds(contacts_ready_at, layers_ready_at) << "ms toolpaths="
            << milliseconds(layers_ready_at, finished_at) << "ms total="
            << milliseconds(started_at, finished_at) << "ms";
    };
    if (base_layers.empty() && top_contacts.empty() && bottom_contacts.empty()) {
        log_timings(layers_ready_at, "empty");
        BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - No supports generated";
        return;
    }

    object.print()->set_status(55, _u8L("Cura support: generating support toolpaths"));
    throw_if_canceled(object);
    SupportProfileStage toolpaths_timing("support-cura", "Generate support toolpaths", base_layers.size());
    SupportGeneratorLayersPtr precomputed_top_interfaces;
    SupportGeneratorLayersPtr precomputed_top_base_interfaces;
    auto [interface_layers, base_interface_layers] = generate_interface_layers(
        object.config(), support_params, bottom_contacts, top_contacts,
        precomputed_top_interfaces, precomputed_top_base_interfaces, base_layers, layer_storage);
    throw_if_canceled(object);

    SupportGeneratorLayersPtr raft_layers = generate_raft_base(
        object, support_params, m_slicing_params, top_contacts,
        interface_layers, base_interface_layers, base_layers, layer_storage);
    throw_if_canceled(object);
    generate_support_layers(
        object, raft_layers, bottom_contacts, top_contacts,
        base_layers, interface_layers, base_interface_layers);
    throw_if_canceled(object);
    generate_support_toolpaths(
        object.support_layers(), object.config(), support_params, m_slicing_params,
        raft_layers, bottom_contacts, top_contacts, base_layers,
        interface_layers, base_interface_layers,
        [&object]() { throw_if_canceled(object); });
    toolpaths_timing.finish(object.support_layer_count());

    log_timings(Clock::now(), "complete");
    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - End";
}

std::vector<Polygons> CuraStyleSupportGenerator::detect_support_demand() const
{
    if (m_object == nullptr)
        return {};
    return analyze_support_demand().combined(
        [this]() { throw_if_canceled(*m_object); });
}

CuraSupportDemand CuraStyleSupportGenerator::analyze_support_demand() const
{
    if (m_object == nullptr || m_object->layer_count() == 0)
        return {};

    throw_if_canceled(*m_object);

    return compute_cura_style_full_overhangs(
        *m_object, true,
        !is_mixed(m_object->config().support_type.value) &&
            (m_force_buildplate_only || m_object->config().support_on_build_plate_only.value));
}

} // namespace Slic3r
