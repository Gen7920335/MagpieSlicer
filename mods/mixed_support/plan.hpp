// Mixed support's plan: which support generator takes each support island. Ported from the core's
// MixedSupport::plan (Magpie line 1's MixedSupportPlan); every polygon operation goes through the core.
#pragma once

#include "geometry.hpp"

#include <vector>

namespace mixed {

enum class Channel { Normal, Tree, Split };

// An island goes to the normal generator when at least coverage_threshold percent of it can reach the bed
// straight down, past the object layers below it (buildplate_shadow); otherwise to the tree generator. With
// selective merge the reachable part goes to normal support and the rest to tree support. Islands are judged on
// their own layer; they are not linked across layers.
struct Plan
{
    std::vector<Polygons> normal_mask; // per object layer
    std::vector<Polygons> tree_mask;   // per object layer
    std::vector<Channel>  decisions;   // per island, in layer order, then by bounding box
    // Per object layer, demand painted for normal support that cannot reach the bed; it went to tree support
    // instead.
    std::vector<Polygons> normal_paint_fallback;

    bool has_normal() const;
    bool has_tree() const;
};

bool any_polygons(const std::vector<Polygons> &layers);

// painted_normal / painted_tree: per object layer, demand painted for normal or tree support (support channel
// painting). Painting overrides the automatic choice; tree painting wins where both overlap, and normal painting
// that cannot reach the bed falls back to tree support.
Plan plan(const std::vector<Polygons> &demand, const std::vector<Polygons> &buildplate_shadow, double coverage_threshold_percent,
          bool selective_merge, const std::vector<Polygons> *painted_normal = nullptr, const std::vector<Polygons> *painted_tree = nullptr);

} // namespace mixed
