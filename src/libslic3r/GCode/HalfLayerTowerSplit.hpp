#pragma once

#include "../libslic3r.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

namespace Slic3r {

// MAGPIE (H/2): a prime tower layer of an H/2 layer is printed as two half-height tower layers when it is taller
// than the smallest nozzle that prints on it, so that nozzle never prints the tower at the full layer height
// (0.15 mm nozzles under 0.16 mm layers). The H/2 bands are two equal halves of the model layer; the tower takes
// the same halves of its own layer. Heights and diameters in mm; EPSILON (mm) is the slicer's height tolerance.
inline bool half_layer_tower_split_needed(double tower_layer_height_mm, double smallest_nozzle_mm)
{
    return tower_layer_height_mm > smallest_nozzle_mm + EPSILON;
}

// The height of each half when a tower layer of tower_layer_height_mm is split, mm.
inline double half_layer_tower_half_height(double tower_layer_height_mm) { return 0.5 * tower_layer_height_mm; }

// How many of a split layer's tool changes, in execution order, go to the lower half: the shortest prefix holding at
// least half of the purge (mm3), so both halves need about the same tower area. One change at least goes low when
// there is any; with no purge at all the changes are halved by count. Purge volumes below 0 count as 0.
inline size_t half_layer_tower_lower_change_count(const std::vector<double> &purge_mm3)
{
    if (purge_mm3.empty())
        return 0;
    double total_mm3 = 0.;
    for (const double volume_mm3 : purge_mm3)
        total_mm3 += std::max(0., volume_mm3);
    if (!(total_mm3 > 0.))
        return (purge_mm3.size() + 1) / 2;
    double lower_mm3 = 0.;
    for (size_t i = 0; i < purge_mm3.size(); ++i) {
        lower_mm3 += std::max(0., purge_mm3[i]);
        if (lower_mm3 >= 0.5 * total_mm3)
            return i + 1;
    }
    return purge_mm3.size();
}

} // namespace Slic3r
