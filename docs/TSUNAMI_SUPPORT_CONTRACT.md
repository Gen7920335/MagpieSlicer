# Tsunami Support Contract

Read this document only for Tsunami Support design, implementation, review, debugging, or verification.

`CODEX_HANDOFF.md` records evolving implementation state, failed approaches, and next tasks. Verify it against current code. This document owns the durable Tsunami design contract.

A change that violates this contract is a design error even if it compiles, slices, benchmarks well, or looks correct in preview.

# 1. Non-Negotiable Invariants

1. Zigzag geometry exists only in XY.
2. At birth, a Straight Rib's XY origin, direction, Virtual Rib index, and birth layer become immutable.
3. A born Straight Rib stacks vertically thereafter.
4. Existing Straight Ribs are never translated, rotated, rephased, repurposed, or reconstructed from a later layer's polygon.
5. All lateral propagation occurs only through an Active U-turn.
6. A grown U-turn may create a new Physical Rib at the next valid Virtual Rib.
7. Virtual Rib Field and Orientation Field persist analytically even where zero Physical Ribs are locally printable.
8. Rib direction is normally perpendicular to the local Branch Guide tangent.
9. Guide curvature may rotate only future Ribs through a continuous, wrap-safe Orientation Field.
10. Tsunami Roots begin on the bed.
11. Support bodies remain outside model clearance.
12. Branches do not reattach to the model mid-span.
13. Model contact is limited to intended Target/interface contact.
14. Normal, Cura Normal, Tree/Organic, and Tsunami support remain independent.
15. Tsunami disabled, support disabled, and other support selections preserve established serialization, geometry, toolpath, preview routing, and G-code.
16. Continuous extrusion is preferred only after correctness, stability, clearance, and printability.
17. Each U-branch's two Straight Ribs remain parallel to each other and to the local source Trunk Straight Rib; this parallelism is local, not global.
18. Route-orientation changes occur only in branch-free connector gaps between adjacent U-branch modules, never inside a branch U-turn or along a branch-bearing Straight Rib. Such a connector remains Active U-turn/frontier geometry and never rotates a born Straight Rib.
19. Non-connected Branches maintain the configured clear XY spacing between actual extrusion exteriors throughout every layer in which their swept footprints coexist.

# 2. Pipeline and Step Boundaries

The Tsunami pipeline is:

1. Target Detection
2. Root Selection
3. Branch Guide
4. Virtual Topology
5. Physical Geometry
6. Printability Validation
7. Toolpath Generation
8. Toolpath Ordering
9. Interface / optional Micro Branch

For Tsunami work, a **step** is one behaviorally coherent stage or stage transition with independently verifiable input and output.

Examples:

- Target set -> selected Root plan
- Branch Guide -> Virtual Rib Field
- Virtual Rib Field -> born Physical Rib set
- Active U-turn state -> next-layer growth and Rib birth
- validated structural geometry -> ordered extrusion path

A step is not one file, one function, one compiler fix, or one command.

Do not advance to a dependent stage until the current stage passes its verification gate.

# 3. Branch Angle Semantics

- Interpret Branch Angle as the maximum angle from vertical.
- Convert it to per-layer lateral growth using layer height, extrusion width, U-turn geometry, prior-layer overlap, and local curvature.
- Slower growth, including zero lateral growth, is valid.
- Generated geometry may be more vertical than configured.
- Generated geometry must never become more horizontal than configured.
- Do not force the requested average angle when local printability requires lower growth.

# 4. Root Selection

Root planning uses:

- model bed-contact footprint;
- Target XY projection;
- collision-free bed space;
- Branch Angle reachability;
- model clearance;
- overturning stability;
- deterministic route cost.

Rules:

- Root lies on the bed.
- Prefer free space outside the model footprint, not the model-interior side.
- Do not choose a Root inside the model footprint merely because vertical projection is convenient.
- Prefer deterministic offsets from suitable model/support boundaries so Zigzag direction and Straight Rib length remain stable.
- Use the largest suitable reachable bed-contact region only after distance, route, angle, collision, and stability are considered.
- A local Root section may contain zero printable Physical Ribs while retaining Virtual Rib and Orientation Fields.
- Do not emit a printable connected support component unless it has a valid physical extrusion path and supported ancestry.
- For annular or other closed Roots, minimally and deterministically correct Rib Spacing when necessary to close phase without a seam.

# 5. Trunk

- Keep geometry vertically unchanged through Trunk Height.
- Keep one continuous Trunk extrusion where safe.
- Do not force continuity through invalid topology, collision, or structural risk.
- Under an explicit stability rule, the starting U-turn may extend one Straight Rib away from the model to resist overturning.
- Do not add unrelated braces or move existing Ribs.
- A safe reduced Trunk Height may be considered only as an explicit reachability fallback.

# 6. Virtual Rib Field and Orientation Field

Represent Virtual Rib Field analytically where practical using:

- origin;
- orientation;
- Rib Spacing;
- phase;
- stable Rib index.

Rules:

- Rib Spacing is an independent centerline parameter.
- Fix origin, phase, orientation reference, and index convention when the Branch is created.
- Do not refit phase to each layer's polygon center.
- Materialize only nearby or currently needed Virtual Ribs.
- Preserve the field through narrow regions with zero Physical Ribs.
- Use vector or equivalent wrap-safe orientation representation.
- Existing Ribs retain birth orientation; only future Ribs follow a changing Orientation Field.

# 7. Physical Rib Rules

A Physical Rib is born only when a Virtual Rib has sufficient printable intersection and supported ancestry.

At birth, freeze:

- XY origin;
- direction;
- Virtual Rib index;
- birth layer;
- main printable length for that born Rib.

Use deterministic activation and retention hysteresis.

Do not allow threshold noise to create repeated birth/death oscillation across adjacent layers.

# 8. U-Turn and Lateral Growth

The U-turn is the only lateral-growth element.

Every U-turn must:

- attach tangentially to its Straight Rib anchors;
- remain inside the preplanned collision-free swept Branch corridor;
- preserve required prior-layer extrusion support;
- satisfy minimum curvature;
- respect model clearance;
- respect Branch Angle.

The intended support domain is the preplanned swept corridor, not merely the already-materialized polygon of the current layer.

A U-turn need not be a perfect semicircle. A tangent-continuous capsule-like or other printable curve is valid when it better satisfies overlap and curvature.

A new Physical Rib is born when the Active U-turn reaches the next valid Virtual Rib and all activation conditions pass.

# 9. Branch Guide, Direction Changes, Split, and Merge

- Branch Guide is a planning/orientation reference, not extrusion geometry.
- Existing Ribs never rotate when the Guide turns.
- Future Rib orientation changes gradually through the Orientation Field.
- Support same-Z/different-XY, similar-XY/different-Z, and fully different-XYZ Targets.
- Prefer a shared Trunk and connected U-shaped Branch loops when structurally valid.
- Multiple U-shaped Branches may be connected like a bulb filament to reduce discontinuity.
- A later similar-XY/different-Z Target may branch from a compatible convex U-turn.
- Branch Split creates a new Active Frontier; it does not move or cut existing Ribs.
- Merge is allowed only when height, orientation, spacing, width, stability, removal, and path cost are compatible.
- Continuity never overrides stability or valid topology.
- Do not aim or rotate an entire U-branch directly toward a Target endpoint. Derive its orientation from its local source Trunk Straight Rib.
- Generate full-length and event-bounded shorter U-branch candidates together. Events include Target boundaries, model-clearance boundaries, neighboring-Branch spacing boundaries, and minimum printable/anchor length.
- `Tsunami Branch Minimum Spacing` is the clear XY distance between actual extrusion exteriors, not centerlines.
- Evaluate candidate compatibility with per-layer swept extrusion footprints, including future growth, connector geometry, U-turns, and terminal geometry. Only the intended topological attachment to the source Trunk is exempt.
- Couple connector angle, module position, and Branch length. Shorten or relocate a candidate before accepting converging or colliding neighbors.
- Apply safety constraints as hard eligibility rules, then select deterministically by unmet Target area, stability, Root/Trunk count, material, and travel/retraction cost.

# 10. Bed-Only Support and Target Failure Isolation

- Do not reattach a long Branch to an intermediate model surface.
- Bed-origin ancestry must remain valid through the Branch.
- Isolate failure per Target.
- Before fallback, consider in deterministic order:
  1. alternative Tsunami route;
  2. alternative Root;
  3. multiple Roots;
  4. safe reduced Trunk Height;
  5. fallback of only the failed Target.
- Preserve successfully generated Tsunami Targets.
- Fallback must produce a known supported result. Never silently skip a Target or fallback the whole object without proven necessity.

# 11. Micro Branch Contract

Micro Branch OFF:

- End at the Tsunami Branch.

Micro Branch ON:

1. Close the terminal U-turn into a complete terminal ring.
2. Extend a vertical pipe downward to that Tsunami Branch only.
3. Keep the pipe within the Branch envelope and do not protrude needlessly below it.
4. After terminal-ring completion, seed local Tree Support for fine interface contact.

The Tree-generated Micro Branch must not mutate Tsunami main-Branch geometry.

Micro Branch assumes interface support unless explicitly redesigned.

# 12. Continuous Extrusion and Toolpath

- Structural topology is decided before path ordering.
- Toolpath ordering must not alter Root, Rib, U-turn, Branch, or Target topology.
- Prefer one continuous extrusion per connected Tsunami island or loop where safe.
- Minimize retraction, travel, and seam count only after structural and printability constraints pass.
- Alternating layer start direction is allowed when it reduces travel without changing geometry.
- Straight Rib and growth-turn speed may differ; do not slow the entire structure solely because Active U-turns require caution.

# 13. Tsunami Efficiency

- Treat born Straight Ribs as reusable immutable geometry.
- Recompute primarily Active Frontier, Active U-turns, nearby lazy Virtual Ribs, collision boundaries, and Target/interface transitions.
- Do not regenerate full support polygons merely to recover persistent Rib identity.
- Prefer event-driven updates when they reduce measured work without changing output.
- Use cached spatial queries only with explicit keys and invalidation.
- Optimize algorithmic work before micro-optimizing code.

# 14. Phase Gates

Use these implementation phases unless current architecture proves a different grouping is safer.

## Phase 0: Audit and Plan

- Trace current support pipeline and ownership.
- Verify handoff against code.
- Identify reusable Normal/Tree/geometry/config/toolpath abstractions.
- Produce a complete staged plan.
- Do not edit production code unless explicitly requested.

## Phase 1: Minimal Vertical Slice

- One Root
- One Target
- Straight Branch Guide
- Fixed Orientation Field
- Virtual Rib Field
- Physical Rib birth
- Active U-turn growth
- Target reach

Do not implement Split, Merge, Micro Branch, complex routing, or aggressive optimization until every Phase 1 stage passes.

## Phase 2: Printability and State Stability

- Branch Angle conversion
- overlap/curvature checks
- minimum anchor requirements
- Rib/Turn hysteresis
- deterministic Root candidate selection
- continuous toolpath for the minimal slice

## Phase 3: Curved Routing and Collision

- curved Guide
- continuous Orientation Field
- direction transition
- collision-aware swept corridor
- alternative Root/route

## Phase 4: Multi-Target and Interface

- Split
- Merge
- same-XY/different-Z branching
- target-isolated fallback
- terminal ring
- optional Tree Micro Branch
- interface integration

## Phase 5: Performance

- Active Frontier
- lazy materialization
- event-driven updates
- measured cache/spatial optimization

Do not advance a phase until all dependent stages in the current phase pass their gates.

# 15. Required Verification

Adversarial coverage includes:

- zero local Physical Ribs;
- one Physical Rib;
- narrow Root;
- high-aspect-ratio/tall Root;
- annular and hole-containing geometry;
- complex cross-sections;
- multiple columns;
- all three multi-Target layouts;
- zero lateral growth;
- configured Branch Angle limit;
- reachability boundary;
- nearby obstacles;
- birth/death and split/merge threshold boundaries;
- representative layer heights;
- representative nozzle and line widths.
- source-Trunk/Branch parallelism;
- connector-only direction changes;
- Branch Minimum Spacing at threshold minus epsilon, exact threshold, and plus epsilon;
- future swept-footprint collision;
- full-length versus shorter-Branch packing and shorter-Branch immutability.

For each threshold, test minus epsilon, exact threshold, and plus epsilon.

For actual slicing verification, inspect raw G-code directly for:

- Target reach;
- model clearance;
- immutable Rib XY origin;
- immutable Rib direction;
- every Branch Straight Rib parallel to its recorded local source Trunk Straight Rib;
- every route-direction change confined to a recorded inter-branch connector;
- pairwise minimum clear spacing between non-connected swept Branch footprints;
- U-turn-only lateral growth;
- Branch Angle limit;
- unintended model attachment;
- continuous extrusion where expected;
- unnecessary travel/retraction;
- Tsunami-off equivalence.

Preview or a verification script alone is insufficient when raw slicing output is available.

Generate an isometric or other result visualization only after geometry, collision, angle, raw G-code, and regression checks pass. Visualization confirms results; it does not prove correctness.
