#pragma once

#include <optional>
#include <vector>

#include "../BoundingBox.hpp"
#include "../Point.hpp"
#include "../Polygon.hpp"

namespace Slic3r {

class ConfigBase;
enum class WipeTowerType;

// Pre-slice footprint of the wipe tower, shared by validation (Print), the GUI's placement
// clamp/preview/arrange and the CLI placement. The arithmetic is shared; the inputs below are
// not, so a change to how one caller derives them has to be mirrored in the others.
struct WipeTowerFootprint
{
    double width      = 0.; // effective width: equals depth for a rib wall, which squares the tower
    double depth      = 0.; // 0 when these inputs imply no tower
    double height     = 0.; // tallest object; drives the stability floor and the auto brim
    double brim_width = 0.; // printed width: auto (-1) resolved by height, laid in whole loops
};

// Which planner builds the tower: Bambu Lab printers always get Type1, the rest follow
// wipe_tower_type. The rule Print::wipe_tower_type() and the CLI apply, read off the config so
// the GUI and CLI placement can resolve it without a Print.
WipeTowerType resolve_wipe_tower_type(const ConfigBase &config);

// First-layer outline of an estimated tower in tower-local scaled coordinates, brim excluded:
// the body box, or for a Type2 cone wall the box unioned with the cone's base. The preview,
// the placement margin and validation all take the outline from here so they cannot disagree
// about whether a cone exists.
Polygon estimate_wipe_tower_first_layer_outline(const ConfigBase &config, WipeTowerType tower_type, double width, double depth, double height);

// MAGPIE: how often one layer can visit each filament. A layer of a banded object (layer.bands slot, H/2) is printed
// as an execution frame (ToolOrdering::finalize_half_layer_frame): the support below the lower outer-wall pass, the
// lower pass, the support above it, the core, and the upper pass inserted inside the core, each block one run per
// filament across objects (group_half_layer_tasks_across_objects). Support that prints last (gcode.events support_last,
// the low temperature interface) leaves those places for runs after the lower pass and after the core, which visit a
// filament no more often. The tower takes a tool change for every visit, so
// a filament printing support and core is visited before the lower pass, for the upper support, and on both sides of
// the upper pass: four times a layer. Three was short: seed 11026 of the U1 random test (two cubes, before the
// grouping) visited filament 0 five times on one layer and the generated tower, 87 x 89 mm, outgrew the 81.7 mm box.
constexpr int BANDED_LAYER_VISITS_PER_FILAMENT = 4;

// filament_ids:  0-based filaments purged on the plate. The config cannot see custom G-code tool
//                changes, so ids derived from the model must include them
//                (Print::extruders(true)) or a real tower is sized as if it were never built.
//                A mixed slot may be passed as itself or already expanded: it is sized as its
//                physical components either way, since those are all the tower ever purges.
// layer_height:  thinnest layer the objects are sliced at. The first layer is folded in here.
// visits_per_filament: how many times a layer can visit each filament: 1, or BANDED_LAYER_VISITS_PER_FILAMENT when
//                an object on the plate prints in layer bands. The estimate is the bound for that many visits.
//
// A raft is deliberately not a reason: normalize_fdm_2 clears enable_prime_tower for a plate
// purging one filament unless smooth timelapse or wrapping detection is on.
WipeTowerFootprint estimate_wipe_tower_footprint(const ConfigBase                &config,
                                                 WipeTowerType                    tower_type,
                                                 const std::vector<unsigned int> &filament_ids,
                                                 double                           layer_height,
                                                 double                           max_object_height,
                                                 int                              visits_per_filament = 1);

// Whether a prime tower is PRINTED for a plate. The footprint estimate above answers how big a
// tower is and never reads enable_prime_tower or print_sequence, so it reports a size for a plate
// that prints no tower at all; the authority is DynamicPrintConfig::normalize_fdm_2(), which clears
// enable_prime_tower before the plate is sliced. This mirrors that rule so pre-slice consumers can
// ask it without mutating a config, and a test pins the two together.
//
// used_filaments: the plate's filament SLOTS as authored - a mixed slot counts once, matching
//                 Print::extruders(), which is what normalize_fdm_2 is handed.
// num_objects:    objects printed on the plate; only ByObject sequencing looks at it.
// has_mixed_filament: filament_is_mixed is a PROJECT option, so it is passed rather than read -
//                 `config` only has to carry the print preset's own keys.
bool prime_tower_is_printed(const ConfigBase &config, int used_filaments, int num_objects, bool has_mixed_filament);

// Where to put a tower whose saved position no longer fits: of the anchors (the tower's
// position) inside `anchor_bounds`, the one nearest `target` whose `footprint` touches none of
// the `obstacles`. `footprint` is relative to the anchor, brim included; an obstacle is taken
// as its convex hull. Empty when every anchor in bounds meets an obstacle. All coordinates scaled.
std::optional<Point> nearest_clear_wipe_tower_anchor(const BoundingBox &anchor_bounds, const Polygon &footprint,
                                                     const Polygons &obstacles, const Point &target);

} // namespace Slic3r
