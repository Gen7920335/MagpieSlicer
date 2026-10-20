#include "Config.hpp"
#include "Exception.hpp"
#include "Flow.hpp"
#include "Print.hpp"
#include <cstddef>
#include "PrintConfig.hpp"
#include "libslic3r.h"
#include <cmath>
#include <vector>
#include <algorithm>

namespace Slic3r {

WidthFeature PrintRegion::width_feature(FlowRole role)
{
    switch (role) {
    case frExternalPerimeter: return WidthFeature::OuterWall;
    case frPerimeter:         return WidthFeature::InnerWall;
    case frInfill:            return WidthFeature::SparseInfill;
    case frSolidInfill:       return WidthFeature::InternalSolidInfill;
    case frTopSolidInfill:    return WidthFeature::TopSurface;
    default:                  throw Slic3r::InvalidArgument("Unknown role");
    }
}

// 1-based extruder identifier for this region and role.
unsigned int PrintRegion::extruder(FlowRole role) const
{
    // MAGPIE: the filament setting comes from the routing table.
    return unsigned((m_config.*width_route(width_feature(role)).region_filament).value);
}

Flow PrintRegion::flow(const PrintObject &object, FlowRole role, double layer_height, bool first_layer) const
{
    return this->flow(object, width_feature(role), layer_height, first_layer);
}

size_t PrintRegion::printing_nozzle(const PrintObject &object, WidthFeature feature) const
{
    return feature_printing_nozzle(feature, { &object.print()->config(), &object.config(), &m_config }, object.print()->is_BBL_printer());
}

Flow PrintRegion::flow(const PrintObject &object, WidthFeature feature, double layer_height, bool first_layer) const
{
    // MAGPIE: the width setting (might be an absolute value, a percent value, or zero for auto) and the nozzle it
    // is resolved on both come from the routing table, the per-nozzle line width fields included.
    const WidthConfigs configs { &object.print()->config(), &object.config(), &m_config };
    return feature_flow(feature, configs, this->printing_nozzle(object, feature), float(layer_height), first_layer);
}

coordf_t PrintRegion::nozzle_dmr_avg(const PrintConfig &print_config) const
{
    return (print_config.nozzle_diameter.get_at(m_config.outer_wall_filament_id.value    - 1) +
            print_config.nozzle_diameter.get_at(m_config.inner_wall_filament_id.value    - 1) +
            print_config.nozzle_diameter.get_at(m_config.sparse_infill_filament_id.value       - 1) +
            print_config.nozzle_diameter.get_at(m_config.internal_solid_filament_id.value - 1) +
            print_config.nozzle_diameter.get_at(m_config.top_surface_filament_id.value    - 1) +
            print_config.nozzle_diameter.get_at(m_config.bottom_surface_filament_id.value - 1)) / 6.;
}

coordf_t PrintRegion::bridging_height_avg(const PrintConfig &print_config) const
{
    return this->nozzle_dmr_avg(print_config) * sqrt(m_config.bridge_flow.value);
}

void PrintRegion::collect_object_printing_extruders(const PrintConfig &print_config, const PrintRegionConfig &region_config, const bool has_brim, std::vector<unsigned int> &object_extruders)
{
    // These checks reflect the same logic used in the GUI for enabling/disabling extruder selection fields.
    // BBS
    auto num_extruders = (int)print_config.filament_diameter.size();
    auto emplace_extruder = [num_extruders, &object_extruders](int extruder_id) {
    	int i = std::max(0, extruder_id - 1);
        object_extruders.emplace_back((i >= num_extruders) ? 0 : i);
    };
    if (region_config.wall_loops.value > 0 || has_brim) {
    	emplace_extruder(region_config.outer_wall_filament_id);
                if (region_config.wall_loops.value > 1)
			emplace_extruder(region_config.inner_wall_filament_id);
    }
    if (region_config.sparse_infill_density.value > 0)
    	emplace_extruder(region_config.sparse_infill_filament_id);
    if (region_config.sparse_infill_density.value > 0 || region_config.top_shell_layers.value > 0 || region_config.bottom_shell_layers.value > 0)
    	emplace_extruder(region_config.internal_solid_filament_id);
    if (region_config.top_shell_layers.value > 0)
    	emplace_extruder(region_config.top_surface_filament_id);
    if (region_config.bottom_shell_layers.value > 0)
    	emplace_extruder(region_config.bottom_surface_filament_id);
}

void PrintRegion::collect_object_printing_extruders(const Print &print, std::vector<unsigned int> &object_extruders) const
{
    // PrintRegion, if used by some PrintObject, shall have all the extruders set to an existing printer extruder.
    // If not, then there must be something wrong with the Print::apply() function.
#ifndef NDEBUG
    // BBS
    auto num_extruders = int(print.config().filament_diameter.size());
    assert(this->config().outer_wall_filament_id    <= num_extruders);
    assert(this->config().inner_wall_filament_id    <= num_extruders);
    assert(this->config().sparse_infill_filament_id       <= num_extruders);
    assert(this->config().internal_solid_filament_id <= num_extruders);
    assert(this->config().top_surface_filament_id <= num_extruders);
    assert(this->config().bottom_surface_filament_id <= num_extruders);
#endif
    collect_object_printing_extruders(print.config(), this->config(), print.has_brim(), object_extruders);
}

}
