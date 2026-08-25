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
- NSIS archive integrity passed and 15,250 files (402,478,083 bytes) were extracted successfully.
- The extracted installer GUI remained responsive for 15.1 seconds and produced no crash log.
- The extracted installer payload passed all three Cura support-geometry slicing cases: curved, stepped, and narrow.

## Installer

`MagpieSlicer_Windows_Installer_V2.5.0.0.7_x64.exe`

Size: 142,245,789 bytes

SHA-256: `5C8C10A6DC42AF27D840FF61AED65FEFFDBEA1A320C8C4F516D7C5D6C74737F4`

Payload commit: `83e86bac6c`
