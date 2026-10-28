<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer.png" width="150">

# Magpie Slicer Next

An FFF 3D printer slicer that adds its features to OrcaSlicer as **mods**

[한국어](README.md) · [Mod repository (magpie-mods)](https://github.com/Gen7920335/magpie-mods) · [Original OrcaSlicer README](README_ORCASLICER.md)

</div>

> This is the second-generation line and is still in development. Always check the preview and the generated G-code before sending a print to a real machine.

## About

Magpie Slicer Next is based on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) nightly. The first generation (2.5.0.0.x, up to Beta 7) patched its features into Orca's slicing code; in this line every Magpie feature is a native mod that the app loads and that can be turned on and off one by one.

- **With every mod off it slices the way Orca does.** Only Orca bug fixes that are safe for output and real use always apply. Comparison runs against Orca on the same settings (`scripts/magpie/golden`) check this.
- **Mod settings beside Orca's:** each mod's settings are shown next to the related group on Orca's settings pages.
- **Separate install:** app key `MagpieSlicerNext`, executable `magpie-slicer-next`, install name "Magpie Slicer Next". It shares no data folder or file associations with OrcaSlicer or the first generation. `orcaslicer://` links are still handled.
- **Orca user preset import:** on first start it copies the user presets from OrcaSlicer's data folder once (existing files are never overwritten). File > Import > "Import OrcaSlicer user presets" runs it again.
- **First-generation settings migrate:** opening a first-generation project or preset moves its Magpie-only settings to the matching mod settings.
- **Korean translation** of the app and of every mod's names and settings.

## Bundled mods

| Mod | What it does |
| --- | --- |
| Multi-nozzle walls (`multi_nozzle_walls`) | Uses toolheads with different nozzle sizes together: sharp outer walls with the small nozzle, the rest with the large one |
| Half-height outer walls and support (`half_layer`, H/2) | Prints the two outermost walls and the support twice at half the layer height |
| Cura support (`cura_support`) | CuraEngine-style normal (area) support |
| Mixed support (`mixed_support`) | Normal support where it stands on the bed, tree support elsewhere |
| Resin support (`resin_support`) | PrusaSlicer's SLA support trees printed as FFF support |
| Support extras (`support_extras`) | Triangle interface pattern, interface sublayers (pattern, angle, temperature), wall counts for normal and tree support |
| Low temperature support interface (`low_temp_interface`) | Prints model and support interface at different temperatures with one nozzle, with a temperature drop tower |
| Layer time blending (`layer_time_blend`) | Evens out layer times with waits |
| LESIC ring calibration (`lesic`) | A ring calibration for temperature and volumetric flow in one print |
| Snapmaker U1 (`snapmaker_u1`) | Guards U1 start G-code that is missing the stock start sequence (PRINT_START, homing, bed mesh, purge) |

A Snapmaker U1 monitor plugin is included as well.

## The mod system

- **Native mods:** a mod is a DLL with a C ABI. The core asks mods for plans at fixed points (slots such as `wall.plan`, `support.areas`, `layer.bands`, `gcode.events`), and mods borrow the core's services (for example `core.geometry`). Mod settings are stored in projects and presets. A mod that crashes is not called again for the rest of that run.
- **Signatures and trust:** official mods are signed with the Magpie official key, verified mods with the verified key (Ed25519). Unsigned mods still work, with an "unverified" tag and a one-time warning. No private key is in this repository.
- **Getting mods:** "Get Mods..." in the main menu finds and installs mods; on the command line use `--mod-search`, `--mod-install`, `--mod-index`.
- **Writing mods:** the SDK (`mods/sdk`, or `sdk/` in magpie-mods) builds a mod without the app's sources. See the [mod developer guide](https://github.com/Gen7920335/magpie-mods/blob/main/docs/index.md) (Korean).

## Building (Windows)

```bat
build_win.bat -s --tests
```

- Tests: `ctest -C Release -j8` in the `build` folder
- Output comparison with Orca: `scripts/magpie/golden/golden.py`
- Random combination sweep: `scripts/magpie/sweep/random_sweep.py`
- Installer: `scripts/build_installer.ps1`
- Signing the official mods: when the environment variable `MAGPIE_MOD_SIGN_KEY` names a key file during `cmake --install`, the mods in `resources/mods` are signed; without it they are installed unsigned.

Versioning: [docs/MAGPIE_VERSIONING.md](docs/MAGPIE_VERSIONING.md). Changes still made inside Orca's code: [ORCA_PATCHES.md](ORCA_PATCHES.md).

## License and credits

Magpie Slicer Next is distributed under the **GNU Affero General Public License v3.0** ([LICENSE.txt](LICENSE.txt)). If you distribute a modified version or offer it over a network, its source must be published under the same license. It comes with no warranty.

Magpie Slicer Next builds on the work of:

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer): SoftFever and the OrcaSlicer contributors (AGPL-3.0). Most of the code in this repository.
- [Bambu Studio](https://github.com/bambulab/BambuStudio): Bambu Lab (AGPL-3.0), which OrcaSlicer is based on.
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer): Prusa Research (AGPL-3.0), which Bambu Studio is based on; the resin support mod contains PrusaSlicer 2.9.6's SLA support code.
- [Slic3r](https://github.com/slic3r/Slic3r): Alessandro Ranellucci and contributors (AGPL-3.0).
- [CuraEngine](https://github.com/Ultimaker/CuraEngine): UltiMaker (AGPL-3.0); the Cura support mod ports CuraEngine's support code.

Copyright notices in the original files are kept. Licenses of the bundled libraries are listed in the app under About > Copyrights. "OrcaSlicer", "Bambu Studio", "PrusaSlicer" and "Cura" are names of their owners; Magpie Slicer Next is not made or endorsed by those projects.

The mod SDK headers and template are also available under the MIT license in [magpie-mods](https://github.com/Gen7920335/magpie-mods), so mods built with the SDK can use any license.
