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

## Implementations behind slots

For some jobs the core runs one implementation a native mod provides, picked from the active mods' slot
declarations (`ModHost/SlotChoice.*`, `ModSet::choose_slot`): the declaration whose `"when"` holds,
read with the parameter values of the object or region the job is for, and of several that hold, the
one with the highest `"priority"`; two that hold at one priority stop the slice, a lower one that also
holds is reported. None picked keeps Orca's own behaviour. A declaration's `"settings"` are values the
core reads for the picked implementation. No setting of the print config carries the choice.

| Job | Slots | Picked per | Implementations | What it gets and gives |
|---|---|---|---|---|
| support | `support.areas`, `support.compose`, `support.contacts` | object | `mixed` (native mod `mixed_support`, priority 2), `resin` (`resin_support`, 1), `cura` (`cura_support`, 0) | fills an object's support layers |
| walls | `wall.plan` | region | `multi_nozzle` (native mod `multi_nozzle_walls`); setting `detail_wall_speed` | splits a region's walls between two nozzles; a C slot ([native-mods.md](native-mods.md)) |
| layer time | `gcode.time_plan` | print | `blend` (native mod `layer_time_blend`); settings `wait_at`, `tower_size`, `park_x`, `park_y`, `heat_rate`, `cool_rate` | the wait sites of a print (estimated interval since the previous site, park trip, tower block times, chain breaks, guessed intervals) in, per-site waits, park choice, tower cut and warnings out |

A support declaration may add entries to the support type dropdown (`ModHost/SupportMenu.*`):
`"menu": {"param": "enable", "entries": [{"label", "label_<lang>", "support_type"}]}`. The dropdown lists
Orca's support types, then every menu entry of the loaded mods in load order, so a support mod installed
later shows up without a new build. Picking an entry writes `support_type` (the Orca value the entry
stands on, which the generator may read: Cura reads auto or manual) and switches the declaring mod's
bool parameter `param` on and every other menu mod's off; picking an Orca type switches them all off.
The dropdown shows the entry of the mod the slot choice picks, so a file or command line that turns two
on still shows, and slices, the one with the highest priority, and `Print::validate` warns. The menu's
parameter is not shown on the Mods page. A string parameter with `"choices_from": "<slot>"` is a dropdown
of `orca` and the implementations the loaded mods register for that slot (Mixed support's normal support
generator lists every `support.areas` generator); the mod checks the value it reads. Line 1's support
types (`normal_cura(auto)`, `mixed(auto)`, ...) load as the matching entries (`LegacySettings`).

`support.style` (`extras`, native mod `support_extras`), `layer.bands` and `gcode.events` ask every
loaded implementation instead; each answers whether it applies. A parameter a `"when"` or a setting
reads must name the steps it changes (`"invalidates"`), since no setting written from it invalidates
them.

`gcode.time_plan` is a C slot filled by native mods only ([native-mods.md](native-mods.md)); the
core has no module of its own there. It exchanges numbers only, never G-code or geometry; the core
marks the sites, estimates their times, fills in the waits and re-estimates the result. Its design
is in [layer-time-blending.md](layer-time-blending.md).

## Printer connections

A native mod can connect a printer the slicer has no agent for, through the slot `printer.agent` v1
(`mods/sdk/include/magpie_slot_printer_agent.h`). It touches nothing of slicing or G-code. Each loaded
implementation becomes one of Orca's printer agents, `mod:<mod id>` (`.<impl>` appended when the implementation
is not named after its mod), in the printer preset's **Printer Agent** dropdown (`ModPrinterAgent`,
`NetworkAgentFactory::register_all_agents`). Orca's Device tab, AMS view and send dialog then drive the printer
unchanged. The mod's manifest must say `"thread_safe": true`; the slot is not registered otherwise.

The core and the mod exchange vendor-neutral JSON, so a mod does not change when Orca's device format does, and the
core knows no printer brand:

- **Status** (`events->status`), version `"v": 1`. A partial object updates the fields it has; objects merge key by
  key, anything else replaces; unknown fields are ignored; fields are only ever added.
  `state` (`idle`, `preparing`, `printing`, `paused`, `finished`, `failed`, `offline`), `stage` (the firmware's own
  word), `progress` 0-100, `layer`, `total_layers`, `remaining_s`, `elapsed_s`, `file`,
  `temps` `{nozzle, bed, chamber: [current, target]}` in °C, `fans` `{part, aux: percent, box: level}`,
  `speed_mode` 1-3, `light` `{camera, chamber: bool}`, `units` (material units: `id`, `model`, `temp`,
  `humidity`, `drying`, `trays` of `slot`, `material`, `color` `#RRGGBB`, `brand`, `remain_pct`,
  `state` `loaded`/`empty`/`feeding`), `errors` (`code`, `text`), `caps` (`nozzle_max`, `bed_max`, and what the
  printer can do). `ModPrinterStatus` turns it into the `push_status` message MachineObject parses; units of more
  than four trays show their first four.
- **Commands** (`send_command`), `{"cmd": ...}`. From the Device tab: `pause`, `resume`, `stop`,
  `set_temp {heater: nozzle|bed, target}`, `set_fan {fan: part|aux|box, pct}`, `set_speed_mode {mode}`,
  `set_light {light, on}`, `home {axes}`, `move {axis, distance_mm, speed_mm_min}`, `extrude {...}`,
  `auto_level`. From the printer panel: an action's `command` with its `args`, and `panel_message` /
  `panel_closed` for web panels. Raw G-code never crosses: of Orca's `gcode_line` only the single fan,
  temperature and home lines its own controls send are recognised. A mod answers an unknown command with
  `MP_ERR_NOT_FOUND` and one its printer cannot do now with `MP_ERR_FAILED` and a reason the user reads.
  `send_command` returns quickly: it publishes and leaves waiting to the mod's own thread.
- **Printing**: `upload` (`{file_name, md5, size}`, progress and cancel from the send dialog) and then
  `start_print` (`file_name`, `remote_path`, `md5`, `size`, `printer_model` of the preset, `use_ams`,
  `ams_mapping` `[{tool, unit, slot, slot_index, color, material}]` from the send dialog's mapping with the
  project's filament colour and type per tool, `options` `{auto_leveling, vibration_compensation,
  flow_calibration, timelapse}`). The mod refuses a start it judges unsafe (another printer model, a tray of
  another material) with a reason; the send dialog shows it.
- **Events** (`events->event`): `{"type": "probe", "info": {model_id, model_name, name, firmware}}` names the
  printer for the device list, `notice` (`text`) becomes a notification, `panel_message` (`panel`, `data`) goes to
  that web panel, anything else reaches the printer panel.

**connect** gets `{dev_id, host, port, fields, profile, secret_keys}`: `host` is the preset's print host,
`fields` the printer panel's form values for that host, `profile` the preset's `printer_model`, name and nozzle
diameter, `secret_keys` the credential-store key of each secret field. A first failed attempt is reported in the
printer panel and as a notification, not to the device code (which would drop the selected machine); a connection
lost after it worked is.

### Network access

A native mod reaches the network only through the core's service `core.net` v1 (`mods/sdk/include/magpie_net.h`,
`ModNetService`), which exists in the GUI application only; the command line tool gives a null import, and a mod
must still load there. The manifest lists every host and port:

```json
"network": { "reason": "Talks to your printer on the local network.", "reason_ko": "...",
             "endpoints": [ { "host": "lan", "ports": [18910, 9883], "purpose": "control" } ] }
```

`host` is `lan` (a private, link-local or `.local` address), a host name, or `*.<domain>` (names below it). Before
the mod's first connection the user sees the list and the reason and allows it; the answer is kept per mod version and
list (`network_consent_key`), so an update that changes either asks again, and the printer panel withdraws it. Every
request is checked against the list and the agreement:

- HTTP through the core's libcurl: GET, POST (bytes or multipart with files), PUT (a file), DELETE; redirects are
  not followed; an internet address is always checked against the system's certificates, and only a LAN address
  may present a self-signed one.
- TLS sockets through the core's OpenSSL, the same rule for certificates; client certificates are files the mod
  names. A `.local` name must resolve to a LAN address.
- Secrets in the operating system's credential store under the mod's own name. The printer panel writes secret
  form fields there and never shows them again; nothing puts them in a project, a preset, an export or the log.
- `queue_on_main` and `thread_start` run the mod's function under its crash guard: a crash disables the mod for the
  run instead of ending the application. Before the mods shut down the core drops queued calls, closes connections
  and joins the threads a mod left.

The core never logs a URL, header or body it carries; its own log lines about mod traffic go through
`ModNet::redact`. A mod's web panel is a page of its signed folder in a web view, whose own requests the core does
not check.

### The printer panel

`"gui": {"printer": {...}}` describes the panel the printer settings' **Printer connection...** button opens
(`ModPrinterDialog`): `fields` (`text`, `secret`, `choice` with `options`, `path`; a secret has no default),
`actions` (a button sending `command` with `args`, grouped by `row`, with an optional `confirm` question, and
`idle_only` refused while the printer prints or pauses), `panels` (`page`, an HTML file inside the mod folder; the
page's `window.wx.postMessage(text)` reaches the session as `panel_message`, and the session's `panel_message`
events call the page's `window.magpiePanelMessage(object)`) and `notices`. Each item may have `visible`, an
expression over the form's field values and `printer_state`. Labels and texts take `_<language>` translations.
The form is kept per printer address: values in the application config section `mod_printer:<mod id>`, secrets in
the credential store as `<field>.<host>`.

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
