# Magpie Slicer 2.5.0.0.6

This is the sixth Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.6`.

## Fixes and performance

- Applies the shared FFF overhang threshold to Resin-style automatic support-point selection. A zero threshold preserves the native PrusaSlicer SLA automatic detector, while painted enforcers continue to override the angle filter.
- Materializes schema defaults before editing options missing from legacy process presets, preventing null option access when changing Resin pillar diameter and other newly introduced settings.
- Hides Tsunami-only settings and their group outside Tsunami support mode.
- Adds Korean labels, enum values, and tooltips for all exposed Resin-style settings.
- Skips Mixed paint projection and per-layer paint clipping when the model has no Mixed paint annotations.
- Caches Mixed channel overlap offsets by layer and extrusion width instead of recomputing the same polygon offset for every tree path.

## Verification

- Resin suite: 8 test cases and 371 assertions passed.
- Mixed suite: 10 test cases and 492 assertions passed.
- Resin threshold regression confirms that 30-degree and 80-degree thresholds produce different automatic support results.
- Default and Branching pillar diameters from 0 through 15 mm complete support generation.
- All 35 Resin editor options missing from a legacy process preset materialize with the correct schema type and default serialization (175 assertions).
- Dense-path Mixed stress fixture completed in 0.73 seconds and remained below the 15-second responsiveness limit.
- Korean PO format and header validation passed.
- Release GUI DLL build passed with CMake and MSVC parallelism limited to two workers.
- Packaged application and installer verification pending.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.6_x64.exe`

Size: pending

SHA-256: pending

Payload commit: pending
