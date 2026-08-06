#include "CuraStyleSupport.hpp"

#include "../BoundingBox.hpp"
#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Gpu/VulkanSlicer.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

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

template<class Function>
static void parallel_for_layers(size_t begin_idx, size_t end_idx, Function &&function)
{
    if (begin_idx >= end_idx)
        return;

    tbb::parallel_for(
        tbb::blocked_range<size_t>(begin_idx, end_idx, 4),
        [&](const tbb::blocked_range<size_t> &range) {
            for (size_t layer_idx = range.begin(); layer_idx < range.end(); ++layer_idx)
                function(layer_idx);
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

CuraStyleSupportGenerator::CuraStyleSupportGenerator(const PrintObject *object, const SlicingParameters &slicing_params)
    : m_object(object)
    , m_slicing_params(slicing_params)
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
        });
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

    std::vector<Polygons> outlines;
    std::vector<BoundingBox> outline_bounds;
    mutable std::vector<Polygons> outlines_with_xy_gap;
    mutable std::vector<BoundingBox> xy_gap_bounds;
    mutable std::vector<uint8_t> xy_gap_ready;
    std::vector<coordf_t> print_z;
    std::vector<coordf_t> bottom_z;
    coord_t xy_gap;
};

static coord_t cura_style_overhang_offset(const PrintObject &object, const SupportParameters &support_params, size_t layer_idx)
{
    const PrintObjectConfig &config = object.config();
    const Layer             &layer  = *object.layers()[layer_idx];
    const double threshold_deg = config.support_threshold_angle.value > 0 ?
        std::min<double>(config.support_threshold_angle.value + 1, 89.) :
        0.;

    if (threshold_deg > 0.) {
        const double threshold_rad = Geometry::deg2rad(threshold_deg);
        return coord_t(scale_(layer.height / std::tan(threshold_rad)));
    }

    return std::max<coord_t>(0, support_params.support_material_flow.scaled_width() / 2);
}

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
        });
    }

    std::vector<Polygons> enforcers;
    std::vector<Polygons> blockers;
};

struct CuraOverhangs
{
    std::vector<Polygons> automatic;
    std::vector<Polygons> enforced;

    std::vector<Polygons> combined() const
    {
        std::vector<Polygons> result(automatic.size());
        parallel_for_layers(0, result.size(), [&](size_t layer_idx) {
            if (automatic[layer_idx].empty())
                result[layer_idx] = enforced[layer_idx];
            else if (enforced[layer_idx].empty())
                result[layer_idx] = automatic[layer_idx];
            else
                result[layer_idx] = union_(automatic[layer_idx], enforced[layer_idx]);
        });
        return result;
    }
};

static CuraOverhangs compute_cura_style_full_overhangs(
    const PrintObject            &object,
    const SupportParameters      &support_params,
    const CuraSupportAnnotations &annotations,
    const CuraLayerGeometry      &geometry,
    bool                          automatic_support)
{
    CuraOverhangs overhangs{
        std::vector<Polygons>(object.layer_count()),
        std::vector<Polygons>(object.layer_count())
    };

    // Translated from CuraEngine AreaSupport::computeBasicAndFullOverhang().
    constexpr coordf_t smooth_height = 0.4;
    parallel_for_layers(1, object.layer_count(), [&](size_t layer_idx) {
        const Polygons &current = geometry.outlines[layer_idx];
        if (current.empty())
            return;

        const coord_t max_dist_from_lower_layer = cura_style_overhang_offset(object, support_params, layer_idx);
        const size_t layers_below = std::max<size_t>(
            1, size_t(std::llround(smooth_height / object.layers()[layer_idx]->height)));

        Polygons outlines_below;
        for (size_t layer_offset = 1; layer_offset <= layers_below && layer_offset <= layer_idx; ++layer_offset) {
            const Polygons &lower = geometry.outlines[layer_idx - layer_offset];
            if (!lower.empty())
                append(outlines_below, offset(
                    lower, float(max_dist_from_lower_layer * coord_t(layer_offset)),
                    SUPPORT_SURFACES_OFFSET_PARAMETERS));
        }
        if (!outlines_below.empty())
            outlines_below = union_(outlines_below);

        auto full_overhang = [&](Polygons basic) {
            if (basic.empty())
                return Polygons{};
            if (!annotations.blockers[layer_idx].empty() &&
                polygons_may_overlap(basic, annotations.blockers[layer_idx]))
                basic = diff(basic, annotations.blockers[layer_idx]);
            if (basic.empty())
                return Polygons{};
            const coord_t extension = max_dist_from_lower_layer * coord_t(layers_below) + scale_(0.1);
            return intersection(offset(basic, float(extension), SUPPORT_SURFACES_OFFSET_PARAMETERS), current);
        };

        if (automatic_support) {
            Polygons basic = !outlines_below.empty() && polygons_may_overlap(current, outlines_below) ?
                diff(current, outlines_below) : current;
            overhangs.automatic[layer_idx] = full_overhang(std::move(basic));
        }

        if (!annotations.enforcers[layer_idx].empty() &&
            polygons_may_overlap(current, annotations.enforcers[layer_idx]))
            overhangs.enforced[layer_idx] = full_overhang(
                intersection(current, annotations.enforcers[layer_idx]));
    });

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

static void keep_buildplate_connected_support(std::vector<Polygons> &support_by_layer, const SupportParameters &support_params)
{
    if (support_by_layer.empty())
        return;

    Polygons touching = support_by_layer.front();
    for (size_t layer_idx = 1; layer_idx < support_by_layer.size(); ++layer_idx) {
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
    const CuraLayerGeometry &geometry,
    const std::vector<Polygons> &overhangs,
    coordf_t gap)
{
    std::vector<size_t> contact_for_overhang(overhangs.size(), size_t(-1));
    for (size_t overhang_layer_idx = 1; overhang_layer_idx < overhangs.size(); ++overhang_layer_idx)
        if (!overhangs[overhang_layer_idx].empty())
            contact_for_overhang[overhang_layer_idx] = closest_top_contact_layer(geometry, overhang_layer_idx, gap);
    return contact_for_overhang;
}

static std::vector<Polygons> seed_overhangs_at_contacts(
    const std::vector<Polygons> &overhangs,
    const std::vector<size_t>   &contact_for_overhang)
{
    std::vector<Polygons> seeds(overhangs.size());
    for (size_t overhang_layer_idx = 1; overhang_layer_idx < overhangs.size(); ++overhang_layer_idx) {
        const size_t contact_idx = contact_for_overhang[overhang_layer_idx];
        if (contact_idx == size_t(-1) || overhangs[overhang_layer_idx].empty())
            continue;
        append(seeds[contact_idx], overhangs[overhang_layer_idx]);
    }
    parallel_for_layers(0, seeds.size(), [&](size_t layer_idx) {
        if (!seeds[layer_idx].empty())
            seeds[layer_idx] = union_(seeds[layer_idx]);
    });
    return seeds;
}

static std::vector<Polygons> propagate_cura_style_support_channel(
    const PrintObject              &object,
    const SupportParameters        &support_params,
    const CuraSupportAnnotations   &annotations,
    const CuraLayerGeometry        &geometry,
    const std::vector<Polygons>    &overhangs,
    coordf_t                        top_gap,
    bool                            keep_buildplate_only)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    std::vector<Polygons> support_by_layer(layer_count, Polygons{});

    if (layer_count < 2)
        return support_by_layer;

    const std::vector<size_t> contact_map = make_top_contact_map(geometry, overhangs, top_gap);
    std::vector<Polygons> seeds = seed_overhangs_at_contacts(overhangs, contact_map);
    const coord_t support_expansion = scale_(config.support_expansion.value);
    const coord_t half_min_feature = std::max<coord_t>(support_params.support_material_flow.scaled_width() / 2, scale_(0.05));
    const auto highest_seed = std::find_if(
        seeds.rbegin(), seeds.rend(), [](const Polygons &polygons) { return !polygons.empty(); });
    if (highest_seed == seeds.rend())
        return support_by_layer;
    const size_t highest_active_layer = size_t(std::distance(seeds.begin(), highest_seed.base())) - 1;

    for (int layer_idx = int(highest_active_layer); layer_idx >= 0; --layer_idx) {
        Polygons layer_this = std::move(seeds[size_t(layer_idx)]);

        const Polygons &model_on_layer = geometry.outlines[size_t(layer_idx)];
        if (!layer_this.empty() && support_expansion > 0)
            layer_this = expand_support_away_from_model(
                std::move(layer_this), model_on_layer, support_expansion,
                support_params.support_material_flow.scaled_width());
        else if (!layer_this.empty() && support_expansion < 0)
            layer_this = offset(layer_this, float(support_expansion), SUPPORT_SURFACES_OFFSET_PARAMETERS);

        if (size_t(layer_idx + 1) < layer_count && !support_by_layer[layer_idx + 1].empty()) {
            if (layer_this.empty())
                layer_this = support_by_layer[layer_idx + 1];
            else
                layer_this = union_(layer_this, support_by_layer[layer_idx + 1]);
        }

        if (!layer_this.empty()) {
            if (!annotations.blockers[size_t(layer_idx)].empty() &&
                polygons_may_overlap(layer_this, annotations.blockers[size_t(layer_idx)]))
                layer_this = diff(layer_this, annotations.blockers[size_t(layer_idx)]);
            if (!model_on_layer.empty() &&
                polygons_may_overlap(layer_this, geometry.outline_bounds[size_t(layer_idx)]))
                layer_this = diff(layer_this, model_on_layer);
        }

        support_by_layer[size_t(layer_idx)] = std::move(layer_this);
    }

    std::vector<uint8_t> vulkan_xy_gap_overlap(layer_count, uint8_t(1));
    {
        std::vector<Gpu::VulkanAabb> support_bounds;
        std::vector<Gpu::VulkanAabb> xy_gap_bounds;
        support_bounds.reserve(layer_count);
        xy_gap_bounds.reserve(layer_count);
        for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
            const BoundingBox support_box = support_by_layer[layer_idx].empty() ? BoundingBox() : get_extents(support_by_layer[layer_idx]);
            geometry.with_xy_gap(layer_idx);
            const BoundingBox &gap_box = geometry.xy_gap_bounds[layer_idx];
            const Point support_min = support_box.defined ? support_box.min : Point(0, 0);
            const Point support_max = support_box.defined ? support_box.max : Point(0, 0);
            const Point gap_min = gap_box.defined ? gap_box.min : Point(0, 0);
            const Point gap_max = gap_box.defined ? gap_box.max : Point(0, 0);
            support_bounds.push_back({ { support_min.x(), support_min.y() }, { support_max.x(), support_max.y() } });
            xy_gap_bounds.push_back({ { gap_min.x(), gap_min.y() }, { gap_max.x(), gap_max.y() } });
        }
        const auto batch = Gpu::VulkanSlicerBackend::dispatch_indexed_aabb_candidates(
            support_bounds, xy_gap_bounds, support_params.support_material_flow.scaled_width(),
            Gpu::VulkanAabbOperation::CuraSupport);
        if (batch.resolved) {
            std::fill(vulkan_xy_gap_overlap.begin(), vulkan_xy_gap_overlap.end(), uint8_t(0));
            for (const auto &pair : batch.overlap_pairs)
                if (pair.query == pair.target)
                    vulkan_xy_gap_overlap[pair.query] = uint8_t(1);
        }
    }

    parallel_for_layers(0, highest_active_layer + 1, [&](size_t layer_idx) {
        Polygons &support = support_by_layer[layer_idx];
        if (support.empty())
            return;

        const Polygons &model_with_xy_gap = geometry.with_xy_gap(layer_idx);
        if (vulkan_xy_gap_overlap[layer_idx] != 0 && !model_with_xy_gap.empty() &&
            polygons_may_overlap(support, geometry.xy_gap_bounds[layer_idx]))
            support = diff(support, model_with_xy_gap);

        support = close_unprintable_parts(support, half_min_feature);
    });

    if (keep_buildplate_only)
        keep_buildplate_connected_support(support_by_layer, support_params);

    for (size_t layer_idx = 1; layer_idx + 1 < layer_count; ++layer_idx) {
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
    }

    return support_by_layer;
}

static std::vector<Polygons> propagate_cura_style_support(
    const PrintObject             &object,
    const SlicingParameters       &slicing_params,
    const SupportParameters       &support_params,
    const CuraSupportAnnotations  &annotations,
    const CuraLayerGeometry       &geometry,
    const CuraOverhangs           &overhangs)
{
    const auto has_support = [](const std::vector<Polygons> &channel) {
        return std::any_of(channel.begin(), channel.end(), [](const Polygons &polygons) { return !polygons.empty(); });
    };

    std::vector<Polygons> automatic(object.layer_count());
    if (has_support(overhangs.automatic))
        automatic = propagate_cura_style_support_channel(
            object, support_params, annotations, geometry, overhangs.automatic, slicing_params.gap_support_object,
            object.config().support_on_build_plate_only.value);

    std::vector<Polygons> enforced(object.layer_count());
    if (has_support(overhangs.enforced))
        enforced = propagate_cura_style_support_channel(
            object, support_params, annotations, geometry, overhangs.enforced, slicing_params.gap_support_object, false);

    for (size_t layer_idx = 0; layer_idx < automatic.size(); ++layer_idx) {
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

    const std::vector<size_t> top_contact_map = make_top_contact_map(geometry, full_overhangs, slicing_params.gap_support_object);
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
    }

    for (size_t object_layer_idx = 0; support_params.num_bottom_interface_layers > 0 && object_layer_idx + 1 < layer_count; ++object_layer_idx) {
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
    }

    for (Polygons &polys : out.top_contacts)
        if (!polys.empty())
            polys = union_(polys);
    for (Polygons &polys : out.bottom_contacts)
        if (!polys.empty())
            polys = union_(polys);

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
    }

    return layers;
}

static SupportGeneratorLayersPtr make_base_layers_from_support_body(
    const PrintObject           &object,
    const std::vector<Polygons> &support_by_layer,
    const std::vector<Polygons> &non_base_support_by_layer,
    SupportGeneratorLayerStorage &layer_storage)
{
    SupportGeneratorLayersPtr base_layers;

    for (size_t layer_idx = 0; layer_idx < support_by_layer.size(); ++layer_idx) {
        Polygons base_polygons = support_by_layer[layer_idx];
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
    }

    return base_layers;
}

void CuraStyleSupportGenerator::generate(PrintObject &object)
{
    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - Start";

    if (m_object == nullptr || object.layer_count() == 0)
        return;

    using Clock = std::chrono::steady_clock;
    const auto started_at = Clock::now();
    SupportParameters support_params(object);
    SupportGeneratorLayerStorage layer_storage;
    const auto parameters_ready_at = Clock::now();
    const CuraLayerGeometry geometry(object, scale_(support_params.gap_xy));
    const auto geometry_ready_at = Clock::now();

    const CuraSupportAnnotations annotations(object);
    const auto annotations_ready_at = Clock::now();
    const bool automatic_support = object.config().support_type.value == stNormalCuraAuto;
    const CuraOverhangs overhangs = compute_cura_style_full_overhangs(object, support_params, annotations, geometry, automatic_support);
    const std::vector<Polygons> full_overhangs = overhangs.combined();
    const auto overhangs_ready_at = Clock::now();
    std::vector<Polygons> support_body = propagate_cura_style_support(
        object, m_slicing_params, support_params, annotations, geometry, overhangs);
    const auto propagation_ready_at = Clock::now();
    ContactFootprints contact_footprints = make_cura_style_contact_footprints(
        object, m_slicing_params, support_params, geometry, full_overhangs, support_body);
    const auto contacts_ready_at = Clock::now();
    std::vector<Polygons> non_base_support_by_layer(support_body.size(), Polygons{});

    for (size_t layer_idx = 0; layer_idx < support_body.size(); ++layer_idx) {
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
    }

    SupportGeneratorLayersPtr top_contacts = make_contact_layers_from_footprints(
        object, contact_footprints.top_contacts, contact_footprints.top_object_layers,
        SupporLayerType::TopContact, layer_storage);
    SupportGeneratorLayersPtr bottom_contacts = make_contact_layers_from_footprints(
        object, contact_footprints.bottom_contacts, contact_footprints.bottom_object_layers,
        SupporLayerType::BottomContact, layer_storage);
    SupportGeneratorLayersPtr base_layers = make_base_layers_from_support_body(object, support_body, non_base_support_by_layer, layer_storage);
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

    SupportGeneratorLayersPtr precomputed_top_interfaces;
    SupportGeneratorLayersPtr precomputed_top_base_interfaces;
    auto [interface_layers, base_interface_layers] = generate_interface_layers(
        object.config(), support_params, bottom_contacts, top_contacts,
        precomputed_top_interfaces, precomputed_top_base_interfaces, base_layers, layer_storage);

    SupportGeneratorLayersPtr raft_layers = generate_raft_base(
        object, support_params, m_slicing_params, top_contacts,
        interface_layers, base_interface_layers, base_layers, layer_storage);
    generate_support_layers(
        object, raft_layers, bottom_contacts, top_contacts,
        base_layers, interface_layers, base_interface_layers);
    generate_support_toolpaths(
        object.support_layers(), object.config(), support_params, m_slicing_params,
        raft_layers, bottom_contacts, top_contacts, base_layers,
        interface_layers, base_interface_layers);

    log_timings(Clock::now(), "complete");
    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - End";
}

} // namespace Slic3r
