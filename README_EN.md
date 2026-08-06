<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer.png" width="150">

# Magpie Slicer

An OrcaSlicer-based fork integrating mixed nozzle sizes, extended supports, and GPU-assisted slicing

</div>

[한국어](README.md) | **English**

## Latest Release

- Version: **2.5.0 (modified)**
- Tag: `v2.5.0-modified-vulkan-preview-1`
- Platform: **Windows x64**
- Status: **Pre-release**
- Commit: `7b2ce903085f9722e2493d0ca3ba9c32e16cea10`
- [Download the Windows installer](https://github.com/Gen7920335/MagpieSlicer/releases/download/v2.5.0-modified-vulkan-preview-1/MagpieSlicer_Windows_Installer_V2.5.0-modified_x64.exe)

```text
SHA-256: 3CD4469DC7DAE1B93AE4FDCE81B11A2981B75DA9A6C1CF8AD2F0E90921C436F0
```

This release fixes Cura-style automatic support being generated while support was disabled. With no raft, no support is generated. Requesting one raft layer creates only that raft layer without enabling automatic support. The slicing timer now includes G-code generation and post-processing.

> This fork is under active development. Inspect the preview and generated G-code before printing on real hardware.

## Main Features

| Feature | Purpose |
| --- | --- |
| Mixed-nozzle walls | Print internal structure quickly with a large nozzle and sharp outer details with a smaller nozzle |
| Multi-nozzle interlocking | Move the small/large wall boundary between adjacent layers to reduce delamination |
| Large-hotend override | Force selected layer ranges to a chosen hotend |
| `Nozzle used` preview | Show the actual nozzle used independently of material color |
| Cura-style normal support | Provide continuous Cura-style support paths while preserving the original support modes |
| Triangle interfaces and sublayers | Control pattern, angle, and temperature for selected interface layers |
| Low-temperature interface | Print model and interface at different temperatures with one nozzle |
| Tree wall count | Reinforce tree branches with up to ten walls |
| Vulkan-assisted slicing | Select Auto, On, Max GPU, or Off acceleration modes |
| LESIC | Integrated temperature and volumetric-flow calibration model |

## Mixed-Nozzle Printing

### Per-hotend configuration

Each toolhead has an independent nozzle diameter and extrusion widths. The toolhead selector, printer extruder settings, and `Hotend` page share the same source value. These values are stored in projects and presets.

- Nozzle diameter and default width
- First-layer, outer-wall, inner-wall, top-surface, infill, and support widths
- Separate hotend and filament preset save/load actions
- Small-nozzle wall speed override

### Automatic detail-nozzle selection

`Use smaller nozzles in crisp corners` finds the smallest usable configured nozzle. If a large-nozzle path cannot cover any part of a connected outer-wall loop, the complete loop is assigned to the smaller nozzle to avoid tool-change seams in the middle of that loop. Inner walls and infill remain on the larger nozzle where space permits.

- Small-nozzle wall count is configured independently from the normal wall count.
- Small outer walls have priority when space is limited.
- Both Classic and Arachne wall generators are supported.
- Tool selection reverses automatically when tool 2 has a larger nozzle than tool 1.

### Interlocking

Interlocking moves one wall between the small- and large-nozzle regions on alternating layers.

```text
L L L S S S S
L L L L S S S
L L L S S S S
L L L L S S S
```

Only the wall/infill boundary moves. The infill pattern itself remains stable.

### Large-hotend override

Specify a start layer, end layer, and hotend to bypass automatic small-nozzle selection in that range. `Add region` creates additional ranges. Reversed and overlapping ranges are normalized safely.

### Preview

- `Nozzle used`: fixed colors per nozzle diameter
- `Layer width`: actual extrusion width
- Slicing time: includes G-code generation and post-processing

## Support

### Cura-style normal support

The original Orca/Prusa normal and tree supports remain available. Magpie adds:

- `Normal (Cura style) auto`: threshold-angle detection
- `Normal (Cura style)`: manual and painted support regions

The Cura-style path propagates required support regions between layers and produces continuous zigzag paths. Its generator is not invoked when support is disabled.

`Cura solid support raft` fills the first Cura support layer at 100% density for bed adhesion. It does not implicitly enable automatic support.

### Interfaces

- Triangle patterns use exactly three directions separated by 120 degrees.
- `Interface density / spacing` keeps both representations synchronized.
- A selected interface hotend uses that nozzle diameter and support width.
- Requested interface thickness is preserved, with underside smoothing for stepped curved surfaces.

### Interface sublayers

The model-contacting interface is layer 1. A selected start/end range may use a different pattern, angle, and temperature. End values are clamped to the available interface layers, and the feature is disabled for a single-layer interface.

### Low-temperature interface

Single-nozzle printing can use separate model and support-interface temperatures.

```text
model -> support body -> cooling/temperature transition -> interface -> reheating -> next layer
```

The temperature-drop tower is visible and movable before slicing. It has a five-line brim and does not automatically move away from models.

- Temperature delta up to 30 C: 50 mm path
- Above 30 C: add 1 mm per degree
- Maximum path: 80 mm
- Final 10 mm: 10 mm/s
- AUX cooling and nozzle wiping are enabled only for supported hardware profiles

### Tree support

- Automatic tree support receives the configured threshold angle.
- Tree Slim and Organic wall counts accept `0-10`.
- `0` preserves automatic behavior; narrow branches generate only walls that physically fit.

## Vulkan-Assisted Slicing

The top selector provides:

- `Vulkan: Auto`: use calibrated CPU/GPU detection to select profitable work
- `Vulkan: On`: prefer validated GPU paths
- `Vulkan: Max GPU`: expand GPU use to the maximum supported range
- `Vulkan: Off`: use the CPU pipeline only

Validation and CPU fallback paths remain for topology-sensitive work. Performance depends on the GPU, driver, CPU, and model complexity.

## Device and Calibration Features

### Snapmaker device view

Magpie extends the Snapmaker U1 print-start flow and native device panel. Camera, current layer, temperatures, fans, motion state, and common device controls are available in one view. PA calibration, bed leveling, and timelapse options default to off.

### LESIC

LESIC creates a centered cylindrical calibration model sized to bed dimensions minus 20 mm. It includes floor labels, perimeter marks, and an internal brim, with reduced label sizing for small beds. Temperature and maximum volumetric speed can be evaluated in one print.

## Verification

Completed for the latest release:

- Full CTest suite: **349/349 passed**
- Release-readiness suite: **12/12 passed**
- Real installer extraction: **15,093 files verified**
- Installed Cura geometry slices: **3/3 passed**
- Support-off regression: `0` support layers without raft, exactly `1` raft layer when requested
- Installed EXE/DLL SHA-256 values match the verified build

These checks do not guarantee every printer and firmware combination. Review multi-tool output, machine-specific start G-code, and low-temperature interface behavior before uploading a job.

## Building on Windows

```powershell
cmake --build build-vulkan --config Release --parallel 8
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -BuildDirectory build-vulkan -Parallel 8
```

## License and Attribution

Magpie Slicer is based on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). OrcaSlicer-specific calibration tools and model names retain their original names. See [LICENSE](LICENSE.txt) and the upstream project license for usage and distribution terms.
