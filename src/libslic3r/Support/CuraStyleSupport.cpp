#include "CuraStyleSupport.hpp"

#include "../ClipperUtils.hpp"
#include "../Geometry.hpp"
#include "../Layer.hpp"
#include "../Print.hpp"
#include "SupportCommon.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <boost/log/trivial.hpp>

namespace Slic3r {

#define SUPPORT_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

CuraStyleSupportGenerator::CuraStyleSupportGenerator(const PrintObject *object, const SlicingParameters &slicing_params)
    : m_object(object)
    , m_slicing_params(slicing_params)
{
}

static Polygons layer_polygons(const PrintObject &object, size_t layer_idx)
{
    return layer_idx < object.layer_count() ? to_polygons(object.layers()[layer_idx]->lslices) : Polygons{};
}

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

        for (Polygons &layer : enforcers)
            if (!layer.empty())
                layer = union_(layer);
        for (Polygons &layer : blockers)
            if (!layer.empty())
                layer = offset(union_(layer), float(SCALED_EPSILON), SUPPORT_SURFACES_OFFSET_PARAMETERS);
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
        for (size_t layer_idx = 0; layer_idx < result.size(); ++layer_idx) {
            if (automatic[layer_idx].empty())
                result[layer_idx] = enforced[layer_idx];
            else if (enforced[layer_idx].empty())
                result[layer_idx] = automatic[layer_idx];
            else
                result[layer_idx] = union_(automatic[layer_idx], enforced[layer_idx]);
        }
        return result;
    }
};

static CuraOverhangs compute_cura_style_full_overhangs(
    const PrintObject            &object,
    const SupportParameters      &support_params,
    const CuraSupportAnnotations &annotations,
    bool                          automatic_support)
{
    CuraOverhangs overhangs{
        std::vector<Polygons>(object.layer_count()),
        std::vector<Polygons>(object.layer_count())
    };

    // Translated from CuraEngine AreaSupport::computeBasicAndFullOverhang().
    constexpr coordf_t smooth_height = 0.4;
    for (size_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx) {
        Polygons current = layer_polygons(object, layer_idx);
        if (current.empty())
            continue;

        const coord_t max_dist_from_lower_layer = cura_style_overhang_offset(object, support_params, layer_idx);
        const size_t layers_below = std::max<size_t>(
            1, size_t(std::llround(smooth_height / object.layers()[layer_idx]->height)));

        Polygons outlines_below;
        for (size_t layer_offset = 1; layer_offset <= layers_below && layer_offset <= layer_idx; ++layer_offset) {
            Polygons lower = layer_polygons(object, layer_idx - layer_offset);
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
            if (!annotations.blockers[layer_idx].empty())
                basic = diff(basic, annotations.blockers[layer_idx]);
            if (basic.empty())
                return Polygons{};
            const coord_t extension = max_dist_from_lower_layer * coord_t(layers_below) + scale_(0.1);
            return intersection(offset(basic, float(extension), SUPPORT_SURFACES_OFFSET_PARAMETERS), current);
        };

        if (automatic_support)
            overhangs.automatic[layer_idx] = full_overhang(diff(current, outlines_below));

        if (!annotations.enforcers[layer_idx].empty())
            overhangs.enforced[layer_idx] = full_overhang(
                intersection(current, annotations.enforcers[layer_idx]));
    }

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
            model_outline = diff(model_outline, horizontal_expansion);
            model_outline = offset(model_outline, float(amount), SUPPORT_SURFACES_OFFSET_PARAMETERS);
            horizontal_expansion = diff(horizontal_expansion, model_outline);
        }
        expanded += amount;
    }
    return union_(support, horizontal_expansion);
}

static size_t closest_top_contact_layer(const PrintObject &object, size_t overhang_layer_idx, coordf_t gap)
{
    if (overhang_layer_idx == 0 || overhang_layer_idx >= object.layer_count())
        return size_t(-1);

    const coordf_t target_z = object.layers()[overhang_layer_idx]->bottom_z() - gap;
    size_t best_idx = size_t(-1);
    coordf_t best_distance = std::numeric_limits<coordf_t>::max();
    for (size_t layer_idx = 0; layer_idx < overhang_layer_idx; ++layer_idx) {
        const coordf_t distance = std::abs(object.layers()[layer_idx]->print_z - target_z);
        if (distance < best_distance) {
            best_idx = layer_idx;
            best_distance = distance;
        }
    }
    return best_idx;
}

static std::vector<size_t> make_top_contact_map(
    const PrintObject &object,
    const std::vector<Polygons> &overhangs,
    coordf_t gap)
{
    std::vector<size_t> contact_for_overhang(overhangs.size(), size_t(-1));
    for (size_t overhang_layer_idx = 1; overhang_layer_idx < overhangs.size(); ++overhang_layer_idx)
        if (!overhangs[overhang_layer_idx].empty())
            contact_for_overhang[overhang_layer_idx] = closest_top_contact_layer(object, overhang_layer_idx, gap);
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
    for (Polygons &seed : seeds)
        if (!seed.empty())
            seed = union_(seed);
    return seeds;
}

static std::vector<Polygons> propagate_cura_style_support_channel(
    const PrintObject              &object,
    const SupportParameters        &support_params,
    const CuraSupportAnnotations   &annotations,
    const std::vector<Polygons>    &overhangs,
    coordf_t                        top_gap,
    bool                            keep_buildplate_only)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    std::vector<Polygons> support_by_layer(layer_count, Polygons{});

    if (layer_count < 2)
        return support_by_layer;

    const std::vector<size_t> contact_map = make_top_contact_map(object, overhangs, top_gap);
    std::vector<Polygons> seeds = seed_overhangs_at_contacts(overhangs, contact_map);
    const coord_t support_expansion = scale_(config.support_expansion.value);
    const coord_t xy_gap = scale_(support_params.gap_xy);
    const coord_t half_min_feature = std::max<coord_t>(support_params.support_material_flow.scaled_width() / 2, scale_(0.05));

    for (int layer_idx = int(layer_count) - 1; layer_idx >= 0; --layer_idx) {
        Polygons layer_this = std::move(seeds[size_t(layer_idx)]);

        const Polygons model_on_layer = layer_polygons(object, size_t(layer_idx));
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
            if (!annotations.blockers[size_t(layer_idx)].empty())
                layer_this = diff(layer_this, annotations.blockers[size_t(layer_idx)]);
            if (!model_on_layer.empty())
                layer_this = diff(layer_this, model_on_layer);
        }

        support_by_layer[size_t(layer_idx)] = std::move(layer_this);
    }

    for (size_t layer_idx = 0; layer_idx < layer_count; ++layer_idx) {
        Polygons &support = support_by_layer[layer_idx];
        if (support.empty())
            continue;

        const Polygons model_on_layer = layer_polygons(object, layer_idx);
        if (!model_on_layer.empty())
            support = diff(support, offset(model_on_layer, float(xy_gap), SUPPORT_SURFACES_OFFSET_PARAMETERS));

        support = close_unprintable_parts(support, half_min_feature);
    }

    if (keep_buildplate_only)
        keep_buildplate_connected_support(support_by_layer, support_params);

    for (size_t layer_idx = 1; layer_idx + 1 < layer_count; ++layer_idx) {
        if (support_by_layer[layer_idx].empty())
            continue;

        Polygons adjacent = support_by_layer[layer_idx - 1];
        if (!support_by_layer[layer_idx + 1].empty())
            adjacent = adjacent.empty() ? support_by_layer[layer_idx + 1] : union_(adjacent, support_by_layer[layer_idx + 1]);
        if (!adjacent.empty())
            support_by_layer[layer_idx] = intersection(support_by_layer[layer_idx], offset(adjacent, float(support_params.support_material_flow.scaled_width()), SUPPORT_SURFACES_OFFSET_PARAMETERS));
    }

    return support_by_layer;
}

static std::vector<Polygons> propagate_cura_style_support(
    const PrintObject             &object,
    const SlicingParameters       &slicing_params,
    const SupportParameters       &support_params,
    const CuraSupportAnnotations  &annotations,
    const CuraOverhangs           &overhangs)
{
    std::vector<Polygons> automatic = propagate_cura_style_support_channel(
        object, support_params, annotations, overhangs.automatic, slicing_params.gap_support_object,
        object.config().support_on_build_plate_only.value);
    std::vector<Polygons> enforced = propagate_cura_style_support_channel(
        object, support_params, annotations, overhangs.enforced, slicing_params.gap_support_object, false);

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

static size_t closest_bottom_contact_layer(const PrintObject &object, size_t object_layer_idx, coordf_t gap)
{
    if (object_layer_idx + 1 >= object.layer_count())
        return size_t(-1);

    const coordf_t target_bottom_z = object.layers()[object_layer_idx]->print_z + gap;
    size_t best_idx = size_t(-1);
    coordf_t best_distance = std::numeric_limits<coordf_t>::max();
    for (size_t layer_idx = object_layer_idx + 1; layer_idx < object.layer_count(); ++layer_idx) {
        const coordf_t distance = std::abs(object.layers()[layer_idx]->bottom_z() - target_bottom_z);
        if (distance < best_distance) {
            best_idx = layer_idx;
            best_distance = distance;
        }
    }
    return best_idx;
}

static ContactFootprints make_cura_style_contact_footprints(
    const PrintObject           &object,
    const SlicingParameters     &slicing_params,
    const SupportParameters     &support_params,
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

    const std::vector<size_t> top_contact_map = make_top_contact_map(object, full_overhangs, slicing_params.gap_support_object);
    const coord_t support_expansion = scale_(config.support_expansion.value);
    const coord_t xy_gap = scale_(support_params.gap_xy);
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
        Polygons interface_envelope = offset(overhang_seed, float(interface_margin), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        if (!interface_envelope.empty())
            footprint = intersection(footprint, interface_envelope);
        if (footprint.empty())
            continue;

        footprint = close_interface_footprint(footprint, close_distance);
        if (footprint.empty())
            continue;

        const Polygons model_on_contact_layer = layer_polygons(object, contact_layer_idx);
        if (!model_on_contact_layer.empty())
            footprint = diff(footprint, offset(model_on_contact_layer, float(xy_gap), SUPPORT_SURFACES_OFFSET_PARAMETERS));

        footprint = close_interface_footprint(footprint, close_distance);
        if (footprint.empty())
            continue;

        append(out.top_contacts[contact_layer_idx], footprint);
        if (out.top_object_layers[contact_layer_idx] == size_t(-1))
            out.top_object_layers[contact_layer_idx] = overhang_layer_idx;
    }

    for (size_t object_layer_idx = 0; support_params.num_bottom_interface_layers > 0 && object_layer_idx + 1 < layer_count; ++object_layer_idx) {
        Polygons object_top = layer_polygons(object, object_layer_idx);
        const Polygons object_above = layer_polygons(object, object_layer_idx + 1);
        if (!object_above.empty())
            object_top = diff(object_top, offset(object_above, float(scale_(0.05)), SUPPORT_SURFACES_OFFSET_PARAMETERS));
        if (object_top.empty())
            continue;

        const size_t contact_layer_idx = closest_bottom_contact_layer(object, object_layer_idx, slicing_params.gap_object_support);
        if (contact_layer_idx == size_t(-1) || support_body[contact_layer_idx].empty())
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

    SupportParameters support_params(object);
    SupportGeneratorLayerStorage layer_storage;

    const CuraSupportAnnotations annotations(object);
    const bool automatic_support = object.config().support_type.value == stNormalCuraAuto;
    const CuraOverhangs overhangs = compute_cura_style_full_overhangs(object, support_params, annotations, automatic_support);
    const std::vector<Polygons> full_overhangs = overhangs.combined();
    std::vector<Polygons> support_body = propagate_cura_style_support(
        object, m_slicing_params, support_params, annotations, overhangs);
    ContactFootprints contact_footprints = make_cura_style_contact_footprints(
        object, m_slicing_params, support_params, full_overhangs, support_body);
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
    if (base_layers.empty() && top_contacts.empty() && bottom_contacts.empty()) {
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

    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - End";
}

} // namespace Slic3r
