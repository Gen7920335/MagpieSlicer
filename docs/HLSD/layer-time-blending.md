# Layer time blending

Layer time blending adds waits after short layers so that the times of neighbouring layers stay
close, which keeps the cooling each layer gets even. It is the bundled native mod
`mods/layer_time_blend` ([native-mods.md](native-mods.md)): its library fills the `gcode.time_plan`
slot (`mods/sdk/include/magpie_slot_time_plan.h`) under the name `blend` and reads the blending
parameters itself. Its slot declaration picks it while blending is on and gives, as settings, what the
core's export reads: where the nozzle waits, the tower size, the park position and the heating and
cooling rates of the temperature estimate ([mods.md](mods.md), Implementations behind slots). The
tower's stored per-plate position stays in `layer_time_blend_tower_x`/`_y`. The core marks the sites,
measures them, hands the numbers to the slot and writes the waits; it has no planner of its own,
and a planner name no loaded mod provides stops the slice. With the mod off nothing is written and
the G-code is Orca's.

## Target times

Layer times can only grow, so the target for layer times `t` is the pointwise smallest `T >= t`
with every pair of neighbours within the limit:

- linear: `|T[i] - T[i-1]| <= D` seconds;
- log: the same in log space, neighbours within a ratio of `1 + r`.

Two passes, forwards and backwards, give it (`T[i] = max_j(t[j] - D|i - j|)`): the layers before a
long layer wait too, so the times ramp up to it. With `ease_layers` N > 0 the convex bends, where a
ramp starts or ends, are limited to a second difference of `D / (N + 1)`; the peak at a long layer
is not rounded, which would lift every layer around it. A layer whose own time already bends more
(a short layer between two longer ones, layers that alternate with the infill direction) keeps its
bend: easing rounds the bends blending makes, not the ones the layer times have, which otherwise
added waits on every other layer of a print that needed none. Each layer's bound is fixed by the
input, so both constraint sets are closed under the pointwise minimum and the smallest solution
still exists; the planner alternates the two-pass envelope with the upper concave hull of `T - q`,
where `q`'s second difference is each layer's bound, until nothing rises.

A chain of layers starts at the first layer and at the first layer of each object of a by-object
print. Layers below `start_layer` (2 by default, counted per chain) get no wait and do not limit
the layer above them. `wait_cap` limits each wait (off by default). The waits are not capped
otherwise: one long layer may spread waits over many layers, which the planner reports instead
(more than `warn_total_pct` of the print time, or one wait over `warn_single` seconds), naming the
long layer and the range.

## Where the time comes from

The export marks a wait site (`;MAGPIE_WAIT_SITE`) at the start of every full layer: one with an
object layer at the layer's own height, after the layer change G-code and before the layer's fan
change. A support-only layer and an H/2 half pass fold into the interval they fall in. After the
G-code is written:

1. The processor's estimate gives the time at each site: the time of the moves up to the site's
   last G1 plus all extra time (dwells, tool changes) asked for before the site. The elapsed time
   at that G1 alone is not it, because the estimator books a dwell on the oldest queued block.
2. The planner turns the intervals into waits.
3. The waits are written into the file before their sites, after the whole pipeline, so the
   cooling buffer never sees them. Fan speeds stay as Orca planned them for the layer's own time.
4. A second estimate checks every site; each wait is corrected once by its error (a wait is pure
   time, so one correction is exact up to the estimator's float resolution).
5. The export's own processor runs over the file as written, so statistics, M73 and the preview
   match it. The result keeps each waiting layer's time before its wait and the wait
   (`GCodeProcessorResult::layer_time_blend`): the Layer Time legend gives their totals and the layer
   slider's hover tooltip both numbers for the layer under the mouse. It also keeps each site's time:
   a site and the wait before it come after the next layer's layer change, so by the moves' layers a
   wait would show on the layer above the one it lengthens. The preview's layer times
   (`layer_time_blend_preview_times`, Normal mode) move the time from each layer's start to its site
   to the layer before, so they run from site to site, the planner's layer times. The G-code and
   the result's move times stay as estimated.

Orca counts an M109 as no time. While blending runs with a G-code event mod that changes nozzle temperatures within
layers (the low temperature support interface; gcode.events `MP_GCODE_CHANGES_TEMPERATURE`) the estimate follows each nozzle's temperature (`layer_time_blend_heat_rate` / `_cool_rate`) and counts
the M109 waits; those layers, and layers with a tool change, are flagged as estimated and reported
when a wait is next to them.

## Where the nozzle waits

- `in_place`: waits of 1 s and more lift the nozzle 1 mm over the new layer, wait and come down to
  the layer's Z (the lazy layer raise, done early); shorter ones are a bare G4.
- `tower` (the mod's default): a hollow 15 mm square tower with a 5-line brim, printed one loop
  per layer next to the objects (20 mm off their bounding box) or at the plate's
  `layer_time_blend_tower_x/_y`, in plate coordinates like the park position and the printable area
  (the plate's origin puts them on the bed). The nozzle waits over its center, then prints the loop,
  which re-primes it. A step taller than the nozzle's `max_layer_height` since the tower's last loop
  (the first object layer on a raft) is printed as several loops, each at its own Z, the last at
  the layer's. Each layer's part is a self-contained block that starts and ends retracted at the
  layer's Z, so the blocks above the last wait that needs the tower are left out; the planner picks
  the lowest cut that keeps every limit. With absolute E a left-out block that does not end with the
  writer's own `G92 E0` is replaced by a `G92` to the E it ended at, so the moves after it push no
  extra filament; on the flavors whose writer never resets E (Mach3, MakerWare, Sailfish) the tower
  stays whole. Not in by-object prints; a tower that does not fit parks instead.
  The tower's spot comes from `place_layer_time_tower` (`GCode/LayerTimeTower.hpp`), which the 3D scene calls too: it
  draws the tower's footprint on each plate where blending waits at the tower (red where a stored position cannot be
  printed, over an object, and the waits park) and dragging it stores the plate's `layer_time_blend_tower_x/_y`,
  which follow the plates and Undo / Redo like the temperature drop tower's. Before slicing the scene places an
  automatic tower from the model instances' footprints and the plate's first nozzle.
- `park`: lift, travel to the park position (printer `layer_time_blend_park_x/_y`, rear left by
  default), wait, back to the same XY, down. Waits shorter than `in_place_max` or the trip wait in
  place.
- `purge_tower`: the sites stay at the layer starts, so what is blended is the model's layer times.
  A wait adds to its interval wherever it falls inside the layer before the site, so when that layer
  visits the temperature drop tower or the prime tower the wait is written at the visit
  (`;MAGPIE_WAIT_VISIT`): where the nozzle has arrived over the tower, retracted, before its first
  line there (after the travel to it, or in a tool change made in place, before the tower
  generator's deretraction). It is a bare G4, the ooze falls on the tower and its lines re-prime the
  nozzle; there is no extra trip. When the layer before did not visit a tower (above the towers) the
  site parks at the park position. Timing the intervals from visit to visit instead would even the
  time at the tower, which does not matter, and makes waits wherever the visit moves within the layer
  (an alternating tool order); Gen chose the model's layer times (2026-10-20).

Waiting away from the part means the part fan does not blow on it: equal layer times do not mean
equal cooling, only closer to it.

## Limits

- The times are the estimator's; machine limits that differ from the firmware's shift them, mostly
  in proportion.
- Objects with different layer heights on one plate are blended by G-code layer.
