# Magpie Slicer versioning

Magpie Slicer keeps the three-field OrcaSlicer base version and appends a
two-field Magpie release sequence:

```text
<Orca major>.<Orca minor>.<Orca patch>.<Magpie line>.<Magpie revision>
```

| Line | Base | Branch | Meaning |
| --- | --- | --- | --- |
| 0, 1 | OrcaSlicer 2.5.0 (upstream `1d61962ea`) | `beta/2.5.0.0.9`, tag `v2.5.0.1.0` | Original fork, features patched into Orca sources |
| 2 | OrcaSlicer nightly (`main`) | `magpie-next` | Rebuild on nightly with the module host |

The revision resets to 0 when the Orca base version or the Magpie line changes.

## Representation rules

`version.inc` owns every number below. Packaging consumes `MAGPIE_RELEASE_VERSION`
and `MAGPIE_PACKAGE_VERSION`; release names must not invent separate counters.

- Public version / Git tag: `2.5.0.2.0` / `v2.5.0.2.0`
- Pre-release builds append `MAGPIE_PRERELEASE_LABEL` to the package version only
  (`2.5.0.2.0-dev`), so installers and Add/Remove Programs can tell them apart.
- Internal SemVer-compatible Orca version (`SoftFever_VERSION`): `2.5.0-modified.2.0`
- Windows numeric file version: `2.5.0.2000` (`line * 1000 + revision` in the fourth field,
  because Windows metadata has four numeric fields)
- G-code producer: `OrcaSlicer 2.5.0.2.0`. Printers and host software recognize Orca output
  by the producer name, so `SLIC3R_GCODE_GENERATOR_NAME` stays `OrcaSlicer`.
