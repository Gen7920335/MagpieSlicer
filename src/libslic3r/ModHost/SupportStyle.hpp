#pragma once

// MAGPIE: the support.style slot (mods/sdk/include/magpie_slot_support_style.h). A native mod changes how
// an object's support is printed whatever generator made it: the interface pattern, the wall counts of
// normal and tree support, and a pattern and angle of its own for some interface layers (interface
// sublayers). The core keeps the toolpath generation; SupportParameters asks for the style once.

#include <map>
#include <optional>
#include <vector>

#include "libslic3r/PrintConfig.hpp"

namespace Slic3r {

class PrintObject;

namespace ModHost {

struct InterfaceLayerStyle
{
    std::optional<InfillPattern> pattern; // none: the object's interface pattern
    float                        angle = 0.f; // radians
};

struct SupportStyle
{
    // Every interface layer's pattern; none keeps Orca's choice.
    std::optional<InfillPattern> interface_pattern;
    int                          normal_wall_loops = -1; // -1 keeps Orca's walls
    int                          tree_wall_loops   = -1;
    // Per interface stack size, per layer number - 1 (the contact layer is 1): the restyled layers.
    std::map<int, std::vector<std::optional<InterfaceLayerStyle>>> interface_layers;

    bool any_interface_layer() const;
    // The style of interface layer `number` of a stack of `total` layers, or none.
    const InterfaceLayerStyle *interface_layer(int number, int total) const;
};

// The style of the object's support by the loaded support.style implementations, interface layers asked for
// the stack sizes `totals`. No implementation, or none with anything to change: the default style. Throws
// SlicingError when two implementations style the object or one fails.
SupportStyle support_style(const PrintObject &object, const std::vector<int> &totals);

} // namespace ModHost
} // namespace Slic3r
