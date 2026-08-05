# Magpie Slicer Code Audit

## Scope

- Baseline: OrcaSlicer commit `1d61962ea720b9b45caa3887057d2e6ec7821e64`
- Magpie head: `f181069e2f6d1de8619725bc15125752e4e44c13`
- Product commits: 34
- Changed files: 812
- Code and verification files: 257
- Branding-only SVG files: 435

The audit prioritizes Magpie changes over unchanged upstream OrcaSlicer code. Resource recoloring is separated from executable logic.

## Priority 1

### Tool, filament, and extruder index domains

`src/libslic3r/GCode.cpp` uses `m_writer.filament()->id()` to read both filament-scoped and printer-extruder-scoped vectors in the low-temperature interface path. In particular, `nozzle_diameter` is read with the filament ID around lines 7914 and 8032. These IDs are not guaranteed to be interchangeable on a multi-tool printer.

Required change:

- Introduce explicit `filament_id` and `physical_extruder_id` accessors.
- Use the physical extruder ID for nozzle geometry and printer hotend settings.
- Use the filament ID only for material temperatures and material properties.
- Add assertions and release-mode fallback for imported profiles with shorter vectors.

### Low-temperature interface sequencing

The main object emission loop in `src/libslic3r/GCode.cpp` around lines 6796-6895 defers support output and mutates shared `GCode` state (`m_layer`, support role, temperatures and ordering). This is a high regression surface because object labels, tool changes, wipe-tower behavior and by-object printing share the same loop.

Required change:

- Build an explicit per-layer emission plan before writing G-code.
- Represent model, support body, cooling, interface and reheating as typed stages.
- Restore temporary layer and temperature state with scoped guards.
- Keep the default Orca path unchanged when the feature is disabled.

### Multi-nozzle wall planning

`src/libslic3r/PerimeterGenerator.cpp` calculates small-wall depth, large-wall minimums, center distances and interlocking compensation in separate helpers around lines 233-281. The hard-coded minimum of two large walls and duplicated spacing math can diverge between Classic, Arachne and infill-boundary generation.

Required change:

- Calculate one immutable `MultiNozzleWallPlan` per island and layer.
- Store requested and feasible small/large wall counts, widths, center offsets and interlocking delta in that plan.
- Make Classic, Arachne, G-code routing and preview consume the same plan.
- Keep narrow-region clipping in the geometry engine instead of silently changing configured counts.

## Priority 2

### Hotend setting ownership

`src/slic3r/GUI/Tab.cpp` creates a separate `m_hotend_config` and synchronizes nozzle diameter and line widths through callbacks around line 4723. Previous save/reload and cross-tool synchronization failures indicate that the temporary config is acting as a second source of truth.

Required change:

- Keep printer preset configuration as the only persisted source.
- Make the hotend page an indexed view over the selected tool's vector values.
- Route edits, preset reload, project load and extruder-count changes through one typed synchronization function.
- Remove string-based duplicate update paths.

### Imported profile vector normalization

Custom per-plate and per-tool vectors are read with `get_at()` in G-code generation, including temperature-drop-tower X/Y around lines 7963-7964. Legacy profiles may omit these entries or provide fewer values.

Required change:

- Normalize every custom vector at preset deserialization and extruder/plate-count changes.
- Define scalar fallback values in the schema.
- Validate vector lengths before slicing and report the repaired keys once.

### Snapmaker monitor lifetime and responsibility

`src/slic3r/GUI/DeviceTab/SnapmakerMonitorPanel.cpp` contains more than 3,000 added lines and combines network transport, camera polling, G-code parsing, printer commands and wxWidgets rendering. The changed GUI code contains multiple `CallAfter` callbacks, making panel destruction during an outstanding request a use-after-free risk unless every callback is cancelled or lifetime-gated.

Required change:

- Split transport, printer state, camera session and view into separate owners.
- Give asynchronous requests a cancellation token owned by the controller.
- Post immutable state snapshots to the UI instead of capturing panel state in worker callbacks.
- Centralize polling intervals and suppress overlapping requests.

### Vulkan resource safety

`src/libslic3r/Gpu/VulkanSlicer.cpp` adds about 1,760 lines of manual Vulkan setup and buffer management. The path needs strict ownership and size validation because the previously observed array conversion failure occurred at this boundary.

Required change:

- Wrap Vulkan handles and mapped memory in RAII owners.
- Replace untyped array conversion with explicit packed transport structs.
- Check element count, byte-size multiplication overflow, alignment and device limits before allocation/copy.
- Keep fallback decisions outside the compute kernel and make fallback reasons observable.

## Priority 3

### Oversized G-code integration

`src/libslic3r/GCode.cpp` has about 1,981 added lines spanning LESIC, multi-nozzle routing, low-temperature interfaces, the temperature-drop tower and Snapmaker startup policy. Independent features currently share mutable generator state.

Required change:

- Extract feature emitters with explicit inputs and outputs.
- Keep machine startup policy, calibration generation and per-layer extrusion separate.
- Replace repeated configuration string lookups with typed accessors.

### Cura-style support geometry

The Cura path spans `CuraStyleSupport.cpp`, `SupportCommon.cpp`, `FillRectilinear.cpp` and interface generation. Polygon clipping and path joining are likely performance hotspots, but optimization must be based on per-layer timings rather than path-count heuristics.

Required change:

- Add timing counters for support area construction, clipping, pattern generation and path ordering.
- Cache immutable layer geometry using configuration and geometry revision keys.
- Assert that generated support paths remain inside the intended support region before G-code emission.

## Review Order

1. Configuration schema, preset serialization and hotend indexing.
2. Multi-nozzle geometry plan and Classic/Arachne consumers.
3. Low-temperature interface and temperature-drop-tower G-code state machine.
4. Cura support geometry and interface generation.
5. Snapmaker monitor transport and UI lifetime.
6. Vulkan ownership, buffer contracts and fallback logic.
7. Branding, installer and setup-flow code.

## Tooling Status

- `git diff --check` passed for committed source changes.
- The installed Cppcheck 2.14 binary is unusable because its required `std.cfg` points to a missing build-machine path.
- No product source was changed during this initial audit.
- A full build was not run because it was not requested.
