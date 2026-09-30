#ifndef slic3r_FilamentMapPolicy_hpp_
#define slic3r_FilamentMapPolicy_hpp_

#include "PrintConfig.hpp"

namespace Slic3r::FilamentMapPolicy {

inline std::vector<int> canonical_map(size_t filament_count)
{
    std::vector<int> map(filament_count);
    for (size_t i = 0; i < map.size(); ++i)
        map[i] = int(i + 1);
    return map;
}

// Direct-tool firmware and its custom macros use Tn as a physical tool ID.
// AMS/MMU logical-material selection is a different protocol; do not rewrite it.
inline bool uses_fixed_tools(const ConfigBase &config, bool is_bbl)
{
    const auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *semm = config.option<ConfigOptionBool>("single_extruder_multi_material");
    return !is_bbl && nozzles && nozzles->size() > 1 && !(semm && semm->value);
}

// Called after physical printer variants, before filament variants and geometry.
// Only Auto is canonicalized. Preserve Manual settings for an explanatory error.
inline void resolve_auto(DynamicPrintConfig &config, bool is_bbl)
{
    if (!uses_fixed_tools(config, is_bbl))
        return;
    const auto *mode = config.option<ConfigOptionEnum<FilamentMapMode>>("filament_map_mode");
    const auto *filaments = config.option<ConfigOptionFloats>("filament_diameter");
    const auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (!mode || mode->value >= fmmManual || !filaments || filaments->size() > nozzles->size())
        return; // Excess materials have no physical destination; validation rejects them.
    config.option<ConfigOptionInts>("filament_map", true)->values = canonical_map(filaments->size());
}

// Shared by normal validation and the export boundary. No state mutation, so a
// rejected export cannot modify a user assignment or remove an existing file.
inline const char *export_error(const ConfigBase &config, bool is_bbl)
{
    if (!uses_fixed_tools(config, is_bbl))
        return nullptr;
    const auto *filaments = config.option<ConfigOptionFloats>("filament_diameter");
    const auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    const auto *map = config.option<ConfigOptionInts>("filament_map");
    if (!filaments || filaments->size() > nozzles->size())
        return "This direct-tool printer cannot assign more filaments than physical toolheads. Remove extra filament slots before slicing.";
    bool identity = map && map->size() == filaments->size();
    for (size_t i = 0; identity && i < map->size(); ++i)
        identity = map->values[i] == int(i + 1);
    return identity ? nullptr :
        "Unsupported filament map for a direct-tool printer: filament N must use toolhead N. Select Auto mapping or restore matching filament/toolhead numbers; arbitrary remapping of toolchange and temperature macros is not supported.";
}

} // namespace Slic3r::FilamentMapPolicy
#endif
