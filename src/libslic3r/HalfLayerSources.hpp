#pragma once

#include "Layer.hpp"
#include <array>
#include <memory>
#include <vector>

namespace Slic3r {

// Only detached half-layer grids use this deleter. Normal model layers retain
// their existing PrintObject ownership and are never transferred to this plan.
struct HalfLayerSourceDeleter {
    void operator()(Layer *layer) const { delete layer; }
};

// Temporary geometry input for the wall planner, not additional model layers.
// Region configuration and object pointers are borrowed from the PrintObject;
// consume this result before that object is destroyed or its config is changed.
// Each phase is a parent-indexed prefix (empty top sections may be trimmed).
// Neighbor pointers cross phase ownership in physical Z order, not phase order.
struct HalfLayerSourceLayers {
    using LayerOwner = std::unique_ptr<Layer, HalfLayerSourceDeleter>;
    std::array<std::vector<LayerOwner>, 2> phases;
};

} // namespace Slic3r
