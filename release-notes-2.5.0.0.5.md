# Magpie Slicer 2.5.0.0.5

This is the fifth Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.5`.

## Fix

- Fixes the immediate crash caused by selecting `Resin style (auto)` with process presets created before resin settings existed.
- Treats a missing `resin_support_tree_type` as the schema default while preserving an explicitly selected Branching strategy.
- Retains all Resin/Mixed support features and Bambu Lab A2L profiles from 2.5.0.0.4.

## Verification

- Reproduced the 2.5.0.0.4 access violation in the real GUI with a legacy process preset.
- Replayed Tree, Normal, Tsunami, Mixed, and Resin support selections after the fix without a crash.
- Switched the Resin tree strategy between Default and Branching without a crash.
- Resin/Mixed FFF suite: 14 test cases and 633 assertions passed.
- Packaged-application and installer verification pending.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.5_x64.exe`

Size: pending

SHA-256: pending

Payload commit: pending
