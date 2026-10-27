# Orca core patches

Fixes to OrcaSlicer's own behaviour that live in the core, not in a mod. They apply with every mod off, so
mods-off output can differ from vanilla Orca where a fix changes it; each entry says where and why.
Since 2026-10-18 such fixes apply unconditionally (no switch) when output and real use stay correct.
Each is a candidate to send upstream.

Format: what was wrong, what changed, where, the guard test.

## Wall filament follows the wall, not its first path

- Wrong: overhang segments carry `erOverhangPerimeter` in inner walls as well as outer walls. Orca decided a
  wall loop's filament from `role()`, which is the role of the loop's first path, so an inner wall that starts
  with an overhang segment printed with `outer_wall_filament_id` when the inner and outer wall filaments differ.
  The split of mixed perimeter collections also dropped any entity whose role was none of outer, overhang or
  inner.
- Change: `is_inner_wall()` (ExtrusionEntityCollection) decides by the wall's first non-overhang segment, and by the generator's `inset_idx` (0 = outer wall) for a wall that is all overhang
  (unknown counts as outer, as before). The role is preferred because H/2 shells restate the outer role on their
  second loop. Used by `LayerTools::extruder()`, the perimeter split in `GCode::process_layer` and the half-layer
  task filament.
- Tests: "A wall's filament follows the wall, not an overhang at its start", "Inner and outer walls keep their
  filaments on overhangs" (test_multifilament.cpp).

## G-code writer never switches to an unregistered filament

- Wrong: `GCodeWriter::toolchange()`, `set_extruder()` and `init_extruder()` looked a filament up with
  `lower_bound` only. An id between registered ids switched to the next registered filament; an id past the last
  one dereferenced `end()` (`set_extruder` read it before its assert). Magpie line 1 hit this as a crash in
  by-object export.
- Change: an exact lookup (`find_filament_extruder`); an unregistered id keeps the current tool and logs an error
  (permanent diagnostic). Line 1 threw instead; that broke Orca's by-object mixed-filament export, which asks for
  the virtual mixed slot.
- Test: "Writer keeps the active tool when asked for an unregistered filament" (test_gcodewriter.cpp).

## Temperature waits inside the print take time in the estimate

- Wrong: the estimator counted every `M109` as no time, so a tool change or a temperature change between layers
  that waits for the nozzle looked instant (print time, M73 progress, preview layer times).
- Change: an `M109` after the first layer change waits for the nozzle to reach the target at
  `layer_time_blend_heat_rate` / `_cool_rate` (printer settings, 2 and 1 degC/s by default). Waits before the
  first layer (start G-code heat-up) and BBL's virtual `VM109` stay at no time, as in Orca, so the estimate of a
  print without mid-print waits is unchanged. The temperature model follows every M104/M109 target.
- Test: "Temperature waits inside the print count without blending" (test_layer_time_blend.cpp).

## Organic tree tips are placed the same way on every run

- Wrong: `generate_initial_areas()` places the tree tips of all overhang layers in parallel, and several overhang
  layers place tips into the same support layer. A tip is dropped when another one already holds its slot
  (`m_already_inserted`, a grid of about a tenth of the tip radius), and the first thread to arrive won, so the tips
  (their position, roof flags, dont_move_until) and the whole tree differed between runs. Seen on a 12.7 M
  triangle model with organic support at threshold 90: vanilla Orca gave two or three different G-codes in a few
  runs; the per-stage content hash showed the tips of one layer differing in the odd run.
- Change: inside the parallel loop each overhang layer collects its tips; they are inserted afterwards in overhang
  order (`insert_ordered_tips`), exactly as a serial run would. The common result is unchanged.
- Check: 16 runs after the fix never gave the tip variant (it came 2 times in 26 runs before). A second, rarer
  variant remains (2 of 16 runs, vanilla too): the support is identical through drawing and toolpaths, and one
  wall loop on some layers starts at a different point in the G-code. Not seen in 18 runs without support. Cause
  UNKNOWN; the next measurement is the seam / loop start chosen for the first differing loop (layer 0.25 mm).

## Binary STL files over 2 GB load

- Wrong: admesh read the file size with `ftell()` into a `long`, which is 32 bits on Windows, so any STL over
  2 GB failed the size check ("wrong size").
- Change: 64-bit `_ftelli64` / `ftello`; a size error or a facet count over 32 bits is reported instead of
  misread (deps_src/admesh/stlinit.cpp).
- Check: a 4.6 GB binary STL loads (manual run; no unit test, the file would be too large for the suite).

## CLI colour mapping with a config of only a preset's own keys

- Wrong: when a loaded filament preset sets `filament_colour`, the CLI maps colours and recomputes the flush
  volumes on its project config (`OrcaSlicer.cpp`, the colour block before slicing). That config holds only the
  keys the loaded presets set; a hand-written "from User" filament preset without `filament_diameter` made
  `GUI::get_min_flush_volumes` (Plater.cpp) dereference a null option, and short retraction lists were indexed
  past their end. The CLI crashed with an access violation before slicing (vanilla Orca too).
- Change: `get_min_flush_volumes` sizes its result by `filament_diameter`, else `filament_colour`, else one
  filament, and reads lists shorter than the filament or nozzle index as their defaults (18 mm, off); the CLI
  pads the volumes to the project's filament count.
- Test: "Minimum flush volumes from a config that lacks the filament lists" (tests/slic3rutils).

## Arachne's minimum widths on a printer with mixed nozzles

- Wrong: `Arachne::make_paths_params` takes the minimum bead width, minimum feature size and transition length
  and deviation (% settings) of the printer's smallest nozzle. On a 0.15/0.4 mm printer the 0.4 mm nozzle's walls
  narrowed to 0.075 mm (50 % of 0.15 mm); vanilla Orca does the same (1/3 scale Hilda, 0.12 mm layers: 1,356 moves
  at 0.101 mm on the 0.4 mm nozzle).
- Change: `make_paths_params` takes the nozzle the % settings refer to (0 keeps the smallest). Walls without a
  multi-nozzle wall plan use the smaller of the region's outer and inner wall nozzles. With a plan, the single pass
  and the full pass keep the smallest nozzle (taking the large one there makes the detail walls 0.41 mm and stops the
  slice at the width check), and the inner wall pass under a single top wall takes only its minimum bead width of
  the large walls' nozzle (taking minimum feature size and transition length too drops a detail wall loop).
  Printers with one nozzle size are unchanged.
- Counterfactual (1/3 Hilda, figure settings): 0.4 mm nozzle wall moves under 0.2 mm 22,313 -> 1,796 with
  multi-nozzle walls off (a printer with 0.4 mm nozzles only: 1,693), 5,562 -> 1,114 with them on; the 0.092 /
  0.109 mm fixed widths go. The 0.15 mm detail walls keep their length at layer and band heights (70,177 ->
  70,172 mm); only scarf ramps move with the seams (4,025 -> 3,738 mm).
- Tests: "Arachne narrows a large nozzle's walls by its own nozzle, not the printer's smallest" and "Inner walls
  under a single top wall narrow by the large nozzle" (test_multi_nozzle_walls.cpp; both fail without the change).

## Bridge width check on a printer with mixed nozzles

- Wrong: the preset check resolved a % bridge width on the largest nozzle and compared it with the smallest, so a
  0.15/0.4 mm printer refused 100 % and the U1 profile's 0.4 mm.
- Change: on a printer with mixed nozzles the width is checked as a fraction of its nozzle; the per-nozzle check in
  `Print::validate` is unchanged. One-size printers keep Orca's check (commit d18ddd9e5f).
- Test: "The bridge width check follows each nozzle on a mixed-nozzle printer" (test_config.cpp).

## Earlier core fixes (commits on magpie-next)

- 0f401f7ce8: a layer height range without `layer_height` (3MF material-only range) inherits the object's
  height instead of crashing; an empty `extruder_variant_list` or nozzle list no longer crashes
  `set_num_extruders`, and `Preset::normalize` rebuilds such printer lists; a first-layer sequence that leaves out
  a used filament prints the unlisted ones last (the comparator was not a strict weak ordering); support geometry
  uses the support flow's width when both line widths are automatic (was 0).
- aaa371dc77: CoolingBuffer counts `G4 P<ms>` dwells (it parsed them as 0 s).
- 14f2813893: support base/interface merge compares filaments in one index domain.
- 15f976e3bf / 3272a672c6: absolute line widths follow each nozzle on printers with different nozzle sizes.

## Variable layer height limits on the printing nozzle

- Wrong: `SlicingParameters::create_from_config` handed `min/max_layer_height_from_nozzle()` (1-based nozzle) the
  0-based ids of `object_extruders()`, so every filament but the first read the previous nozzle's limits: filament 3
  of a 0.4/0.4/0.6 mm printer (Snapmaker U1) got 0.08-0.32 mm instead of 0.12-0.48 mm, which layer height ranges
  and adaptive layer height are clamped to. A BBL filament_map was never applied there either.
- Change: each filament goes through `printing_nozzle_index()` (BBL map when the printer is Bambu Lab: the Print's
  flag, or `printer_model` in the static `PrintObject::slicing_parameters()` the GUI layer editor uses); the support
  and raft nozzle diameters too. Single-nozzle printers and filament 1 are unchanged.
- Test: "Variable layer height limits come from the nozzle each filament prints on" (test_print.cpp); it fails with the old
  reads.
- Output: U1 corpus cases whose object uses the 0.6 mm nozzle change one or two auto-lift travels (frog_mn_classic_2w_il,
  frog_mn_single_wall_top: one spiral lift becomes a slope lift; mixed_support_on_small, mixed_per_nozzle_support_small,
  ltc_tower_mixed_nozzles: one tower approach no longer lifts). Layers, extrusions and Z are unchanged; the overhang test
  for auto lift (`is_through_overhang`, z range [z - 0.4, z] in float) took 3 layers before and 2 after at layer 8 of the
  frog, so a layer sits on that boundary and its print_z moved below 1e-4 mm with the new minimum layer height
  (0.08 -> 0.12 mm, which `generate_object_layers` steps with). Measured with a probe on frog_mn_classic_2w_il; the
  three tower cases are ASSUMED to be the same boundary (reverting the fix makes all five identical to main).

## Type-1 tower approach starts where the head is

- Wrong: with `prime_tower_skip_points` the Type-1 toolchange routes the travel into the tower from the position
  parsed out of `change_filament_gcode`. `get_last_position_from_gcode()` only sets the axes the G-code moves, and
  the `Vec3f` it filled was uninitialized, so a change G-code with no XY move (Snapmaker U1) started the route at
  garbage: the travel into the tower (and the time estimate) differed on every run.
- Change: the position is seeded with the writer's position first, as the Type-2 tower-gap route already did.
- Check: the frog_wt2_classic_type1 setup (corpus_wipe_tower) sliced 6 times is byte-identical; the unfixed binary gave 4
  different files in 4 runs.

## Generated tower bed check after the tower step; CLI refit

- Wrong: the exact footprint check of the generated tower threw inside `_make_wipe_tower`, so the step was never
  done and nobody could see the tower that failed. The pre-slice placement (arrange, CLI clamp, GUI reload clamp)
  sizes the tower with `estimate_wipe_tower_footprint`, which purges once per filament and layer; H/2 frames visit
  tools more often (measured: 4 tool changes per layer against 1 without H/2 on frog + cube, U1 type-1 tower,
  tower 77 x 90 mm against 45 x 44 mm), so a tower placed at the edge by the estimate left the bed and the CLI
  stopped with "Prime tower is partially outside the printable area".
- Change: `Print::validate_generated_wipe_tower_footprint()` runs after psWipeTower is done (every process());
  `Print::generated_wipe_tower_refit()` gives the position the GUI drag clamp would give the real tower
  (`move_box_inside_polygon`, WIPE_TOWER_MARGIN), refusing one that lands on an object hull or exclusion area; the
  CLI applies it and slices once more. The GUI is unchanged apart from showing the real tower after the error.
- Test: "A half-layer tower placed by the estimate is refitted onto the bed once generated"
  (test_half_layer_assembly.cpp); with the check back inside the step it fails.

## Painted outer wall width on the BBL-mapped nozzle

- `resolve_outer_wall_line_width` (multi-material segmentation) takes the printer's BBL flag; it read the outer
  wall filament's number as its nozzle. Test: "The painted outer wall width follows a BBL filament map".

## H/2 shell bridges and the one-wall-top infill clip on the BBL-mapped nozzle

- Wrong: PerimeterGenerator's `outer_wall_path_nozzle` (the H/2 shell bridge width) and the single-top-wall sparse
  infill clip read the filament's number as its nozzle (`is_bbl_printer = false`). Per-nozzle width fields only
  count when the nozzle sizes differ, so only a mixed-size BBL printer is affected: an H2D with 0.4/0.8 mm nozzles,
  filament 1 mapped to the 0.8 mm nozzle and bridge_line_width_per_nozzle 0.3/0.6 printed the H/2 overhang walls
  0.3 mm wide on the 0.8 mm nozzle; the same print with the nozzles swapped printed 0.6 mm.
- Change: `PerimeterGenerator::is_bbl_printer` (the Print's flag, set in LayerRegion) goes to both reads.
- Test: "H/2 shell bridges of a BBL printer take the bridge width of the nozzle filament_map names"
  (test_multi_nozzle_walls.cpp); 4 of 8 assertions fail with both reads back on `false`. The clip read alone
  changed no output in the BBL corpus (D:\claude\golden\bblwall\corpus3.json: pyramid, cube, slab, tee with only
  one wall on top and 0.4/0.8 mm nozzles); it is changed for the same routing, without a failing test.

## Outer wall overhang and bridge checks on the BBL-mapped nozzle

- Wrong: PerimeterGenerator read `nozzle_diameter.get_at(outer_wall_filament_id - 1)` (bridge margin of the top
  surface split, overhang detection in Classic and Arachne, the overhang offsets), the slot numbered like the
  filament. On an H2D with 0.4/0.8 mm nozzles and filament 1 mapped to the 0.8 mm nozzle it took the unused 0.4 mm
  nozzle: overhang.obj (classic walls) printed 0.8 mm longer overhang walls on its 3.2 mm layer than the same print
  with the nozzles swapped.
- Attribution (main 987bb6eb70 CLI, D:/claude/golden/unk/h2d): filament 2 on nozzle 2, so the read slot is the
  printing nozzle, matched the swapped print move for move; with the read slot left unused, changing only that
  unused nozzle (0.8 -> 0.6 mm) moved the overhang walls 0.4 mm, and changed nothing once the read slot printed.
  The four reads were not separated.
- Change: `outer_wall_nozzle_diameter()` maps the filament with `printing_nozzle_index` (the BBL flag). Same value
  on other printers and when the nozzles share one size.
- Test: "Overhang walls of a BBL printer do not depend on which slot holds the nozzle filament_map names"
  (test_multi_nozzle_walls.cpp). Written unbuilt: its failure without the change is not yet shown.

## Fixes from the U1 random-settings test (2026-10-24, branch rand-fixes)

Found by slicing randomized U1 settings (harness D:\claude\goldenand); each has a regression test that fails on the
pre-fix sources (counterfactual build). Golden gate D:\claude\goldenfix: 181/197 identical to golden/thui/b4,
mods off identical; the 16 others are the intended H/2 second-wall width (Arachne, single wall on top) and the
detail-nozzle overhang width.

- Surface fill role (ToolOrdering `surface_fill_role`): the gap fill Fill::_create_gap_fill puts into a surface's
  collection no longer decides the filament. Orca printed a bottom surface with gap fill (erMixed) with the internal
  solid filament, and registered a top surface whose first entity was gap fill as internal solid.
- VariableWidth: a medial axis run whose mean width is under 0.005 mm is dropped; Orca threw "negative spacing".
- PerimeterGenerator classic gap fill: on mixed nozzle sizes no wider than 2 x the solid infill flow's spacing (the
  nozzle that prints it); Orca filled up to 2 x the walls' spacing through a smaller nozzle.
- Tree support raft interface layers use the support interface flow (interface nozzle).
- SupportCommon generate_interface_layers: a snug style with tree contact layers (no overhang polygons) uses the
  contact polygons instead of dereferencing null.
- Generated tower check and CLI refit include the combined skirt around the tower.

## Settings: a parent off hides its children (GUI only)

- Wrong: Orca greys out a setting whose parent is off (toggle_field) and leaves it on the page; Gen wants it
  hidden, as Orca already does for some (toggle_line). A few child settings had no rule at all.
- Change: Tab collects each toggle_options round: an option's last toggle_line and last toggle_option both count,
  and its line shows when both allow it (Tab::begin_line_round/end_line_round). Options greyed for what the
  printer is keep Orca's greying (keep_greyed_out: vendor locks, one extruder, belt, air filter, SLA, plate).
  New rules: support_remove_small_overhang and support_line_width with support or a raft, wipe_tower_filament
  with a prime tower, slow_down_min_speed with slow_down_for_layer_cooling, the filament tower interface
  settings with enable_tower_interface_features. Each new rule was checked by slicing with the child changed
  and the parent off (the same G-code); fan_speedup_overhangs stays (it shares its line with its parent).
- Output: none; the G-code is unchanged (golden gate).

## Known Orca bugs not patched

- The Type-1 tower (`WipeTower::set_extruder`) takes one line width for every tool from the nozzle of the last
  configured filament: on a U1 0.4/0.4/0.6/0.6 printing with the 0.4 mm nozzles only, the tower prints 0.75 mm
  lines; making the unused fourth nozzle 0.4 mm gives 0.5 mm. Per-tool widths are separate work (about 200 sites).
- One object whose walls and infill use two filaments prints its tool changes without a tower: the plate's
  filament count comes from the object's own filament, as in vanilla Orca.

## Tree support geometry width on the support body's nozzle

- Wrong: `SupportParameters::support_extrusion_width` (the width Tree support builds its branches, base areas and
  move distances with, next to the body nozzle's diameter in `TreeSupport.cpp`) was resolved on the nozzle of
  `support_interface_filament`. With the body and interface on nozzles of different sizes the body was built at the
  interface nozzle's width (0.42 mm instead of 0.63 mm for a 0.6 mm body nozzle and a 0.4 mm interface nozzle).
- Change: resolved on the nozzle printing the support body (`feature_printing_nozzle(WidthFeature::Support)`, BBL
  filament maps included), the nozzle `support_material_flow()` uses. One nozzle or one support filament: unchanged.
- Tests: "Tree support geometry width follows the body nozzle instead of the interface nozzle", "Support flow
  resolves filaments through the BBL filament to nozzle map" (test_support_material.cpp).
