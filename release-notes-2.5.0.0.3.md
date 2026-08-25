# Magpie Slicer 2.5.0.0.3

This is the third Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.3`.

## Changes

- Adds `Mixed (auto)` support, combining normal and tree support generators in one sliced object.
- Lets users select Prusa or Cura normal support and Organic, Slim, Strong, or Tree Hybrid tree support.
- Adds a 0–100% normal-support coverage threshold for connected support-demand regions.
- Adds Selective merge, splitting build-plate-reachable demand to normal support and the remainder to tree support.
- Removes overlapping same-layer extrusion deterministically and merges the two channels into one ordered support-layer stream.
- Produces a single non-overlapping raft from normal and tree channel output.
- Preserves support/interface filament selection, preview, brim, retraction, arrangement, and support-cache behavior for mixed layers.
- Adds Korean UI translations for the Mixed settings.

## Verification

- Release `fff_print_tests` target built successfully.
- Mixed suite: 8 test cases and 480 assertions passed.
- Prusa/Cura × Organic/Slim/Strong/Tree Hybrid selective-split integration matrix passed.
- 0.2/0.4/0.6/0.8 mm nozzle × four tree styles passed with support-layer and G-code verification.
- Non-Tsunami support regression suite: 33 test cases and 5,840 assertions passed.
- Release application target built and linked successfully.
- Final release-readiness, installed-payload, installer integrity, commit, and SHA-256 details are recorded below after packaging.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.3_x64.exe`

SHA-256: `PENDING`
