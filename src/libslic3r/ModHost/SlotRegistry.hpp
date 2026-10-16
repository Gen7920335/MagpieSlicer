#pragma once

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Flow.hpp"

namespace Slic3r {

class ExPolygon;
class PerimeterGenerator;
class PrintConfig;
class PrintObject;
class PrintRegionConfig;
struct WallPlan;

namespace ModHost {

// support.generator: replaces Orca's support generation for an object. It fills the object's
// support layers the way PrintObject::generate_orca_support_material() does, and is chosen per
// object by the support_generator setting. An empty setting keeps Orca's own generator.
struct SupportGenerator
{
    std::string                        name;  // the support_generator value that selects it
    std::string                        label; // shown in the settings
    std::function<void(PrintObject &)> generate;
};

// The generator registered under `name`, or nullptr.
const SupportGenerator *find_support_generator(const std::string &name);
// Every registered generator, in name order.
std::vector<const SupportGenerator *> support_generators();
// Registers a generator; a later registration under the same name replaces the earlier one.
void register_support_generator(SupportGenerator generator);

// wall.plan: splits a region's walls between its own nozzle and a second, detail nozzle. Chosen per
// region by the wall_planner setting; an empty setting keeps Orca's walls.
struct WallPlanner
{
    std::string name;
    std::string label;
    // The detail nozzle's wall flow on this generator's layer, or nothing when the layer has no detail
    // nozzle (then plan is not asked and the walls are Orca's).
    std::function<std::optional<Flow>(const PerimeterGenerator &)> detail_flow;
    // The plan for one island, given the wall count Orca would print (base_loop_number + 1).
    std::function<WallPlan(const PerimeterGenerator &, const ExPolygon &island, int base_loop_number)> plan;
    // The filament (1-based) that prints walls tagged `hint` on layer layer_id of a region whose walls
    // would otherwise use base_filament_1based; 0 keeps Orca's choice.
    std::function<unsigned int(const PrintConfig &, const PrintRegionConfig &, size_t layer_id, ExtrusionToolHint hint,
                               unsigned int base_filament_1based)> wall_filament;
    // The nozzle diameter (mm) that prints every wall of layer layer_id when the planner moves the whole layer's
    // walls to another nozzle, so their widths are resolved on it; 0 keeps the region's own nozzle.
    std::function<double(const PrintConfig &, const PrintRegionConfig &, size_t layer_id)> layer_wall_nozzle;
};

const WallPlanner *find_wall_planner(const std::string &name);
void register_wall_planner(WallPlanner planner);

} // namespace ModHost
} // namespace Slic3r
