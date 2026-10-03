<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer.png" width="150">

# Magpie Slicer

An OrcaSlicer-based slicer that combines different nozzle sizes in one print and gives finer control over supports

[한국어](README.md) | **English**

</div>

> This fork is under active development. Inspect the preview and the generated G-code before sending a job to real hardware.

## Download

| Item | Details |
| --- | --- |
| Latest version | **2.5.0.0.9 Beta 6** (tag `v2.5.0.0.9-beta.6`, publication pending) |
| Platform | Windows x64 installer |
| Status | Pre-release |
| Get it | [GitHub Releases](https://github.com/Gen7920335/MagpieSlicer/releases) |

- Installs alongside OrcaSlicer without sharing its settings folder or file associations. `orcaslicer://` links are still handled for website integration.
- The installer is not code-signed, so Windows SmartScreen may warn.
- Runs without Vulkan or NVIDIA drivers; slicing then uses the CPU.

## What's new in Beta 6

- **Snapmaker U1 defaults:** printer and default process values follow Snapmaker's official Orca (2026-09-30): 20,000 acceleration and 500 mm/s (the stock firmware limits), per-nozzle retraction, and tree support with Classic walls by default. The default plate reads as Textured PEI, so the smooth-plate `Z_OFFSET -0.07` is not added.
- **Half-height outer walls (H/2):** enabling them also enables half-height support. The print order keeps the time between the two outer-wall passes even when support is present, and the preview slider steps one half layer at a time.
- **Stability:** starts on PCs without a Vulkan loader. Fixed Classic walls + H/2 + four walls failing to slice, the missing first-layer normal-support outline, and missing range checks for per-toolhead line widths and low-temperature interface temperatures.
- **Installer:** Add/Remove Programs and the installer name show the beta number, and uninstalling removes the file associations Magpie created.

## Main features

| Feature | What it does |
| --- | --- |
| Mixed-nozzle walls | Large nozzle for fast internals, small nozzle for text and sharp outer walls |
| Multi-nozzle interlocking | Offsets the small/large wall boundary by one line per layer to reduce delamination |
| Large-hotend override | Forces selected layer ranges to a chosen hotend |
| Half-height outer walls and support (H/2) | Prints the outer two walls and support at half layer height for better surfaces |
| Cura-style normal support | Continuous Cura-style support paths alongside the Prusa-style modes |
| Mixed automatic support | Normal support where it reaches the bed, tree support elsewhere, in one print |
| Resin-style automatic support | PrusaSlicer SLA supports printed through the FFF interface and material pipeline |
| Triangle interfaces and sublayers | Separate pattern, angle, and temperature for selected interface layers |
| Low-temperature interface | One nozzle prints model and support interface at different temperatures |
| Tree support wall count | Reinforces branches with up to ten walls |
| GPU-assisted slicing | Accelerates parts of slicing with Vulkan or CUDA, validated against the CPU |
| LESIC | Temperature and volumetric-flow calibration in one model |
| `Nozzle used` preview | Colors paths by the nozzle actually used, independent of material color |

## Mixed-nozzle printing

- **Per-hotend settings:** each toolhead has its own nozzle diameter and line widths (first layer, outer wall, inner wall, top surface, infill, support, bridge). Values that do not fit the nozzle are reported before slicing.
- **Automatic detail nozzle:** with `Use smaller nozzles in crisp corners`, sharp wall loops the large nozzle cannot fill are printed whole by the small nozzle. Inner walls and infill stay on the large nozzle. Classic and Arachne are both supported.
- **Interlocking:** the small/large wall boundary moves one line between odd and even layers.

  ```text
  L L L S S S S
  L L L L S S S
  ```

- **Large-hotend override:** set start and end layers and a hotend to bypass automatic selection for that range. Multiple ranges are supported.

## Half-height outer walls and support (H/2)

- `Half-height outer walls`: prints the outer two XY walls twice at half the layer height. The total wall count and the inner wall and infill heights are unchanged.
- `Half-height support`: prints support at half height. It is always on while half-height outer walls are enabled, so the walls are supported on the same grid.
- Within a layer, the lower half (support and outer wall) prints first, then the upper half. The upper outer wall is placed where the two outer-wall passes are evenly spaced in time, avoiding banding every half layer.
- Travel Z lift is at least 1.5 times the layer height.

## Supports

- **Cura-style normal support:** `Normal (Cura style) auto/manual` propagates demand downward and generates continuous ZigZag paths. `Cura solid support raft` fills the first support layer at 100%.
- **Mixed / Resin style:** split one print between normal and tree support, or use SLA-style thin pillar supports.
- **Tree support:** the automatic threshold angle drives overhang detection. Tree Slim and Organic wall counts accept `0-10` (`0` is automatic).
- **Interfaces:** triangle pattern (three directions at 120 degrees), synchronized density and spacing, a dedicated interface hotend, and underside smoothing. Sublayers count the model-contact face as layer 1 and apply their own pattern, angle, and temperature to a chosen range.
- **Low-temperature interface:** one nozzle prints model and interface at different temperatures.

  ```text
  model -> support body -> cooling/temperature change -> interface -> reheating -> next layer
  ```

  The interface temperature must be at least 170 °C (the firmware cold-extrusion limit) and no higher than the filament's maximum. The temperature-drop tower is visible before slicing and can be moved by hand.

## GPU-assisted slicing

Choose Vulkan or CUDA in the top bar (only one is active at a time).

- `Vulkan: Auto / On / Max GPU / Off`
- `CUDA: Off / On / Max GPU` (NVIDIA)

Geometry-sensitive stages are validated against or fall back to the CPU. Speedups depend on the GPU, driver, and model.

## Snapmaker U1 and calibration

- **Snapmaker U1:** the native device panel shows the camera, current layer, temperatures, fans, motion state, and common controls in one view. PA calibration, bed leveling, and timelapse default to off. Printer and default process values follow Snapmaker's official Orca.
- **LESIC:** one bed-sized cylindrical model checks temperature and maximum volumetric flow together.

## Verification (Beta 6)

- Tests: fff_print 164 (1 skipped), libslic3r 206, half-layer 69 passed
- Installer: 15,250 files; EXE/DLL SHA-256 match the build
- U1 default Benchy sliced with the packaged program: identical CPU and Vulkan output, normal exit without a Vulkan loader

A real administrator install and uninstall, the installed GUI, and physical prints were not checked for this beta. Review multi-tool output, machine-specific start G-code, and low-temperature interface behavior before sending a job.

## Building from source (Windows)

```powershell
cmake --build build-vulkan --config Release --parallel 8
powershell -ExecutionPolicy Bypass -File scripts\build_installer.ps1 -BuildDirectory build-vulkan -Parallel 4
```

The installer script will not start while another build is running, or with less than 8 GiB of free memory or 20 GiB of free staging disk.

## License and attribution

Magpie Slicer is based on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). OrcaSlicer's own calibration tools and model names keep their original names. Use and distribution follow [LICENSE](LICENSE.txt) and the original project's license.
