#pragma once

// MAGPIE: the layer.bands slot (mods/sdk/include/magpie_slot_layer_bands.h). A native mod says which of an object's
// paths print in thinner bands inside each model layer, and where a later band goes in the layer's print time;
// the core's band executor prints them.

#include "libslic3r/LayerBands.hpp"

#include <string>

namespace Slic3r {

class DynamicPrintConfig;
class PrintObjectConfig;

namespace ModHost {

// The band plan of an object with these settings, from the loaded layer.bands implementations. Never throws: an
// implementation that fails, or two banding one object, leave nothing banded and the reason in `error`.
LayerBandPlan layer_band_plan(const DynamicPrintConfig &print_config, const PrintObjectConfig &object_config,
                              const std::string &object_name);

} // namespace ModHost
} // namespace Slic3r
