<div align="center">

<img alt="Magpie Slicer logo" src="resources/images/MagpieSlicer_192px_transparent.png" width="150">

# Magpie Slicer

An FFF 3D printer slicer that adds its features to OrcaSlicer as **mods**

[Download](../../releases) · [한국어](README.md) · [Mod repository (magpie-mods)](https://github.com/Gen7920335/magpie-mods) · [Original OrcaSlicer README](README_ORCASLICER.md)

</div>

> Always check the preview and the generated G-code before sending a print to a real machine. Some features add temperature changes, tool changes and auxiliary towers.

## About

Magpie Slicer is based on [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer) nightly. Earlier versions (2.5.0.0.x, up to Beta 7) patched its features into Orca's slicing code; in this version every Magpie feature is a native mod that the app loads and that can be turned on and off one by one.

- **With every mod off it slices the way Orca does.** Only Orca bug fixes that are safe for output and real use always apply; they are listed in [ORCA_PATCHES.md](ORCA_PATCHES.md). Comparison runs against Orca on the same settings (`scripts/magpie/golden`) check this.
- **Mod settings beside Orca's:** each mod's settings are shown next to the related group on Orca's settings pages. Support generators are picked in the single `Support > Type` dropdown.
- **Installs apart from OrcaSlicer:** it shares no data folder (`%APPDATA%\MagpieSlicer`) or file associations with OrcaSlicer, so both can be installed side by side. `orcaslicer://` links are still handled.
- **Orca user preset import:** on first start it copies the user presets from OrcaSlicer's data folder once (existing files are never overwritten). File > Import > "Import OrcaSlicer user presets" runs it again.
- **Settings from earlier versions migrate:** opening a project or preset moves its Magpie-only settings to the matching mod settings.
- **Manual update check:** Help > Check for updates queries the GitHub releases. Nothing is checked automatically at start.
- **Korean translation** of the app and of every mod's names and settings.

Versions follow `<Orca version>.<Magpie line>.<revision>`. The latest stable release is 2.5.0.2.0 and the latest beta is [2.5.0.2.1 beta](../../releases/tag/v2.5.0.2.1). The Windows 10/11 x64 installer is on the [Releases](../../releases) page.

### What's new in 2.5.0.2.1 beta

- **Settings categories:** options on the process, filament and printer tabs are grouped into foldable categories with option counts and a header that stays pinned while scrolling.
- **Complete Korean translation,** including the printer connection panel and the category titles.
- **Snapmaker U1 tool change height:** the cooling pass now reads G91 moves in tool change macros as relative, so outer walls are no longer printed 2 mm in the air after a tool change.
- **H/2 support:** an option to use half-height support only in the contact zones, plus several band fixes.
- **Error and crash fixes:** the "Serializing NaN" slicing error, the H2D 0.4/0.6 nozzle tag crash, a hang when reslicing after Bambu dual-nozzle auto-mapping, plate filament sequence and flow validation crashes, the tree support radius 0 error. Crash logs now come with a minidump.
- **Support and output:** resin support contacts and pillar bases, first support layer over a raft, narrow islands, per-nozzle prime tower width, low-temperature interface drop tower layer heights.

See the [release notes](../../releases/tag/v2.5.0.2.1) for the full list.

## Bundled mods

The 10 official mods bundled with the app. Their sources are in `mods/` here and in [magpie-mods](https://github.com/Gen7920335/magpie-mods).

| Mod | What it does | Where |
| --- | --- | --- |
| Multi-nozzle walls (`multi_nozzle_walls`) | Uses toolheads with different nozzle sizes together: sharp outer walls with the small nozzle, the rest with the large one. Automatic or chosen detail tool, per-layer-range toolhead override | Quality > Multi-nozzle walls |
| Half-height outer walls and support (`half_layer`, H/2) | Prints the two outermost walls and the support twice at half the layer height; the second pass is slotted mid-layer for even cooling; support can be limited to the contact zones | next to Quality > Wall loops, next to Support |
| Cura support (`cura_support`) | CuraEngine-style normal (area) support with Cura's stair, tower and conical settings | Support > Type |
| Mixed support (`mixed_support`) | Normal support where it stands on the bed, tree support elsewhere; regions can be painted | Support > Type |
| Resin support (`resin_support`) | PrusaSlicer's SLA support trees printed as FFF support; the object is lifted and held by point contacts | Support > Type |
| Support extras (`support_extras`) | Triangle interface pattern, interface sublayers (pattern, angle, temperature), wall counts for normal and tree support | next to Support > Interface pattern |
| Low temperature support interface (`low_temp_interface`) | Prints only the support interface at a lower temperature so it sticks less; cools in place or on a temperature drop tower, with auxiliary fan and brush | Support > Low-temperature support interface |
| Layer time blending (`layer_time_blend`) | Evens out layer times with waits at a dedicated tower, the prime tower, a park position or in place | Speed > Layer time blending |
| Low temperature interface calibration (`low_temp_calibration`) | LESIC ring calibration (temperature × volumetric flow) and a one-touch grid of specimens over two settings | Calibration menu |
| Snapmaker U1 (`snapmaker_u1`) | Guards U1 start G-code that is missing the stock start sequence (PRINT_START, homing, bed mesh, purge). On by default | Printer > next to start G-code |

A Snapmaker U1 monitor plugin is included as well.

### What changes even with every mod off

- **Per-nozzle line widths:** on a printer with mixed nozzle sizes, absolute widths (mm) are read as a share of the reference nozzle and scaled to each nozzle. Set them per nozzle with the nozzle switch under Quality > Line width.
- **Tool view palette:** toolheads sharing a filament colour are still told apart in the preview's Tool view.
- **Tidier settings:** a child setting of a switched-off parent is hidden instead of greyed, and settings duplicated between a mod and Orca are shown as one. Options on the settings tabs are grouped into foldable categories.

## Mods

### Using mods

- **Install:** Main menu > "Get Mods..." searches the mod index and installs. Manually, copy the package folder (`<id>/mod.json`) into `%APPDATA%\MagpieSlicer\mods\`.
- **Manage:** Main menu > "Installed Mods..." turns mods on or off and removes them. Changes apply from the next start.
- **Per object:** right-click an object or part > "Mod settings..." to give it its own values (where the mod provides object- or region-scoped parameters).
- **Command line:** `--mod-search "words"`, `--mod-install id1,id2`, `--mod-index <url>`.
- **Checking the output:** the G-code header line `; mods:` names the mods that print actually used.

### Trust tiers

Mod packages are signed with Ed25519. The three tiers have the same permissions; they differ only in what is shown and asked.

| Tier | Condition | Shown as |
| --- | --- | --- |
| Official | Bundled with the installer, or signed with the Magpie official key | Blue badge |
| Verified | Source submitted by the author, reviewed, then signed with the Magpie verified key | Blue badge "Verified" |
| Not verified | No signature, unknown key, or files changed after signing | Red badge; a one-time warning before native code loads |

A mod can change the slicing result and the G-code your printer runs, so use unverified mods only from sources you trust. A mod that crashes is disabled for the rest of that run; the app keeps running. No private key is in this repository.

### Writing mods

The developer documentation and the SDK live in the [magpie-mods](https://github.com/Gen7920335/magpie-mods) repository.

- **[Mod developer guide](https://github.com/Gen7920335/magpie-mods/blob/main/docs/index.md)** (Korean): trust tiers, the two kinds of mods, toolchain and SDK, package layout, manifest (`mod.json`) reference, native ABI, mandatory rules, localisation, a Hello mod walkthrough, testing, signing and distribution, slot reference, printer connection, checklist, troubleshooting, official mod examples.
- **SDK** ([`sdk/`](https://github.com/Gen7920335/magpie-mods/tree/main/sdk), identical to `mods/sdk` here): the C header `magpie_mod_api.h` and per-slot headers, the C++ helper `magpie_mod.hpp`, the CMake template `MagpieMod.cmake`. MIT licensed, so mods built with it may use any license.
- **Official mod sources** ([`mods/`](https://github.com/Gen7920335/magpie-mods/tree/main/mods)): a starting point for new mods.

A mod places its settings next to Orca's with the manifest's `"ui"`: right after a setting's line (`after`), in a page's group (`page`, `group`, `group_after`), or merged with an Orca setting into one control (`merge_into`). From 2.5.0.2.1 a new group can also name the foldable category it goes under (`category`, e.g. `"Support engines"`, `"Walls"`); without one it takes the category of the group before it. The guide's section 4.9 lists each page's categories and the rules.

There are two kinds of mods.

| Kind | Content |
| --- | --- |
| JSON mod | A single `mod.json`. Adds parameters to the settings pages and changes existing Orca settings through rules (expressions). No code. |
| Native mod | `mod.json` + DLL. Implements the core's slots to change slicing behaviour itself. The C ABI lets it build from the SDK alone, without the app's sources. |

Slots a native mod can implement, with the official mods as examples:

| Slot | Role | Example |
| --- | --- | --- |
| `wall.plan` | Per-region wall plan (wall counts, nozzle assignment) | multi_nozzle_walls |
| `support.areas` / `support.compose` / `support.contacts` | Support generation (areas / composed from core generators / contacts and body) | cura_support / mixed_support / resin_support |
| `support.style` | Interface pattern, sublayers, wall counts | support_extras |
| `object.z_offset` | Object lift | resin_support |
| `layer.bands` | Physical band split within a layer (support: all of it or the contact zones only) | half_layer |
| `gcode.events` | G-code export events (temperatures, waits, auxiliary towers, start G-code filter) | low_temp_interface, snapmaker_u1 |
| `gcode.time_plan` | Layer time wait planning | layer_time_blend |
| `calibration.project` | Calibration menu entries and project generation | low_temp_calibration |
| `printer.agent` | Printer connection (status, commands, upload) | — |

The core services `core.geometry` (polygon operations) and `core.net` (HTTP/TLS, GUI only) are available to mods.

Building a native mod, in short (see the guide's walkthrough, section 8):

1. Copy `sdk/` and configure the project with `MagpieMod.cmake`. The floating-point mode must be `precise` (the template sets it); a library built otherwise is refused at load.
2. Declare id, version, API version, binary name, implemented slots and parameters in `mod.json`. Declared slots and actual registrations must match exactly.
3. Export `magpie_mod_query` / `magpie_mod_init` / `magpie_mod_shutdown` and register the slot tables.
4. Assemble the package folder (`<id>/mod.json`, `bin/win-x64/<binary>.dll`, optionally `i18n/` and `tests/`).
5. Verify with `magpie-slicer.exe --mod-test <id>`: load, slot registration, determinism (the same input sliced in two processes and compared) and G-code safety.
6. Distribute. An unsigned package loads as "Not verified". For verification, submit the source and build instructions for review (guide, section 10).

Mandatory rules (guide, section 6): exchange data only with the core (no C++ objects or exceptions across the boundary), read the coordinate scale from `host->scaling_factor_mm` on every call, give the same output for the same input, report warnings and errors through the host API.

Design documents: [mod system](docs/HLSD/mods.md), [native mods](docs/HLSD/native-mods.md), [layer time blending](docs/HLSD/layer-time-blending.md).

## Building (Windows)

```bat
build_win.bat -s --tests
```

- Tests: `ctest -C Release -j8` in the `build` folder
- Output comparison with Orca: `scripts/magpie/golden/golden.py`
- Random combination sweep: `scripts/magpie/sweep/random_sweep.py`
- Installer: `scripts/build_installer.ps1 [-ModSignKey <pem>]`
- Signing the official mods: when the environment variable `MAGPIE_MOD_SIGN_KEY` names a key file during `cmake --install`, the mods in `resources/mods` are signed; without it they are installed unsigned.
- Mod tools: `--mod-test <id>`, `--mod-sign <folder> --mod-sign-key <pem>`, `--mod-keygen`

Versioning is described in [docs/MAGPIE_VERSIONING.md](docs/MAGPIE_VERSIONING.md), the fixes kept in Orca's code in [ORCA_PATCHES.md](ORCA_PATCHES.md), and the development rules in [AGENTS.md](AGENTS.md) and [docs/PROJECT_CONTRACTS.md](docs/PROJECT_CONTRACTS.md).

## Known limitations

- Only Windows builds have been made.
- Features that change tools several times within a layer (H/2) enlarge the prime tower. On a placement error, move the tower in the preview or use automatic placement.
- On a printer with mixed nozzle sizes, an absolute line width typed for the small nozzle can fail validation. Use a percentage or per-nozzle line widths.

Bug reports and suggestions go to [Issues](../../issues); for security reports see [SECURITY.md](SECURITY.md).

## License and credits

Magpie Slicer is distributed under the **GNU Affero General Public License v3.0** ([LICENSE.txt](LICENSE.txt)). If you modify and distribute it, or offer it over a network, you must publish your source under the same license. It comes with no warranty.

Magpie Slicer stands on the work of:

- [OrcaSlicer](https://github.com/OrcaSlicer/OrcaSlicer): SoftFever and the OrcaSlicer contributors (AGPL-3.0). Most of the code in this repository.
- [Bambu Studio](https://github.com/bambulab/BambuStudio): Bambu Lab (AGPL-3.0). The basis of OrcaSlicer.
- [PrusaSlicer](https://github.com/prusa3d/PrusaSlicer): Prusa Research (AGPL-3.0). The basis of Bambu Studio; the resin support mod carries the SLA support code of PrusaSlicer 2.9.6.
- [Slic3r](https://github.com/slic3r/Slic3r): Alessandro Ranellucci and contributors (AGPL-3.0).
- [CuraEngine](https://github.com/Ultimaker/CuraEngine): UltiMaker (AGPL-3.0). The Cura support mod ports CuraEngine's support code.

Copyright notices in the original files are kept. The licenses of the libraries used are listed in the app under About > Copyright. "OrcaSlicer", "Bambu Studio", "PrusaSlicer" and "Cura" are the names of their respective owners; Magpie Slicer is not made or endorsed by those projects.

The mod SDK headers and templates are also offered under the MIT license in [magpie-mods](https://github.com/Gen7920335/magpie-mods), so mods built with the SDK may be distributed under any license.
