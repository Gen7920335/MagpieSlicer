# Magpie Slicer 2.5.0.0.9 Beta 2

Beta 2 packages the fixes developed after Beta 1 while retaining the optional CUDA and detailed slicing-timing features.

## Changes

- Fixed stale material and derived-slice state after material, mapping, support-contact, and detail-wall changes.
- Fixed multi-nozzle material-to-physical-tool mapping for support flow, Resin support geometry, layer-height limits, tool ordering, purge data, and first-layer ordering.
- Fixed Resin Default/Branching hole masks and kept Resin object elevation independent from support enablement.
- Hardened Cura and Tree cancellation and asynchronous camera lifetime handling.
- Fixed low-temperature interface restoration, sublayer-only temperature-tower generation, tower Undo/Redo state, and multi-nozzle tower spacing and participating-tool print-area placement.
- Fixed imported layer ranges that override material or another option without defining `layer_height`; they now inherit the object's normal layer height instead of dereferencing a missing option.
- Changed an uncertain Mixed fallback success message into a critical warning: a normal-support retry may still leave areas unsupported and the preview must be checked.
- Preserved CLI setting normalization and save/reload behavior, including malformed or shortened legacy vectors.
- Preserved CUDA Off/On controls and detailed timing JSON. This beta does not claim a new CUDA speedup.

## Automated verification

- The final product and FFF test targets built successfully.
- Focused final core regression: 10 test cases, 838 assertions, zero failures.
- CLI configuration, reload, and save comparison: 340 + 8 + 16 = 364 cases, zero failures; 16 project archives and 80 PNG payloads passed structural checks.
- Full Stanford Bunny at a 60-degree support threshold, 0.2/0.8 mm temperature-tower tool order in both directions: 7,445 assertions, zero spacing mismatches, zero tower layers outside the participating-tool area.
- Latest single-nozzle Bunny 60-degree temperature-tower run checked 579,295 model extrusion moves with zero wrong temperature targets and retained the prior known-good command hash.
- The Korean source catalog passed format validation. The installer is packaged with a freshly compiled runtime catalog.

## Limits

- Actual installation and GUI interaction are assigned to the user and were not reported as completed by automation.
- Mixed fallback geometry still needs a Bunny 60-degree case that produces an actual Tree routing failure and measures coverage of the Tree-owned contact demand before and after fallback. The warning is accurate; complete fallback support recovery is not claimed.
- Dynamic detail-wall tool collection, heterogeneous Auto nozzle selection, and some purge/material-removal combinations remain review candidates rather than cleared defects.
- Vulkan spatial work, kernel, and host totals are not fully measured. A recorded zero must not be read as zero cost.
- The build host has a GeForce GT 1030. RTX 4070 SUPER performance and GUI-lag qualification were not performed.
- No physical print was performed. Inspect the preview and generated G-code before using this beta on hardware.

This release provides the supported Windows x64 NSIS installer. No portable package is provided.
