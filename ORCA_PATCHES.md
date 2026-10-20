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

## Earlier core fixes (commits on magpie-next)

- 0f401f7ce8: a layer height range without `layer_height` (3MF material-only range) inherits the object's
  height instead of crashing; an empty `extruder_variant_list` or nozzle list no longer crashes
  `set_num_extruders`, and `Preset::normalize` rebuilds such printer lists; a first-layer sequence that leaves out
  a used filament prints the unlisted ones last (the comparator was not a strict weak ordering); support geometry
  uses the support flow's width when both line widths are automatic (was 0).
- aaa371dc77: CoolingBuffer counts `G4 P<ms>` dwells (it parsed them as 0 s).
- 14f2813893: support base/interface merge compares filaments in one index domain.
- 15f976e3bf / 3272a672c6: absolute line widths follow each nozzle on printers with different nozzle sizes.

## Known Orca bug not patched

- `SlicingParameters::create_from_config` reads the nozzle-indexed `min_layer_height`, `max_layer_height` and
  `nozzle_diameter` with filament ids. On a Bambu multi-nozzle printer whose filament-to-nozzle map is not the
  identity, the variable layer height limits come from the wrong nozzle. Fixing it needs to know whether the
  printer maps filaments to nozzles (Bambu) at every caller, including the GUI layer editor; non-Bambu printers
  use the filament index as the nozzle and are not affected.
