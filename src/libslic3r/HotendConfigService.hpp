#ifndef slic3r_HotendConfigService_hpp_
#define slic3r_HotendConfigService_hpp_

#include "PrintConfig.hpp"

#include <array>
#include <cstddef>

namespace Slic3r {

const std::array<const char *, 9> &hotend_preset_width_keys();
void validate_hotend_preset_config(const DynamicPrintConfig &config);

class HotendConfigService
{
public:
    static size_t toolhead_count(const DynamicPrintConfig &printer_config);
    static size_t clamp_toolhead_index(const DynamicPrintConfig &printer_config, size_t index);
    static double nozzle_diameter(const DynamicPrintConfig &printer_config, size_t index);
    static void normalize_printer_config(DynamicPrintConfig &printer_config);

    static DynamicPrintConfig extract(const DynamicPrintConfig &printer_config, size_t index);
    static DynamicPrintConfig normalization_patch(const DynamicPrintConfig &printer_config, size_t index);
    static void apply_nozzle_diameter(DynamicPrintConfig &printer_config, size_t index, double nozzle_diameter);
    static void apply_width(DynamicPrintConfig &printer_config, size_t index, const char *key, const FloatOrPercent &width);
    static void apply(DynamicPrintConfig &printer_config, size_t index, const DynamicPrintConfig &hotend_config);
};

} // namespace Slic3r

#endif
