# Magpie Slicer Project Contracts

This document contains conditional project-specific contracts. Read or search only the sections relevant to the current task.

# 1. Project Purpose

- Magpie Slicer is an OrcaSlicer-based FDM slicer focused on mixed-nozzle toolheads, extended support generation and interfaces, low-temperature support interfaces, selected Snapmaker integration, and optional Vulkan-assisted slicing.
- Preserve OrcaSlicer's normal workflows, project/profile compatibility, output correctness, and printer safety while adding opt-in Magpie behavior.
- Generated geometry and G-code correctness outrank preview appearance and benchmark speed.

# 2. Architecture and Feature-Off Contract

- Upstream Orca behavior is the default.
- A disabled Magpie feature must not change slicing, UI state, serialization, tool ordering, preview routing, or G-code.
- Use existing Orca configuration, preset, geometry, flow, tool-ordering, and G-code abstractions when they correctly own the behavior.
- The printer preset is the persisted source of truth for per-tool nozzle diameter and hotend line-width vectors. UI pages are indexed views, not independent stores.
- Keep physical extruder/tool indices, filament/material indices, and UI selection indices as explicit domains. Convert only at named boundaries.
- Classic and Arachne wall generation must consume the same multi-nozzle planning semantics.
- Preview and G-code routing must consume the same resolved tool assignment used by geometry generation.
- Prusa-style normal, Cura-style normal, Tree/Organic, and Tsunami support must remain selectable and independent.
- Low-temperature interface sequencing must remain isolated from the normal G-code path, with explicit and restored temporary state.
- Vulkan is optional and CPU-authoritative. GPU output must be exact or conservative and validated by deterministic CPU logic.
- Stable Magpie and profiler builds must retain separate application, binary, configuration, installer, registry, and package identities.
- Device transport/state ownership must remain separate from wxWidgets view lifetime. Asynchronous work must be cancellable or lifetime-gated.

# 3. Mixed-Nozzle Wall Contract

- Per-tool nozzle diameter and line widths are configurable and persist with presets/projects.
- A connected wall loop that a large nozzle cannot completely represent is assigned to the small nozzle as a whole. Never switch nozzle midway through that loop.
- `Small nozzle wall count` is independent of normal wall-loop count.
- Preserve requested small outer walls first when space is limited; retain inner large-nozzle walls where feasible.
- Interlocking alternates only the small/large wall boundary by one wall on adjacent layers. It must not shift the infill pattern.
- Large-nozzle override supports multiple inclusive layer ranges, a selected hotend, normalized reversed input, and deterministic overlap handling.
- Classic and Arachne must both work.

# 4. Support and Interface Contract

- Preserve existing support algorithms. Cura-style normal support is a separate choice.
- Cura-style support must generate native continuous support paths; do not repair only the top-view silhouette after path generation.
- Triangle interface uses exactly three line directions separated by 120 degrees and remains continuous.
- Preserve interface thickness.
- Top and bottom interface footprints must not split unexpectedly.
- Interface density and spacing stay synchronized.
- Interface sublayers use inclusive start/end range, pattern, angle, and temperature.
- Model-contact interface is layer 1.
- Clamp or disable unavailable interface ranges safely.
- Tree support wall loops support 0 (automatic) through 10 requested walls, limited only by feasible branch width.
- Cura solid support raft is opt-in and fills only the Cura support bed-contact first layer. It must not implicitly enable support.

# 5. Low-Temperature Interface Contract

- Normal order is: model, support body, cooling/temperature transition, interface, reheating, next layer.
- The temperature-drop tower remains visible and movable before slicing.
- Do not automatically move the tower around the model.
- Use a five-line brim and the configured temperature-delta length rule.
- Hardware-dependent AUX cooling and nozzle-wiping controls remain unavailable when the active machine profile does not support them.
- Hidden reserved controls must not alter output.

# 6. LESIC, Snapmaker, Branding, and Distribution

- LESIC remains an integrated low-temperature interface calibration tool.
- Preserve OrcaSlicer-owned calibration tool/model names and compatibility-facing identifiers.
- Snapmaker output must preserve firmware-compatible G-code identity and safe homing/leveling behavior.
- Pre-print PA calibration, bed leveling, and timelapse choices default off.
- Branding uses Magpie Slicer for the application and installer.
- Preserve upstream/internal Orca names where required for source compatibility, formats, calibration assets, protocols, attribution, or third-party interoperability.
- Do not create portable packages. The supported distributable is the installer unless the user explicitly changes policy.
- Stable and profiler builds must install side-by-side without sharing writable configuration or uninstall ownership.

# 7. Coding and State Ownership

- Follow existing C++17 style: PascalCase types, snake_case functions/variables, `#pragma once`, RAII, smart pointers, and TBB-aware shared-state handling.
- Before changing a setting, inspect schema, typed owner, producer, consumer, serializer, invalidation, UI binding, CLI/project loading, preview path, and tests as applicable.
- A setting has one typed semantic owner.
- Update serialization, preset migration, UI enablement, invalidation, CLI loading, project loading, and consumers together.
- Prefer immutable per-layer/per-island plans over mutating generated extrusion paths.
- Use structured polygon, flow, configuration, and serialization APIs.
- Do not parse typed geometry or configuration through ad hoc text manipulation.
- Validate vector lengths during deserialization and count changes.
- Use schema-defined default fallback, report repair once, and never index imported vectors unchecked.
- Use scoped guards for temporary G-code state such as active layer, role, temperature, tool, and deferred output.
- Comments explain non-obvious invariants or numerical contracts, not straightforward narration.

# 8. Numerical and Geometry Contract

- Orca geometry uses signed 64-bit fixed-point coordinates at 1e-6 mm.
- Preserve established coordinate, unit, epsilon, winding, clipping, and polygon conventions.
- Avoid unnecessary fixed-point/floating-point conversion and document unavoidable conversion boundaries.
- Before GPU dispatch, prove intermediate and restored values fit. Use wider CPU preflight arithmetic when necessary.
- Validate element count, byte-size multiplication, alignment, device limits, stable IDs, denominators, and restored coordinates.
- If a GPU batch fails, discard the complete batch. Never combine partial GPU output with CPU output.
- Treat empty polygons/intersections, degenerate loops, zero-length segments, invalid normals, narrow regions, duplicate/coincident geometry, tangent intersections, nearly parallel lines, disconnected output, zero denominators, and out-of-bed auxiliary geometry as expected edge cases.
- Keep support paths inside their intended support region.
- Do not solve support artifacts by clipping only the final rendered silhouette.
- Normalize reversed and overlapping layer ranges before use.
- Clamp interface sublayer ranges to available layers.
- Compute feasible wall counts from actual geometry. Do not reinterpret configured normal wall loops as total mixed-nozzle walls.

# 9. Performance Contract

- Profile before optimizing geometry.
- Separate timing for relevant stages, including support-area/Root candidates, Guide/Virtual Rib/Active Frontier work, collision/U-turn generation, clipping, pattern generation, path ordering, GPU dispatch, CPU validation, and G-code completion.
- Avoid repeated polygon conversion, offset/intersection, bounding-box construction, tool resolution, and nozzle-width resolution inside hot loops.
- Cache only immutable or revision-keyed data with explicit invalidation.
- Reuse resolved nozzle widths, tool mappings, spatial candidates, and layer plans where ownership permits.
- Minimize allocation and hash/set reconstruction in per-segment and per-wall loops.
- Skip GPU dispatch when setup and transfer are unlikely to beat CPU work.
- Auto mode uses bounded hardware/workload qualification, not expensive full duplicate slicing.
- Optimization must not silently change normalized geometry, motion, extrusion, assignment, or G-code.

# 10. Compatibility Contract

- Maintain backward compatibility for `.3mf` projects, printer/process/filament presets, and legacy profiles with missing or short custom vectors.
- New options require stable keys, defaults preserving upstream behavior, serialization coverage, preset/project reload coverage, and slicing invalidation.
- Per-tool vectors resize predictably when extruder/tool/plate counts change and preserve existing indexed values.
- Preserve toolchange and startup G-code conventions of the selected machine profile.
- Never assume a Snapmaker U1 sequence is valid for another printer.
- Preserve application protocol/file associations and upstream identifiers when changing them would break compatibility.

# 11. Build and Test Authorization

Do not build or run tests unless explicitly authorized.

Before an authorized build or test:

- inspect `git status --short --branch`;
- inspect relevant diffs;
- confirm build options and script parameters from current files;
- do not clean or delete untracked artifacts.

Confirmed Windows release commands:

```powershell
cmake --build build-vulkan --config Release --parallel 8
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -BuildDirectory build-vulkan -Parallel 8
```

Profiler build:

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -DevelopmentProfiler
```

Inspect the current script parameters before adding arguments.

General upstream Catch2/CTest commands:

```powershell
ctest --test-dir build --output-on-failure
ctest --test-dir build\tests\libslic3r --output-on-failure
ctest --test-dir build\tests\fff_print --output-on-failure
```

Relevant repository scripts include:

```text
scripts/verify_multinozzle_orcacube.ps1
scripts/verify_small_nozzle_geometry.ps1
scripts/verify_small_nozzle_wall_speed.ps1
scripts/verify_imported_multinozzle_wall_counts.ps1
scripts/verify_hotend_filament_matrix.ps1
scripts/verify_support_features.ps1
scripts/verify_cura_support_geometry.ps1
scripts/verify_cura_solid_support_raft.ps1
scripts/verify_tree_support_wall_counts.ps1
scripts/verify_interface_underside_smoothing.ps1
scripts/verify_vulkan_integration_contract.ps1
scripts/verify_vulkan_cpu_baseline_matrix.ps1
scripts/verify_slicing_profiler_contract.ps1
scripts/verify_setup_guide_scroll.ps1
scripts/verify_installer.ps1
scripts/verify_magpie_release_readiness.ps1
```

For parameterized slicing behavior:

- use at least four meaningful configurations where practical;
- include defined lower and upper bounds;
- inspect adjacent layers for interlocking;
- use Classic and Arachne when wall generation changes.

Verify as applicable:

- feature-off equivalence;
- project/preset save-reload;
- imported legacy profiles;
- CLI slicing completion;
- generated G-code and tool assignments.

Installer validation covers installed payload, application identity, applicable side-by-side behavior, and package hash. A build-directory executable alone is not installer validation.

Run `git diff --check` before reporting completion when verification is authorized.

# 12. Repository and Safety Rules

- Do not use GUI coordinate clicking or GUI automation.
- Prefer source inspection, CLI slicing, structured geometry/G-code analysis, and repository tests.
- Batch independent read-only work where practical, but keep result-dependent debugging sequential.
- Cancel a stalled or timed-out process, determine why it stalled, and retry only with a changed plan.
- Before a large authorized structural change, create a non-destructive backup.
- For authorized execution, begin with the narrowest affected target or test.
- Complete local implementation and validation before a requested push.
- Never push merely as a checkpoint.
- Use existing authenticated GitHub CLI state.
- Never expose access tokens in source, commands, logs, documentation, or chat.
- Do not command a physical printer or start, resume, or cancel a print without an explicit request for that exact action and a checked machine state.

# 13. Permanent Project-Specific DO NOT

- Do not assign different nozzles inside one connected cosmetic wall loop.
- Do not derive physical nozzle geometry from a filament/material index.
- Do not maintain a second persisted hotend/nozzle configuration beside printer-preset vectors.
- Do not accept partial, unvalidated, overflow-prone, or device-failed Vulkan results.
- Do not remove or rename OrcaSlicer-owned calibration tools/models or compatibility identifiers as part of branding.
- Do not reintroduce AUX-fan always-on controls that were explicitly removed.
- Do not package or publish a portable archive under current policy.
