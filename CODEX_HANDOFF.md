# CODEX_HANDOFF

## Routing Header

- **Current scope:** Line 2 (`magpie-next`): rebuild Magpie Slicer on OrcaSlicer nightly with the module host from the plan "나이틀리 기반 모듈형 슬라이서 상세 플랜". This branch is P0, the base: nightly made into Magpie (identity layer) plus the feature-off golden corpus harness.
- **Affected components:** `version.inc`, top-level and `src/CMakeLists.txt`, `src/dev-utils/platform/*`, `src/libslic3r/libslic3r_version.h.in`, `GCodeCompatibility.hpp` and its three users (`GCode.cpp`, `GCodeProcessor.cpp`, `Config.cpp`), `utils.cpp` header string, GUI identity strings/colors across `src/slic3r/GUI`, `resources/images` + `resources/web` theme, installer scripts, `scripts/magpie/*`.
- **Current implementation phase:** P0 identity layer ported and statically reviewed. No build, test or slice has been run for this branch.
- **Upstream base:** OrcaSlicer `main` at `07a12f5fe` (2026-10-11).
- **How the identity layer was ported:** hunks of line 1 (`1d61962ea..beta/2.5.0.0.9`) whose old and new lines differ only in brand names or colors were extracted and 3-way merged onto nightly; 28 conflicting files were resolved by hand toward nightly code with Magpie names/colors. Build files, version, installer, `GUI_App` integration and `libslic3r` producer handling were ported by hand. Colors Magpie replaced everywhere are in `scripts/magpie/theme_colors.json`; `apply_theme.py` applies them.
- **Known failures / DO NOT REPEAT:** none recorded yet on this line. Do not copy line-1 feature code into Orca files; features return as modules (plan P1-P5).
- **Not yet ported from line 1 (deliberately):** every Magpie slicing feature; slicing timing/profiler (`MAGPIE_SLICING_TIMING` option exists, no instrumentation); automatic OrcaSlicer user-preset import and the "Converted for Magpie" menu; `MAGPIE_GUI_TEST_*` environment hooks; Korean strings for Magpie features; README/branding text in docs and flatpak/msix manifests.
- **Unresolved decisions:** see the plan's decision list (script language, native module process model, repository/distribution naming, mod tag, upstream PR timing).
- **Next verified step:** build vanilla Orca and this branch from the same upstream commit on Windows, run `golden.py determinism` on each, then `golden.py compare` between them. Expected differences: none after normalization. Then start P1 (parameter graph).
