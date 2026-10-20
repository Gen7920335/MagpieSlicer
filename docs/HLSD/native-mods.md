# Native mods

A native mod is a mod package that carries a shared library. The library fills slots the core
offers (a support generator, a wall planner, a batch accelerator) and talks to the core through one C
header, `mods/sdk/include/magpie_mod_api.h`. The core and a mod exchange numbers, polygons, strings
and opaque handles only, never C++ objects, so a mod built against API major 1 keeps loading in every
core of major 1: an Orca merge does not force every mod to be rebuilt.

JSON mods ([mods.md](mods.md)) only set settings. A native mod runs code with the user's rights,
which is why loading it is stricter.

## Package

```
<mods dir>/<id>/
    mod.json            the manifest: a mod definition plus the fields below
    bin/win-x64/<binary>.dll        (linux-x64/lib<binary>.so, macos-arm64/lib<binary>.dylib)
    i18n/, tests/       optional
    SIGNATURE           Ed25519 signature over every file of the folder
```

Manifest fields on top of a JSON mod:

| Field | Meaning |
|---|---|
| `api` | `{"major": 1, "min_minor": 0}`: the API the library was built for |
| `binary` | library name under `bin/<platform>/`, without prefix or extension |
| `slots` | `[{"slot", "version", "impl"}]`: every slot implementation the library registers. Registering one not listed, or not registering one listed, fails the load |
| `depends` | `[{"id", "min"}]`: mods (or `core`, the mod API version) loaded first |
| `conflicts` | mod ids that cannot load together with this one |
| `thread_safe` | `true` lets the core call the mod from several threads; otherwise calls into it are serialised |
| `legacy_keys` | old setting key -> parameter name, for projects from before the feature was a mod |
| `gui` | declarative GUI: `paint_channels` (`id`, `color`, `label`), `aux_towers`, `legends`, `calibrations`, `object_params` (order of the object settings rows) |

## Loading

At start-up, before presets load, the application applies what was scheduled (removals, pending
packages), then reads `<resources>/mods` (bundled, official) and `<data dir>/mods` (installed), orders
the mods so each comes after what it depends on, checks them together, and loads each library:

1. An installed package must carry a valid signature by a key the application trusts
   (`ModPackage.cpp`, `trusted_keys()`); unsigned or community libraries are not loaded. Its
   `bin/<platform>/` folder is copied to `<data dir>/cache/mods/<id>-<version>-<hash>/` and the copy is
   loaded, after checking it is exactly the signed file, so the installed files stay free to be
   replaced while the application runs. Bundled libraries load from where they are.
2. `LoadLibraryExW` with `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32`: a library's
   dependencies resolve from its own folder and System32 only.
3. `magpie_mod_query()` answers `"<id> <version> <major> <minor> fp=<mode>"`. The id and version must
   match the manifest, the major must be the core's, the manifest's `min_minor` at most the core's,
   and the floating-point mode `precise`: the same source built with fast math or FMA contraction
   gives different last digits and different G-code. The SDK template `mods/sdk/MagpieMod.cmake` sets
   the mode and limits the include path to the SDK.
4. `magpie_mod_init(host, registrar)` registers slots and services. A mod that fails is left out
   entirely, manifest and parameters too, and so is every mod that depends on it.

The set never changes while the application runs. Turning a mod off or on, removing it (Installed
Mods) and installing or updating a package (Get Mods with a `.zip` entry) take effect at the next
start, because slicing objects may hold a slot's function table. JSON-only mods installed from Get
Mods are used at once, as before.

## Calls

Every call into a mod runs through `NativeMod::guarded_call`: serialised unless the mod is thread
safe, refused once the mod has crashed, and on Windows under structured exception handling
(`NativeCall.cpp`; MSVC refuses `__try` in a function with C++ objects to unwind, so the guard is a
function with none). A crash ends the call with an error naming the mod, and the mod is disabled for
the rest of the run; restarting is recommended because the process state is not trusted after it.
No exception crosses the boundary: the SDK wrapper (`magpie_mod.hpp`) turns a C++ exception into a
status.

The host table gives a mod its parameters as they apply to the call (region, then object, then
process value), slicer settings, Clipper offsets and booleans, memory, logging, slicing warnings, the
cancel flag and the core's thread pool. One integer coordinate unit is `host->scaling_factor_mm`
millimetres, read at each call: the application switches it for very large beds.

## Core slots and services

Each slot and service is defined by its own C header in `mods/sdk/include/`, next to the base header.

| Name | Header | What the core does with it |
|---|---|---|
| slot `accel.batch` v1 | `magpie_accel.h` | Batch accelerator (`Accel/BatchAccel.*`). Four inner loops (facet layer ranges, rectilinear crossings, Lightning boundary distances, tree support contact points) keep their CPU functions as the reference; the core offers a loop's items as one batch and computes it itself when the accelerator declines. The accelerator asks for a mode at the start of each slice; in Verify mode the core recomputes every item and throws the batch away on any difference, so an accelerator cannot change the G-code. Trusted mode (no check) is granted to official mods only; any other mod asking for it is checked anyway. |
| slot `wall.plan` v1 | `magpie_slot_wall_plan.h` | Wall planner (`ModHost/WallPlanner.*`). The core opens the planner for each region it builds or routes walls of (open reads the planner's parameters and the region's settings once; the printer's nozzles and filaments are handed over as numbers and strings) and asks per layer for the detail nozzle and the toolhead of the whole layer's walls, per island for the plan (wall counts, overlap, infill compensation; the island comes simplified as the walls follow it), and per wall tag for the filament. The core keeps the detail flow (the outer wall width resolved on the detail nozzle), the Classic and Arachne generators that print a plan, the wall tags, the tool ordering and G-code routing, and the line width checks, for which the planner lists the filaments it may use. Picked by the wall_planner value; a value no loaded mod provides stops the slice. |
| slot `support.areas` v1 | `magpie_support.h` | Support generator by areas (`ModHost/NativeSupport.*`). The core hands over an object's islands, enforcers, blockers, an optional mask and the printable area, gets back body, roof and bottom areas per layer, and builds Orca support layers from them (raft, interfaces, toolpaths). Picked by the support_generator value, after the built-in generators. |
| slot `support.compose` v1 | `magpie_slot_support_compose.h` | Support composer (`ModHost/NativeCompose.*`). The mod builds an object's support out of the core's generators: the core gives it support demand (Orca's normal support's overhangs or a support.areas mod's), the build plate shadow and the two support paint channels per object layer, runs Orca's normal support, a support.areas mod or Orca's tree support (with a style, optionally clear of a normal set's plastic) on the mod's masks and keeps each result as a set the mod names by a handle; the mod finally merges a normal and a tree set into the object's support layers (layers of both at one height become a multi-channel layer, `stInnerMultiChannel`, the normal set owning overlaps). The core owns every layer; an exception in a core generator is kept and rethrown once the mod returns, never thrown into the mod. Picked by the support_generator value, after the built-in generators and support.areas. |
| slot `support.contacts` v1 | `magpie_slot_support_contacts.h` | Support generator by contacts and body (`ModHost/NativeContacts.*`). The core hands over an object's islands, slicing heights, enforcers and blockers and its mesh at print height; the mod returns per object layer the areas to support (contacts) and keeps a support body of its own. The core runs Orca's normal-support generator on them (`SupportMaterialOverrides`: the contacts replace its overhang detection, the body replaces its propagation and is asked for once, cut at every intermediate support layer's print Z, then trimmed by the object as usual), so interfaces, gaps, raft and toolpaths are Orca's; support_type does not apply. Picked by the support_generator value, after support.areas and support.compose. |
| slot `object.z_offset` v1 | `magpie_slot_object_z_offset.h` | Object lift (`ModHost/ObjectZOffset.*`). Every loaded implementation is asked, per object, how far to lift the object when it has support; `SlicingParameters` lifts only an object with support of its own, so nothing prints in the air, and records the lift (`object_z_offset`) for the support code that keeps the first support layer's feet under a lifted object. Two implementations lifting one object, or one failing, is reported by `Print::validate`; the slicing heights then take no lift. |
| slot `layer.bands` v1 | `magpie_slot_layer_bands.h` | Layer bands (`ModHost/LayerBands.*`, `LayerBands.hpp`, kept per object as `PrintObject::layer_bands()`). A model layer keeps its settings, cooling and layer change; a band plan prints some of its paths in thinner physical bands inside it. Every loaded implementation is asked per object whenever the print is applied (object-scope parameters apply); one banding nothing leaves the object alone, two banding one object or banded objects of one print on different implementations are reported by `Print::validate`. The plan names the banded wall depths (from the outer wall), whether the support is banded, the band fractions of a model layer, the travel lift over banded paths (model layer heights) and the support band limits (fractions of the layer height); the core's band executor (the `HalfLayer*` sources, walls, support sources and execution plan, wipe tower travel, seams, validation) slices, builds and prints the bands. The slot carries any band layout; the executor prints two equal bands with the outer two walls and refuses other plans in `Print::validate`. Per layer the core lists where the later band's block can go in the layer's print order with each place's timing, and the implementation picks one (`schedule`); the core checks the choice is an allowed boundary after the lower block. A plan that changes when the print is applied redoes the object's walls and support even when the mod's parameters did not declare it. |
| slot `support.style` v1 | `magpie_slot_support_style.h` | Support style (`ModHost/SupportStyle.*`, read in `SupportParameters`). Every loaded implementation is opened per object (object-scope parameters apply); one with nothing to change returns no state and is not used, two that both style an object stop the slice. The style gives the interface pattern, the normal and tree wall counts (-1 to 10) and, per interface layer number of each interface stack size the object prints, a pattern and angle of its own (interface sublayers); the core asks once per object and keeps the answers. The style may give the restyled layers a nozzle temperature of their own (`sublayer_temperature`, appended in API 1.1): the core groups their paths in a collection carrying it (`ExtrusionEntityCollection::interface_temperature`), prints them at it without a wait and returns to the filament's temperature when the support ends, unless a gcode.events mod owns the support's temperatures; `Print::validate` checks it against the firmware's cold-extrusion limit and the filament's maximum. |
| slot `gcode.events` v1 | `magpie_slot_gcode_events.h` | G-code events (`ModHost/GCodeEvents.*` dispatches, `GCode/GCodeEventHost.*` is the writer). Every loaded implementation is opened once per G-code export, before anything is written; one with nothing to do returns no state. The core raises events at fixed points (layer begin, every extrusion path with its role and the temperature its support collection carries, a band visit about to change tools, the end of an object's support, of a layer's band visits, of the first layer and of each layer) and asks for an order (an object's support after its other extrusions, mixed support by role), for auxiliary towers on a layer (layer time blending waits over them), for per-point path speeds and, official mods only, for start G-code filters before and after the placeholders are filled in (with a comment line and a slicing warning). A mod writes through the writer it was given: temperatures, dwells and the auxiliary fan as commands the core puts in each firmware's form (a wait for a lower temperature is M109 R on Marlin), travels with the core's travel rules, free moves (a nozzle brush, a calibration's marks) after which the wipe path is dropped, extrusions through the core's extrusion code (with an optional slower tail), and comment lines only. The core keeps an auxiliary tower's stored per-plate position and its placement (`GCode/AuxiliaryTower.hpp`, the area every nozzle printing it reaches); the mod owns its shape and passes. An exception in a writer function is kept and rethrown once the mod returns; extrusions a mod asks for raise no events of their own. Outside an export the same table answers `Print::validate` (one object's settings), the 3D scene's tower preview (whether a tower shows and its size) and whether a tower prints with the support filament at full layer height (line width checks). |
| slot `gcode.time_plan` v1 | `magpie_slot_time_plan.h` | Time planner (`ModHost/SlotRegistry.*`, `GCode::apply_layer_time_plan`). The core marks wait sites in the exported G-code, estimates their intervals and hands the numbers over; the planner returns per-site waits, park choices, a tower cut and warning kinds, which the core checks (counts, indices, finite non-negative waits) before writing them. Picked by layer_time_planner; a name no mod provides stops the slice. |
| service `core.geometry` v1 | `magpie_geometry.h` | The core's own ClipperUtils operations (offset, booleans, simplify, union_ex, area, nearest-edge distances, and appended later intersection_ex/diff_ex: a mod checks the table size), so a mod that came out of the core computes exactly the polygons it computed inside it. Services named `core.*` need no `depends`. |

## Official mods

Official mods live in `mods/<id>/` (source, `mod.json`, `tests/`) and build, with the SDK template only,
into package folders `<build>/official_mods/<id>/`: `cuda_compute` (fills `accel.batch`), `cura_support`
(fills `support.areas` as "cura" and offers `cura.support_areas`), `multi_nozzle_walls` (fills `wall.plan` as
"multi_nozzle"), `mixed_support` (fills `support.compose` as "mixed" and offers `mixed_support.plan`, its split of support
demand, which the core's tests call), `support_extras` (fills `support.style` as "extras"), `resin_support` (fills `support.contacts` and
`object.z_offset` as "resin"), `half_layer` (fills `layer.bands`: H/2, the outer two walls and the support in two
half-height bands), and on `gcode.events`: `low_temp_interface` (the low temperature support interface and its
temperature drop tower), `snapmaker_u1` (the U1 start G-code guard, on by default) and `lesic` (the G-code of the LESIC ring
calibration, which the application's Calibration menu starts). They are signed with `--mod-sign` and
the official key, which stays off the repository, and installed like any package. Tests load the build
folders directly (`magpie_use_official_mods` in `tests/CMakeLists.txt`; `fff_print/test_helpers.cpp`
makes their slots active for the whole run). A mod's own unit tests (`mods/<id>/unit/`, `tests/mods/CMakeLists.txt`) compile
its sources with Catch2 and nothing of the core; the core's side of its slot is tested through the built
library. The resin support mod is the one official mod built against the core: PrusaSlicer 2.9.6's SLA support
points and trees (its own copy under `mods/resin_support/prusa/`, compiled with `sla=resin_sla` so it cannot
collide with OrcaSlicer's own SLA library, which the core keeps) work on the core's geometry types, so the mod links
libslic3r statically and is compiled with libslic3r's include directories, definitions and options. It still reaches
the slicer only through its two slots; it sets its copy's scaling factor from the host at each call. Its unit tests
(`mod_resin_support_tests`) are PrusaSlicer's SLA tests on that copy. `layer_time_blend` fills `gcode.time_plan` as "blend".

## Declarative GUI

The core's GUI reads what a mod declares in its manifest and draws it; mod code runs in the GUI only to answer a
question with data, such as the auxiliary tower the 3D scene previews and drags (gcode.events `tower_preview`).
`gui.paint_channels` (at most two, `id`, `color`, `label`/`label_<lang>`) with `gui.paint_when` (an expression over
parameters) gives the support paint tool its channels: while the first such mod's condition holds, the tool offers
the channels next to plain enforcers, paints them in their colours and stores channel 0 and 1 in
`ModelVolume::support_channel_facets` (3MF attribute names of line 1, so its projects keep their painting); the
support.compose host hands them to the mod as `paint_channel` 0 and 1.

## Native mods in the G-code

When a print uses a native mod (its time planner or support generator made part of the print, or it
accelerated the slicing session), the G-code header names it
(`; mods: id version, ...`) so the print can be reproduced. A mod that is loaded but not used, such
as a bundled one whose feature is off, is not named, so output without native code stays
byte-identical to Orca's.

## Tools

- `--mod-test <id>`: checks that the mod loaded (and for native code its query and slots), then
  slices each case in its `tests/` folder twice in separate processes; the two G-codes must match and
  pass the G-code checks (bed bounds, temperatures, finite numbers, extrusion per move), and a case
  file can list texts the G-code must or must not contain.
- `--mod-sign <folder> --mod-sign-key <pem> [--mod-sign-key-id <name>]` writes `SIGNATURE`.
- `--mod-keygen <pem>` writes a new Ed25519 key and prints its public half.
