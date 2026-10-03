#include "Layer.hpp"
#include "BridgeDetector.hpp"
#include "ClipperUtils.hpp"
#include "Geometry.hpp"
#include "PerimeterGenerator.hpp"
#include "HalfLayerWalls.hpp"
#include "Point.hpp"
#include "Print.hpp"
#include "Surface.hpp"
#include "BoundingBox.hpp"
#include "SVG.hpp"
#include "Algorithm/RegionExpansion.hpp"

#include <string>
#include <map>
#include <cmath>

#include <boost/log/trivial.hpp>
#include <boost/algorithm/clamp.hpp>

namespace Slic3r {

Flow LayerRegion::flow(FlowRole role) const
{
    return this->flow(role, m_layer->height);
}

Flow LayerRegion::flow(FlowRole role, double layer_height) const
{
    return m_region->flow(*m_layer->object(), role, layer_height, m_layer->id() == 0);
}

static Flow region_bridging_flow(const LayerRegion &source, FlowRole role, bool thick_bridge, double height_mm)
{
    const PrintRegion       &region         = source.region();
    const PrintRegionConfig &region_config  = region.config();
    const PrintObject       &print_object   = *source.layer()->object();
    Flow bridge_flow;
    // Here this->extruder(role) - 1 may underflow to MAX_INT, but then the get_at() will fall back to zero'th element, so everything is all right.
    const PrintConfig &print_config = print_object.print()->config();
    const int          extruder_id  = int(region.extruder(role));
    const ResolvedWallTool tool = wall_tool_for_filament(print_config, extruder_id);
    auto nozzle_diameter = tool ? float(tool.nozzle_diameter) : float(print_config.nozzle_diameter.get_at(extruder_id - 1));
    // Toolhead widths are indexed by physical hotend; the material index differs under a non-identity filament_map.
    const ConfigOptionFloatOrPercent bridge_width_opt = toolhead_bridge_line_width_or(
        print_config, tool ? int(tool.hotend_id_1based) : extruder_id, region_config.bridge_line_width);
    const double bridge_width      = bridge_width_opt.get_abs_value(nozzle_diameter);
    const bool   has_bridge_width  = bridge_width > 0.;
    const double bridge_flow_ratio = region_config.bridge_flow;

    if (thick_bridge) {
        // The old Slic3r way (different from all other slicers): Use rounded extrusions.
        // Get the configured nozzle_diameter for the extruder associated to the flow role requested.
        float thread_diameter = has_bridge_width ? float(bridge_width) : nozzle_diameter;
        if (bridge_flow_ratio > 0.)
            thread_diameter *= float(sqrt(bridge_flow_ratio));
        bridge_flow = Flow::bridging_flow(thread_diameter, nozzle_diameter);
    } else {
        // The same way as other slicers: Use normal extrusions. Apply bridge_flow while maintaining the original spacing.
        Flow base_flow = source.flow(role, height_mm);
        if (has_bridge_width)
            base_flow = Flow(float(bridge_width), base_flow.height(), nozzle_diameter);
        bridge_flow = base_flow.with_flow_ratio(bridge_flow_ratio);
    }
    return bridge_flow;

}

Flow LayerRegion::bridging_flow(FlowRole role, bool thick_bridge) const
{
    return region_bridging_flow(*this, role, thick_bridge, this->layer()->height);
}

// Fill in layerm->fill_surfaces by trimming the layerm->slices by the cummulative layerm->fill_surfaces.
void LayerRegion::slices_to_fill_surfaces_clipped()
{
    // Note: this method should be idempotent, but fill_surfaces gets modified 
    // in place. However we're now only using its boundaries (which are invariant)
    // so we're safe. This guarantees idempotence of prepare_infill() also in case
    // that combine_infill() turns some fill_surface into VOID surfaces.
    // Collect polygons per surface type.
    std::array<SurfacesPtr, size_t(stCount)> by_surface;
    for (Surface &surface : this->slices.surfaces)
        by_surface[size_t(surface.surface_type)].emplace_back(&surface);
    // Trim surfaces by the fill_boundaries.
    this->fill_surfaces.surfaces.clear();
    for (size_t surface_type = 0; surface_type < size_t(stCount); ++ surface_type) {
        const SurfacesPtr &this_surfaces = by_surface[surface_type];
        if (! this_surfaces.empty())
            this->fill_surfaces.append(intersection_ex(this_surfaces, this->fill_expolygons), SurfaceType(surface_type));
    }
}

static void make_region_perimeters(const LayerRegion &source,
    const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions,
    ExtrusionEntityCollection &perimeters, ExtrusionEntityCollection &thin_fills,
    SurfaceCollection *fill_surfaces, ExPolygons *fill_no_overlap,
    double parent_height_mm, bool half_layer_outer_walls,
    std::vector<PerimeterGapDemand> *gap_demands = nullptr)
{
    perimeters.clear();
    thin_fills.clear();

    const Layer &layer = *source.layer();
    const PrintConfig       &print_config  = layer.object()->print()->config();
    const PrintRegionConfig &region_config = source.region().config();
    const PrintObjectConfig& object_config = layer.object()->config();
    // This needs to be in sync with PrintObject::_slice() slicing_mode_normal_below_layer!
    bool spiral_mode = print_config.spiral_mode &&
        //FIXME account for raft layers.
        (layer.id() >= size_t(region_config.bottom_shell_layers.value) &&
         layer.print_z >= region_config.bottom_shell_thickness - EPSILON);

    const unsigned int base_wall_filament = region_config.outer_wall_filament_id.value > 0 ?
        unsigned(region_config.outer_wall_filament_id.value) : 1u;
    const ResolvedWallTool override_tool = large_nozzle_override_wall_tool(
        print_config, region_config, layer.id(), base_wall_filament);
    Flow perimeter_flow = source.flow(frPerimeter, parent_height_mm);
    Flow external_perimeter_flow = source.flow(frExternalPerimeter, parent_height_mm);
    if (override_tool) {
        const float nozzle = float(override_tool.nozzle_diameter);
        const bool first_layer = layer.id() == 0;
        perimeter_flow = Flow::new_from_config_width(
            frPerimeter,
            toolhead_line_width_or(print_config, frPerimeter, int(override_tool.hotend_id_1based), first_layer, region_config.inner_wall_line_width),
            nozzle,
            float(parent_height_mm));
        external_perimeter_flow = Flow::new_from_config_width(
            frExternalPerimeter,
            toolhead_line_width_or(print_config, frExternalPerimeter, int(override_tool.hotend_id_1based), first_layer, region_config.outer_wall_line_width),
            nozzle,
            float(parent_height_mm));
    }

    double model_rotation_rad = 0.0;
    if (region_config.align_infill_direction_to_model) {
        auto m = layer.object()->trafo().matrix();
        model_rotation_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
    }

    PerimeterGenerator g(
        // input:
        &slices,
        &compatible_regions,
        parent_height_mm,
        layer.slice_z,
        perimeter_flow,
        &region_config,
        &object_config,
        &print_config,
        spiral_mode,
        model_rotation_rad,
        
        // output:
        &perimeters,
        &thin_fills,
        fill_surfaces,
        //BBS
        fill_no_overlap
    );
    
    if (layer.lower_layer != nullptr)
        // Cummulative sum of polygons over all the regions.
        g.lower_slices = &layer.lower_layer->lslices;
    if (layer.upper_layer != NULL)
        g.upper_slices = &layer.upper_layer->lslices;

    int region_id = source.region().print_object_region_id();
    if (layer.upper_layer != NULL)
        g.upper_slices_same_region = &layer.upper_layer->get_region(region_id)->slices;

    g.layer_id              = (int)layer.id();
    g.half_layer_outer_walls = half_layer_outer_walls;
    g.gap_demands           = gap_demands;
    g.ext_perimeter_flow    = external_perimeter_flow;
    g.overhang_flow         = region_bridging_flow(source, frPerimeter, object_config.thick_bridges, parent_height_mm);
    g.solid_infill_flow     = source.flow(frSolidInfill, parent_height_mm);
    if (object_config.wall_generator.value == PerimeterGeneratorType::Arachne && !spiral_mode)
        g.process_arachne();
    else
        g.process_classic();
}

void LayerRegion::make_perimeters(const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions, SurfaceCollection* fill_surfaces, ExPolygons* fill_no_overlap)
{
    make_region_perimeters(*this, slices, compatible_regions, perimeters, thin_fills,
        fill_surfaces, fill_no_overlap, layer()->height, false);
}

HalfLayerWallGeometry make_half_layer_wall_geometry(const LayerRegion &source,
    const SurfaceCollection &slices, const LayerRegionPtrs &compatible_regions, double parent_height_mm)
{
    if (!std::isfinite(parent_height_mm) || parent_height_mm <= 0.)
        throw std::invalid_argument("Half-layer wall parent height must be positive and finite");
    // Spiral output has continuous Z and cannot be divided into discrete wall
    // events. Do not silently produce an invalid detached plan.
    if (source.layer()->object()->print()->config().spiral_mode)
        throw std::invalid_argument("Half-layer walls require discrete layers, not spiral mode");
    HalfLayerWallGeometry result;
    make_region_perimeters(source, slices, compatible_regions, result.perimeters, result.thin_fills,
        &result.fill_surfaces, &result.fill_no_overlap, parent_height_mm, true, &result.gap_demands);
    return result;
}

std::unique_ptr<ExtrusionEntityCollection> select_half_layer_wall_group(
    const ExtrusionEntityCollection &perimeters, HalfLayerWallGroup group)
{
    auto result = std::make_unique<ExtrusionEntityCollection>();
    result->no_sort = perimeters.no_sort;
    if (!perimeters.can_reverse())
        result->set_reverse();
    result->inset_idx = perimeters.inset_idx;
    result->tool_hint = perimeters.tool_hint;
    result->entities.reserve(perimeters.entities.size());
    for (const ExtrusionEntity *entity : perimeters.entities) {
        std::unique_ptr<ExtrusionEntity> selected;
        if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(entity)) {
            auto children = select_half_layer_wall_group(*collection, group);
            if (!children->empty())
                selected = std::move(children);
        } else {
            if (entity->inset_idx < 0)
                throw std::invalid_argument("Half-layer wall selection requires a known XY inset identity");
            const bool is_shell = entity->inset_idx < 2;
            if (is_shell == (group == HalfLayerWallGroup::Shell))
                selected.reset(entity->clone());
        }
        if (selected) {
            result->entities.push_back(selected.get());
            selected.release();
        }
    }
    return result;
}

static bool gaps_have_height(const ExtrusionEntity &entity, double height_mm)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity))
        return std::all_of(collection->entities.begin(), collection->entities.end(),
            [height_mm](const ExtrusionEntity *child) { return gaps_have_height(*child, height_mm); });
    const auto same_height = [height_mm](const ExtrusionPath &path) {
        // Physical path height tolerance, mm (one geometry coordinate).
        constexpr double path_height_tolerance_mm = 0.000001;
        return path.role() == erGapFill && std::isfinite(path.height) &&
            std::abs(path.height - height_mm) <= path_height_tolerance_mm;
    };
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
        return same_height(*path);
    if (const auto *paths = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        return std::all_of(paths->paths.begin(), paths->paths.end(), same_height);
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        return std::all_of(loop->paths.begin(), loop->paths.end(), same_height);
    return false;
}

static std::optional<HalfLayerGapFill> assign_shell_only_half_layer_gaps(
    const HalfLayerRegionWallCandidate &candidate, HalfLayerGapOwnership &ownership)
{
    if (candidate.unassigned_core_gap_fill.empty() && candidate.unassigned_phase_gap_fill[0].empty() &&
        candidate.unassigned_phase_gap_fill[1].empty()) {
        ownership = HalfLayerGapOwnership::NoGaps;
        return HalfLayerGapFill{};
    }

    // A coupled outline is only a construction input. With no selected inner
    // wall or interior fill domain, it owns no physical deposition. The real
    // source sections own their shell gaps, once in each physical H/2 band.
    // Never extend this rule to mixed shell/core islands: their joint gap
    // demand must be regenerated against the selected, not placeholder, core.
    if (!candidate.core->empty() || !candidate.core_fill_surfaces.empty() || !candidate.core_fill_no_overlap.empty()) {
        ownership = HalfLayerGapOwnership::NeedsJointCore;
        return std::nullopt;
    }
    HalfLayerGapFill result;
    for (unsigned phase = 0; phase < 2; ++phase) {
        const auto &gaps = candidate.unassigned_phase_gap_fill[phase];
        if (!gaps_have_height(gaps, candidate.bands[phase].height_mm())) {
            ownership = HalfLayerGapOwnership::IncompatiblePhaseHeight;
            return std::nullopt;
        }
        result.phases[phase] = gaps;
    }
    ownership = HalfLayerGapOwnership::ShellOnly;
    return result;
}

static ExPolygons half_layer_gap_regions(const std::vector<PerimeterGapDemand> &demands)
{
    ExPolygons regions;
    for (const auto &demand : demands)
        append(regions, demand.regions);
    return union_ex(regions);
}

// Reserve deposited cross-sectional area, not the larger rounded bead width.
// Half-height bridge-role shells also use their actual fixed-height area here,
// rather than the legacy role-based assumption of a round nozzle-sized bridge.
static void half_layer_volume_footprint(const ExtrusionEntity &entity, Polygons &out)
{
    if (const auto *collection = dynamic_cast<const ExtrusionEntityCollection *>(&entity)) {
        for (const auto *child : collection->entities)
            half_layer_volume_footprint(*child, out);
        return;
    }
    const auto append_path = [&out](const ExtrusionPath &path) {
        if (!std::isfinite(path.height) || path.height <= 0. ||
            !std::isfinite(path.mm3_per_mm) || path.mm3_per_mm <= 0.)
            throw std::invalid_argument("Half-layer joint gaps require positive finite wall height and volume");
        const double spacing_mm = path.mm3_per_mm / path.height;
        polygons_append(out, offset(path.polyline.to_polyline(), float(0.5 * scale_(spacing_mm))));
    };
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
        append_path(*path);
    else if (const auto *paths = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        for (const auto &path : paths->paths)
            append_path(path);
    else if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        for (const auto &path : loop->paths)
            append_path(path);
    else
        throw std::invalid_argument("Half-layer joint gaps require known wall geometry");
}

static HalfLayerGapFill assign_joint_half_layer_gaps(const HalfLayerRegionWallCandidate &candidate,
    const std::array<ExPolygons, 2> &source_sections)
{
    HalfLayerGapFill result;
    const std::array<ExPolygons, 2> phase_regions{
        half_layer_gap_regions(candidate.phase_gap_demands[0]), half_layer_gap_regions(candidate.phase_gap_demands[1])};
    const auto core_regions = half_layer_gap_regions(candidate.core_gap_demands);
    ExPolygons common_regions;
    const double parent_height_mm = candidate.bands[0].height_mm() + candidate.bands[1].height_mm();
    // Retain a whole, unchanged common component at parent H. Splitting its
    // tiny moving edge into a separate medial axis could lose printable demand.
    for (const auto &demand : candidate.core_gap_demands) {
        constexpr double flow_height_tolerance_mm = 0.000001; // Physical height, mm.
        if (std::abs(demand.flow.height() - parent_height_mm) > flow_height_tolerance_mm)
            continue;
        for (const auto &region : demand.regions) {
            const ExPolygons component{region};
            bool unchanged = true;
            for (unsigned phase = 0; phase < 2 && unchanged; ++phase) {
                const auto matching = std::find_if(phase_regions[phase].begin(), phase_regions[phase].end(),
                    [&component](const ExPolygon &other) {
                        return diff_ex(component, ExPolygons{other}).empty() && diff_ex(ExPolygons{other}, component).empty();
                    });
                unchanged = matching != phase_regions[phase].end();
            }
            if (unchanged) {
                auto common_demand = demand;
                common_demand.regions = component;
                auto paths = make_perimeter_gap_fill(std::move(common_demand));
                result.core.append(std::move(paths.entities));
                append(common_regions, component);
            }
        }
    }
    common_regions = union_ex(common_regions);
    Polygons reserved_core;
    half_layer_volume_footprint(*candidate.core, reserved_core);
    half_layer_volume_footprint(result.core, reserved_core);
    polygons_append(reserved_core, to_polygons(candidate.core_fill_surfaces.surfaces));
    for (unsigned phase = 0; phase < 2; ++phase) {
        const auto &demands = candidate.phase_gap_demands[phase].empty() ?
            candidate.core_gap_demands : candidate.phase_gap_demands[phase];
        if (demands.empty())
            continue;
        const auto &policy = demands.front();
        // All demands in this homogeneous region share the generator's gap
        // policy. Keep this precondition explicit for future per-island tools.
        for (const auto *inputs : {&candidate.core_gap_demands, &candidate.phase_gap_demands[phase]})
            for (const auto &demand : *inputs)
                if (demand.min_spacing_scaled != policy.min_spacing_scaled ||
                    demand.max_spacing_scaled != policy.max_spacing_scaled ||
                    demand.simplify_distance_scaled != policy.simplify_distance_scaled ||
                    demand.minimum_length_scaled != policy.minimum_length_scaled ||
                    demand.flow.nozzle_diameter() != policy.flow.nozzle_diameter())
                    throw std::invalid_argument("Half-layer joint gap demand requires a homogeneous gap policy");
        auto owned = policy;
        owned.flow = owned.flow.with_height(float(candidate.bands[phase].height_mm()));
        owned.regions = intersection_ex(diff_ex(union_ex(phase_regions[phase], core_regions), common_regions), source_sections[phase]);
        Polygons occupied = reserved_core;
        half_layer_volume_footprint(*candidate.shells[phase], occupied);
        owned.regions = diff_ex(owned.regions, occupied);
        // The producer opens its gaps by half the minimum gap width (PerimeterGenerator); the boolean
        // steps above recreate slivers below that width, which broke the medial axis Voronoi
        // ("source index out of range", Classic Benchy with 4 walls). The medial axis would drop them anyway.
        owned.regions = opening_ex(owned.regions, float(0.5 * owned.min_spacing_scaled));
        result.phases[phase] = make_perimeter_gap_fill(std::move(owned));
    }
    return result;
}

static bool same_half_layer_surface_metadata(const Surface &a, const Surface &b)
{
    return a.surface_type == b.surface_type && a.extra_perimeters == b.extra_perimeters &&
        a.thickness == b.thickness && a.thickness_layers == b.thickness_layers && a.bridge_angle == b.bridge_angle;
}

static HalfLayerRegionWallCandidate make_half_layer_region_wall_candidate_impl(
    const LayerRegion &parent, const LayerRegion &lower, const LayerRegion &upper,
    const SurfaceCollection &parent_slices, const SurfaceCollection &lower_slices, const SurfaceCollection &upper_slices,
    const std::array<LayerRegionPtrs, 3> &compatible_regions)
{
    const Layer &parent_layer = *parent.layer();
    HalfLayerRegionWallCandidate result;
    result.bands = split_model_layer_half(parent_layer.id(), parent_layer.bottom_z(), parent_layer.print_z);
    result.print_object_region_id = parent.region().print_object_region_id();
    const std::array<const LayerRegion *, 2> phases{&lower, &upper};
    constexpr double band_position_tolerance_mm = 0.000001; // One fixed-point coordinate, in mm.
    for (unsigned phase = 0; phase < 2; ++phase) {
        const LayerRegion &region = *phases[phase];
        const Layer &layer = *region.layer();
        if (&region.region() != &parent.region() || layer.object() != parent_layer.object())
            throw std::invalid_argument("Half-layer candidate requires identical source and parent region ownership");
        if (layer.id() != parent_layer.id())
            throw std::invalid_argument("Half-layer candidate source has a different parent layer identity");
        if (!std::isfinite(layer.height) || !std::isfinite(layer.print_z) ||
            std::abs(layer.height - result.bands[phase].height_mm()) > band_position_tolerance_mm ||
            std::abs(layer.print_z - result.bands[phase].print_z_mm) > band_position_tolerance_mm)
            throw std::invalid_argument("Half-layer candidate source does not match its physical Z band");
    }
    const auto retain_unsplit_geometry = [&]() {
        // There is no physical half-wall work to schedule (for example, zero
        // requested walls). Preserve the original parent's fill/geometry,
        // rather than expanding it to the union of unused source sections.
        HalfLayerWallGeometry geometry;
        make_region_perimeters(parent, parent_slices, compatible_regions[0], geometry.perimeters, geometry.thin_fills,
            &geometry.fill_surfaces, &geometry.fill_no_overlap, parent_layer.height, false);
        result.core = std::make_unique<ExtrusionEntityCollection>(std::move(geometry.perimeters));
        for (auto &shell : result.shells)
            shell = std::make_unique<ExtrusionEntityCollection>();
        result.core_slices = parent_slices;
        result.core_fill_surfaces = std::move(geometry.fill_surfaces);
        result.core_fill_no_overlap = std::move(geometry.fill_no_overlap);
        result.gap_fill.emplace();
        result.gap_fill->core = std::move(geometry.thin_fills);
        result.gap_ownership = HalfLayerGapOwnership::Unsplit;
    };
    const SurfaceCollection *template_slices = !parent_slices.empty() ? &parent_slices :
        (!lower_slices.empty() ? &lower_slices : &upper_slices);
    if (template_slices->empty()) {
        retain_unsplit_geometry();
        return result;
    }

    // Surface classification/extra-perimeter requests must survive union. A
    // heterogeneous parent needs a layer-level partition, not a first-surface
    // fallback that silently applies one request to all islands.
    const Surface &surface_template = template_slices->surfaces.front();
    const auto same_metadata = [&surface_template](const Surface &surface) {
        return same_half_layer_surface_metadata(surface, surface_template);
    };
    for (const auto *slices : {&parent_slices, &lower_slices, &upper_slices})
        if (!std::all_of(slices->surfaces.begin(), slices->surfaces.end(), same_metadata))
            throw std::invalid_argument("Half-layer candidate requires homogeneous surface metadata across its source sections");

    result.core_slices.append(union_ex(to_expolygons(lower_slices.surfaces), to_expolygons(upper_slices.surfaces)), surface_template);
    // Use the original parent for inner-wall Flow, logical interlocking parity,
    // modifiers and tool override ranges. Only its contour input is coupled.
    auto core_geometry = make_half_layer_wall_geometry(parent, result.core_slices,
        compatible_regions[0], parent_layer.height);
    result.core = select_half_layer_wall_group(core_geometry.perimeters, HalfLayerWallGroup::Core);
    result.unassigned_core_gap_fill = std::move(core_geometry.thin_fills);
    result.core_gap_demands = std::move(core_geometry.gap_demands);
    result.core_fill_surfaces = std::move(core_geometry.fill_surfaces);
    result.core_fill_no_overlap = std::move(core_geometry.fill_no_overlap);
    const std::array<const SurfaceCollection *, 2> phase_slices{&lower_slices, &upper_slices};
    for (unsigned phase = 0; phase < 2; ++phase) {
        const auto &region = *phases[phase];
        // The existing generator API borrows non-const region pointers for
        // fuzzy configuration lookup; it does not mutate these regions.
        auto geometry = make_half_layer_wall_geometry(region, *phase_slices[phase],
            compatible_regions[phase + 1], parent_layer.height);
        result.shells[phase] = select_half_layer_wall_group(geometry.perimeters, HalfLayerWallGroup::Shell);
        result.unassigned_phase_gap_fill[phase] = std::move(geometry.thin_fills);
        result.phase_gap_demands[phase] = std::move(geometry.gap_demands);
    }
    if (result.shells[0]->empty() && result.shells[1]->empty()) {
        retain_unsplit_geometry();
        return result;
    }
    result.gap_fill = assign_shell_only_half_layer_gaps(result, result.gap_ownership);
    if (!result.gap_fill && result.gap_ownership == HalfLayerGapOwnership::NeedsJointCore) {
        result.gap_fill = assign_joint_half_layer_gaps(result,
            {to_expolygons(lower_slices.surfaces), to_expolygons(upper_slices.surfaces)});
        result.gap_ownership = HalfLayerGapOwnership::JointDemand;
    }
    return result;
}

HalfLayerRegionWallCandidate make_half_layer_region_wall_candidate(
    const LayerRegion &parent, const LayerRegion &lower, const LayerRegion &upper)
{
    return make_half_layer_region_wall_candidate_impl(parent, lower, upper, parent.slices, lower.slices, upper.slices,
        {{{const_cast<LayerRegion *>(&parent)}, {const_cast<LayerRegion *>(&lower)}, {const_cast<LayerRegion *>(&upper)}}});
}

HalfLayerLayerCandidate make_half_layer_layer_candidate(const Layer &parent, const Layer &lower, const Layer &upper)
{
    if (parent.object() != lower.object() || parent.object() != upper.object() ||
        parent.region_count() != lower.region_count() || parent.region_count() != upper.region_count())
        throw std::invalid_argument("Half-layer layer assembly requires matching object and region domains");
    HalfLayerLayerCandidate result;
    result.fills.resize(parent.region_count());
    std::vector<bool> done(parent.region_count(), false);
    const std::array<const Layer *, 3> inputs{&parent, &lower, &upper};
    const auto active_region = [&inputs](size_t index) {
        return std::any_of(inputs.begin(), inputs.end(), [index](const Layer *layer) {
            return !layer->get_region(int(index))->slices.empty();
        });
    };
    for (size_t region_index = 0; region_index < parent.region_count(); ++region_index) {
        if (done[region_index])
            continue;
        done[region_index] = true;
        if (!active_region(region_index))
            continue;
        std::vector<size_t> members{region_index};
        for (size_t other = region_index + 1; other < parent.region_count(); ++other)
            if (!done[other] && active_region(other) && Layer::is_perimeter_compatible(*parent.object()->print(),
                parent.get_region(int(region_index))->region(), parent.get_region(int(other))->region())) {
                members.push_back(other);
                done[other] = true;
            }
        size_t flow_owner = members.front();
        std::array<LayerRegionPtrs, 3> compatible;
        struct MetadataGroup {
            Surface prototype;
            std::array<SurfaceCollection, 3> slices;
        };
        std::vector<MetadataGroup> metadata_groups;
        for (const size_t member : members) {
            if (parent.get_region(int(member))->region().config().sparse_infill_density >
                parent.get_region(int(flow_owner))->region().config().sparse_infill_density)
                flow_owner = member;
            for (unsigned input = 0; input < 3; ++input) {
                const auto *region = inputs[input]->get_region(int(member));
                compatible[input].push_back(const_cast<LayerRegion *>(region));
                for (const auto &surface : region->slices.surfaces) {
                    auto it = std::find_if(metadata_groups.begin(), metadata_groups.end(), [&surface](const MetadataGroup &group) {
                        return same_half_layer_surface_metadata(surface, group.prototype);
                    });
                    if (it == metadata_groups.end()) {
                        metadata_groups.push_back({surface, {}});
                        it = std::prev(metadata_groups.end());
                    }
                    it->slices[input].surfaces.push_back(surface);
                }
            }
        }
        for (auto &metadata : metadata_groups) {
            for (auto &slices : metadata.slices) {
                auto merged = members.size() > 1 ? offset_ex(slices.surfaces, ClipperSafetyOffset) : union_ex(slices.surfaces);
                slices.set(std::move(merged), metadata.prototype);
            }
            auto candidate = make_half_layer_region_wall_candidate_impl(
                *parent.get_region(int(flow_owner)), *lower.get_region(int(flow_owner)), *upper.get_region(int(flow_owner)),
                metadata.slices[0], metadata.slices[1], metadata.slices[2], compatible);
            for (const size_t member : members) {
                auto &fill = result.fills[member];
                const auto &original_slices = parent.get_region(int(member))->slices.surfaces;
                for (const auto &surface : candidate.core_fill_surfaces.surfaces)
                    fill.surfaces.append(intersection_ex(ExPolygons{surface.expolygon}, to_expolygons(original_slices)), surface);
                append(fill.no_overlap, intersection_ex(candidate.core_fill_no_overlap, to_expolygons(original_slices)));
            }
            // Keep the existing compatible-region counterbore exception: these
            // intentional bridge fills extend outside the original slice holes.
            if (parent.get_region(int(flow_owner))->region().config().counterbore_hole_bridging.value != chbNone) {
                for (const auto &surface : candidate.core_fill_surfaces.surfaces) {
                    auto extra = diff_ex(ExPolygons{surface.expolygon}, to_expolygons(metadata.slices[0].surfaces), ApplySafetyOffset::Yes);
                    result.fills[members.front()].surfaces.append(std::move(extra), surface);
                }
            }
            result.groups.push_back(std::move(candidate));
        }
    }
    for (auto &fill : result.fills)
        fill.no_overlap = union_ex(fill.no_overlap);
    return result;
}

#if 1

// Extract surfaces of given type from surfaces, extract fill (layer) thickness of one of the surfaces.
static ExPolygons fill_surfaces_extract_expolygons(Surfaces &surfaces, std::initializer_list<SurfaceType> surface_types, double &thickness)
{
    size_t cnt = 0;
    for (const Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end()) {
            ++cnt;
            thickness = surface.thickness;
        }
    if (cnt == 0)
        return {};

    ExPolygons out;
    out.reserve(cnt);
    for (Surface &surface : surfaces)
        if (std::find(surface_types.begin(), surface_types.end(), surface.surface_type) != surface_types.end())
            out.emplace_back(std::move(surface.expolygon));
    return out;
}

struct ExpansionZone
{
    ExPolygons                           expolygons;
    Algorithm::RegionExpansionParameters parameters;
    bool                                 expanded_into = false;
};

// Cache for detecting bridge orientation and merging regions with overlapping expansions.
struct Bridge {
    ExPolygon expolygon;
    uint32_t group_id;
    std::vector<Algorithm::RegionExpansionEx>::const_iterator bridge_expansion_begin;
    std::optional<double> angle{std::nullopt};
};

// Group the bridge surfaces by overlaps.
uint32_t group_id(std::vector<Bridge> &bridges, uint32_t src_id) {
    uint32_t group_id = bridges[src_id].group_id;
    while (group_id != src_id) {
        src_id = group_id;
        group_id = bridges[src_id].group_id;
    }
    bridges[src_id].group_id = group_id;
    return group_id;
};

std::vector<Bridge> get_grouped_bridges(
    ExPolygons&& bridge_expolygons,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions
) {
    using namespace Algorithm;

    std::vector<Bridge> result;
    {
        result.reserve(bridge_expansions.size());
        uint32_t group_id = 0;
        using std::move_iterator;
        for (ExPolygon& expolygon : bridge_expolygons)
            result.push_back({ std::move(expolygon), group_id ++, bridge_expansions.end() });
    }


    // Detect overlaps of bridge anchors inside their respective shell regions.
    // bridge_expansions are sorted by boundary id and source id.
    for (auto expansion_iterator = bridge_expansions.begin(); expansion_iterator != bridge_expansions.end();) {
        auto boundary_region_begin = expansion_iterator;
        auto boundary_region_end = std::find_if(
            next(expansion_iterator),
            bridge_expansions.end(),
            [&](const RegionExpansionEx& expansion){
                return expansion.boundary_id != expansion_iterator->boundary_id;
            }
        );

        // Cache of bboxes per expansion boundary.
        std::vector<BoundingBox> bounding_boxes;
        bounding_boxes.reserve(std::distance(boundary_region_begin, boundary_region_end));
        std::transform(
            boundary_region_begin,
            boundary_region_end,
            std::back_inserter(bounding_boxes),
            [](const RegionExpansionEx& expansion){
                return get_extents(expansion.expolygon.contour);
            }
        );

        // For each bridge anchor of the current source:
        for (;expansion_iterator != boundary_region_end; ++expansion_iterator) {
            auto candidate_iterator = std::next(expansion_iterator);
            for (;candidate_iterator != boundary_region_end; ++candidate_iterator) {
                const BoundingBox& current_bounding_box{
                    bounding_boxes[expansion_iterator - boundary_region_begin]
                };
                const BoundingBox& candidate_bounding_box{
                    bounding_boxes[candidate_iterator - boundary_region_begin]
                };
                if (
                    expansion_iterator->src_id != candidate_iterator->src_id
                    && current_bounding_box.overlap(candidate_bounding_box)
                    // One may ignore holes, they are irrelevant for intersection test.
                    && !intersection(expansion_iterator->expolygon.contour, candidate_iterator->expolygon.contour).empty()
                ) {
                    // The two bridge regions intersect. Give them the same (lower) group id.
                    uint32_t id  = group_id(result, expansion_iterator->src_id);
                    uint32_t id2 = group_id(result, candidate_iterator->src_id);
                    if (id < id2)
                        result[id2].group_id = id;
                    else
                        result[id].group_id = id2;
                }
            }
        }
    }
    return result;
}

void detect_bridge_directions(
    const Algorithm::WaveSeeds& bridge_anchors,
    std::vector<Bridge>& bridges,
    const std::vector<ExpansionZone>& expansion_zones
) {
    if (expansion_zones.empty()) {
        throw std::runtime_error("At least one expansion zone must exist!");
    }
    auto it_bridge_anchor = bridge_anchors.begin();
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        Bridge &bridge = bridges[bridge_id];
        Polygons anchor_areas;
        int32_t last_anchor_id = -1;
        for (; it_bridge_anchor != bridge_anchors.end() && it_bridge_anchor->src == bridge_id; ++ it_bridge_anchor) {
            if (last_anchor_id != int(it_bridge_anchor->boundary)) {
                last_anchor_id = int(it_bridge_anchor->boundary);

                unsigned start_index{};
                unsigned end_index{};
                for (const ExpansionZone& expansion_zone: expansion_zones) {
                    end_index += expansion_zone.expolygons.size();
                    if (last_anchor_id < static_cast<int64_t>(end_index)) {
                        append(anchor_areas, to_polygons(expansion_zone.expolygons[last_anchor_id - start_index]));
                        break;
                    }
                    start_index += expansion_zone.expolygons.size();
                }
            }
        }
        Lines lines{to_lines(diff_pl(to_polylines(bridge.expolygon), expand(anchor_areas, float(SCALED_EPSILON))))};
        auto [bridging_dir, unsupported_dist] = detect_bridging_direction(lines, to_polygons(bridge.expolygon));
        bridge.angle = M_PI + std::atan2(bridging_dir.y(), bridging_dir.x());

        if constexpr (false) {
            coordf_t    stroke_width = scale_(0.06);
            BoundingBox bbox         = get_extents(anchor_areas);
            bbox.merge(get_extents(bridge.expolygon));
            bbox.offset(scale_(1.));
            ::Slic3r::SVG
                svg(debug_out_path(("bridge" + std::to_string(*bridge.angle) + "_" /* + std::to_string(this->layer()->bottom_z())*/).c_str()),
                bbox);
            svg.draw(bridge.expolygon, "cyan");
            svg.draw(lines, "green", stroke_width);
            svg.draw(anchor_areas, "red");
        }
    }
}

Surfaces merge_bridges(
    std::vector<Bridge>& bridges,
    const std::vector<Algorithm::RegionExpansionEx>& bridge_expansions,
    const float closing_radius
) {
    for (auto it = bridge_expansions.begin(); it != bridge_expansions.end(); ) {
        bridges[it->src_id].bridge_expansion_begin = it;
        uint32_t src_id = it->src_id;
        for (++ it; it != bridge_expansions.end() && it->src_id == src_id; ++ it) ;
    }

    Surfaces result;
    for (uint32_t bridge_id = 0; bridge_id < uint32_t(bridges.size()); ++ bridge_id) {
        if (group_id(bridges, bridge_id) == bridge_id) {
            // Head of the group.
            Polygons acc;
            for (uint32_t bridge_id2 = bridge_id; bridge_id2 < uint32_t(bridges.size()); ++ bridge_id2)
                if (group_id(bridges, bridge_id2) == bridge_id) {
                    append(acc, to_polygons(std::move(bridges[bridge_id2].expolygon)));
                    auto it_bridge_expansion = bridges[bridge_id2].bridge_expansion_begin;
                    assert(it_bridge_expansion == bridge_expansions.end() || it_bridge_expansion->src_id == bridge_id2);
                    for (; it_bridge_expansion != bridge_expansions.end() && it_bridge_expansion->src_id == bridge_id2; ++ it_bridge_expansion)
                        append(acc, to_polygons(it_bridge_expansion->expolygon));
                }
            //FIXME try to be smart and pick the best bridging angle for all?
            if (!bridges[bridge_id].angle) {
                assert(false && "Bridge angle must be pre-calculated!");
            }
            Surface templ{ stBottomBridge, {} };
            templ.bridge_angle = bridges[bridge_id].angle ? *bridges[bridge_id].angle : -1;
            //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
            // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
            // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
            ExPolygons final = closing_ex(acc, closing_radius);
            // without safety offset, artifacts are generated (GH #2494)
            // union_safety_offset_ex(acc)
            for (ExPolygon &ex : final)
                result.emplace_back(templ, std::move(ex));
        }
    }
    return result;
}

struct ExpansionResult {
    Algorithm::WaveSeeds anchors;
    std::vector<Algorithm::RegionExpansionEx> expansions;
};

ExpansionResult expand_expolygons(
    const ExPolygons& expolygons,
    std::vector<ExpansionZone>& expansion_zones
) {
    using namespace Algorithm;
    WaveSeeds bridge_anchors;
    std::vector<RegionExpansionEx> bridge_expansions;

    unsigned processed_bridges_count = 0;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        WaveSeeds seeds{wave_seeds(
            expolygons,
            expansion_zone.expolygons,
            expansion_zone.parameters.tiny_expansion,
            true
        )};
        std::vector<RegionExpansionEx> expansions{propagate_waves_ex(
            seeds,
            expansion_zone.expolygons,
            expansion_zone.parameters
        )};

        for (WaveSeed &seed : seeds)
            seed.boundary += processed_bridges_count;
        for (RegionExpansionEx &expansion : expansions)
            expansion.boundary_id += processed_bridges_count;

        expansion_zone.expanded_into = ! expansions.empty();

        append(bridge_anchors, std::move(seeds));
        append(bridge_expansions, std::move(expansions));

        processed_bridges_count += expansion_zone.expolygons.size();
    }
    return {bridge_anchors, bridge_expansions};
}

// Extract bridging surfaces from "surfaces", expand them into "shells" using expansion_params,
// detect bridges.
// Trim "shells" by the expanded bridges.
Surfaces expand_bridges_detect_orientations(
    Surfaces &surfaces,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons bridge_expolygons = fill_surfaces_extract_expolygons(surfaces, {stBottomBridge}, thickness);
    if (bridge_expolygons.empty())
        return {};

    // Calculate bridge anchors and their expansions in their respective shell region.
    ExpansionResult expansion_result{expand_expolygons(
        bridge_expolygons,
        expansion_zones
    )};

    std::vector<Bridge> bridges{get_grouped_bridges(
        std::move(bridge_expolygons),
        expansion_result.expansions
    )};
    bridge_expolygons.clear();

    std::sort(expansion_result.anchors.begin(), expansion_result.anchors.end(), Algorithm::lower_by_src_and_boundary);
    detect_bridge_directions(expansion_result.anchors, bridges, expansion_zones);

    // Merge the groups with the same group id, produce surfaces by merging source overhangs with their newly expanded anchors.
    std::sort(expansion_result.expansions.begin(), expansion_result.expansions.end(), [](auto &l, auto &r) {
        return l.src_id < r.src_id || (l.src_id == r.src_id && l.boundary_id < r.boundary_id);
    });
    Surfaces out{merge_bridges(bridges, expansion_result.expansions, closing_radius)};

    // Clip by the expanded bridges.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, out);
    return out;
}

Surfaces expand_merge_surfaces(
    Surfaces &surfaces,
    SurfaceType surface_type,
    std::vector<ExpansionZone>& expansion_zones,
    const float closing_radius,
    const double bridge_angle = -1
)
{
    using namespace Slic3r::Algorithm;

    double thickness;
    ExPolygons src = fill_surfaces_extract_expolygons(surfaces, {surface_type}, thickness);
    if (src.empty())
        return {};

    unsigned processed_expolygons_count = 0;
    std::vector<RegionExpansion> expansions;
    for (ExpansionZone& expansion_zone : expansion_zones) {
        std::vector<RegionExpansion> zone_expansions = propagate_waves(src, expansion_zone.expolygons, expansion_zone.parameters);
        expansion_zone.expanded_into = !zone_expansions.empty();

        for (RegionExpansion &expansion : zone_expansions)
            expansion.boundary_id += processed_expolygons_count;

        processed_expolygons_count += expansion_zone.expolygons.size();
        append(expansions, std::move(zone_expansions));
    }

    std::vector<ExPolygon> expanded = merge_expansions_into_expolygons(std::move(src), std::move(expansions));
    //NOTE: The current regularization of the shells can create small unasigned regions in the object (E.G. benchy)
    // without the following closing operation, those regions will stay unfilled and cause small holes in the expanded surface.
    // look for narrow_ensure_vertical_wall_thickness_region_radius filter.
    expanded = closing_ex(expanded, closing_radius);
    // Trim the zones by the expanded expolygons.
    for (ExpansionZone& expansion_zone : expansion_zones)
        if (expansion_zone.expanded_into)
            expansion_zone.expolygons = diff_ex(expansion_zone.expolygons, expanded);

    Surface templ{ surface_type, {} };
    templ.bridge_angle = bridge_angle;
    Surfaces out;
    out.reserve(expanded.size());
    for (auto &expoly : expanded)
        out.emplace_back(templ, std::move(expoly));
    return out;
}

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    using namespace Slic3r::Algorithm;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // Width of the perimeters.
    float shell_width = 0;
    float expansion_min = 0;
    if (int num_perimeters = this->region().config().wall_loops; num_perimeters > 0) {
        Flow external_perimeter_flow = this->flow(frExternalPerimeter);
        Flow perimeter_flow          = this->flow(frPerimeter);
        shell_width  = 0.5f * external_perimeter_flow.scaled_width() + external_perimeter_flow.scaled_spacing();
        shell_width += perimeter_flow.scaled_spacing() * (num_perimeters - 1);
        expansion_min = perimeter_flow.scaled_spacing();
    } else {
        // TODO: Maybe there is better solution when printing with zero perimeters, but this works reasonably well, given the situation
        shell_width   = float(SCALED_EPSILON);
        expansion_min = float(SCALED_EPSILON);;
    }

    // Scaled expansions of the respective external surfaces.
    float                           expansion_top           = shell_width * sqrt(2.);
    float                           expansion_bottom        = expansion_top;
    float                           expansion_bottom_bridge = expansion_top;
    // Expand by waves of expansion_step size (expansion_step is scaled), but with no more steps than max_nr_expansion_steps.
    const float                     expansion_step          = scaled<float>(0.1);
    // Don't take more than max_nr_steps for small expansion_step.
    static constexpr const size_t   max_nr_expansion_steps  = 5;
    // Radius (with added epsilon) to absorb empty regions emering from regularization of ensuring, viz  const float narrow_ensure_vertical_wall_thickness_region_radius = 0.5f * 0.65f * min_perimeter_infill_spacing;
    const float closing_radius = 0.55f * 0.65f * 1.05f * this->flow(frSolidInfill).scaled_spacing();

    // Expand the top / bottom / bridge surfaces into the shell thickness solid infills.
    double     layer_thickness;
    ExPolygons shells = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, { stInternalSolid }, layer_thickness));
    ExPolygons sparse = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stInternal}, layer_thickness));
    ExPolygons top_expolygons = union_ex(fill_surfaces_extract_expolygons(this->fill_surfaces.surfaces, {stTop}, layer_thickness));
    const auto expansion_params_into_sparse_infill = RegionExpansionParameters::build(expansion_min, expansion_step, max_nr_expansion_steps);
    const auto expansion_params_into_solid_infill  = RegionExpansionParameters::build(expansion_bottom_bridge, expansion_step, max_nr_expansion_steps);

    std::vector<ExpansionZone> expansion_zones{
        ExpansionZone{std::move(shells), expansion_params_into_solid_infill},
        ExpansionZone{std::move(sparse), expansion_params_into_sparse_infill},
        ExpansionZone{std::move(top_expolygons), expansion_params_into_solid_infill},
    };

    SurfaceCollection bridges;
    {
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z;
        // ORCA: Relative/Align Bridge Angle
        const auto  &region_config    = this->region().config();
        const double custom_angle_deg = region_config.bridge_angle.value;
        const bool   relative_angle   = region_config.relative_bridge_angle.value;
        const double custom_angle_rad = Geometry::deg2rad(custom_angle_deg);

        double align_offset_rad = 0.0;
        if (region_config.align_infill_direction_to_model) {
            auto m = this->layer()->object()->trafo().matrix();
            align_offset_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
        }

        bridges.surfaces = (custom_angle_deg > 0.0 && !relative_angle) ?
            expand_merge_surfaces(this->fill_surfaces.surfaces, stBottomBridge, expansion_zones, closing_radius, custom_angle_rad + align_offset_rad) :
            expand_bridges_detect_orientations(this->fill_surfaces.surfaces, expansion_zones, closing_radius);
        if (custom_angle_deg > 0.0 && relative_angle) {
            for (Surface &bridge_surface : bridges.surfaces) {
                if (bridge_surface.bridge_angle >= 0)
                    bridge_surface.bridge_angle += custom_angle_rad;
            }
        }
        BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun++).c_str(), true);
        }
#endif
    }

    this->fill_surfaces.remove_types({stTop});
    {
        Surface top_templ(stTop, {});
        top_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones.back().expolygons), top_templ);
    }

    expansion_zones.pop_back();

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_bottom, expansion_step, max_nr_expansion_steps);
    Surfaces bottoms = expand_merge_surfaces(this->fill_surfaces.surfaces, stBottom, expansion_zones, closing_radius);

    expansion_zones.at(0).parameters = RegionExpansionParameters::build(expansion_top, expansion_step, max_nr_expansion_steps);
    Surfaces tops = expand_merge_surfaces(this->fill_surfaces.surfaces, stTop, expansion_zones, closing_radius);

    // turn too small internal regions into solid regions according to the user setting
    if (!this->layer()->object()->print()->config().spiral_mode && this->region().config().sparse_infill_density.value > 0) {
        // scaling an area requires two calls!
        double min_area = scale_(scale_(this->region().config().minimum_sparse_infill_area.value));
        ExPolygons small_regions{};
        expansion_zones[1].expolygons.erase(std::remove_if(expansion_zones[1].expolygons.begin(), expansion_zones[1].expolygons.end(), [min_area, &small_regions](ExPolygon& ex_polygon) {
            if (ex_polygon.area() <= min_area) {
                small_regions.push_back(ex_polygon);
                return true;
            }
            return false;
        }), expansion_zones[1].expolygons.end());

        if (!small_regions.empty()) {
            expansion_zones[0].expolygons = union_ex(expansion_zones[0].expolygons, small_regions);
        }
    }

//    this->fill_surfaces.remove_types({ stBottomBridge, stBottom, stTop, stInternal, stInternalSolid });
    this->fill_surfaces.clear();
    unsigned zones_expolygons_count = 0;
    for (const ExpansionZone& zone : expansion_zones)
        zones_expolygons_count += zone.expolygons.size();
    reserve_more(this->fill_surfaces.surfaces, zones_expolygons_count + bridges.size() + bottoms.size() + tops.size());
    {
        Surface solid_templ(stInternalSolid, {});
        solid_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[0].expolygons), solid_templ);
    }
    {
        Surface sparse_templ(stInternal, {});
        sparse_templ.thickness = layer_thickness;
        this->fill_surfaces.append(std::move(expansion_zones[1].expolygons), sparse_templ);
    }
    this->fill_surfaces.append(std::move(bridges.surfaces));
    this->fill_surfaces.append(std::move(bottoms));
    this->fill_surfaces.append(std::move(tops));

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("4_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
#else

//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 3.
//#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtMiter, 1.5
#define EXTERNAL_SURFACES_OFFSET_PARAMETERS ClipperLib::jtSquare, 0.

void LayerRegion::process_external_surfaces(const Layer *lower_layer, const Polygons *lower_layer_covered)
{
    const bool      has_infill = this->region().config().sparse_infill_density.value > 0.;
    //BBS
    auto nozzle_diameter = this->region().nozzle_dmr_avg(this->layer()->object()->print()->config());
    const float margin = float(scale_(EXTERNAL_INFILL_MARGIN));
    const float bridge_margin = std::min(float(scale_(BRIDGE_INFILL_MARGIN)), float(scale_(nozzle_diameter * BRIDGE_INFILL_MARGIN / 0.4)));

    // BBS
    const PrintObjectConfig& object_config = this->layer()->object()->config();

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */

    // 1) Collect bottom and bridge surfaces, each of them grown by a fixed 3mm offset
    // for better anchoring.
    // Bottom surfaces, grown.
    Surfaces                    bottom;
    // Bridge surfaces, initialy not grown.
    Surfaces                    bridges;
    // Top surfaces, grown.
    Surfaces                    top;
    // Internal surfaces, not grown.
    Surfaces                    internal;
    // Areas, where an infill of various types (top, bottom, bottom bride, sparse, void) could be placed.
    Polygons                    fill_boundaries = to_polygons(this->fill_expolygons);
    Polygons  					lower_layer_covered_tmp;

    // Collect top surfaces and internal surfaces.
    // Collect fill_boundaries: If we're slicing with no infill, we can't extend external surfaces over non-existent infill.
    // This loop destroys the surfaces (aliasing this->fill_surfaces.surfaces) by moving into top/internal/fill_boundaries!

    {
        // Voids are sparse infills if infill rate is zero.
        Polygons voids;

        double max_grid_area = -1;
        if (this->layer()->lower_layer != nullptr)
            max_grid_area = this->layer()->lower_layer->get_sparse_infill_max_void_area();
        for (const Surface &surface : this->fill_surfaces.surfaces) {
            if (surface.is_top()) {
                // Collect the top surfaces, inflate them and trim them by the bottom surfaces.
                // This gives the priority to bottom surfaces.
                if (max_grid_area < 0 || surface.expolygon.area() < max_grid_area)
                    surfaces_append(top, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
                else
                    //BBS: Don't need to expand too much in this situation. Expand 3mm to eliminate hole and 1mm for contour
                    surfaces_append(top, intersection_ex(offset(surface.expolygon.contour, margin / 3.0, EXTERNAL_SURFACES_OFFSET_PARAMETERS),
                                                         offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS)), surface);
            } else if (surface.surface_type == stBottom || (surface.surface_type == stBottomBridge && lower_layer == nullptr)) {
                // Grown by 3mm.
                surfaces_append(bottom, offset_ex(surface.expolygon, margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS), surface);
            } else if (surface.surface_type == stBottomBridge) {
                if (! surface.empty())
                    bridges.emplace_back(surface);
            }
            if (surface.is_internal()) {
            	assert(surface.surface_type == stInternal || surface.surface_type == stInternalSolid);
            	if (! has_infill && lower_layer != nullptr)
            		polygons_append(voids, surface.expolygon);
            	internal.emplace_back(std::move(surface));
            }
        }
        if (! has_infill && lower_layer != nullptr && ! voids.empty()) {
        	// Remove voids from fill_boundaries, that are not supported by the layer below.
            if (lower_layer_covered == nullptr) {
            	lower_layer_covered = &lower_layer_covered_tmp;
            	lower_layer_covered_tmp = to_polygons(lower_layer->lslices);
            }
            if (! lower_layer_covered->empty())
            	voids = diff(voids, *lower_layer_covered);
            fill_boundaries = diff(fill_boundaries, voids);
        }
    }

#if 0
    {
        static int iRun = 0;
        bridges.export_to_svg(debug_out_path("bridges-before-grouping-%d.svg", iRun ++), true);
    }
#endif

    if (bridges.empty())
    {
        fill_boundaries = union_safety_offset(fill_boundaries);
    } else
    {
        // 1) Calculate the inflated bridge regions, each constrained to its island.
        ExPolygons               fill_boundaries_ex = union_safety_offset_ex(fill_boundaries);
        std::vector<Polygons>    bridges_grown;
        std::vector<BoundingBox> bridge_bboxes;

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
        {
            static int iRun = 0;
            SVG svg(debug_out_path("3_process_external_surfaces-fill_regions-%d.svg", iRun ++).c_str(), get_extents(fill_boundaries_ex));
            svg.draw(fill_boundaries_ex);
            svg.draw_outline(fill_boundaries_ex, "black", "blue", scale_(0.05)); 
            svg.Close();
        }

//        export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
 
        {
            // Bridge expolygons, grown, to be tested for intersection with other bridge regions.
            std::vector<BoundingBox> fill_boundaries_ex_bboxes = get_extents_vector(fill_boundaries_ex);
            bridges_grown.reserve(bridges.size());
            bridge_bboxes.reserve(bridges.size());
            for (size_t i = 0; i < bridges.size(); ++ i) {
                // Find the island of this bridge.
                const Point pt = bridges[i].expolygon.contour.points.front();
                int idx_island = -1;
                for (int j = 0; j < int(fill_boundaries_ex.size()); ++ j)
                    if (fill_boundaries_ex_bboxes[j].contains(pt) && 
                        fill_boundaries_ex[j].contains(pt)) {
                        idx_island = j;
                        break;
                    }
                // Grown by 3mm.
                //BBS: eliminate too narrow area to avoid generating bridge on top layer when wall loop is 1
                //Polygons polys = offset(bridges[i].expolygon, bridge_margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                Polygons polys = offset2({ bridges[i].expolygon }, -scale_(nozzle_diameter * 0.1), bridge_margin, EXTERNAL_SURFACES_OFFSET_PARAMETERS);
                if (idx_island == -1) {
				    BOOST_LOG_TRIVIAL(trace) << "Bridge did not fall into the source region!";
                } else {
                    // Found an island, to which this bridge region belongs. Trim it,
                    polys = intersection(polys, fill_boundaries_ex[idx_island]);
                }
                bridge_bboxes.push_back(get_extents(polys));
                bridges_grown.push_back(std::move(polys));
            }
        }

        // 2) Group the bridge surfaces by overlaps.
        std::vector<size_t> bridge_group(bridges.size(), (size_t)-1);
        size_t n_groups = 0; 
        for (size_t i = 0; i < bridges.size(); ++ i) {
            // A grup id for this bridge.
            size_t group_id = (bridge_group[i] == size_t(-1)) ? (n_groups ++) : bridge_group[i];
            bridge_group[i] = group_id;
            // For all possibly overlaping bridges:
            for (size_t j = i + 1; j < bridges.size(); ++ j) {
                if (! bridge_bboxes[i].overlap(bridge_bboxes[j]))
                    continue;
                if (intersection(bridges_grown[i], bridges_grown[j]).empty())
                    continue;
                // The two bridge regions intersect. Give them the same group id.
                if (bridge_group[j] != size_t(-1)) {
                    // The j'th bridge has been merged with some other bridge before.
                    size_t group_id_new = bridge_group[j];
                    for (size_t k = 0; k < j; ++ k)
                        if (bridge_group[k] == group_id)
                            bridge_group[k] = group_id_new;
                    group_id = group_id_new;
                }
                bridge_group[j] = group_id;
            }
        }

        // 3) Merge the groups with the same group id, detect bridges.
        {
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges. layer" << this->layer()->print_z << ", bridge groups: " << n_groups;
            for (size_t group_id = 0; group_id < n_groups; ++ group_id) {
                size_t n_bridges_merged = 0;
                size_t idx_last = (size_t)-1;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] == group_id) {
                        ++ n_bridges_merged;
                        idx_last = i;
                    }
                }
                if (n_bridges_merged == 0)
                    // This group has no regions assigned as these were moved into another group.
                    continue;
                // Collect the initial ungrown regions and the grown polygons.
                ExPolygons  initial;
                Polygons    grown;
                for (size_t i = 0; i < bridges.size(); ++ i) {
                    if (bridge_group[i] != group_id)
                        continue;
                    initial.push_back(std::move(bridges[i].expolygon));
                    polygons_append(grown, bridges_grown[i]);
                }
                // detect bridge direction before merging grown surfaces otherwise adjacent bridges
                // would get merged into a single one while they need different directions
                // also, supply the original expolygon instead of the grown one, because in case
                // of very thin (but still working) anchors, the grown expolygon would go beyond them
                // ORCA: Relative/Align Bridge Angle
                const auto &region_config   = this->region().config();
                const double custom_angle_deg = region_config.bridge_angle.value;
                const bool   relative_angle   = region_config.relative_bridge_angle.value;
                const double custom_angle_rad = Geometry::deg2rad(custom_angle_deg);

                double align_offset_rad = 0.0;
                if (region_config.align_infill_direction_to_model) {
                    auto m = this->layer()->object()->trafo().matrix();
                    align_offset_rad = std::atan2((double)m(1, 0), (double)m(0, 0));
                }

                if (custom_angle_deg > 0.0 && !relative_angle) {
                    bridges[idx_last].bridge_angle = custom_angle_rad + align_offset_rad;
                } else {
                    auto [bridging_dir, unsupported_dist] = detect_bridging_direction(to_polygons(initial), to_polygons(lower_layer->lslices));
                    bridges[idx_last].bridge_angle = PI + std::atan2(bridging_dir.y(), bridging_dir.x());
                    if (custom_angle_deg > 0.0 && relative_angle)
                        bridges[idx_last].bridge_angle += custom_angle_rad;
                }

                /*
                BridgeDetector bd(initial, lower_layer->lslices, this->bridging_flow(frInfill, object_config.thick_bridges).scaled_width());
                #ifdef SLIC3R_DEBUG
                printf("Processing bridge at layer %zu:\n", this->layer()->id());
                #endif
                //BBS: use 0 as custom angle to enable auto detection all the time
                double custom_angle = Geometry::deg2rad(this->region().config().bridge_angle.value);
                if(custom_angle > 0)
                        bridges[idx_last].bridge_angle = custom_angle;
				else if (bd.detect_angle(custom_angle)) {
                    bridges[idx_last].bridge_angle = bd.angle;
                    if (this->layer()->object()->has_support()) {
//                        polygons_append(this->bridged, bd.coverage());
                        append(this->unsupported_bridge_edges, bd.unsupported_edges());
                    }
				} else if (custom_angle > 0) {
					// Bridge was not detected (likely it is only supported at one side). Still it is a surface filled in
					// using a bridging flow, therefore it makes sense to respect the custom bridging direction.
					bridges[idx_last].bridge_angle = custom_angle;
				}
                */
                // without safety offset, artifacts are generated (GH #2494)
                surfaces_append(bottom, union_safety_offset_ex(grown), bridges[idx_last]);
            }

            fill_boundaries = to_polygons(fill_boundaries_ex);
			BOOST_LOG_TRIVIAL(trace) << "Processing external surface, detecting bridges - done";
		}

    #if 0
        {
            static int iRun = 0;
            bridges.export_to_svg(debug_out_path("bridges-after-grouping-%d.svg", iRun ++), true);
        }
    #endif
    }

    Surfaces new_surfaces;
    {
        // Merge top and bottom in a single collection.
        surfaces_append(top, std::move(bottom));
        // Intersect the grown surfaces with the actual fill boundaries.
        Polygons bottom_polygons = to_polygons(bottom);
        for (size_t i = 0; i < top.size(); ++ i) {
            Surface &s1 = top[i];
            if (s1.empty())
                continue;
            Polygons polys;
            polygons_append(polys, to_polygons(std::move(s1)));
            for (size_t j = i + 1; j < top.size(); ++ j) {
                Surface &s2 = top[j];
                if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                    polygons_append(polys, to_polygons(std::move(s2)));
                    s2.clear();
                }
            }
            if (s1.is_top())
                // Trim the top surfaces by the bottom surfaces. This gives the priority to the bottom surfaces.
                polys = diff(polys, bottom_polygons);
            surfaces_append(
                new_surfaces,
                // Don't use a safety offset as fill_boundaries were already united using the safety offset.
                intersection_ex(polys, fill_boundaries),
                s1);
        }
    }
    
    // Subtract the new top surfaces from the other non-top surfaces and re-add them.
    Polygons new_polygons = to_polygons(new_surfaces);
    for (size_t i = 0; i < internal.size(); ++ i) {
        Surface &s1 = internal[i];
        if (s1.empty())
            continue;
        Polygons polys;
        polygons_append(polys, to_polygons(std::move(s1)));
        for (size_t j = i + 1; j < internal.size(); ++ j) {
            Surface &s2 = internal[j];
            if (! s2.empty() && surfaces_could_merge(s1, s2)) {
                polygons_append(polys, to_polygons(std::move(s2)));
                s2.clear();
            }
        }
        ExPolygons new_expolys = diff_ex(polys, new_polygons);
        polygons_append(new_polygons, to_polygons(new_expolys));
        surfaces_append(new_surfaces, std::move(new_expolys), s1);
    }
    
    this->fill_surfaces.surfaces = std::move(new_surfaces);

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_fill_surfaces_to_svg_debug("3_process_external_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}
#endif

void LayerRegion::prepare_fill_surfaces()
{
#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-initial");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-initial");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */ 

    /*  Note: in order to make the psPrepareInfill step idempotent, we should never
        alter fill_surfaces boundaries on which our idempotency relies since that's
        the only meaningful information returned by psPerimeters. */
    
    bool spiral_mode = this->layer()->object()->print()->config().spiral_mode;

    // if no solid layers are requested, turn top/bottom surfaces to internal
    if (! spiral_mode && this->region().config().top_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_top())
                //BBS
                //surface.surface_type = this->layer()->object()->config().infill_only_where_needed ? stInternalVoid : stInternal;
                surface.surface_type = PrintObject::infill_only_where_needed ? stInternalVoid : stInternal;
    }
    if (this->region().config().bottom_shell_layers == 0) {
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.is_bottom()) // (surface.surface_type == stBottom)
                surface.surface_type = stInternal;
    }

    if (!spiral_mode && fabs(this->region().config().sparse_infill_density.value - 100.) < EPSILON) {
        // Turn all internal sparse infill into solid infill, if sparse_infill_density is 100%
        for (Surface &surface : this->fill_surfaces.surfaces)
            if (surface.surface_type == stInternal)
                surface.surface_type = stInternalSolid;
    }

#ifdef SLIC3R_DEBUG_SLICE_PROCESSING
    export_region_slices_to_svg_debug("2_prepare_fill_surfaces-final");
    export_region_fill_surfaces_to_svg_debug("2_prepare_fill_surfaces-final");
#endif /* SLIC3R_DEBUG_SLICE_PROCESSING */
}

double LayerRegion::infill_area_threshold() const
{
    double ss = this->flow(frSolidInfill).scaled_spacing();
    return ss*ss;
}

void LayerRegion::trim_surfaces(const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
	this->slices.set(intersection_ex(this->slices.surfaces, trimming_polygons), stInternal);
}

void LayerRegion::elephant_foot_compensation_step(const float elephant_foot_compensation_perimeter_step, const Polygons &trimming_polygons)
{
#ifndef NDEBUG
    for (const Surface &surface : this->slices.surfaces)
        assert(surface.surface_type == stInternal);
#endif /* NDEBUG */
    Polygons tmp = intersection(this->slices.surfaces, trimming_polygons);
    append(tmp, diff(this->slices.surfaces, opening(this->slices.surfaces, elephant_foot_compensation_perimeter_step)));
    this->slices.set(union_ex(tmp), stInternal);
}

void LayerRegion::export_region_slices_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (Surfaces::const_iterator surface = this->slices.surfaces.begin(); surface != this->slices.surfaces.end(); ++surface)
        svg.draw(surface->expolygon, surface_type_to_color_name(surface->surface_type), transparency);
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        svg.draw(surface->expolygon.lines(), surface_type_to_color_name(surface->surface_type));
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_slices_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_slices_to_svg(debug_out_path("LayerRegion-slices-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::export_region_fill_surfaces_to_svg(const char *path) const
{
    BoundingBox bbox;
    for (Surfaces::const_iterator surface = this->fill_surfaces.surfaces.begin(); surface != this->fill_surfaces.surfaces.end(); ++surface)
        bbox.merge(get_extents(surface->expolygon));
    Point legend_size = export_surface_type_legend_to_svg_box_size();
    Point legend_pos(bbox.min(0), bbox.max(1));
    bbox.merge(Point(std::max(bbox.min(0) + legend_size(0), bbox.max(0)), bbox.max(1) + legend_size(1)));

    SVG svg(path, bbox);
    const float transparency = 0.5f;
    for (const Surface &surface : this->fill_surfaces.surfaces) {
        svg.draw(surface.expolygon, surface_type_to_color_name(surface.surface_type), transparency);
        svg.draw_outline(surface.expolygon, "black", "blue", scale_(0.05)); 
    }
    export_surface_type_legend_to_svg(svg, legend_pos);
    svg.Close();
}

// Export to "out/LayerRegion-name-%d.svg" with an increasing index with every export.
void LayerRegion::export_region_fill_surfaces_to_svg_debug(const char *name) const
{
    static std::map<std::string, size_t> idx_map;
    size_t &idx = idx_map[name];
    this->export_region_fill_surfaces_to_svg(debug_out_path("LayerRegion-fill_surfaces-%s-%d.svg", name, idx ++).c_str());
}

void LayerRegion::simplify_entity_collection(ExtrusionEntityCollection* entity_collection)
{
    for (size_t i = 0; i < entity_collection->entities.size(); i++) {
        if (ExtrusionEntityCollection* collection = dynamic_cast<ExtrusionEntityCollection*>(entity_collection->entities[i]))
            this->simplify_entity_collection(collection);
        else if (ExtrusionPath* path = dynamic_cast<ExtrusionPath*>(entity_collection->entities[i]))
            this->simplify_path(path);
        else if (ExtrusionMultiPath* multipath = dynamic_cast<ExtrusionMultiPath*>(entity_collection->entities[i]))
            this->simplify_multi_path(multipath);
        else if (ExtrusionLoop* loop = dynamic_cast<ExtrusionLoop*>(entity_collection->entities[i]))
            this->simplify_loop(loop);
        else
            throw Slic3r::InvalidArgument("Invalid extrusion entity supplied to simplify_entity_collection()");
    }
}

void LayerRegion::simplify_path(ExtrusionPath* path)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    if (enable_arc_fitting &&
        !spiral_mode) {
        if (path->role() == erInternalInfill)
            path->simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
        else
            path->simplify_by_fitting_arc(scaled_resolution);
    } else {
        path->simplify(scaled_resolution);
    }
}

void LayerRegion::simplify_multi_path(ExtrusionMultiPath* multipath)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < multipath->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (multipath->paths[i].role() == erInternalInfill)
                multipath->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                multipath->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            multipath->paths[i].simplify(scaled_resolution);
        }
    }
}

void LayerRegion::simplify_loop(ExtrusionLoop* loop)
{
    const auto print_config = this->layer()->object()->print()->config();
    const bool spiral_mode = print_config.spiral_mode;
    const bool enable_arc_fitting = print_config.enable_arc_fitting;
    const auto scaled_resolution = scaled<double>(print_config.resolution.value);

    for (size_t i = 0; i < loop->paths.size(); ++i) {
        if (enable_arc_fitting &&
            !spiral_mode) {
            if (loop->paths[i].role() == erInternalInfill)
                loop->paths[i].simplify_by_fitting_arc(SCALED_SPARSE_INFILL_RESOLUTION);
            else
                loop->paths[i].simplify_by_fitting_arc(scaled_resolution);
        } else {
            loop->paths[i].simplify(scaled_resolution);
        }
    }
}

}
 
