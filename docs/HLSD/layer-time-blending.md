# Layer time blending

Layer time blending adds waits after short layers so that the times of neighbouring layers stay
close, which keeps the cooling each layer gets even. It is the bundled native mod
`mods/layer_time_blend` ([native-mods.md](native-mods.md)): its library fills the `gcode.time_plan`
slot (`mods/sdk/include/magpie_slot_time_plan.h`) under the name `blend` and reads the blending
parameters itself; its rules write only what the core's export reads, the hidden
`layer_time_planner` (which marks the sites) and the `layer_time_blend_*` settings for where the
nozzle waits, the tower, the park position and the temperature estimate. The core marks the sites,
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
is not rounded, which would lift every layer around it. Both constraint sets are closed under the
pointwise minimum, so the smallest solution still exists; the planner alternates the two-pass
envelope with the upper concave hull of `T - C i^2 / 2` until nothing rises.

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
   match it.

Orca counts an M109 as no time. While blending runs with the low temperature support interface the
estimate follows each nozzle's temperature (`layer_time_blend_heat_rate` / `_cool_rate`) and counts
the M109 waits; those layers, and layers with a tool change, are flagged as estimated and reported
when a wait is next to them.

## Where the nozzle waits

- `in_place`: waits of 1 s and more lift the nozzle 1 mm over the new layer, wait and come down to
  the layer's Z (the lazy layer raise, done early); shorter ones are a bare G4.
- `tower` (the mod's default): a hollow 15 mm square tower with a 5-line brim, printed one loop
  per layer next to the objects (20 mm off their bounding box) or at the plate's
  `layer_time_blend_tower_x/_y`. The nozzle waits over its center, then prints the loop, which
  re-primes it. Each layer's part is a self-contained block that starts and ends retracted at the
  layer's Z, so the blocks above the last wait that needs the tower are left out; the planner picks
  the lowest cut that keeps every limit. With absolute E a left-out block that does not end with the
  writer's own `G92 E0` is replaced by a `G92` to the E it ended at, so the moves after it push no
  extra filament; on the flavors whose writer never resets E (Mach3, MakerWare, Sailfish) the tower
  stays whole. Not in by-object prints; a tower that does not fit parks instead.
- `park`: lift, travel to the park position (printer `layer_time_blend_park_x/_y`, rear left by
  default), wait, back to the same XY, down. Waits shorter than `in_place_max` or the trip wait in
  place.
- `purge_tower`: on a layer that visits the temperature drop tower or the prime tower, the site is
  where the nozzle has arrived over the tower, retracted, before its first line there: after the
  travel to it, or in a tool change made in place, before the tower generator's deretraction. The
  wait is a bare G4 and the tower's lines re-prime the nozzle; there is no extra trip. The site is
  then mid-layer, so the interval it measures runs from one visit to the next, and where the visit
  moves within the layer (a tool order that alternates) those intervals vary more than the layer
  times do. Layers above the towers park at the park position. A layer expected to visit a tower
  that did not still gets its site, in place at the layer's end (logged).

Waiting away from the part means the part fan does not blow on it: equal layer times do not mean
equal cooling, only closer to it.

## Limits

- The times are the estimator's; machine limits that differ from the firmware's shift them, mostly
  in proportion.
- Objects with different layer heights on one plate are blended by G-code layer.
