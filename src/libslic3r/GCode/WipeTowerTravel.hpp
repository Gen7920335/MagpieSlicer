#pragma once

#include <string>
#include <string_view>
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

// The markers above sit on their own lines so the line-based tower move
// rewriting leaves them alone. Without H/2 travel they expand to nothing, so
// the emitter drops their lines first; otherwise each would leave a blank line
// that vanilla Orca does not write.
inline void strip_wipe_tower_travel_lines(std::string &gcode)
{
    if (gcode.find("half_layer_tower_") == std::string::npos)
        return;
    std::string kept;
    kept.reserve(gcode.size());
    size_t begin = 0;
    while (begin < gcode.size()) {
        size_t end = gcode.find('\n', begin);
        end = end == std::string::npos ? gcode.size() : end + 1;
        const std::string_view line(gcode.data() + begin, end - begin);
        const bool marker = line.rfind("[half_layer_tower_lift_", 0) == 0 ||
                            line.rfind("[half_layer_tower_restore_", 0) == 0 ||
                            line.rfind("{if half_layer_tower_travel_enabled}", 0) == 0;
        if (!marker)
            kept.append(line);
        begin = end;
    }
    gcode = std::move(kept);
}

}
