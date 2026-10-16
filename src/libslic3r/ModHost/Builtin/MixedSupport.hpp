#pragma once

#include "libslic3r/Polygon.hpp"

#include <string>
#include <vector>

namespace Slic3r {

class PrintObject;

namespace ModHost {
namespace MixedSupport {

// The mixed_support mod parameters.
struct ModSettings
{
    std::string normal_generator   = "orca";    // "orca" (Orca's normal support) or "cura"
    std::string tree_style         = "organic"; // "organic", "tree_slim", "tree_strong" or "tree_hybrid"
    double      coverage_threshold = 100.;      // percent of an island reachable from the bed
    bool        selective_merge    = false;
};

ModSettings read_settings(const PrintObject &object);

enum class Channel { Normal, Tree, Split };

// Which support generator takes each support island. An island goes to the normal generator when
// at least coverage_threshold percent of it can reach the bed straight down, past the object
// layers below it (buildplate_shadow); otherwise to the tree generator. With selective merge the
// reachable part goes to normal support and the rest to tree support. Islands are judged on
// their own layer; they are not linked across layers.
struct Plan
{
    std::vector<Polygons> normal_mask; // per object layer
    std::vector<Polygons> tree_mask;   // per object layer
    std::vector<Channel>  decisions;   // per island, in layer order, then by bounding box

    bool has_normal() const;
    bool has_tree() const;
};

Plan plan(const std::vector<Polygons> &demand, const std::vector<Polygons> &buildplate_shadow, double coverage_threshold_percent,
          bool selective_merge);

// Support demand per object layer from the selected normal generator's own overhang detection.
std::vector<Polygons> detect_demand(const PrintObject &object, const ModSettings &settings);

// The support.generator module "mixed": normal and tree support in one object, merged layer by layer.
void generate(PrintObject &object);

} // namespace MixedSupport
} // namespace ModHost
} // namespace Slic3r
