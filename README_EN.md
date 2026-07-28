# Magpie Slicer

[한국어](README.md) | **English**

Magpie Slicer is an experimental OrcaSlicer fork focused on mixed-nozzle
printing and support-generation controls.

- Application name: **Magpie Slicer**
- Current version: **2.5.0 (modified)**
- Installer package name: **OrcaSlicer(name pending)**
- Platform: **Windows x64**
- Upstream: [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer)

> This project is under active development. Verify generated G-code and machine
> behavior before using it on production hardware.

## Main Features

### Mixed-nozzle wall printing

Multiple hotends may use different nozzle diameters in one print.

- Per-hotend nozzle diameter and line-width settings
- Automatic smaller-nozzle detection
- Small-nozzle wall count
- Classic and Arachne wall-generator support
- Small/large-nozzle wall overlap
- Optional alternating-layer interlocking
- Small-nozzle wall speed override
- Layer-range large-nozzle override with selectable hotend
- Project and printer-preset persistence
- `Nozzle used` preview mode with a separate color for each nozzle

The smaller nozzle is selected only when the larger nozzle cannot completely
cover a connected wall loop. The selected loop remains on one nozzle to avoid
visible seams and weak mixed-width sections.

### Cura-style normal support

Magpie Slicer adds Cura-style normal-support choices while retaining the
original Orca/Prusa and tree-support paths.

- `Normal (Cura style) auto`
- `Normal (Cura style)`
- Cura-style support-region handling
- Continuous ZigZag support paths
- 70-degree and 90-degree threshold-angle handling

### Triangle support interface

The support-interface pattern list includes `Triangles`.

- Three fixed line directions
- 120-degree directional spacing
- Zero-spacing dense interface support
- Consistent behavior for Prusa, Cura-style, and tree support
- Smoothed interface underside

### Interface sublayers

Selected interface layers may use a separate pattern.

- Enable/disable toggle
- Start and end interface-layer range
- Pattern type
- Pattern angle
- Interface temperature
- Contact-side interface is counted as layer 1
- End values are clamped to the available interface-layer count
- Automatically disabled when the interface has one layer or less

### Low-temperature support interface

Single-nozzle printing may use a separate support-interface temperature.

- Interface extrusion temperature
- Interface-exit heating time
- Auxiliary-fan cooling toggle and speed
- Nozzle-wiping toggle
- Temperature-drop tower
- Hardware-dependent options remain disabled when the selected printer does
  not provide the required coordinates or hardware capability

The temperature-drop tower uses a rear-left bed location when available. Its
path length starts at 50 mm for a temperature difference up to 30 C, increases
by 1 mm per additional degree, and is capped at 80 mm. The final 10 mm uses a
reduced speed when additional cooling time is needed.

### Tree-support wall count

Tree Slim and Organic support wall count may be selected from 0 through 10.

### LESIC calibration

The calibration menu includes `LESIC`, a cylindrical calibration model that
adapts to the selected bed and nozzle.

- Temperature range and step
- Layers per temperature
- Minimum and maximum volumetric speed
- Bed-aware diameter and placement
- Perimeter labels, tick marks, and internal brim

## Verification Status

The current Release build was tested on 2026-07-28.

| Area | Result |
| --- | --- |
| `libslic3r` unit tests | 50,042 assertions, 138 cases passed |
| `fff_print` unit tests | 35,058 assertions, 85 cases passed |
| Support type, angle, triangle, and sublayer matrix | 9/9 passed |
| Interface thickness, spacing, pattern, and nozzle matrix | 13/13 passed |
| Tree Slim and Organic wall count, 0.4/0.15 mm | 44/44 passed |
| Mixed-nozzle OrcaCube, Classic | 25/25 passed |
| Mixed-nozzle OrcaCube, Arachne | 25/25 passed |
| Small-nozzle speed override | 8/8 passed |
| Low-temperature interface and drop tower | 4,330 assertions, 9 cases passed |

The long 66-case complex-geometry matrix was stopped at the per-step time
limit after its first 10 cases passed. Equivalent setting and routing paths
were completed with the 50-case OrcaCube matrix.

Physical-printer validation remains the user's responsibility.

## Building on Windows

Requirements are the same as the upstream OrcaSlicer Windows build.

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_cura_port.ps1
```

The Release executable is generated at:

```text
build/src/Release/orca-slicer.exe
```

To build the NSIS installer:

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_installer.ps1
```

## Repository Layout

- `src/`: application and slicing-engine source
- `tests/`: unit and regression tests
- `scripts/`: build, slicing, geometry, and regression-verification scripts
- `resources/`: profiles, translations, icons, and calibration assets

Generated builds, local sandboxes, backups, and verification output are not
tracked.

## Project Status

This repository is experimental and is not an official OrcaSlicer release.
Compatibility with every printer profile and firmware is not guaranteed.

## License and Attribution

Magpie Slicer is based on
[OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer). Upstream copyright,
third-party notices, and license requirements remain applicable. See
[LICENSE.txt](LICENSE.txt).
