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

static std::vector<Polygons> compute_cura_style_full_overhangs(const PrintObject &object, const SupportParameters &support_params)
{
    std::vector<Polygons> full_overhangs(object.layer_count(), Polygons{});

    for (size_t layer_idx = 1; layer_idx < object.layer_count(); ++layer_idx) {
        Polygons current = layer_polygons(object, layer_idx);
        if (current.empty())
            continue;

        const coord_t supported_offset = cura_style_overhang_offset(object, support_params, layer_idx);
        Polygons supported_below = offset(layer_polygons(object, layer_idx - 1), float(supported_offset), SUPPORT_SURFACES_OFFSET_PARAMETERS);
        Polygons basic_overhang  = diff(current, supported_below);
        if (basic_overhang.empty())
            continue;

        const coord_t smooth_offset = supported_offset + scale_(0.1);
        full_overhangs[layer_idx] = intersection(offset(basic_overhang, float(smooth_offset), SUPPORT_SURFACES_OFFSET_PARAMETERS), current);
    }

    return full_overhangs;
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

        touching = offset(touching, float(support_params.support_material_flow.scaled_width()), SUPPORT_SURFACES_OFFSET_PARAMETERS);
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

static std::vector<Polygons> propagate_cura_style_support(
    const PrintObject               &object,
    const SlicingParameters         &slicing_params,
    const SupportParameters         &support_params,
    const std::vector<Polygons>     &full_overhangs)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    std::vector<Polygons> support_by_layer(layer_count, Polygons{});

    if (layer_count < 2)
        return support_by_layer;

    const double layer_height = std::max<double>(slicing_params.layer_height, EPSILON);
    const size_t top_gap_layers = std::max<size_t>(1, size_t(std::ceil(slicing_params.gap_support_object / layer_height)));
    const coord_t support_expansion = scale_(config.support_expansion.value);
    const coord_t xy_gap = scale_(support_params.gap_xy);
    const coord_t half_min_feature = std::max<coord_t>(support_params.support_material_flow.scaled_width() / 2, scale_(0.05));

    for (int layer_idx = int(layer_count) - 1 - int(top_gap_layers); layer_idx >= 0; --layer_idx) {
        Polygons layer_this;
        const size_t overhang_layer_idx = size_t(layer_idx) + top_gap_layers;
        if (overhang_layer_idx < full_overhangs.size())
            layer_this = full_overhangs[overhang_layer_idx];

        if (!layer_this.empty() && support_expansion != 0)
            layer_this = offset(layer_this, float(support_expansion), SUPPORT_SURFACES_OFFSET_PARAMETERS);

        if (size_t(layer_idx + 1) < layer_count && !support_by_layer[layer_idx + 1].empty()) {
            if (layer_this.empty())
                layer_this = support_by_layer[layer_idx + 1];
            else
                layer_this = union_(layer_this, support_by_layer[layer_idx + 1]);
        }

        if (!layer_this.empty()) {
            const Polygons model_on_layer = layer_polygons(object, size_t(layer_idx));
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

    if (config.support_on_build_plate_only.value)
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

struct InterfaceFootprints
{
    std::vector<Polygons> top_contacts;
    std::vector<Polygons> top_interfaces;
};

static InterfaceFootprints make_cura_style_interface_footprints(
    const PrintObject           &object,
    const SlicingParameters     &slicing_params,
    const SupportParameters     &support_params,
    const std::vector<Polygons> &full_overhangs,
    const std::vector<Polygons> &support_body)
{
    const PrintObjectConfig &config = object.config();
    const size_t layer_count = object.layer_count();
    InterfaceFootprints out{
        std::vector<Polygons>(layer_count, Polygons{}),
        std::vector<Polygons>(layer_count, Polygons{})
    };

    if (layer_count < 2 || support_params.num_top_interface_layers == 0)
        return out;

    const double layer_height = std::max<double>(slicing_params.layer_height, EPSILON);
    const size_t top_gap_layers = std::max<size_t>(1, size_t(std::ceil(slicing_params.gap_support_object / layer_height)));
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

    for (size_t overhang_layer_idx = top_gap_layers; overhang_layer_idx < full_overhangs.size(); ++overhang_layer_idx) {
        Polygons overhang_seed = full_overhangs[overhang_layer_idx];
        if (overhang_seed.empty())
            continue;

        const size_t contact_layer_idx = overhang_layer_idx - top_gap_layers;
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

        for (size_t depth = 1; depth <= support_params.num_top_interface_layers && contact_layer_idx >= depth; ++depth)
            append(out.top_interfaces[contact_layer_idx - depth], footprint);
    }

    for (Polygons &polys : out.top_contacts)
        if (!polys.empty())
            polys = union_(polys);
    for (Polygons &polys : out.top_interfaces)
        if (!polys.empty())
            polys = union_(polys);

    return out;
}

static SupportGeneratorLayersPtr make_layers_from_footprints(
    const PrintObject             &object,
    const std::vector<Polygons>   &footprints,
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
            support_layer.idx_object_layer_above = std::min(layer_idx + 1, object.layer_count() - 1);
            support_layer.contact_polygons = std::make_unique<Polygons>(footprints[layer_idx]);
            support_layer.overhang_polygons = std::make_unique<Polygons>(footprints[layer_idx]);
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

    std::vector<Polygons> full_overhangs = compute_cura_style_full_overhangs(object, support_params);
    std::vector<Polygons> support_body   = propagate_cura_style_support(object, m_slicing_params, support_params, full_overhangs);
    InterfaceFootprints interface_footprints = make_cura_style_interface_footprints(object, m_slicing_params, support_params, full_overhangs, support_body);
    std::vector<Polygons> non_base_support_by_layer(support_body.size(), Polygons{});

    for (size_t layer_idx = 0; layer_idx < support_body.size(); ++layer_idx) {
        if (!interface_footprints.top_contacts[layer_idx].empty()) {
            support_body[layer_idx] = support_body[layer_idx].empty() ? interface_footprints.top_contacts[layer_idx] : union_(support_body[layer_idx], interface_footprints.top_contacts[layer_idx]);
            non_base_support_by_layer[layer_idx] = interface_footprints.top_contacts[layer_idx];
        }
        if (!interface_footprints.top_interfaces[layer_idx].empty()) {
            support_body[layer_idx] = support_body[layer_idx].empty() ? interface_footprints.top_interfaces[layer_idx] : union_(support_body[layer_idx], interface_footprints.top_interfaces[layer_idx]);
            non_base_support_by_layer[layer_idx] = non_base_support_by_layer[layer_idx].empty() ?
                interface_footprints.top_interfaces[layer_idx] :
                union_(non_base_support_by_layer[layer_idx], interface_footprints.top_interfaces[layer_idx]);
        }
    }

    SupportGeneratorLayersPtr empty_layers;
    SupportGeneratorLayersPtr top_contacts = make_layers_from_footprints(object, interface_footprints.top_contacts, SupporLayerType::TopContact, layer_storage);
    SupportGeneratorLayersPtr interface_layers = make_layers_from_footprints(object, interface_footprints.top_interfaces, SupporLayerType::TopInterface, layer_storage);
    SupportGeneratorLayersPtr base_layers = make_base_layers_from_support_body(object, support_body, non_base_support_by_layer, layer_storage);
    if (base_layers.empty() && top_contacts.empty() && interface_layers.empty()) {
        BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - No supports generated";
        return;
    }

    SupportGeneratorLayersPtr raft_layers = generate_raft_base(object, support_params, m_slicing_params, top_contacts, interface_layers, empty_layers, base_layers, layer_storage);
    generate_support_layers(object, raft_layers, empty_layers, top_contacts, base_layers, interface_layers, empty_layers);
    generate_support_toolpaths(object.support_layers(), object.config(), support_params, m_slicing_params, raft_layers, empty_layers, top_contacts, base_layers, interface_layers, empty_layers);

    BOOST_LOG_TRIVIAL(info) << "Cura-style normal support generator - End";
}

} // namespace Slic3r
