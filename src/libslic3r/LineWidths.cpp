#include "LineWidths.hpp"

#include "Exception.hpp"

#include <algorithm>
#include <cstring>

namespace Slic3r {

const std::array<WidthRoute, size_t(WidthFeature::Count)> &width_routes()
{
    // MAGPIE: the one place that says which settings give a feature its width and which filament setting picks the
    // nozzle printing it. The chains are Orca's (PrintRegion::flow, support_material_*_flow, Print::skirt_flow and
    // brim_flow before this table). A bottom surface is printed by bottom_surface_filament_id with the internal
    // solid infill width; skin and skeleton infill only come with the sparse infill pattern, so its filament.
    static const std::array<WidthRoute, size_t(WidthFeature::Count)> routes { {
        { WidthFeature::OuterWall, WidthNozzleSource::RegionFilament, "outer_wall_filament_id", &PrintRegionConfig::outer_wall_filament_id, nullptr, frExternalPerimeter, true,
          { "outer_wall_line_width", "line_width", nullptr } },
        { WidthFeature::InnerWall, WidthNozzleSource::RegionFilament, "inner_wall_filament_id", &PrintRegionConfig::inner_wall_filament_id, nullptr, frPerimeter, true,
          { "inner_wall_line_width", "line_width", nullptr } },
        { WidthFeature::SparseInfill, WidthNozzleSource::RegionFilament, "sparse_infill_filament_id", &PrintRegionConfig::sparse_infill_filament_id, nullptr, frInfill, true,
          { "sparse_infill_line_width", "line_width", nullptr } },
        { WidthFeature::InternalSolidInfill, WidthNozzleSource::RegionFilament, "internal_solid_filament_id", &PrintRegionConfig::internal_solid_filament_id, nullptr, frSolidInfill, true,
          { "internal_solid_infill_line_width", "line_width", nullptr } },
        { WidthFeature::TopSurface, WidthNozzleSource::RegionFilament, "top_surface_filament_id", &PrintRegionConfig::top_surface_filament_id, nullptr, frTopSolidInfill, true,
          { "top_surface_line_width", "line_width", nullptr } },
        { WidthFeature::BottomSurface, WidthNozzleSource::RegionFilament, "bottom_surface_filament_id", &PrintRegionConfig::bottom_surface_filament_id, nullptr, frSolidInfill, true,
          { "internal_solid_infill_line_width", "line_width", nullptr } },
        { WidthFeature::SkinInfill, WidthNozzleSource::RegionFilament, "sparse_infill_filament_id", &PrintRegionConfig::sparse_infill_filament_id, nullptr, frInfill, false,
          { "skin_infill_line_width", nullptr, nullptr } },
        { WidthFeature::SkeletonInfill, WidthNozzleSource::RegionFilament, "sparse_infill_filament_id", &PrintRegionConfig::sparse_infill_filament_id, nullptr, frInfill, false,
          { "skeleton_infill_line_width", nullptr, nullptr } },
        { WidthFeature::Support, WidthNozzleSource::ObjectFilament, "support_filament", nullptr, &PrintObjectConfig::support_filament, frSupportMaterial, true,
          { "support_line_width", "line_width", nullptr } },
        { WidthFeature::SupportInterface, WidthNozzleSource::ObjectFilament, "support_interface_filament", nullptr, &PrintObjectConfig::support_interface_filament, frSupportMaterialInterface, false,
          { "support_line_width", "line_width", nullptr } },
        { WidthFeature::Skirt, WidthNozzleSource::PrintingTool, nullptr, nullptr, nullptr, frPerimeter, false,
          { "initial_layer_line_width", "line_width", nullptr } },
        { WidthFeature::Brim, WidthNozzleSource::PrintingTool, nullptr, nullptr, nullptr, frPerimeter, false,
          { "initial_layer_line_width", "inner_wall_line_width", "line_width" } },
    } };
    return routes;
}

int printing_filament(const PrintConfig &print, int filament_id)
{
    if (filament_id != 0 || print.nozzle_diameter.values.size() < 2)
        return filament_id;
    // Nozzle diameters closer than this are one size, mm (absolute_line_width_reference_nozzle()).
    const auto [smallest, largest] = std::minmax_element(print.nozzle_diameter.values.begin(), print.nozzle_diameter.values.end());
    return *largest - *smallest > EPSILON ? 1 : 0;
}

int support_interface_printing_filament(const PrintConfig &print, int interface_filament, int support_filament)
{
    if (interface_filament != 0 || printing_filament(print, 0) == 0)
        return interface_filament;
    return printing_filament(print, support_filament);
}

size_t printing_nozzle_index(const PrintConfig &print, int filament_id, bool is_bbl_printer)
{
    filament_id  = printing_filament(print, filament_id);
    int extruder = filament_id;
    if (is_bbl_printer && print.nozzle_diameter.size() > 1 && filament_id >= 1 && size_t(filament_id - 1) < print.filament_map.size())
        extruder = print.filament_map.get_at(filament_id - 1);
    // nozzle_diameter.get_at(extruder - 1): 0 underflows, and get_at() falls back to the first nozzle.
    return extruder >= 1 && size_t(extruder - 1) < print.nozzle_diameter.size() ? size_t(extruder - 1) : 0;
}

size_t feature_printing_nozzle(WidthFeature feature, const WidthConfigs &configs, bool is_bbl_printer)
{
    const WidthRoute &route = width_route(feature);
    int filament_id = -1;
    if (route.region_filament != nullptr && configs.region != nullptr)
        filament_id = (configs.region->*route.region_filament).value;
    else if (route.object_filament != nullptr && configs.object != nullptr)
        filament_id = (configs.object->*route.object_filament).value;
    if (filament_id < 0 || configs.print == nullptr)
        throw Slic3r::InvalidArgument("feature_printing_nozzle(): the feature is printed by the printing tool, or a config is missing");
    if (feature == WidthFeature::SupportInterface && configs.object != nullptr)
        filament_id = support_interface_printing_filament(*configs.print, filament_id, configs.object->support_filament.value);
    return printing_nozzle_index(*configs.print, filament_id, is_bbl_printer);
}

ConfigOptionFloatOrPercent line_width_setting_on_nozzle(const ConfigOptionResolver &config, const char *width_key, size_t nozzle)
{
    const auto *width = config.option<ConfigOptionFloatOrPercent>(width_key);
    if (width == nullptr)
        throw Slic3r::InvalidArgument(std::string("line_width_setting_on_nozzle(): no ") + width_key);
    // One per-nozzle key per width key; looked up by name so the static configs need no extra table.
    static const std::array<std::pair<const char *, std::string>, 11> per_nozzle_keys = [] {
        std::array<std::pair<const char *, std::string>, 11> out;
        for (size_t i = 0; i < out.size(); ++i)
            out[i] = { per_nozzle_line_width_base_keys()[i], per_nozzle_line_width_key(per_nozzle_line_width_base_keys()[i]) };
        return out;
    }();
    for (const auto &[key, per_nozzle_key] : per_nozzle_keys)
        if (std::strcmp(key, width_key) == 0) {
            if (const auto *per_nozzle = config.option<ConfigOptionFloats>(per_nozzle_key);
                per_nozzle != nullptr && nozzle < per_nozzle->values.size() && per_nozzle->values[nozzle] > 0.)
                return ConfigOptionFloatOrPercent(per_nozzle->values[nozzle], false);
            break;
        }
    return *width;
}

// The config holding width_key: initial_layer_line_width is a print setting, line_width and support_line_width
// object settings, the rest region settings.
static const ConfigBase *width_holder(const WidthConfigs &configs, const char *width_key)
{
    if (std::strcmp(width_key, "initial_layer_line_width") == 0)
        return configs.print;
    if (std::strcmp(width_key, "line_width") == 0 || std::strcmp(width_key, "support_line_width") == 0)
        return configs.object;
    return configs.region;
}

ConfigOptionFloatOrPercent feature_line_width(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, bool first_layer)
{
    const WidthRoute &route = width_route(feature);
    ConfigOptionFloatOrPercent width(0., false);
    // A setting at or below 0 (0 is automatic, 0 % too) moves on to the next one.
    auto take = [&](const char *key) {
        const ConfigBase *holder = width_holder(configs, key);
        if (holder == nullptr)
            return false;
        width = line_width_setting_on_nozzle(*holder, key, nozzle);
        return width.value > 0.;
    };
    if (first_layer && route.first_layer_initial && take("initial_layer_line_width"))
        return width;
    for (const char *key : route.chain)
        if (key != nullptr && take(key))
            return width;
    return ConfigOptionFloatOrPercent(0., false);
}

Flow feature_flow(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, float height_mm, bool first_layer)
{
    return Flow::new_from_config_width(width_route(feature).auto_role, feature_line_width(feature, configs, nozzle, first_layer),
                                       float(configs.print->nozzle_diameter.get_at(nozzle)), height_mm);
}

double feature_line_width_mm(WidthFeature feature, const WidthConfigs &configs, size_t nozzle, bool first_layer)
{
    return Flow::config_width_mm(width_route(feature).auto_role, feature_line_width(feature, configs, nozzle, first_layer),
                                 float(configs.print->nozzle_diameter.get_at(nozzle)));
}

double bridge_line_width_mm(const PrintConfig &print, const PrintRegionConfig &region, size_t nozzle)
{
    // On the float diameter Flow keeps, as the bridge flows always resolved it.
    return line_width_setting_on_nozzle(region, "bridge_line_width", nozzle).get_abs_value(float(print.nozzle_diameter.get_at(nozzle)));
}

double per_nozzle_line_width_fallback_mm(const DynamicPrintConfig &config, const char *width_key, size_t nozzle)
{
    DynamicPrintConfig copy = config;
    // The cell itself is empty.
    auto *field = copy.option<ConfigOptionFloats>(per_nozzle_line_width_key(width_key), true);
    if (field->values.size() <= nozzle)
        field->values.resize(nozzle + 1, 0.);
    field->values[nozzle] = 0.;
    // Prepared as Print::apply prepares the print's own configs.
    PrintConfig       print;
    PrintObjectConfig object;
    PrintRegionConfig region;
    print.apply(copy, true);
    object.apply(copy, true);
    region.apply(copy, true);
    const LineWidthContext context = line_width_context(copy);
    prepare_line_widths(print, context);
    prepare_line_widths(object, context);
    prepare_line_widths(region, context);
    const WidthConfigs configs { &print, &object, &region };
    const std::string  key(width_key);
    if (key == "line_width")
        return Flow::config_width_mm(frExternalPerimeter, line_width_setting_on_nozzle(object, width_key, nozzle),
                                     float(print.nozzle_diameter.get_at(nozzle)));
    if (key == "initial_layer_line_width")
        return feature_line_width_mm(WidthFeature::OuterWall, configs, nozzle, true);
    if (key == "bridge_line_width") {
        const double bridge_mm = bridge_line_width_mm(print, region, nozzle);
        return bridge_mm > 0. ? bridge_mm : feature_line_width_mm(WidthFeature::InternalSolidInfill, configs, nozzle, false);
    }
    // The feature whose chain starts with the setting.
    for (const WidthRoute &route : width_routes())
        if (route.nozzle_source != WidthNozzleSource::PrintingTool && route.chain.front() != nullptr && key == route.chain.front())
            return feature_line_width_mm(route.feature, configs, nozzle, false);
    return 0.;
}

} // namespace Slic3r
