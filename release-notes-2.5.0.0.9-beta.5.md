# Magpie Slicer 2.5.0.0.9 Beta 5

This beta fixes material routing and half-height printing interactions, and includes the accumulated nozzle-editing, preview, and device-dialog improvements since Beta 4.

## Fixes

- Keep distinct inner and outer wall materials on their assigned tools in both normal and half-height printing, including Classic and Arachne walls.
- Resolve direct-tool automatic material mapping before geometry generation. Preserve assigned colours when no compatible smaller nozzle exists. Unsupported non-identity manual direct-tool mappings now stop with an error.
- Validate half-height paths against the nozzle and height of their actual role. Full-height infill, raft, skirt, brim and tower constraints still apply.
- Restore continuous Resin support below elevated objects with synchronized support layers or a prime tower, including narrow support sections and raft transitions.
- Apply half-height travel clearance inside both prime-tower backends and on return to the model, preserving larger configured Z hops.
- Preserve skirts when half-height support visits precede the skirt's assigned tool.
- Preserve nozzle-linked widths during nozzle edits, correctly recognize shell-only half-height layers, and display half-height extrusion planes in the preview.
- Improve filament setup loading status, error reporting, and device-camera recovery.

## Verification and known limitations

The fixes passed 96 CPU verification command groups and 50 CLI slicing cases; four unsafe full-height skirt configurations were correctly rejected. Independent G-code checks covered tool assignment, extrusion, travel clearance, temperature restoration, and unchanged feature-OFF commands. One pre-existing allowed assertion failure and one dedicated-sweep skip remain separately recorded; all 34 dedicated Resin setting mutations passed.

The installer includes CUDA and Vulkan support. User-reported GUI loading freezes, external video/audio stutter, the original multicolour project, and physical print quality still require direct confirmation. Existing large hidden-fixture timeout and missing historical benchmark baselines are not reported as passing. The installer is unsigned.
