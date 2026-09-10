#include "ProjectConfigService.hpp"
#include "Model.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string_view>

namespace Slic3r {

namespace {

constexpr std::array<const char *, 2> temperature_drop_tower_position_keys {
    "support_interface_temperature_drop_tower_x",
    "support_interface_temperature_drop_tower_y"
};

size_t inferred_plate_count(const DynamicConfig &config)
{
    size_t count = 1;
    for (const char *key : temperature_drop_tower_position_keys) {
        if (const auto *positions = config.option<ConfigOptionFloats>(key))
            count = std::max(count, positions->values.size());
    }
    return count;
}

void append_unique(std::vector<std::string> &destination, const std::vector<std::string> &source)
{
    for (const std::string &key : source)
        if (std::find(destination.begin(), destination.end(), key) == destination.end())
            destination.emplace_back(key);
}

bool normalize_filament_map_values(
    std::vector<int> &values, size_t filament_count, size_t toolhead_count)
{
    filament_count = std::max<size_t>(1, filament_count);
    toolhead_count = std::max<size_t>(1, toolhead_count);
    auto default_tool = [toolhead_count](size_t filament_index) {
        return int(std::min(filament_index + 1, toolhead_count));
    };

    bool changed = values.size() != filament_count;
    values.resize(filament_count, 0);
    for (size_t index = 0; index < filament_count; ++index) {
        int &tool = values[index];
        if (tool < 1 || size_t(tool) > toolhead_count) {
            tool = default_tool(index);
            changed = true;
        }
    }
    return changed;
}

template<class Option, class Value>
bool resize_project_vector(DynamicConfig &config, const char *key, size_t count, const Value &fill)
{
    auto *option = config.option<Option>(key);
    if (option == nullptr || option->values.size() == count)
        return false;
    option->values.resize(count, fill);
    return true;
}

void initialize_flush_matrix(
    std::vector<double> &matrix, const std::vector<double> &flush_vector,
    size_t filament_count, size_t toolhead_count)
{
    const size_t matrix_size = filament_count * filament_count;
    matrix.assign(matrix_size * toolhead_count, 0.0);
    for (size_t tool = 0; tool < toolhead_count; ++tool)
        for (size_t from = 0; from < filament_count; ++from)
            for (size_t to = 0; to < filament_count; ++to)
                if (from != to)
                    matrix[tool * matrix_size + from * filament_count + to] =
                        flush_vector[2 * from] + flush_vector[2 * to + 1];
}

constexpr std::array<const char *, 12> assignment_keys {
        "extruder",
        "wall_filament", "sparse_infill_filament", "solid_infill_filament",
        "support_filament", "support_interface_filament",
        "outer_wall_filament_id", "inner_wall_filament_id",
        "sparse_infill_filament_id", "internal_solid_filament_id",
        "top_surface_filament_id", "bottom_surface_filament_id"
};

template<class Config>
std::vector<std::string> normalize_filament_assignment_options_impl(
    Config &config, size_t filament_count)
{
    filament_count = std::max<size_t>(1, filament_count);
    std::vector<std::string> repaired;
    for (const char *key : assignment_keys) {
        const auto *option = dynamic_cast<const ConfigOptionInt *>(config.option(key));
        if (option == nullptr || (option->value >= 0 && size_t(option->value) <= filament_count))
            continue;
        if (std::string_view(key) == "extruder")
            config.erase(key);
        else
            config.set_key_value(key, new ConfigOptionInt(0));
        repaired.emplace_back(key);
    }
    return repaired;
}

} // namespace

ProjectConfigNormalizationContext infer_project_config_normalization_context(
    const DynamicConfig &config, size_t plate_count)
{
    ProjectConfigNormalizationContext context;
    context.plate_count = plate_count;

    if (const auto *filament_ids = config.option<ConfigOptionStrings>("filament_settings_id");
        filament_ids != nullptr && !filament_ids->values.empty())
        context.filament_count = filament_ids->values.size();
    else if (const auto *filament_colours = config.option<ConfigOptionStrings>("filament_colour");
             filament_colours != nullptr && !filament_colours->values.empty())
        context.filament_count = filament_colours->values.size();
    else if (const auto *filament_map = config.option<ConfigOptionInts>("filament_map");
             filament_map != nullptr && !filament_map->values.empty())
        context.filament_count = filament_map->values.size();

    if (const auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
        nozzles != nullptr && !nozzles->values.empty())
        context.toolhead_count = nozzles->values.size();
    return context;
}

std::vector<std::string> normalize_loaded_project_config(
    DynamicConfig &config, const ProjectConfigNormalizationContext &context)
{
    std::vector<std::string> repaired;
    append_unique(repaired, normalize_temperature_drop_tower_positions(config, context.plate_count));
    append_unique(repaired, normalize_project_filament_map(
        config, context.filament_count, context.toolhead_count));
    append_unique(repaired, normalize_project_filament_arrays(
        config, context.filament_count, context.toolhead_count));
    append_unique(repaired, normalize_project_tool_enums(config, context.toolhead_count));
    return repaired;
}

template<class Config>
size_t remap_filament_assignments_after_delete_impl(
    Config &config, size_t filament_index, int replacement_index, bool overrides, bool object_config = false)
{
    size_t changed = 0;
    for (const char *key : assignment_keys) {
        const auto *option = dynamic_cast<const ConfigOptionInt *>(config.option(key));
        if (option == nullptr || option->value <= 0 || option->value < int(filament_index) + 1)
            continue;
        if (option->value == int(filament_index) + 1) {
            if (replacement_index >= 0)
                config.set_key_value(key, new ConfigOptionInt(replacement_index + 1));
            else if (object_config && std::string_view(key) == "extruder")
                config.set_key_value(key, new ConfigOptionInt(1));
            else if (overrides)
                config.erase(key);
            else
                config.set_key_value(key, new ConfigOptionInt(0));
        } else {
            config.set_key_value(key, new ConfigOptionInt(option->value - 1));
        }
        ++changed;
    }
    return changed;
}

std::vector<std::string> normalize_project_tool_enums(DynamicConfig &config, size_t toolhead_count)
{
    std::vector<std::string> repaired;
    toolhead_count = std::max<size_t>(1, toolhead_count);
    for (const char *key : {"extruder_type", "nozzle_volume_type"}) {
        auto *option = config.option<ConfigOptionEnumsGeneric>(key);
        if (option == nullptr)
            continue; // A project-only patch does not own missing printer fields.
        const auto *defaults = dynamic_cast<const ConfigOptionEnumsGeneric *>(print_config_def.get(key)->default_value.get());
        const int fallback = defaults->values.front();
        const int maximum = std::string_view(key) == "extruder_type" ? int(etMaxExtruderType) : int(nvtMaxNozzleVolumeType);
        const auto previous = option->values;
        option->values.resize(toolhead_count, fallback);
        for (int &value : option->values)
            if (value < 0 || value > maximum)
                value = fallback;
        if (option->values != previous)
            repaired.emplace_back(key);
    }
    return repaired;
}

DynamicPrintConfig normalized_project_config_for_save(
    const DynamicPrintConfig &config, const ProjectConfigNormalizationContext &context)
{
    DynamicPrintConfig normalized = config;
    normalize_loaded_project_config(normalized, context);
    return normalized;
}

std::vector<std::string> normalize_temperature_drop_tower_positions(
    DynamicConfig &config, size_t plate_count)
{
    plate_count = plate_count == 0 ? inferred_plate_count(config) : std::max<size_t>(1, plate_count);
    std::vector<std::string> repaired;

    for (const char *key : temperature_drop_tower_position_keys) {
        auto *positions = config.option<ConfigOptionFloats>(key);
        if (positions == nullptr) {
            config.set_key_value(key, new ConfigOptionFloats(
                std::vector<double>(plate_count, TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION)));
            repaired.emplace_back(key);
            continue;
        }

        bool changed = positions->values.size() != plate_count;
        positions->values.resize(plate_count, TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION);
        for (double &position : positions->values) {
            if (!std::isfinite(position)) {
                position = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
                changed = true;
            }
        }
        if (changed)
            repaired.emplace_back(key);
    }

    return repaired;
}

std::vector<std::string> normalize_project_filament_map(
    DynamicConfig &config, size_t filament_count, size_t toolhead_count)
{
    filament_count = std::max<size_t>(1, filament_count);
    toolhead_count = std::max<size_t>(1, toolhead_count);

    auto *filament_map = config.option<ConfigOptionInts>("filament_map");
    if (filament_map == nullptr) {
        std::vector<int> values;
        normalize_filament_map_values(values, filament_count, toolhead_count);
        config.set_key_value("filament_map", new ConfigOptionInts(std::move(values)));
        return {"filament_map"};
    }

    const bool changed = normalize_filament_map_values(
        filament_map->values, filament_count, toolhead_count);
    return changed ? std::vector<std::string>{"filament_map"} : std::vector<std::string>{};
}

bool normalize_optional_filament_map(
    std::vector<int> &filament_map, size_t filament_count, size_t toolhead_count)
{
    if (filament_map.empty())
        return false;
    return normalize_filament_map_values(filament_map, filament_count, toolhead_count);
}

std::vector<std::string> normalize_project_filament_arrays(
    DynamicConfig &config, size_t filament_count, size_t toolhead_count)
{
    filament_count = std::max<size_t>(1, filament_count);
    toolhead_count = std::max<size_t>(1, toolhead_count);
    std::vector<std::string> repaired;

    auto record = [&repaired](bool changed, const char *key) {
        if (changed)
            repaired.emplace_back(key);
    };

    record(resize_project_vector<ConfigOptionStrings>(
        config, "filament_colour", filament_count, std::string("#26A69A")), "filament_colour");
    record(resize_project_vector<ConfigOptionStrings>(
        config, "filament_colour_type", filament_count, std::string("1")), "filament_colour_type");

    if (auto *multi_colours = config.option<ConfigOptionStrings>("filament_multi_colour")) {
        const size_t old_size = multi_colours->values.size();
        if (old_size != filament_count) {
            multi_colours->values.resize(filament_count);
            const auto *colours = config.option<ConfigOptionStrings>("filament_colour");
            for (size_t index = old_size; index < filament_count; ++index)
                multi_colours->values[index] = colours != nullptr && index < colours->values.size()
                    ? colours->values[index] : "#26A69A";
            repaired.emplace_back("filament_multi_colour");
        }
    }

    auto *flush_vector = config.option<ConfigOptionFloats>("flush_volumes_vector");
    if (flush_vector != nullptr && flush_vector->values.size() != 2 * filament_count) {
        const double unload = flush_vector->values.empty() ? 140.0 : flush_vector->values.front();
        const double load = flush_vector->values.size() < 2 ? 140.0 : flush_vector->values[1];
        const size_t old_size = flush_vector->values.size();
        flush_vector->values.resize(2 * filament_count);
        for (size_t index = old_size; index < flush_vector->values.size(); ++index)
            flush_vector->values[index] = index % 2 == 0 ? unload : load;
        repaired.emplace_back("flush_volumes_vector");
    }

    auto *multipliers = config.option<ConfigOptionFloats>("flush_multiplier");
    const size_t old_toolhead_count = multipliers == nullptr || multipliers->values.empty()
        ? 1 : multipliers->values.size();
    record(resize_project_vector<ConfigOptionFloats>(
        config, "flush_multiplier", toolhead_count, 1.0), "flush_multiplier");

    auto *matrix = config.option<ConfigOptionFloats>("flush_volumes_matrix");
    if (matrix != nullptr) {
        const std::vector<double> old_matrix = matrix->values;
        const size_t target_matrix_size = filament_count * filament_count;
        if (old_matrix.size() != target_matrix_size * toolhead_count) {
            std::vector<double> vector_values = flush_vector == nullptr
                ? std::vector<double>(2 * filament_count, 140.0) : flush_vector->values;
            if (vector_values.size() != 2 * filament_count)
                vector_values.resize(2 * filament_count, 140.0);
            std::vector<double> rebuilt;
            initialize_flush_matrix(rebuilt, vector_values, filament_count, toolhead_count);

            const size_t old_per_tool = old_toolhead_count == 0 ? 0 : old_matrix.size() / old_toolhead_count;
            const size_t old_filament_count = size_t(std::sqrt(double(old_per_tool)) + 0.5);
            const bool old_layout_valid = old_toolhead_count > 0 &&
                old_per_tool * old_toolhead_count == old_matrix.size() &&
                old_filament_count * old_filament_count == old_per_tool;
            if (old_layout_valid) {
                const size_t copy_tools = std::min(old_toolhead_count, toolhead_count);
                const size_t copy_filaments = std::min(old_filament_count, filament_count);
                for (size_t tool = 0; tool < copy_tools; ++tool)
                    for (size_t from = 0; from < copy_filaments; ++from)
                        for (size_t to = 0; to < copy_filaments; ++to)
                            rebuilt[tool * target_matrix_size + from * filament_count + to] =
                                old_matrix[tool * old_per_tool + from * old_filament_count + to];
            }
            matrix->values = std::move(rebuilt);
            repaired.emplace_back("flush_volumes_matrix");
        }
    }
    return repaired;
}

std::vector<std::string> normalize_filament_assignment_options(
    DynamicConfig &config, size_t filament_count)
{
    return normalize_filament_assignment_options_impl(config, filament_count);
}

size_t normalize_model_filament_assignments(Model &model, size_t filament_count)
{
    filament_count = std::max<size_t>(1, filament_count);
    size_t repaired = 0;
    for (ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        repaired += normalize_filament_assignment_options_impl(object->config, filament_count).size();
        for (ModelVolume *volume : object->volumes) {
            if (volume == nullptr)
                continue;
            repaired += normalize_filament_assignment_options_impl(volume->config, filament_count).size();
            volume->update_extruder_count(filament_count);
        }
        if (object->volumes.size() == 1 && object->volumes.front() != nullptr &&
            object->volumes.front()->config.has("extruder")) {
            object->volumes.front()->config.erase("extruder");
            ++repaired;
        }
    }
    return repaired;
}

void remap_model_tool_changes_after_filament_delete(
    Model &model, size_t filament_index, int replacement_index)
{
    for (auto &plate : model.plates_custom_gcodes) {
        auto &gcodes = plate.second.gcodes;
        auto destination = gcodes.begin();
        for (auto source = gcodes.begin(); source != gcodes.end(); ++source) {
            int extruder = source->extruder;
            if (source->type == CustomGCode::Type::ToolChange) {
                if (extruder == int(filament_index) + 1) {
                    if (replacement_index < 0)
                        continue;
                    // Sidebar already converted this index to the post-deletion domain.
                    extruder = replacement_index + 1;
                } else if (extruder > int(filament_index) + 1) {
                    --extruder;
                }
            }
            if (destination != source)
                *destination = std::move(*source);
            destination->extruder = extruder;
            ++destination;
        }
        gcodes.erase(destination, gcodes.end());
    }
}

size_t remap_filament_assignments_after_delete(
    DynamicConfig &config, size_t filament_index, int replacement_index)
{
    return remap_filament_assignments_after_delete_impl(config, filament_index, replacement_index, false);
}

size_t remap_model_filament_assignments_after_delete(
    Model &model, size_t filament_count_after, size_t filament_index, int replacement_index)
{
    size_t changed = 0;
    for (ModelObject *object : model.objects) {
        changed += remap_filament_assignments_after_delete_impl(
            object->config, filament_index, replacement_index, true, true);
        for (auto &range : object->layer_config_ranges)
            changed += remap_filament_assignments_after_delete_impl(
                range.second, filament_index, replacement_index, true);
        for (ModelVolume *volume : object->volumes) {
            changed += remap_filament_assignments_after_delete_impl(
                volume->config, filament_index, replacement_index, true);
            // Remap first: cleanup would otherwise erase a surviving last-slot assignment.
            volume->update_extruder_count_when_delete_filament(
                filament_count_after, filament_index + 1, replacement_index + 1);
        }
    }
    return changed;
}

void erase_temperature_drop_tower_plate(
    DynamicConfig &config, size_t plate_index, size_t plate_count_before_erase)
{
    const size_t old_count = std::max<size_t>(1, plate_count_before_erase);
    normalize_temperature_drop_tower_positions(config, old_count);

    if (old_count <= 1)
        return;

    for (const char *key : temperature_drop_tower_position_keys) {
        auto *positions = config.option<ConfigOptionFloats>(key);
        if (positions != nullptr && plate_index < positions->values.size())
            positions->values.erase(positions->values.begin() + plate_index);
    }
}

void transfer_temperature_drop_tower_plate(
    DynamicConfig &config, size_t source_plate_index, size_t destination_plate_index,
    size_t plate_count, bool reset_source)
{
    if (source_plate_index == destination_plate_index ||
        source_plate_index >= plate_count || destination_plate_index >= plate_count)
        return;

    normalize_temperature_drop_tower_positions(config, plate_count);
    auto *tower_x = config.option<ConfigOptionFloats>(temperature_drop_tower_position_keys[0]);
    auto *tower_y = config.option<ConfigOptionFloats>(temperature_drop_tower_position_keys[1]);
    if (tower_x == nullptr || tower_y == nullptr)
        return;

    const double source_x = temperature_drop_tower_position_at(tower_x, source_plate_index);
    const double source_y = temperature_drop_tower_position_at(tower_y, source_plate_index);
    const double destination_x = temperature_drop_tower_position_at(tower_x, destination_plate_index);
    const double destination_y = temperature_drop_tower_position_at(tower_y, destination_plate_index);

    const bool source_is_manual = source_x >= 0.0 && source_y >= 0.0;
    const bool destination_is_automatic = destination_x < 0.0 || destination_y < 0.0;
    if (source_is_manual && destination_is_automatic) {
        tower_x->values[destination_plate_index] = source_x;
        tower_y->values[destination_plate_index] = source_y;
    }

    if (reset_source) {
        tower_x->values[source_plate_index] = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
        tower_y->values[source_plate_index] = TEMPERATURE_DROP_TOWER_AUTOMATIC_POSITION;
    }
}

void capture_project_tower_positions(const DynamicConfig &config, ModelWipeTower &tower)
{
    const auto *tower_x_opt = config.option<ConfigOptionFloats>("wipe_tower_x");
    const auto *tower_y_opt = config.option<ConfigOptionFloats>("wipe_tower_y");
    const size_t position_count = tower_x_opt != nullptr && tower_y_opt != nullptr ?
        std::min(tower_x_opt->values.size(), tower_y_opt->values.size()) : 0;
    tower.positions.clear();
    tower.positions.resize(position_count);
    for (size_t plate_idx = 0; plate_idx < position_count; ++plate_idx) {
        tower.positions[plate_idx] = Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx));
        tower.rotation = config.opt_float("wipe_tower_rotation_angle");
    }
    tower.temperature_drop_positions.clear();
    for (const char *key : temperature_drop_tower_position_keys)
        if (const ConfigOption *option = config.option(key))
            tower.temperature_drop_positions.set_key_value(key, option->clone());
}

bool restore_project_tower_positions(const ModelWipeTower &tower, DynamicConfig &config)
{
    auto *tower_x_opt = config.option<ConfigOptionFloats>("wipe_tower_x", true);
    auto *tower_y_opt = config.option<ConfigOptionFloats>("wipe_tower_y", true);
    bool need_update = false;
    if (tower_x_opt->values.size() != tower.positions.size()) {
        tower_x_opt->clear();
        ConfigOptionFloat default_tower_x(40.f);
        tower_x_opt->resize(tower.positions.size(), &default_tower_x);
        need_update = true;
    }
    if (tower_y_opt->values.size() != tower.positions.size()) {
        tower_y_opt->clear();
        ConfigOptionFloat default_tower_y(200.f);
        tower_y_opt->resize(tower.positions.size(), &default_tower_y);
        need_update = true;
    }
    for (size_t plate_idx = 0; plate_idx < tower.positions.size(); ++plate_idx) {
        if (Vec2d(tower_x_opt->get_at(plate_idx), tower_y_opt->get_at(plate_idx)) != tower.positions[plate_idx]) {
            ConfigOptionFloat tower_x_new(tower.positions[plate_idx].x());
            ConfigOptionFloat tower_y_new(tower.positions[plate_idx].y());
            tower_x_opt->set_at(&tower_x_new, plate_idx, 0);
            tower_y_opt->set_at(&tower_y_new, plate_idx, 0);
            need_update = true;
        }
    }
    for (const char *key : temperature_drop_tower_position_keys) {
        const ConfigOption *saved = tower.temperature_drop_positions.option(key);
        const ConfigOption *current = config.option(key);
        if (saved != nullptr) {
            if (current == nullptr || *current != *saved) {
                config.set_key_value(key, saved->clone());
                need_update = true;
            }
        } else if (current != nullptr) {
            config.erase(key);
            need_update = true;
        }
    }
    return need_update;
}

double temperature_drop_tower_position_at(
    const ConfigOptionFloats *positions, size_t plate_index, double fallback)
{
    if (positions == nullptr || plate_index >= positions->values.size())
        return fallback;
    const double position = positions->values[plate_index];
    return std::isfinite(position) ? position : fallback;
}

} // namespace Slic3r
