# Mods

A mod is a JSON file that adds parameters to the settings tabs and sets existing settings from
them. Mods change no code: everything a mod can do is reach a setting the slicer already has,
including the hidden settings that switch on a built-in module (a support generator, the wall
planner, half-height walls). With no mod enabled the slicer slices exactly like OrcaSlicer.

## Where mods come from

The app loads every `*.json` and every package folder `<id>/mod.json` in `<resources>/mods`
(bundled with the app) and then in `<data dir>/mods` (installed by the user), checks them together
and keeps the set for GUI and CLI slicing. A file that fails to load is logged and left out; the
others still load. A package may carry native code ([native-mods.md](native-mods.md)).

Bundled mods are official. An installed package is official or verified when its `SIGNATURE` verifies
with a key the application trusts; a single installed `.json` file is a community mod. Only official
mods may write G-code a printer runs (`machine_start_gcode` and every other `*_gcode` setting, and
`post_process`): a rule of any other mod on one of them does not load.

Installed Mods (main menu) turns mods off or on and removes installed ones; those changes, and
packages from Get Mods, apply at the next start.

## The definition

```json
{
    // Comments are allowed.
    "id": "first_layer_slowdown",          // a-z, 0-9 and _, not starting with a digit
    "name": "First layer slowdown",
    "version": "1.0.0",
    "params": {
        "enable": { "type": "bool",  "default": false, "label": "Slow the first layer" },
        "speed":  { "type": "float", "default": 20, "unit": "mm/s", "label": "First layer speed" }
    },
    "rules": [
        { "target": "initial_layer_speed", "value": "first_layer_slowdown.speed",
          "when": "first_layer_slowdown.enable" }
    ]
}
```

- **params** become rows on the Mods page of a settings tab. `type` is `float`, `int`, `bool`,
  `string` or `float_or_percent` (a number, or `"50%"`, which a rule passes on to a setting that
  takes a percentage). `scope` picks where the value is stored: `process` (default, stored in
  `mod_settings`), `printer` (`printer_mod_settings`), `filament` (`filament_mod_settings`,
  one value per filament, read as `<id>.<name>[i]`), `object` or `region`. An object or region
  parameter keeps its default value on the process Mods page and can be set per object
  (`object_mod_settings`) or per object, layer range, part or modifier (`region_mod_settings`,
  joined per parameter, the most specific level winning) from the object's "Mod settings...". Rules
  read the process value; a native mod reads the value of the object or region it works on.
  `label`, `unit` and `page` are display text.
- **invalidates** lists the slicing steps a change of the value redoes, by step name
  (`posSlice`, `posPerimeters`, `posPrepareInfill`, `posInfill`, `posSupportMaterial`,
  `psWipeTower`, `psSkirtBrim`, `psGCodeExport`, ...). A parameter that lists none redoes
  everything from slicing, so a missing list is slower but never stale. `["none"]` says the value
  changes the print only through the settings its rules write, which invalidate what they change
  themselves; an object or region parameter cannot say so.
- A `string` parameter with **options** is a dropdown: each option is a value, or an object
  `{"value": ..., "label": ..., "label_ko": ...}`. The default must be one of them, and a stored
  value outside them is reported like a value of the wrong type.
- **visible** is an expression over mod parameters; the row is shown only while it holds (a
  condition that cannot be evaluated shows it). It is display only: a hidden row keeps its
  stored value and rules read it as usual, so a mod gates behaviour in its rules, not here.
- **rules** set `target` (any print, printer or filament setting) to the expression `value`,
  when the optional `when` holds. `index` writes one element of a list setting. A higher
  `priority` is applied later and wins; two rules on one setting with the same priority do not
  load together.
- **Expressions** read settings by key and parameters as `<id>.<name>`; they support
  arithmetic, comparisons, `and`/`or`/`not`, `if(c, a, b)`, `min`/`max` and string literals in
  double quotes.

Rules sit above the printer, filament and process presets and below a value the user changed
in a preset: a setting the user edited stays as the user set it. A rule that cannot be
evaluated is reported and slicing stops rather than slicing with a half-applied mod. When rules of
two mods set one setting to different values (two support generators turned on), the higher
priority wins and slicing warns which mod's value is used.

Keep the defaults neutral, so that installing a mod changes nothing until the user turns it on.

## Built-in modules behind slots

Some settings a mod writes choose a built-in module at a slot of the slicer (`ModHost/SlotRegistry`);
an empty value keeps Orca's own behaviour.

| Slot | Setting | Modules | What the module gets and gives |
|---|---|---|---|
| `support.generator` | `support_generator` | `orca`, `resin`; `cura` (native mod `cura_support`, C slot `support.areas`), `mixed` (native mod `mixed_support`, C slot `support.compose`) | fills an object's support layers |
| `support.style` | (every loaded implementation) | `extras` (native mod `support_extras`) | how an object's support is printed: interface pattern, normal and tree wall counts, interface sublayers; a C slot ([native-mods.md](native-mods.md)) |
| `wall.plan` | `wall_planner` | `multi_nozzle` (native mod `multi_nozzle_walls`) | splits a region's walls between two nozzles; a C slot ([native-mods.md](native-mods.md)) |
| `gcode.time_plan` | `layer_time_planner` | `blend` (native mod `layer_time_blend`) | the wait sites of a print (estimated interval since the previous site, park trip, tower block times, chain breaks, guessed intervals) in, per-site waits, park choice, tower cut and warnings out |

`gcode.time_plan` is a C slot filled by native mods only ([native-mods.md](native-mods.md)); the
core has no module of its own there. It exchanges numbers only, never G-code or geometry; the core
marks the sites, estimates their times, fills in the waits and re-estimates the result. Its design
is in [layer-time-blending.md](layer-time-blending.md).

## Mod index

The app finds mods to install in an index: `{"version": 1, "mods": [entry, ...]}`. Each entry
has `id`, `name`, `version`, `description`, `author`, `tags`, `trust`, `url` (http, https or
file) and `sha256` (lower-case hex of the file at `url`). A malformed entry makes the whole
index invalid.

`trust` is `official` (published with the app), `verified` (reviewed by the index maintainers)
or `community`. The app asks before installing a community mod.

Installing downloads the definition, refuses it unless its SHA-256 matches the index, it parses
with the entry's id, it does not take a bundled mod's id, and it loads together with the mods
already present. Only then is it written to `<data dir>/mods/<id>.json`; the settings pages are
rebuilt so its parameters appear.

An entry whose `url` ends in `.zip` is a package. It is downloaded and checked the same way (hash,
id, no file outside the package, a trusted signature when it carries native code), unpacked to
`<data dir>/mods/<id>.pending` and installed at the next start.

- GUI: **Get Mods...** in the main menu searches the index and installs with one button.
- Opening a project whose mod settings name mods that are not installed offers to look them up.
- CLI: `--mod-search "words"` lists matching entries (id, version, trust, name, description,
  tab-separated); `--mod-install id1,id2` installs. `--mod-index <url>` picks the index; the GUI
  reads the app config key `mod_index_url`. Both default to the official index.

## Publishing mods

`scripts/magpie/mod-index` is the layout of an index repository: `mods/<id>.json` with an
optional `mods/<id>.meta.json` (`description`, `author`, `tags`, `trust`), `build_index.py`,
which validates the mods and writes `index.json` with their hashes, and a GitHub Actions
workflow that rebuilds `index.json` on every push. A mod author opens a pull request adding
their two files; the workflow validates it.
