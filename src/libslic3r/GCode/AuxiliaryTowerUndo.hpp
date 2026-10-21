#pragma once

// MAGPIE: the per-plate auxiliary tower positions in an Undo / Redo snapshot. The positions are project options, not
// part of the Model, so the Plater copies them into the snapshot's ModelWipeTower when it takes a snapshot and back
// when it jumps to one, like the prime tower's positions.

#include "AuxiliaryTower.hpp"

#include <vector>

namespace Slic3r {

// The tower positions of every plate as config holds them (a missing key as an empty list).
inline void capture_aux_tower_positions(const DynamicConfig &config, std::vector<double> &x, std::vector<double> &y)
{
    const auto *stored_x = config.option<ConfigOptionFloats>(AUX_TOWER_POSITION_KEYS[0]);
    const auto *stored_y = config.option<ConfigOptionFloats>(AUX_TOWER_POSITION_KEYS[1]);
    x = stored_x != nullptr ? stored_x->values : std::vector<double>();
    y = stored_y != nullptr ? stored_y->values : std::vector<double>();
}

// Puts captured positions back into config. Returns whether anything changed.
inline bool restore_aux_tower_positions(DynamicConfig &config, const std::vector<double> &x, const std::vector<double> &y)
{
    bool changed = false;
    for (const auto &[key, saved] : { std::make_pair(AUX_TOWER_POSITION_KEYS[0], &x), std::make_pair(AUX_TOWER_POSITION_KEYS[1], &y) }) {
        auto *current = config.option<ConfigOptionFloats>(key, true);
        if (current->values != *saved) {
            current->values = *saved;
            changed         = true;
        }
    }
    return changed;
}

} // namespace Slic3r
