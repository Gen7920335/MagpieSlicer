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
- **Next verified step:** start P1 (parameter graph). Settle the plan decisions it depends on first.

## History

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
