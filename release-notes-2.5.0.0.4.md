# Magpie Slicer 2.5.0.0.4

This is the fourth Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.4`.

## Changes

- Integrates the latest PrusaSlicer SLA support engine as FFF `Resin style (auto)` support.
- Supports both Default and Branching resin-tree strategies through native FFF support, interface, material, preview, and toolpath generation.
- Exposes 35 resin-support controls while intentionally excluding model penetration depth.
- Keeps resin object elevation available even when support generation is disabled.
- Preserves separate support-paint channels for the normal and tree generators in `Mixed (auto)` mode.
- Adds the official Bambu Lab A2L model with 0.2, 0.4, 0.6, and 0.8 mm nozzle variants.
- Adds 17 A2L process presets and 124 A2L filament presets, using Orca-compatible embedded machine G-code validated against Bambu Studio templates.
- Adds Korean UI translations for the resin support settings.

## Verification

- A2L static index, inheritance, compatible-printer, asset, and embedded G-code validation passed.
- Magpie's native Orca profile validator loaded the A2L dependency closure successfully.
- Resin/Mixed FFF suite: 14 test cases and 633 assertions passed.
- SLA support suite: 25 test cases and 2,851 assertions passed.
- Default/Branching strategies passed with 0.2/0.4/0.6/0.8 mm nozzle configurations.
- Core release-readiness matrix: 10/10 checks passed.
- Release application and installer verification details will be recorded after packaging.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.4_x64.exe`

SHA-256: pending
