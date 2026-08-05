#include "HotendConfigService.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

namespace {

FloatOrPercent normalized_width(const DynamicPrintConfig &config, const char *key, size_t index, double nozzle_diameter)
{
    FloatOrPercent value = default_toolhead_line_width_for_nozzle(key, nozzle_diameter);
    if (const auto *widths = config.option<ConfigOptionFloatsOrPercents>(key);
        widths != nullptr && !widths->values.empty()) {
        value = widths->values[std::min(index, widths->values.size() - 1)];
        if (value.percent)
            value = FloatOrPercent(std::round(value.get_abs_value(nozzle_diameter) * 1000.) / 1000., false);
        if (value.value <= 0.)
            value = default_toolhead_line_width_for_nozzle(key, nozzle_diameter);
    }
    return value;
}

void set_width_at(DynamicPrintConfig &config, const DynamicPrintConfig &source, const char *key, size_t index,
                  const FloatOrPercent &value)
{
    std::vector<FloatOrPercent> values;
    if (const auto *widths = source.option<ConfigOptionFloatsOrPercents>(key))
        values = widths->values;
    if (values.size() <= index)
        values.resize(index + 1, default_toolhead_line_width_for_nozzle(key, HotendConfigService::nozzle_diameter(source, index)));
    values[index] = value;
    config.set_key_value(key, new ConfigOptionFloatsOrPercents(values));
}

} // namespace

const std::array<const char *, 9> &hotend_preset_width_keys()
{
    static constexpr std::array<const char *, 9> keys = {
        "toolhead_line_width",
        "toolhead_initial_layer_line_width",
        "toolhead_outer_wall_line_width",
        "toolhead_inner_wall_line_width",
        "toolhead_top_surface_line_width",
        "toolhead_sparse_infill_line_width",
        "toolhead_internal_solid_infill_line_width",
        "toolhead_support_line_width",
        "toolhead_bridge_line_width"
    };
    return keys;
}

void validate_hotend_preset_config(const DynamicPrintConfig &config)
{
    const auto *nozzles = config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzles == nullptr || nozzles->values.size() != 1 || !std::isfinite(nozzles->values.front()) || nozzles->values.front() <= 0.)
        throw std::runtime_error("The hotend preset has an invalid nozzle diameter.");

    for (const char *key : hotend_preset_width_keys()) {
        const auto *widths = config.option<ConfigOptionFloatsOrPercents>(key);
        if (widths == nullptr || widths->values.size() != 1 || widths->values.front().percent ||
            !std::isfinite(widths->values.front().value) || widths->values.front().value <= 0.)
            throw std::runtime_error(std::string("The hotend preset has an invalid value for ") + key + '.');
    }
}

size_t HotendConfigService::toolhead_count(const DynamicPrintConfig &printer_config)
{
    const auto *nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    return nozzles == nullptr ? 1 : std::max<size_t>(1, nozzles->values.size());
}

size_t HotendConfigService::clamp_toolhead_index(const DynamicPrintConfig &printer_config, size_t index)
{
    return std::min(index, toolhead_count(printer_config) - 1);
}

double HotendConfigService::nozzle_diameter(const DynamicPrintConfig &printer_config, size_t index)
{
    const auto *nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzles == nullptr || nozzles->values.empty())
        return 0.4;
    return nozzles->values[std::min(index, nozzles->values.size() - 1)];
}

void HotendConfigService::normalize_printer_config(DynamicPrintConfig &printer_config)
{
    auto *nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    if (nozzles == nullptr)
        return;
    if (nozzles->values.empty()) {
        printer_config.set_key_value("nozzle_diameter", new ConfigOptionFloats({0.4}));
        nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    }

    const size_t count = std::max<size_t>(1, nozzles->values.size());
    for (const char *key : hotend_preset_width_keys()) {
        if (printer_config.option<ConfigOptionFloatsOrPercents>(key) == nullptr)
            printer_config.set_key_value(key, new ConfigOptionFloatsOrPercents(
                std::vector<FloatOrPercent>(count, FloatOrPercent(0., false))));
    }

    printer_config.set_num_extruders(unsigned(count));
    nozzles = printer_config.option<ConfigOptionFloats>("nozzle_diameter");
    for (const char *key : hotend_preset_width_keys()) {
        auto *widths = printer_config.option<ConfigOptionFloatsOrPercents>(key, true);
        widths->values.resize(count, FloatOrPercent(0., false));
        for (size_t index = 0; index < count; ++index) {
            FloatOrPercent &width = widths->values[index];
            const double diameter = nozzles->values[std::min(index, nozzles->values.size() - 1)];
            if (width.percent)
                width = FloatOrPercent(std::round(width.get_abs_value(diameter) * 1000.) / 1000., false);
            if (!std::isfinite(width.value) || width.value <= 0.)
                width = default_toolhead_line_width_for_nozzle(key, diameter);
        }
    }
}

DynamicPrintConfig HotendConfigService::extract(const DynamicPrintConfig &printer_config, size_t index)
{
    index = clamp_toolhead_index(printer_config, index);
    DynamicPrintConfig hotend_config;
    const double diameter = nozzle_diameter(printer_config, index);
    hotend_config.set_key_value("nozzle_diameter", new ConfigOptionFloats({diameter}));
    for (const char *key : hotend_preset_width_keys())
        hotend_config.set_key_value(key, new ConfigOptionFloatsOrPercents({normalized_width(printer_config, key, index, diameter)}));
    return hotend_config;
}

DynamicPrintConfig HotendConfigService::normalization_patch(const DynamicPrintConfig &printer_config, size_t index)
{
    index = clamp_toolhead_index(printer_config, index);
    DynamicPrintConfig patch;
    const double diameter = nozzle_diameter(printer_config, index);
    for (const char *key : hotend_preset_width_keys()) {
        std::vector<FloatOrPercent> values;
        if (const auto *widths = printer_config.option<ConfigOptionFloatsOrPercents>(key))
            values = widths->values;
        if (values.size() <= index)
            values.resize(index + 1, FloatOrPercent(0., false));
        values[index] = normalized_width(printer_config, key, index, diameter);
        patch.set_key_value(key, new ConfigOptionFloatsOrPercents(values));
    }
    return patch;
}

void HotendConfigService::apply_nozzle_diameter(DynamicPrintConfig &printer_config, size_t index, double diameter)
{
    if (index >= toolhead_count(printer_config))
        throw std::out_of_range("Hotend index is outside the configured toolhead range.");
    if (!std::isfinite(diameter) || diameter <= 0.)
        throw std::invalid_argument("Hotend nozzle diameter must be a positive finite value.");
    set_toolhead_nozzle_diameter(printer_config, index, diameter);
}

void HotendConfigService::apply_width(DynamicPrintConfig &printer_config, size_t index, const char *key,
                                      const FloatOrPercent &width)
{
    if (index >= toolhead_count(printer_config))
        throw std::out_of_range("Hotend index is outside the configured toolhead range.");
    const bool known_key = key != nullptr && std::any_of(hotend_preset_width_keys().begin(), hotend_preset_width_keys().end(),
        [key](const char *candidate) { return std::strcmp(candidate, key) == 0; });
    if (!known_key)
        throw std::invalid_argument("Unknown hotend line-width key.");
    if (width.percent || !std::isfinite(width.value) || width.value <= 0.)
        throw std::invalid_argument("Hotend line width must be a positive millimeter value.");

    const DynamicPrintConfig original = printer_config;
    set_width_at(printer_config, original, key, index, width);
}

void HotendConfigService::apply(DynamicPrintConfig &printer_config, size_t index, const DynamicPrintConfig &hotend_config)
{
    if (index >= toolhead_count(printer_config))
        throw std::out_of_range("Hotend index is outside the configured toolhead range.");
    validate_hotend_preset_config(hotend_config);

    const DynamicPrintConfig original = printer_config;
    const auto *nozzles = hotend_config.option<ConfigOptionFloats>("nozzle_diameter");
    apply_nozzle_diameter(printer_config, index, nozzles->values.front());
    for (const char *key : hotend_preset_width_keys()) {
        const auto *widths = hotend_config.option<ConfigOptionFloatsOrPercents>(key);
        set_width_at(printer_config, original, key, index, widths->values.front());
    }
}

} // namespace Slic3r
