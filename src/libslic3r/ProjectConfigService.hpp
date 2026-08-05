#ifndef slic3r_ProjectConfigService_hpp_
#define slic3r_ProjectConfigService_hpp_

#include "Config.hpp"

#include <cstddef>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class Model;

inline constexpr double TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION = -1.0;

struct ProjectConfigNormalizationContext
{
    // Zero lets plate-indexed settings infer their size from legacy data.
    size_t plate_count    = 0;
    size_t filament_count = 1;
    size_t toolhead_count = 1;
};

ProjectConfigNormalizationContext infer_project_config_normalization_context(
    const DynamicConfig &config, size_t plate_count = 0);

// Single project-load migration boundary. Returns the keys that were repaired
// so callers can report compatibility changes without duplicating policy.
std::vector<std::string> normalize_loaded_project_config(
    DynamicConfig &config, const ProjectConfigNormalizationContext &context = {});

// Produces a serialization-safe snapshot without mutating the live project or
// the full configuration assembled by the preset system.
DynamicPrintConfig normalized_project_config_for_save(
    const DynamicPrintConfig &config, const ProjectConfigNormalizationContext &context);

// Plate overrides are optional: an empty vector means inherit the global map
// and must remain empty. Returns true only when a non-empty map was repaired.
bool normalize_optional_filament_map(
    std::vector<int> &filament_map, size_t filament_count, size_t toolhead_count);

std::vector<std::string> normalize_project_filament_arrays(
    DynamicConfig &config, size_t filament_count, size_t toolhead_count);

std::vector<std::string> normalize_filament_assignment_options(
    DynamicConfig &config, size_t filament_count);

size_t normalize_model_filament_assignments(Model &model, size_t filament_count);

// Keeps Magpie's plate-indexed project options valid at project boundaries.
// A plate_count of zero infers the count from the existing vectors.
std::vector<std::string> normalize_temperature_drop_tower_positions(
    DynamicConfig &config, size_t plate_count = 0);

// Repairs legacy project filament-to-tool mappings without changing valid
// assignments. Both counts are explicit because project_config does not own
// printer nozzle or filament preset data.
std::vector<std::string> normalize_project_filament_map(
    DynamicConfig &config, size_t filament_count, size_t toolhead_count);

void erase_temperature_drop_tower_plate(
    DynamicConfig &config, size_t plate_index, size_t plate_count_before_erase);

double temperature_drop_tower_position_at(
    const ConfigOptionFloats *positions, size_t plate_index,
    double fallback = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION);

} // namespace Slic3r

#endif
