#pragma once

#include "../ExtrusionEntity.hpp"

namespace Slic3r {
class FullPrintConfig;

struct ExtrusionSpeedContext {
    size_t physical_tool;
    unsigned filament;
    int logical_layer_id;
    bool first_layer;
    bool object_layer_over_raft;
    bool sloped;
    // MAGPIE: the speed of walls a wall planner sends to its detail nozzle (ExtrusionToolHint::DetailWall): mm/s, or
    // with detail_wall_speed_percent a percentage of the outer wall speed; 0 keeps the wall speed. The wall planner
    // the region uses gives it (its wall.plan setting detail_wall_speed).
    double detail_wall_speed = 0.;
    bool   detail_wall_speed_percent = false;
};

struct ExtrusionBaseSpeed {
    double effective_mm3_per_mm;
    double speed_mm_s;
};

// Shared emitter/planner calculation, before dynamic quality, cooling,
// resonance and acceleration adjustments. Inputs keep material and nozzle IDs
// separate. A scheduling estimate using this speed is not a motion simulation.
ExtrusionBaseSpeed extrusion_base_speed(const FullPrintConfig &config, const ExtrusionPath &path,
                                        const ExtrusionSpeedContext &context,
                                        double requested_speed_mm_s = -1.,
                                        ExtrusionToolHint tool_hint = ExtrusionToolHint::Auto);

} // namespace Slic3r
