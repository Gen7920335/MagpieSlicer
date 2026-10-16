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
    // Per object layer, demand painted for normal support that cannot reach the bed; it went to
    // tree support instead.
    std::vector<Polygons> normal_paint_fallback;

    bool has_normal() const;
    bool has_tree() const;
};

// painted_normal / painted_tree: per object layer, demand painted for normal or tree support
// (support channel painting). Painting overrides the automatic choice; tree painting wins where
// both overlap, and normal painting that cannot reach the bed falls back to tree support.
Plan plan(const std::vector<Polygons> &demand, const std::vector<Polygons> &buildplate_shadow, double coverage_threshold_percent,
          bool selective_merge, const std::vector<Polygons> *painted_normal = nullptr, const std::vector<Polygons> *painted_tree = nullptr);

// The support channel painting of the object, per object layer, kept only where the same surface
// is also painted as a support enforcer. Both are left empty when nothing is painted.
void painted_channels(const PrintObject &object, std::vector<Polygons> &normal, std::vector<Polygons> &tree);

// Support demand per object layer from the selected normal generator's own overhang detection.
std::vector<Polygons> detect_demand(const PrintObject &object, const ModSettings &settings);

// The support.generator module "mixed": normal and tree support in one object, merged layer by layer.
void generate(PrintObject &object);

} // namespace MixedSupport
} // namespace ModHost
} // namespace Slic3r
