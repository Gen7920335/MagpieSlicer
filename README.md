# OrcaProject

This fork is used for OrcaSlicer support-generation experiments.

Upstream project: https://github.com/OrcaSlicer/OrcaSlicer

## Test Branches

### trinterface-test1

Purpose: Cura-like triangular support interface pattern test.

Status:
- Adds `Triangles` to the support interface pattern setting.
- Maps the new interface setting to OrcaSlicer's existing `ipTriangles` fill generator.
- Applies the pattern to normal and tree support interface/contact regions.
- Does not change support area generation.

### supportgen-algorithm-cura-test1

Purpose: future Cura-like normal support generation algorithm test.

Status:
- Reserved branch for replacing or bypassing Orca's normal support area generator.
- Target scope is normal support only, not organic/tree support.
- No Cura-like support generator implementation yet.
- Design plan: `docs/orcaproject/cura_style_normal_support_plan.md`

## Local Build

Current known local executable after environment setup:

`build/OrcaSlicer/orca-slicer.exe`

The Windows build environment was bootstrapped in this workspace. See commit history for the Boost configure quoting fix required for this local CMake/Windows toolchain.
