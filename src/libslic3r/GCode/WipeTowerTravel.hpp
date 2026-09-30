#pragma once

#include <string>
#include "../LocalesUtils.hpp"

namespace Slic3r {

// Generator-owned non-wiping travel uses these deferred commands. Wiping
// and user toolchange G-code have different semantics and are not rewritten.
inline std::string wipe_tower_travel_key(bool lift, int filament)
{
    return std::string(lift ? "half_layer_tower_lift_" : "half_layer_tower_restore_") + std::to_string(filament);
}

inline std::string wipe_tower_interblock_travel(const std::string &xy_move, int filament, float feedrate_mm_min)
{
    // Deferred Z uses its own feedrate. Restore the tower writer's modal F both
    // before XY and after lowering; do not add any machine command when OFF.
    const std::string feed = feedrate_mm_min > 0.f ?
        "{if half_layer_tower_travel_enabled}G1 F" + float_to_string_decimal_point(feedrate_mm_min, 3) + "{endif}\n" : "";
    return "[" + wipe_tower_travel_key(true, filament) + "]\n" + feed + xy_move +
           "[" + wipe_tower_travel_key(false, filament) + "]\n" + feed;
}

}
