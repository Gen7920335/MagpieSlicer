# Tsunami Support — session record, 2026-08-15 to 2026-08-18

Written so that someone with no memory of the session can continue. Read this
after `AGENTS.md` and the `CODEX_HANDOFF.md` routing header, before touching
`src/libslic3r/Support/TsunamiSupport.cpp`.

Branch `vulkan-profiler-dev`, 27 commits from `e01f96f4f4` to `e389dc429e`,
nothing pushed. Working tree clean, no temporary instrumentation in the tree.

---

## 1. Where the suite actually stands

Run the whole tag, not a subset. **Five of the eleven Tsunami cases fail**, and
for most of this session only five cases were being run at all, which is why
several failures went unseen.

| case | time | result |
| --- | --- | --- |
| Selecting Tsunami does not generate support while support is disabled | 0 s | pass |
| Tsunami falls back for independent support-layer heights | 0 s | pass |
| Tsunami terminal ring feeds local tree tips into a separate interface stack | 3 s | pass |
| Tsunami slices a solid circular footprint with an external contour root | 11 s | pass |
| Tsunami covers every quadrant of a hollow gear overhang | 26 s | pass 32/32 |
| Tsunami micro branch reaches the hollow gear overhang | 87 s | pass 15/15 |
| Tsunami routes from snug overhang demand ... | 25 s | **2863/2864** |
| Tsunami micro branch serves the snug overhang behind the column | 82 s | **9/10** |
| Tsunami keeps unobstructed zero-angle support vertically aligned | 5 s | **2/4** |
| Tsunami covers every overhang of a multi-column model ... | 64 s | **176/320** |
| Tsunami slices a corpus of complex upstream test models | - | **does not finish** |

`ipadstand` alone is the corpus timeout: the other five corpus models total 12 s,
`ipadstand` was killed at 2399 s with the CPU pegged. Support disabled 0 s,
`stNormalAuto` 0 s, Tsunami over 200 s, and zero diagnostic lines in 120 s of
planning, so the time is in root and trunk search rather than the coverage loop.
Not caused by this session: the only change touching the macro path is the gain
floor, and it exceeds 240 s either way.

---

## 2. How to build and run

Build, then check the exit code explicitly. Never chain the build to the run
with `&&` and read only the tail: a failed build then reports the previous
binary's numbers as if they were new, which happened in this session.

    cmake --build build --target fff_print_tests --config Release > /tmp/build.log 2>&1
    rc=$?; [ $rc -ne 0 ] && grep -iE "error C[0-9]+|LNK" /tmp/build.log | head

Per-case timing, with a cap so nothing runs away, and a `pkill` because
`timeout` here kills the wrapper and leaves the exe running (that leftover
process burns a core and corrupts the next measurement):

    EXE=build/tests/fff_print/Release/fff_print_tests.exe
    for t in "Tsunami covers every quadrant of a hollow gear overhang" \
             "Tsunami micro branch reaches the hollow gear overhang" \
             "Tsunami routes from snug overhang demand instead of expanded contact-grid fragments" \
             "Tsunami micro branch serves the snug overhang behind the column" \
             "Tsunami terminal ring feeds local tree tips into a separate interface stack"; do
      s=$(date +%s)
      r=$(timeout 300 "$EXE" "$t" 2>&1 | grep -E "^assertions:|All tests passed" | head -1)
      printf '%5ss  %-56.56s %s\n' "$(( $(date +%s) - s ))" "$t" "${r:-TIMEOUT}"
      pkill -f fff_print_tests 2>/dev/null
    done

Reading the planner's own reasoning for one case:

    "$EXE" "<case name>" 2>&1 | grep -E "Tsunami (root selection|coverage stall|contact sampling|final coverage|fan-gap|micro reach|planning failed)" | sed 's/^.*\[warning\] //'

Long silent runs drop the Claude Code remote session: a server-side TTL treats a
long background task with no interaction as idle and disconnects at roughly
5-20 minutes (anthropics/claude-code#32050 and #32982). Keep runs short and emit
progress.

---

## 3. Diagnostics that now exist - read these before proposing any cause

All are marked `PERMANENT DIAGNOSTIC` and must not be removed with temporary
probes. Each was added because a failure was otherwise unattributable, and each
answered its question the same day it was added.

| line | fires when | says |
| --- | --- | --- |
| `Tsunami coverage stall:` | the branch coverage loop gives up | ribs, demand, uncovered, `branches_for_target`, and the rejection histogram including `no_plan_reasons` |
| `Tsunami fan-gap acceptance:` | a stalled plan is accepted anyway | residue split into `fan_gap_mm2` and `beyond_tips_mm2`. **Not a coverage guarantee** |
| `Tsunami micro reach rejection:` | micro is on and residue is out of reach | area, piece count, and the largest piece's centroid and size |
| `Tsunami contact sampling failed:` | the target could not be sampled | demand and spacing. Distinct from branches failing to cover |
| `Tsunami final coverage gate:` | planning finished but support misses part of the target | target and uncovered area |
| `Tsunami root selection:` | the chosen root does not span its target | candidates by kind, regions dropped as too far, and the winner's ribs, coverage, bed area, score |

`branches` in the stall line counts every branch the trunk placed across all
targets; `branches_for_target` is the one you usually want. Reading the pair as
one target's numbers produced a wrong conclusion in this session.

---

## 4. What changed, with the measurement that justified it

**Micro branch works on the hollow gear.** Credit given to a branch is clipped
to a disc of the micro reach available from that branch's terminal ring
(`micro_reach_disc`, shared with the residue gate so the two cannot drift).
`micro_tree_geometry` no longer fires; compiling the clip out brings it back
identically.

**The residue gate stopped blocking its own recovery.** It judged plans by
branch count, which passed a two-branch plan covering 8 % of a target and
suppressed the root retry that finds a fourteen-branch root. With micro on it
now tests the residue against terminal-ring reach.

**Micro branch angle default 25 -> 45 degrees.** Per-layer growth is
`min(tan(angle) * layer_height, extrusion_width * (1 - support_ratio))`; those
cross near 46 degrees at 0.2 mm layers and 0.42 mm width, so 25 bound the angle
at half the printability cap for nothing. 45 and 60 degrees give identical
geometry, which is what makes 45 a crossing point rather than a fitted value.
Residue outside reach fell 52.95 -> 1.50 mm2.

**Two branches per source turn when micro is on.** One per turn hands each turn
to a long branch that completes high, so its ring is born late with too little
height for a tree: the nearest ring sat 4.19 mm away and reached 3.8 mm. Two
takes out-of-reach residue to zero; three is identical to two. Gated on micro
because the payoff is the lower ring; with micro off the extra branch adds
geometry and moves coverage by nothing.

**A branch must earn its place.** `best_gain <= 0.` compared scaled area, where
1 mm2 is about 1e12, so gains of 5e-13 mm2 passed and built whole branches. The
gate is now one extrusion square. Measured on hollow gear target 1: the first
thirteen branches gain 7.9-13.2 mm2 each, the last two gained 2.05e-05 and
1.23e-06.

**Model clearance split from the bridging span.** One constant answered two
questions, so widening it under micro did one right thing and one wrong thing
together. Measured on the snug overhang against an independent analytic sector,
macro-only being 1.84 mm2:

| model clearance | bridging span | snug micro uncovered | terminal ring |
| --- | --- | --- | --- |
| wide | wide (the old shared value) | 6.04 mm2 | passes |
| narrow | narrow | 2.59 mm2 | fails |
| wide | narrow | 6.76 mm2 | fails |
| **narrow** | **wide** | **0.78 mm2** | **passes** |

**Fixtures measure coverage instead of asserting non-emptiness.** The old
quadrant check passed a plan leaving 93 % of a target uncovered. Both hollow
gear cases now gate the fraction within bridging distance, dilated by the test's
own rib spacing rather than the planner's constant. Micro beats macro-only in
every quadrant: 0.998/0.997/0.990/0.976 against 0.983/0.983/0.958/0.941.

**Every Tsunami fixture pins `tsunami_micro_branch_enabled`.** Seven inherited
the default, so flipping it would have silently turned macro-only cases into
micro cases failing on macro assertions.

**Two fixture assertions were unsatisfiable and one aborted its case.** The
multi-column per-overhang lookup indexed `support_layers[layer_index - 1]`,
which runs off the end for the topmost overhang because support stops one layer
below it, the same Z-gap bug already fixed on the hollow gear. Finding the
serving layer by `print_z` fixes it, and with the abort gone the case reports
144 failures it had been hiding.

---

## 5. The five failures, each with its cause named

**snug overhang, macro (2863/2864).** 1.84 mm2, collision-limited under rigid
parallelism. Unchanged by everything in this session.

**snug overhang, micro (9/10).** 0.783 mm2 against a 0.099 mm2 bar, down from
6.04. **Mechanism unknown.** Two hypotheses were measured and refuted: it is not
the column, since the residue is eight scattered pieces at radii 12-17 mm with
the largest 0.24 mm2; and it is not tip discretisation, since sweeping
`tsunami_micro_branch_size` gives 0.78 / 0.30 / 0.83 / 0.77 mm2 at 2.0 / 1.4 /
1.0 / 0.8 mm, which is non-monotonic. Do not adopt 1.4 as a default on that
evidence. What is certain is a mismatch: the planner **accepts** this target, so
every residue point lies inside some ring's reach, while the emitted geometry
leaves 0.78 mm2 outside bridging distance. A reach disc is filled; a built tree
is not. **The measurement that would settle it**: instrument
`plan_seeded_micro_tree` for the branch owning one residue piece and compare the
tree's emitted contacts against the disc that credited it, distinguishing "the
tree grew elsewhere" from "it grew toward it and fell short".

**zero-angle (2/4).** Three stacked causes, each confirmed by toggle. (1)
Contact sampling refuses the 1219.97 mm2 target at its 512-point cap, so Tsunami
never plans and the Normal fallback runs, which is what the two failing
assertions actually see (`no_sort` false, three paths instead of one). (2)
`tsunami_branch_angle` is 0 and reachability requires
`atan2(distance, height) <= angle`, so no candidate is ever evaluated;
`evaluated` goes 0 -> 244 when the angle is raised. This one is the fixture's
own premise, not a defect. (3) Even at a workable angle
`no_plan_reasons=[printability_limited=244]`, every branch refused for one
reason, making zero forward progress. Trunk-only coverage is 289 of 1219.97 mm2
at any angle. **Deciding needs a design answer, not more measurement**: should a
zero-angle configuration cover a 1250 mm2 target at all, or should the fixture
use a target a trunk alone can serve?

**multi-column (176/320).** Roots are undersized, and it is a reach limit rather
than a selection bug: `regions=4 too_far=3 candidates=18 ribs=2 coverage=0.143`
against the hollow gear's `regions=1 too_far=0 candidates=12 ribs=31
coverage=1.000`. Three of four contour regions sit beyond `maximum_xy_distance`
of 13.3 mm, so one region supplies every candidate and its best spans a seventh
of the target. The score is arithmetically right, 142.7 being 1000 x 0.143;
there is nothing better to choose. The hollow gear never showed this because its
one region covers the target outright, so **its health was luck of geometry
rather than evidence the stage works**. The remaining
`support_islands.size() >= target_regions.size()` check is a count proxy of the
kind removed elsewhere but is **deliberately left in place**: while planning
falls back that count describes Normal's output, so deleting it would hide the
sizing defect rather than a bad assertion.

**corpus / ipadstand (unbounded).** Located to Tsunami's root and trunk search;
see section 1. Not profiled.

---

## 6. Retracted claims - do not re-derive these

Nine causes were stated in this session and later refuted by measurement.

| claim | what measurement said |
| --- | --- |
| "A sector with no branch" is the hollow gear residue | It sits inside an occupied segment's served band, 0.55 mm lateral against a 0.95 mm half width |
| Three micro couplings explain micro-mode behaviour | `allow_direct_projection` has no effect; turn ordering and `complete_early` are necessities, not side effects |
| The bridge constant also mattered as sampling spacing | Splitting them and narrowing only the sampling left the branch count unchanged |
| Micro Branch solves the snug overhang | Against an independent sector micro was worse, 6.04 against 1.84 mm2 |
| The three uses of the bridge constant collapse to one value | The terminal ring regression refuted it; the two act in opposite directions |
| The terminal ring regression came from model clearance | Forcing model clearance back wide while the span stayed narrow still failed |
| Gain estimate and outcome disagree | The estimate was accurate; the threshold had no units |
| The 512-point sampling cap is not a cause of zero-angle | It **is** one of three; the counterfactual was judged by the test result instead of `cap_hit` |
| The direct-projection early return causes undersized roots | `direct_candidates=0` on both fixtures, so that path never runs there |

The pattern in all nine: a mechanism was reasoned out from reading code, in a
stage that had no observable. Every one was settled the moment a diagnostic
existed. **Add the observable first.**

---

## 7. Rules this session added to `AGENTS.md`

Section 8 gained the following, phrased as requirements on artifacts rather than
principles, because the three rules already there had each been broken by the
session that wrote them:

- **Comparisons carry their units.** Two bugs came from the same hole: a gain
  threshold compared in scaled area, and a miter limit reaching Clipper as an
  unscaled arc tolerance.
- **One quantity, one function.** Six diagnosed defects were an estimating stage
  measuring a proxy for what a later stage builds. Its mirror: one constant
  answering two questions is two quantities sharing a name.
- **Attribution requires a counterfactual**, judged by the mechanism rather than
  the test result. Stacked causes are normal, so a test that still fails proves
  nothing. If no observable names the mechanism, add one first.
- **A refusal counter needs its reasons.** `no_plan` stood for seven distinct
  refusals; split, it said all 244 were one reason.
- **Fixtures do not borrow the constants they judge.**
- **Temporary probes live on their own lines** and are removed by restoring the
  file, never by deleting lines matching a marker. That strip destroyed working
  code twice in one session: nine `return` statements once, a sampling call the
  second time.

The step gate (section 5 of `AGENTS.md`) and the completion report (section 12)
were wired to two of these so they are checked rather than merely read.

---

## 8. Where to go next, in the order I would take it

1. **snug micro, 0.783 mm2.** The named measurement in section 5. Bounded, and
   it closes the last coverage gap on a fixture that already improved 8x.
2. **multi-column reach limit.** Decide whether a target whose contour regions
   all sit beyond `maximum_xy_distance` should get multiple roots. The
   "multiple roots per target" item in `CODEX_HANDOFF.md` was refuted **on the
   hollow gear**, where reach was measured not to bind; on multi-column it does
   bind. Neither verdict transfers between models, and the root selection line
   now says which case a model is in.
3. **zero-angle.** Needs the design answer in section 5 before any code.
4. **ipadstand.** Profile root and trunk search. Longest cycle, and no fixture
   depends on it beyond the corpus case.
5. **Micro as the shipping default.** Micro planning costs about 3.3x macro
   (26 -> 87 s, 25 -> 82 s), unattributed; the candidates are the uncached
   `plan_terminal_ring` call inside `micro_reach_disc` and the doubled candidate
   loop. The temporary fan-gap acceptance can only be deleted once micro is the
   default, since macro alone cannot close fan gaps.

Still open and untouched: `docs/PROJECT_CONTRACTS.md` is untracked though
`AGENTS.md` names it as required reading; the section 7.0.1 depth sweep removal;
and three uncalibrated defaults (micro angle 45, trunk thickness 4.0, branch
minimum spacing 0.35), each marked uncalibrated at its declaration.
