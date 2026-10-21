#pragma once

// MAGPIE: the per-plate auxiliary tower positions in an Undo / Redo snapshot. The positions are project options, not
// part of the Model, so the Plater copies them into the snapshot's ModelWipeTower when it takes a snapshot and back
// when it jumps to one, like the prime tower's positions.

#include "AuxiliaryTower.hpp"

#include <array>
#include <vector>

namespace Slic3r {

// The tower positions of every plate as config holds them (a missing key as an empty list); `keys` names the tower.
inline void capture_aux_tower_positions(const DynamicConfig &config, std::vector<double> &x, std::vector<double> &y,
                                        const std::array<const char *, 2> &keys = AUX_TOWER_POSITION_KEYS)
{
    const auto *stored_x = config.option<ConfigOptionFloats>(keys[0]);
    const auto *stored_y = config.option<ConfigOptionFloats>(keys[1]);
    x = stored_x != nullptr ? stored_x->values : std::vector<double>();
    y = stored_y != nullptr ? stored_y->values : std::vector<double>();
}

// Puts captured positions back into config. Returns whether anything changed.
inline bool restore_aux_tower_positions(DynamicConfig &config, const std::vector<double> &x, const std::vector<double> &y,
                                        const std::array<const char *, 2> &keys = AUX_TOWER_POSITION_KEYS)
{
    bool changed = false;
    for (const auto &[key, saved] : { std::make_pair(keys[0], &x), std::make_pair(keys[1], &y) }) {
        auto *current = config.option<ConfigOptionFloats>(key, true);
        if (current->values != *saved) {
            current->values = *saved;
            changed         = true;
        }
    }
    return changed;
}

} // namespace Slic3r
