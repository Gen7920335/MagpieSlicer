# Magpie Slicer versioning

Magpie Slicer keeps the three-field OrcaSlicer base version and appends a
two-field Magpie release sequence:

```text
<Orca major>.<Orca minor>.<Orca patch>.<Magpie line>.<Magpie revision>
```

The first release based on OrcaSlicer 2.5.0 is therefore `2.5.0.0.1`.
Subsequent installers on the same Magpie line are `2.5.0.0.2`,
`2.5.0.0.3`, and so on. The Magpie revision resets to `1` when either the
Orca base version or the Magpie line changes.

## Representation rules

- Current local candidate version: `2.5.0.0.9`
- Public version / Git tag format: `2.5.0.0.9` / `v2.5.0.0.9` (not tagged or published)
- Installer: `MagpieSlicer_Windows_Installer_V2.5.0.0.9_x64.exe`
- Internal SemVer-compatible Orca version: `2.5.0-modified.0.9`
- Windows numeric file version: `2.5.0.9`

Windows executable metadata permits four numeric fields, so its final field
encodes `Magpie line * 1000 + Magpie revision`. This gives `9` for Magpie
sequence `0.9` while the public version remains the full five-field value.

The version owner is `version.inc`. Packaging must consume
`MAGPIE_RELEASE_VERSION`; release names must not invent separate feature-based
version counters.
