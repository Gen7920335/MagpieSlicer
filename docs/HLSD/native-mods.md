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
| slot `support.areas` v1 | `magpie_support.h` | Support generator by areas (`ModHost/NativeSupport.*`). The core hands over an object's islands, enforcers, blockers, an optional mask and the printable area, gets back body, roof and bottom areas per layer, and builds Orca support layers from them (raft, interfaces, toolpaths). Picked by the support_generator value, after the built-in generators. |
| service `core.geometry` v1 | `magpie_geometry.h` | The core's own ClipperUtils operations (offset, booleans, simplify, union_ex, area, nearest-edge distances), so a mod that came out of the core computes exactly the polygons it computed inside it. Services named `core.*` need no `depends`. |

## Official mods

Official mods live in `mods/<id>/` (source, `mod.json`, `tests/`) and build, with the SDK template only,
into package folders `<build>/official_mods/<id>/`: `cuda_compute` (fills `accel.batch`) and `cura_support`
(fills `support.areas` as "cura" and offers `cura.support_areas`). They are signed with `--mod-sign` and
the official key, which stays off the repository, and installed like any package. Tests load the build
folders directly (`magpie_use_official_mods` in `tests/CMakeLists.txt`; `fff_print/test_helpers.cpp`
makes their slots active for the whole run).

## Native mods in the G-code

When native mods are loaded, the G-code header names them (`; mods: id version, ...`) so a print can
be reproduced. With none loaded nothing is written.

## Tools

- `--mod-test <id>`: checks that the mod loaded (and for native code its query and slots), then
  slices each case in its `tests/` folder twice in separate processes; the two G-codes must match and
  pass the G-code checks (bed bounds, temperatures, finite numbers, extrusion per move), and a case
  file can list texts the G-code must or must not contain.
- `--mod-sign <folder> --mod-sign-key <pem> [--mod-sign-key-id <name>]` writes `SIGNATURE`.
- `--mod-keygen <pem>` writes a new Ed25519 key and prints its public half.
