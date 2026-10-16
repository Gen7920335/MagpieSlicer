# CODEX_HANDOFF

## Routing Header

- **Current scope:** Line 2 (`magpie-next`): rebuild Magpie Slicer on OrcaSlicer nightly with the module host from the plan "나이틀리 기반 모듈형 슬라이서 상세 플랜". This branch is P0, the base: nightly made into Magpie (identity layer) plus the feature-off golden corpus harness.
- **Affected components:** `version.inc`, top-level and `src/CMakeLists.txt`, `src/dev-utils/platform/*`, `src/libslic3r/libslic3r_version.h.in`, `GCodeCompatibility.hpp` and its three users (`GCode.cpp`, `GCodeProcessor.cpp`, `Config.cpp`), `utils.cpp` header string, GUI identity strings/colors across `src/slic3r/GUI`, `resources/images` + `resources/web` theme, installer scripts, `scripts/magpie/*`.
- **Current implementation phase:** P0 verified on Windows (gen792, 2026-10-12). Built with fresh nightly deps; unit tests 2086/2086 on vanilla and this branch; `golden.py determinism` 15/15 on each; `golden.py compare` vanilla vs this branch 15/15 identical (`bunny_support_tree` skipped: no external model). Splash, setup wizard, home page and window title checked by screenshot.
- **Upstream base:** OrcaSlicer `main` at `1494de66de` (2026-10-12), merged into the P0 line on 2026-10-13; P0 was built on `07a12f5fe`.
- **How the identity layer was ported:** hunks of line 1 (`1d61962ea..beta/2.5.0.0.9`) whose old and new lines differ only in brand names or colors were extracted and 3-way merged onto nightly; 28 conflicting files were resolved by hand toward nightly code with Magpie names/colors. Build files, version, installer, `GUI_App` integration and `libslic3r` producer handling were ported by hand. Colors Magpie replaced everywhere are in `scripts/magpie/theme_colors.json`; `apply_theme.py` applies them.
- **Known failures / DO NOT REPEAT:** Do not copy line-1 feature code into Orca files; features return as modules (plan P1-P5). The hunk-filter port missed binary assets and non-rename code changes; compare against line 1 by blob for assets (see history 2026-10-12). Launching the GUI from a build tree re-registers .3mf/.stl/.step and `orcaslicer://` to that exe (startup association code); test with `--datadir` and expect to restore associations afterwards. The nightly CLI needs `--datadir` with `system/` vendor profiles, and resolves a `"from": "User"` preset only from `<datadir>/user/default/<kind>/` with a `version` key; `golden.py --datadir` handles this.
- **Not yet ported from line 1 (deliberately):** every Magpie slicing feature; slicing timing/profiler (`MAGPIE_SLICING_TIMING` option exists, no instrumentation); automatic OrcaSlicer user-preset import and the "Converted for Magpie" menu; `MAGPIE_GUI_TEST_*` environment hooks; Korean strings for Magpie features; README/branding text in docs and flatpak/msix manifests.
- **Unresolved decisions:** see the plan's decision list (script language, native module process model, repository/distribution naming, mod tag, upstream PR timing). Decided 2026-10-12: line 2 has its own app key `MagpieSlicerNext`, command `magpie-slicer-next`, package/install name "Magpie Slicer Next"; the display name stays "Magpie Slicer". `orcaslicer://` stays shared with Orca and line 1.
- **Next verified step:** P2 support modules: Cura (b3dcb484ce), Resin (046af81966) and Mixed are built-in support generators behind the `support_generator` slot, each switched on by a bundled mod in `resources/mods`. Next: Gen to try Mixed painting in the app (not seen on screen yet), then triangle interface/sublayer, tree wall count; then P1 leftovers.

## History

### 2026-10-16 Mixed support painting

- `ModelVolume::support_channel_facets` (ENFORCER = normal, BLOCKER = tree), stored in 3MF under line 1's names (`slic3rpe:mixed_supports`, `paint_mixed_supports`) so line-1 projects keep their painting; copied/reset/remapped wherever `fuzzy_skin_facets` is. It deliberately has no object ID and is serialized inline on the Undo/Redo stack: giving it an ID shifted every later ObjectBase ID, which changed the X1C G-code's "model label id" (golden caught it: 2 x1c cases differed at line 7; without the ID all 15 match again).
- Support paint gizmo: with `mixed_support.enable` on, "Paint support as Normal/Tree" radio buttons; the session keeps states 3/4 in one selector and splits them into supported_facets and support_channel_facets on store (`TriangleSelector::overlay_painting` merges them back when same splits).
- `MixedSupport::plan` takes painted masks (tree wins overlap, unreachable normal paint falls back to tree with a warning), as line 1.
- Checked: painted 3MF sliced by the CLI: tree painting turns the pillar fixture's split support into all-tree support; normal painting with selective merge equals automatic (expected). The GUI 3D canvas cannot be captured on this desktop (PrintWindow gives grey, CopyFromScreen has no handle), so the gizmo itself is not yet seen on screen. Test project: D:\claude\golden\paint	ree.3mf.

### 2026-10-16 Cura support first layer

- The Cura module's extra filament against Beta 7 (overhang.obj 426 vs 327) was all in the first support layer: Orca's dense, widened flange (133 vs 17.5 E). Cura prints that layer with the same lines as the layers above (support_initial_layer_line_distance defaults to the support line distance), and line 1 did the same unless `cura_solid_support_raft`. `SupportParameters::aligned_first_layer` (no flange, no first-layer expansion) and `first_layer_density` now let the Cura module do that; `cura_support.solid_first_layer` brings the dense flange back. Toggling it moves the tee's first-layer path from 119 mm (aligned) to 947 mm (flange), which the new test checks. After the fix overhang.obj prints 316 vs Beta 7's 327.
- Remaining gap on the tee (193 vs 155) is the support base filler: nightly adds a boundary connector line Beta 7's filler does not (same grid lines otherwise); Orca normal support differs between the two bases by 7-10 % for the same reason. My earlier note that the wider Cura first layer was intended Orca behaviour was wrong: line 1 removed it on purpose.

### 2026-10-16 P2 Mixed support

- Module `ModHost/Builtin/MixedSupport` + `resources/mods/mixed_support.json` (normal generator orca/cura, tree style, coverage threshold, selective merge; rule priority 2). Ported from line 1's MixedSupportPlan and `_generate_support_material` Mixed branch. Painted channel assignment (green normal / blue tree) is not ported yet: it needs a model annotation, the gizmo and 3MF storage.
- Core hooks, all inert by default: `SupportMaterialOverrides::contact_mask` (keep only overhangs inside the mask) and `ignore_support_type`; `PrintObjectSupportMaterial::detect_top_contact_layers()`; free `buildplate_covered_by_object()` (extracted, used by both the generator and the plan); `TreeSupportOverrides` (demand mask, style, extra obstacles for non-organic trees) with the collision cache adding the obstacles; `stInnerMixed` with `has_normal_channel()/has_tree_channel()` at the brim, first-layer island and retraction sites; the tree wipe also fires on mixed layers; `PrintObject::take_support_layers()/delete_support_layer()/add_module_warning()`.
- `CuraSupport` takes a mask and exposes `overhang_demand()` (one overhang function shared by demand and generation).
- Check: with demand fully reachable, Mixed output equals its normal generator alone byte for byte on both Beta 7 and this build (overhang model, support path hash). On the pillar fixture (60 % reachable) both builds print normal support beside the pillar and organic branches over it; support extrusion within 4 % (Orca normal) and 19 % (Cura; the same gap as Cura alone).
- Unit tests 2167/2167; golden mods off and on 15/15 identical. `SupportMaterial.hpp` includes are now path-qualified so tests can include it.

### 2026-10-16 P2 Resin support

- `libslic3r/SLA` and `BranchingTree` were brought to PrusaSlicer 2.9.6 (the latest stable release; master is a 3.0 alpha whose SLA changes are a namespace refactor plus one rounding change) by 3-way merging line 1's port (`44866d5a5^..HEAD`) onto nightly. Line 1's support-path files equal 2.9.6 except its Default-tree max-bridge-length fix. Conflicts were include lists; `SupportPointGenerator.cpp` and the SLA tests take line 1's version. Nightly's concave-hull check in `sla_test_utils.cpp` (offset by SCALED_EPSILON) was kept: line 1's older check fails the two pad tests on the new Clipper, and restoring nightly's check makes them pass.
- Module `ModHost/Builtin/ResinSupport` + `resources/mods/resin_support.json` (35 params; tree type, density, enforcers-only, 16 per strategy). It runs Orca's `PrintObjectSupportMaterial` with `SupportMaterialOverrides` (contact demand per layer, body cross-sections, keep narrow islands). With default overrides the generator is unchanged; golden mods-off and mods-on stay 15/15 identical.
- New core setting `object_elevation` (comDevelop, default 0, kept out of the G-code dump) lifts the object in `SlicingParameters`; the resin mod sets it only while `enable_support` is on (line 1 kept the lift with support off).
- Elevated-object fixes ported from line 1 behind the overrides or `object_elevation`: first-layer contact under a lifted object (`new_contact_layer`, `fill_contact_layer`), support layers filling the lift gap, first-layer support feet not trimmed by a lifted footprint, and the narrow-island toolpath fallback. Without the toolpath fallback the 1 mm pillars print nothing (observed: islands but no extrusions).
- `golden.py` now fails a case when the CLI logs "Mod not loaded". The noop golden mod clashed with the bundled cura_support rule at priority 0 and was silently dropped in earlier mods-on runs; it now uses priority -1.
- Beta 7 comparison (U1, tee/overhang/frog, Default and Branching): same support Z range and layer counts, support extrusion within 6 %, one fewer interface layer (same base-version difference as Orca normal support), model extrusion difference equals plain Orca normal support's.

### 2026-10-12 P0 verification and identity completion (gen792)

- `golden.py`: added `--datadir`, per-printer `bed_type` (`--curr-bed-type`; nightly defaults to Cool Plate, which the U1 PLA profile rejects), UTF-8 decoding of CLI output (cp949 Windows crashed the harness).
- Identity gaps found by screenshot: window title, setup wizard logo and text, home page teal. Cause: the P0 port took only text hunks differing in brand or color, so line-1 binary assets and code-level renames were skipped. Fixed: 8 web PNGs and 3 logo SVGs taken from line 1 by blob comparison (the other 21 line-1 binary changes are not branding); title uses `SLIC3R_APP_NAME`; `I18N::rebrand()` swaps the app name after translation for messages that name the app, keeping the OrcaSlicer catalog's Korean; web `TranslatePage` swaps "Orca Slicer"; `logo_name()` returns MagpieSlicer.
- Dark mode: P0 had moved the dark-map keys to `#D7E9FD`/`#EBF4FE` while callers kept `#BFE1DE`/`#E5F0EE`, and nightly's `#EBF9F0` entry mapped onto a duplicate `#EBF4FE` key that won with a green value. `theme_colors.json` now maps the two tints to the dark-map keys and the duplicate entry is removed.
- Tests `test_action_source.cpp` and `test_plugin_audit.cpp` hardcoded Orca names; they now follow the Magpie icon and `SLIC3R_APP_KEY`.
- Checked: 3MF written by either CLI slices identically in both; Gen's 62 line-1 user presets load in this branch.

### 2026-10-13 Merge of Orca main 1494de66de

- Upstream moved web page text into the .po catalogs (`resources/web/data/text.js` removed; pages read `window.ORCA_UI_STRINGS`). The Magpie name swap moved with it into `WebViewHostDialog::localized_strings_script`, the one injection point. Native command titles are now translated in `CommandAction`, so the swap happens there.
- Upstream deleted `SysInfoDialog.cpp` and the old guide pages 3, 6 and 31; the deletions were taken.
- Verified after the merge: unit tests 2121/2121 on vanilla and this branch, `golden.py determinism` 15/15 on each, `golden.py compare` 15/15 identical. Deps unchanged.

### 2026-10-13 P1 parameter graph (steps 1-3)

- `src/libslic3r/ModHost/`: `ParamGraph` (every print_config_def option as a core node), `Expr` (pure expression evaluator, Orca placeholder-style syntax), `ModSet` (JSON mods with comments from `<data_dir>/mods`, load-time checks, rule ordering, resolution). Decided by Gen: JSON, not YAML.
- Layering as implemented: rules sit above the printer/filament/process presets and below any key listed in `different_settings_to_system` (a value the user changed). Same setting + same priority is a load error; higher priority applies later and wins; a rule that reads another rule's setting runs after it; cycles are load errors.
- `Print::apply` resolves rules before normalization; a rule that fails to evaluate is left out and `Print::validate` reports it (no silent fallback; the GUI does not catch exceptions from apply, so resolve never throws).
- Mod parameter values live in `mod_settings` / `printer_mod_settings` / `filament_mod_settings` (JSON objects keyed `<mod>.<name>`), added to the preset option lists and to the G-code dump's banned keys.
- Mod changes invalidate through the resolved core settings only; per-parameter `invalidates` is parsed but not used yet (a parameter read only by rules needs nothing more).
- Verified: unit tests 2147/2147; golden 15/15 identical with no mods; CLI demo mod gives walls 3+2=5 (stored value 4 gives 7), a user-set preset value wins, a broken mod file is skipped with a logged reason, a 3MF carries `mod_settings` and slices back to the same result.

### 2026-10-16 Mods-on equivalence (Gen's rule)

- `golden.py mods --cli <magpie> --vanilla-cli <orca> --datadir <mod-free datadir> --out <dir>`.
- Measured: no-op mod 15/15 identical to vanilla (including a rule that writes `layer_height * 1.5 / 1.5`); value-setting mod 15/15 identical to vanilla with the same values in the preset once `different_settings_to_system` is dropped. Without dropping it, each case differs in that one comment line only (vanilla lists the overridden keys; Magpie does not, since the rule is not a user change). In the vase case spiral mode marks wall_loops/top_shell_layers as changed, so the rule leaves them, and the output still matches.
