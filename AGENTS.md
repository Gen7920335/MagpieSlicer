# Magpie Slicer Agent Contract

## 1. Engineering Priority

When goals conflict, use this order:

1. Correctness and printer safety.
2. Project and feature invariants, including feature-off equivalence.
3. Evidence, reproducibility, and root-cause correctness.
4. Regression compatibility.
5. Determinism and numerical/geometry robustness.
6. Testability and maintainability.
7. Simplicity and minimum necessary diff.
8. Context/token efficiency.
9. Runtime and memory efficiency.
10. Code elegance.

Context efficiency never justifies guessing, skipping required source inspection, skipping reproduction, or skipping required verification.

## 2. Deterministic Context Routing

At task start, inspect in this order:

1. `AGENTS.md`.
2. Contract sections selected by the routing rules below.
3. `CODEX_HANDOFF.md` when required by the handoff rules below.
4. Directly relevant source and tests.
5. `git status --short --branch`.
6. Relevant current diffs.

Treat contracts and handoff/history as constraints and leads, not as proof of current implementation behavior. Verify implementation claims against current source and tests.

Select contracts from the task domain, referenced symbols, touched files, settings, tests, and generated output. Do not rely only on literal user wording.

### Contract Routing

| Task trigger | Required reading |
|---|---|
| Any production-code, configuration, serialization, geometry, G-code, compatibility, build, installer, or release task | The relevant section of `docs/PROJECT_CONTRACTS.md` |
| Nozzle, hotend, extrusion width, wall count, Classic, Arachne, tool mapping, filament mapping, or interlocking | `docs/PROJECT_CONTRACTS.md` sections 2, 3, 7, 8, and 10 as applicable |
| Prusa/Normal Support, Cura Support, Tree/Organic Support, interface, raft, support pattern, or support serialization | `docs/PROJECT_CONTRACTS.md` sections 2, 4, 7, 8, and 10 as applicable |
| Tsunami, Root, Trunk, Branch Guide, Rib, Virtual Rib, Physical Rib, Orientation Field, U-turn, Active Frontier, terminal ring, or Micro Branch | The relevant section of `docs/TSUNAMI_SUPPORT_CONTRACT.md` and `docs/PROJECT_CONTRACTS.md` sections 2, 4, 7, 8, and 10 as applicable |
| Low-temperature interface, temperature tower, AUX cooling, wiping, interface temperature, or reheating | `docs/PROJECT_CONTRACTS.md` sections 2, 5, 7, 8, and 10 as applicable |
| Vulkan, GPU, CPU fallback, dispatch, overflow, device buffer, or profiler | `docs/PROJECT_CONTRACTS.md` sections 2, 8, 9, 11, and 12 as applicable |
| Snapmaker, LESIC, branding, installer, packaging, application identity, file association, or release | `docs/PROJECT_CONTRACTS.md` sections 6, 10, 11, 12, and 13 as applicable |
| Performance, profiling, benchmark, hot loop, allocation, cache, batching, parallelism, latency, throughput, or asymptotic-work changes in any feature domain | Add `docs/PROJECT_CONTRACTS.md` section 9 to the feature-specific sections above |
| Build or test execution | Add `docs/PROJECT_CONTRACTS.md` sections 11 and 12; add section 13 for packaging or release work |

When a task crosses domains, load every applicable contract section.

When relevance is uncertain, read the narrowest plausible contract heading. Uncertainty is not a reason to skip a potentially applicable contract.

Search or read the relevant heading first. Do not load an entire conditional document when one or two sections are sufficient.

### Handoff Routing

Always inspect `CODEX_HANDOFF.md` when any of the following applies:

- the task concerns Tsunami Support;
- the task continues prior work;
- the user refers to an existing implementation, previous attempt, known bug, failed approach, or remaining task;
- relevant working-tree changes already exist;
- the result depends on current implementation status rather than only the durable project contract.

For an isolated, unrelated task with no relevant existing changes, `CODEX_HANDOFF.md` may be skipped.

If the handoff is long, inspect its routing header first and then only the relevant detailed history.

Preserve applicable design decisions, failed approaches, `DO NOT REPEAT` items, known defects, and unresolved work, but verify them against current source before relying on them.

`CODEX_HANDOFF.md` must keep a compact routing header near the top containing:

- Current scope
- Affected components
- Current implementation phase
- Known failures / `DO NOT REPEAT`
- Unresolved decisions
- Next verified step

Keep detailed chronological history below that header. Refresh the header without deleting, rewriting, or reordering the underlying history. Agents read the routing header first and load detailed history only when relevant.

Preserve user changes and untracked files. Never clean, delete, move, overwrite, reformat, reset, checkout, or revert them without explicit approval.

## 3. Authorization Boundaries

- Do not edit production code without an explicit implementation request.
- Audits, reviews, diagnoses, and planning remain read-only except for explicitly requested documentation or artifacts.
- Implementation authorization does not automatically authorize build or test execution.
- Do not build or run tests unless the user explicitly authorizes that execution.
- When build/test execution is not authorized, perform all permitted static verification, state that runtime verification remains blocked, and do not claim the task is fully verified or complete.
- Do not push, publish, release, commit, reset, checkout, revert, package, or operate a physical printer unless explicitly requested.

## 4. Single Work Loop

Use one loop for debugging and implementation:

OBSERVE -> REPRODUCE / MINIMIZE -> LOCATE THE FIRST BAD STATE -> FORM ONE FALSIFIABLE HYPOTHESIS -> VERIFY THE HYPOTHESIS -> PLAN ONE STEP -> IMPLEMENT ONE STEP -> VERIFY THAT STEP -> REVIEW THE RESULT -> ACCEPT OR REVISE -> ONLY THEN CONTINUE

Keep one primary hypothesis at a time. It must state:

- what is wrong;
- where it first becomes wrong;
- why it becomes wrong;
- what observation proves or refutes it.

Fix the owning classification, planning, topology, geometry, clipping, serialization, state-management, or extrusion stage. Never repair a structural defect only in preview or final G-code.

## 5. Step Definition and Verification Gate

A **step** is the smallest behaviorally coherent unit with:

- a defined input;
- a defined output;
- protected invariants;
- an independently meaningful verification.

A step is **not** every file edit, function edit, compiler-fix iteration, or shell command.

For staged pipelines, use the pipeline stage or one independently verifiable stage transition as the step. Tsunami-specific stage boundaries are defined in `docs/TSUNAMI_SUPPORT_CONTRACT.md`.

A step passes only when all applicable conditions hold:

1. Its intended behavior is implemented.
2. Relevant invariants still hold.
3. Its output is directly inspected, measured, or otherwise verified.
4. The smallest relevant regression, unit, or reproduction case passes when execution is authorized.
5. No new unexplained failure or regression remains.
6. The next step can safely use this result as an input.
7. Every threshold the step introduced or moved names its quantity and units at its declaration (§8, Comparisons Carry Their Units).
8. Every cause the step records anywhere was toggled and observed, or is labelled `UNKNOWN` with the measurement that would settle it (§8, Attribution Requires A Counterfactual).

If a gate fails:

1. Do not start the next step.
2. Find the first bad state inside the current step.
3. Re-evaluate the hypothesis.
4. Revise the current step, or the higher-level plan only if evidence invalidates it.
5. Re-run the same gate.

Do not pre-implement later stages to conceal or compensate for an unverified earlier stage.

Maintain a compact step record internally:

- Step
- Invariants checked
- Tests/checks
- Observed result
- Remaining issue
- Next-step readiness: YES / NO

Surface that record only when a gate fails, user input is required, a checkpoint is requested, or in the final report.

## 6. Planning and Design Selection

Before a substantive behavior change, identify:

- the existing owning abstraction or stage;
- protected invariants;
- affected data flow;
- intended files;
- regression boundaries;
- verification method.

Do not finalize implementation details before understanding current architecture.

Compare 2-3 realistic alternatives only for architecture-sensitive work, such as:

- new ownership or persistent state;
- geometry or routing architecture;
- cross-Orca abstraction changes;
- major cache/state-model changes;
- large performance work.

Compare correctness, invariant preservation, architecture fit, regression risk, determinism, numerical robustness, testability, complexity, runtime, memory, maintainability, and extensibility.

Do not invent alternatives for an obvious local fix.

Stop design exploration when the simplest verifiable solution satisfies the requirements and invariants, survives known counterexamples, fits current architecture, is numerically stable, and does not introduce an unacceptable measured performance problem.

Replan only when evidence disproves the root-cause or architecture assumption, an invariant cannot hold, ownership conflicts appear, unrelated edits spread, scope expands sharply, or the selected design loses its demonstrated advantage. Handle ordinary compiler errors and local implementation mistakes within the current step.

### Critical Path Before Action

Before proposing or taking the next action — a measurement, an edit, a build — state **which decision it informs and what will differ between its possible outcomes**. If no outcome changes what you do next, it is not the next action, however interesting it is.

A measurement result is a fact, not an assignment. The obstacle a measurement exposes is usually not the thing that gates the decision, and converting the newest fact into the next task is the most common way to spend a session moving without progressing. Ask what the decision is, then ask which unknown gates it, then act on that unknown only.

Apply this to work you would not naturally label a design decision. The planning requirements above — owning stage, invariants, data flow, files, regression boundary, verification method, and 2-3 alternatives for architecture-sensitive work — are triggered by what a change *touches*, never by how you framed it to yourself. "Fixing what the measurement showed" in routing, geometry, ownership or selection logic is architecture-sensitive work and needs the same treatment as a change announced as a redesign.

Three checks that would each have caught a wasted step in practice:

- **Progress:** for any loop or retry, name what decreases each iteration and the mechanism forcing it. If the mechanism cannot be named, it does not converge.
- **Decision value:** for any measurement, name the branch it decides. Attributing a result between two changes you intend to discard decides nothing.
- **Stage ownership:** for any patch, ask whether the stage being patched is one an approved design replaces. Strengthening a stage that is slated for removal is waste even when the patch works.

## 7. Editing and Architecture Rules

- Reuse existing Orca/Magpie abstractions and typed APIs when they correctly own the behavior.
- Add an abstraction only when it establishes one clear semantic owner or removes demonstrated duplication.
- Keep geometry/planning decisions separate from toolpath ordering, G-code emission, and preview.
- Prefer immutable plans and stable IDs over reconstructing persistent identity from spatial proximity.
- Make forbidden state changes difficult to represent in data structures.
- Use deterministic ordering, explicit tie-breaks, and hysteresis around discrete numerical state changes.
- Keep changes scoped. Do not mix unrelated refactors, renames, formatting, cleanup, or speculative frameworks into the patch.
- If a local patch keeps expanding, stop and reassess the hypothesis and ownership boundary.
- Before the first edit, give one concise preamble naming the intended files, owning subsystem, protected invariant/behavior, and supporting evidence. Do not repeat that preamble for every file.

## 8. Evidence, Tests, and Robustness

For numerical or geometry work, apply the coordinate and edge-case contract in `docs/PROJECT_CONTRACTS.md` section 8. Feature-specific adversarial cases remain in the applicable feature contract.

For every threshold, consider:

- threshold minus epsilon;
- exact threshold;
- threshold plus epsilon.

Every bug fix should add the smallest practical root-condition regression test where feasible.

Verify narrow to broad:

1. smallest affected unit/regression;
2. affected subsystem;
3. slicing/integration;
4. justified broader regression suites.

Do not repeat a failed command without changed code, hypothesis, input, or environment.

After implementation:

- try to falsify the solution with likely counterexamples;
- review the final diff for ownership, lifetime, unit, index-domain, coordinate-space, epsilon, ordering, cache, feature-off, and regression errors;
- remove or gate diagnostics.

### Temporary Probes Live On Their Own Lines

A probe never shares a line with code that must survive it, and it is never removed by
deleting lines that match a marker. Restore the file from the last commit and re-apply
what should stay.

Twice in one session a marker-based strip destroyed working code: nine `return`
statements in one instance and a sampling call in the other, both because the marker
had been appended to an existing line. The damage is silent until the build breaks,
and it breaks far from the edit.

### Permanent Failure Diagnostics

A diagnostic that reports **why a failure path was taken** is not temporary instrumentation and must not be deleted with it. Keep one when all of these hold:

- it runs only on a failure path, or its success-path cost is a few counter increments;
- the failure it explains otherwise forces a fallback or an abort that hides its own cause;
- reconstructing it requires a build and a run.

Mark such a diagnostic `PERMANENT DIAGNOSTIC` at its declaration and list it in `CODEX_HANDOFF.md`. Temporary profiling and probe scaffolding is still removed at the end of its step; only these survive.

The rule exists because the same probe was hand-written and deleted in three separate sessions, and each session that lost it began by inferring values it could have read. Prefer a permanent failure diagnostic over a comment recording what a past session measured.

### Bound Versus Outcome

When a value passes through a search, score, clamp, or cap, the code that supplies it tells you its **bound**, not the value that comes out. Do not assert the outcome without observing it. Before claiming any planner value, check whether the receiving function compares candidates; if it does, the number you read is an input to a search and the selected value is unknown until printed.

State causal claims as `KNOWN` (observed), `ASSUMED` (current hypothesis), or `UNKNOWN`. Never record an `ASSUMED` claim in a contract or handoff document as if it were measured.

Before proposing any iterative recovery loop, state its progress invariant: what decreases each iteration, by at least how much, and the mechanism that forces it. If the mechanism cannot be named, the loop does not converge.

### Why The Rules Below Are Phrased As Artifacts

The three rules above this line were written after real failures and then broken by
the sessions that wrote them. `Bound Versus Outcome` was authored and violated twice
in its own session. The fault is the form, not the discipline: a principle states
what is true and leaves you to remember it at the one moment it applies.

Each rule below is therefore attached to something the work already has to produce --
a declaration, a commit message, a handoff entry, a test. Skipping the rule leaves a
visible hole in that artifact rather than a thought you failed to have.

### Comparisons Carry Their Units

At every threshold, tolerance, or epsilon, the declaration states the physical
quantity and its units. When the comparison happens in scaled coordinates, say so and
give the millimetre equivalent.

```cpp
// Minimum useful gain: one extrusion square, in scaled area (1 mm2 ~ 1e12).
const double minimum_useful_gain =
    double(scale_(extrusion_width)) * double(scale_(extrusion_width));
```

A bare literal in a comparison is a defect until it carries this. Two separate bugs
came from the same hole: `best_gain <= 0.` compared scaled area and admitted gains of
5e-13 mm2, and `DefaultLineMiterLimit` reached Clipper as an unscaled `ArcTolerance`
and produced ~2000-segment arcs. Both are visible the instant the units are written
next to the number, and invisible otherwise.

### One Quantity, One Function

When an estimating stage and a building stage answer the same physical question, they
call **one function**. Two implementations of one quantity drift, and the drift lands
as coverage credited but not delivered.

Before adding any estimate, name the code that will later do the real thing. If that
code computes the same quantity, extract it and call it from both. If it cannot be
extracted, write in the commit message why, and what keeps the two in step.

This is the single largest defect class in this project: rib depth, reachability, the
trunk arc, the coverage estimate, micro credit, and the residue gate were all one
stage measuring a proxy while the next stage required the whole thing.

A constant is subject to the same rule in reverse. One constant answering two
different questions is two quantities sharing a name, and tuning it for one silently
moves the other -- `maximum_bridge_distance` was model clearance and bridging span at
once, so widening it for micro did one right thing and one wrong thing together.
Split on the question asked, not on the value that happens to fit.

### Attribution Requires A Counterfactual

A cause may not be written into a comment, commit message, or handoff entry unless
the suspected cause was **toggled** and the effect was observed to appear or
disappear. Measuring a symptom and reasoning to a cause is not attribution.

The counterfactual belongs in the same artifact as the claim:

> Compiling the clip out brings `micro_tree_geometry` back identically, which is the
> attribution.

Without that sentence the claim is `ASSUMED` and must be labelled so. Three wrong
causes were recorded as fact in one session for want of this: an angular-pitch
calculation became "a sector with no branch" when the residue was inside an occupied
segment's band; a list of micro couplings named three that measurement showed were
one no-op and two necessities; and a terminal ring regression was blamed on model
clearance when forcing model clearance back still failed.

When a toggle is not cheap, the honest output is `UNKNOWN` plus the measurement that
would settle it. `UNKNOWN` with a named next measurement is a finished step. A
plausible story is not.

**Judge a counterfactual by the mechanism, not by the test result.** Toggling the
suspected cause must move a number that names that mechanism -- a counter, a flag, a
printed measurement. A test that still fails proves nothing when several causes are
stacked, and stacking is normal: the zero-angle fixture had three, so raising the
sampling cap left the same 2 of 4 failing and the cap was wrongly dismissed as
irrelevant. If no observable names the mechanism, add one before running the
experiment. `UNKNOWN` is the honest result when the toggle changes nothing observable.

**A refusal counter needs its reasons.** When a stage can decline for several distinct
reasons, count them separately from the start. `no_plan` stood for seven different
geometric refusals; splitting it showed all 244 were a single reason, which no amount
of reading the aggregate could have said.

### Fixtures Do Not Borrow The Constants They Judge

A test's pass criterion is computed from the test's own values. A fixture that
dilates by the planner's `maximum_bridge_distance`, or compares the planner's own
`uncovered_mm2`, lets the code under test choose its own grade -- when the planner
redefines the quantity, the number improves while the part gets worse.

Measured: enabling micro moved the planner's snug residue from 1.84 to 0.267 mm2
while the emitted geometry covered **less** of an independent analytic sector, 6.04
against 1.84 mm2, because the same flag shrank the demand the residue was measured
against. The fixture caught it only because its sector and its dilation were its own.

Two consequences:

- Never compare a planner-internal number across a configuration change that alters
  that number's definition. Say which region a demand figure is over, or do not quote
  it.
- Assert an outcome fraction, not a structural property. Non-emptiness, entity
  counts, and polyline identity all pass while coverage collapses: the hollow gear
  fixture passed with 93 % of a target uncovered. Gate a measured fraction, record
  the measured values in a comment, and set the bar below the worst with room.

### Defect Root Causes And Their Rules

Every defect this project has diagnosed falls in one of four classes. A new defect
that fits none of them is worth a new rule; one that fits is a rule that was skipped.

| Class | What it looks like | Rule |
| --- | --- | --- |
| Proxy for the whole | An early stage validates a point, a bound, or a count; a later stage needs the built thing | One Quantity, One Function; Bound Versus Outcome |
| Unitless comparison | A literal threshold in scaled coordinates, or a value passed into an argument measured in different units | Comparisons Carry Their Units |
| Story for a cause | A cause inferred from arithmetic or from one measurement, recorded as fact | Attribution Requires A Counterfactual |
| Self-graded outcome | A fixture, or a report, built from the constants of the thing it judges | Fixtures Do Not Borrow The Constants They Judge |

Known instances, for recognising the shapes: rib depth (bound versus selected), branch
reachability (endpoint versus source pairing), trunk arc (approach point versus built
arc), coverage estimate (aimed centreline versus fixed normal), micro credit
(bridgeable distance versus remaining height), residue gate (branch count versus
actual reach), micro reach disc (filled disc versus a built tree's discrete contacts),
Clipper arc tolerance (miter limit as unscaled tolerance), acceptance threshold
(scaled area read as mm2), shared bridge constant (two questions, one number).

## 9. Context and Tool Efficiency

Use this investigation ladder:

1. exact symbol, config, test, error, or entry-point search;
2. focused relevant ranges;
3. direct callers and relevant callees;
4. nearby tests;
5. subsystem inspection;
6. cross-subsystem or repository-wide inspection only when evidence requires it.

Every tool call must answer a concrete question.

Do not repeatedly reopen unchanged files, rerun unchanged commands, regenerate settled plans, restate confirmed architecture, or dump large irrelevant logs.

For complex work, maintain a compact decision state only when useful:

- `KNOWN`: directly confirmed;
- `ASSUMED`: current hypothesis;
- `UNKNOWN`: unresolved fact that could change correctness, architecture, regression, testing, numerical behavior, or measured performance.

Resolve only decision-relevant unknowns.

Batch independent read-only discovery when practical. Keep result-dependent debugging commands sequential. Do not impose an artificial fixed shell-call count when more evidence is required for correctness.

### Bounded Execution

Every potentially long-running command must have:

- an explicit or repository-established timeout;
- an expected progress signal;
- an expected output artifact or decision;
- a reason it is necessary at the current verification gate.

A single step must not run beyond 20 minutes without an explicit progress-based reason.

If a command stalls or times out:

1. Cancel it.
2. Identify whether the cause is workload size, deadlock, configuration, environment, invalid input, or an incorrect plan.
3. Retry only after changing the cause, input, command, or plan.

If the same step produces two consecutive non-informative failures despite a changed attempt, stop execution and re-evaluate the step plan before issuing more commands.

Do not impose a fixed shell-call count when additional evidence is required for correctness. Conversely, do not continue issuing calls that no longer reduce decision-relevant uncertainty.

Stop investigation when root cause, ownership, patch location, regression boundary, and verification strategy are sufficiently established.

## 10. Performance

Optimize only after correctness and representative measurement.

Prefer:

1. eliminating unnecessary work;
2. reducing algorithmic work;
3. incremental or active-state processing;
4. lazy materialization;
5. spatial/cache reuse with explicit invalidation;
6. allocation reduction;
7. micro-optimization.

Every cache must define its key, owner, validity lifetime, and invalidation rule.

Do not claim a speedup without comparable before/after evidence. Optimization must not silently change normalized geometry, tool motion, extrusion, assignment, or G-code behavior.

## 11. Definition of Done

A coding task is complete only when, as applicable:

- the requirement or root cause is confirmed;
- the owning stage was changed;
- protected invariants hold;
- feature-off/upstream behavior is preserved;
- relevant serialization, invalidation, ownership, lifetime, units, and index domains are correct;
- required step gates passed;
- relevant tests and counterexamples passed when execution was authorized;
- the final diff was self-reviewed;
- performance claims are measured;
- no relevant unexplained behavior remains.

Compilation alone, a plausible preview, or one happy-path result is not completion.

If execution was not authorized, report the code change as statically reviewed but not runtime-verified.

## 12. Completion Report

Report only decision-relevant evidence:

### Root Cause / Design Decision
Confirmed cause or selected design. State the **counterfactual** that confirmed it -- what was toggled and what changed -- or label the cause `ASSUMED` or `UNKNOWN`. A cause with no counterfactual is not a root cause, whatever else supports it.

### Changed
Files/components and purpose.

### Verified
Checks, tests, and actual results.

### Regression
Existing behavior and feature-off equivalence checked.

### Performance
Only when relevant, with before/after evidence.

### Remaining
Only genuine limitations, blocked verification, or unresolved uncertainty.

Do not provide a chronological command diary.

## 13. Permanent DO NOT

- Do not conceal wrong geometry in preview or final G-code.
- Do not hide an unknown cause with unconditional skips, model/layer/coordinate special cases, unexplained epsilon/clamp changes, or unproven fallback.
- Do not let disabled Magpie features alter upstream behavior.
- Do not proceed to a later implementation stage while an earlier required stage is unverified.
- Do not compensate for an unverified earlier stage in a later stage.
- Do not save tokens by omitting evidence required for correctness.
- Do not continue exploring after decision-relevant uncertainty is resolved.
- Do not write a bare numeric literal into a comparison without its quantity and units at the declaration.
- Do not let an estimating stage and the stage that builds the thing compute the same quantity twice.
- Do not give one constant two different questions to answer; split on the question, not on the value that fits.
- Do not record a cause that was not toggled and observed.
- Do not build a fixture's pass criterion from the constants of the code it judges.
- Do not compare a planner-internal figure across a configuration change that redefines it.
