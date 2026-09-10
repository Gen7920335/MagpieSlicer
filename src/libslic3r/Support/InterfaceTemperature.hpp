#pragma once

#include "../Config.hpp"

namespace Slic3r {

// Lowest positive target in degrees Celsius among configured interface roles.
// Used for tower planning, not to override the target of an individual path.
inline int lowest_configured_interface_temperature(const ConfigBase &config)
{
    const auto *base = config.option<ConfigOptionInt>("support_interface_temperature");
    const auto *sub = config.option<ConfigOptionInt>("support_interface_sublayer_temperature");
    const auto *enabled = config.option<ConfigOptionBool>("support_interface_sublayer_pattern");
    const auto *layers = config.option<ConfigOptionInt>("support_interface_top_layers");
    int target_celsius = base != nullptr && base->value > 0 ? base->value : 0;
    // The generator clamps inclusive sublayer ranges into [2, total], so any
    // enabled range has a possible sublayer exactly when total exceeds one.
    if (enabled != nullptr && enabled->value && layers != nullptr && layers->value > 1 &&
        sub != nullptr && sub->value > 0 && (target_celsius == 0 || sub->value < target_celsius))
        target_celsius = sub->value;
    return target_celsius;
}

} // namespace Slic3r
