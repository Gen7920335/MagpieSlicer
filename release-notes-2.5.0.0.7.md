# Magpie Slicer 2.5.0.0.7

This is the seventh Magpie release on the OrcaSlicer 2.5.0 base: Orca `2.5.0` + Magpie `.0.7`.

## Fix

- Fixes the immediate GUI startup access violation introduced in 2.5.0.0.6.
- Removes the unsafe whole-group visibility call on the Tsunami settings group before its wxWidgets sizer has been created.
- Keeps the existing per-setting visibility rules, so Tsunami-only controls remain hidden outside Tsunami mode.
- Extends installer verification to require the installed GUI to remain alive and responsive for 15 seconds before CLI slicing checks run.

## Verification

- Reproduced the 2.5.0.0.6 crash with a clean data directory: `ConfigOptionsGroup::Show()` dereferenced a null sizer after the main frame was first shown.
- The fixed Release GUI remained responsive for 20 seconds under the identical clean-data startup probe.
- Packaged GUI and installer verification pending.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.7_x64.exe`

Size: pending

SHA-256: pending

Payload commit: pending
