# Magpie Slicer 2.5.0.0.5

This is the fifth Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.5`.

## Fix

- Fixes the immediate crash caused by selecting `Resin style (auto)` with process presets created before resin settings existed.
- Treats a missing `resin_support_tree_type` as the schema default while preserving an explicitly selected Branching strategy.
- Retains all Resin/Mixed support features and Bambu Lab A2L profiles from 2.5.0.0.4.
- Prevents installer builds from exhausting workstation resources by capping both CMake and MSVC parallelism at two workers, using below-normal process priority, and checking competing builds, free memory, and staging-disk space before starting.

## Verification

- Reproduced the 2.5.0.0.4 access violation in the real GUI with a legacy process preset.
- Replayed Tree, Normal, Tsunami, Mixed, and Resin support selections after the fix without a crash.
- Switched the Resin tree strategy between Default and Branching without a crash.
- Resin/Mixed FFF suite: 14 test cases and 633 assertions passed.
- The packaged application loaded an isolated copy containing 454 legacy process presets, selected `Resin style (auto)`, switched to `Branching (experimental)`, and remained responsive.
- Packaged Cura support-geometry slicing: 3/3 checks passed.
- NSIS archive integrity passed; 15,250 payload files (402,466,819 bytes) were extracted successfully.
- The packaged EXE and DLL SHA-256 values match the verified build outputs.
- Resource-guard preflight passed with two CMake/MSVC workers and BelowNormal priority; an unsafe `-Parallel 8` request was rejected before build startup.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.5_x64.exe`

Size: 142,233,974 bytes

SHA-256: `FA36EE0FC5BDF94F653D0027B7C56D80DE5843423CE910D60E30E0ACEDF1306D`

Payload commit: `a41875d38197159d142848c20a8f4b0a4a5c6eb0`
