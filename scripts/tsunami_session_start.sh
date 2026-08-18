#!/usr/bin/env bash
# Tsunami Support - session bootstrap.
#
# Run this first in a new session. It prints every document you must read and
# in what order, the current state of the repository and the test suite, and
# the commands for building, timing and reading the planner's own diagnostics.
#
#   bash scripts/tsunami_session_start.sh            # orientation only, no build
#   bash scripts/tsunami_session_start.sh --verify   # also build and time the five main cases
#
# Written 2026-08-18. Keep it current: if a document moves or a diagnostic is
# added, change it here too, because this is what the next session reads.

set -u
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
EXE=build/tests/fff_print/Release/fff_print_tests.exe

rule() { printf '\n== %s ==\n' "$1"; }

rule "Required reading, in order"
cat <<'DOCS'
  1  AGENTS.md                             the agent contract. Section 8 carries the
                                           defect classes and the rules that prevent them
  2  CODEX_HANDOFF.md                      routing header first; it points here
  3  docs/TSUNAMI_SESSION_2026-08-18.md    self-contained session record: suite status,
                                           every diagnostic, all five failures with causes,
                                           nine refuted claims, ordered next steps
  4  docs/TSUNAMI_SUPPORT_CONTRACT.md      the Tsunami specification itself
  5  docs/PROJECT_CONTRACTS.md             coordinate and edge-case contract, section 8
DOCS

rule "Document state"
for f in AGENTS.md CODEX_HANDOFF.md docs/TSUNAMI_SESSION_2026-08-18.md \
         docs/TSUNAMI_SUPPORT_CONTRACT.md docs/PROJECT_CONTRACTS.md; do
    if [ ! -f "$f" ]; then
        printf '  %-38s MISSING\n' "$f"
    elif git ls-files --error-unmatch "$f" >/dev/null 2>&1; then
        printf '  %-38s %5s lines  tracked\n' "$f" "$(wc -l < "$f")"
    else
        printf '  %-38s %5s lines  UNTRACKED - decide whether to commit\n' "$f" "$(wc -l < "$f")"
    fi
done

rule "Repository state"
printf '  branch      %s\n' "$(git rev-parse --abbrev-ref HEAD)"
printf '  head        %s\n' "$(git log --oneline -1)"
printf '  upstream    %s\n' "$(git status -sb | head -1)"
dirty=$(git status --short -- src tests docs AGENTS.md CODEX_HANDOFF.md | wc -l)
printf '  dirty files %s (src tests docs contracts)\n' "$dirty"
probes=$(grep -rn "TEMPORARY PROBE\|TEMPORARY EXPERIMENT\|tsunami-probe" \
         src/libslic3r/Support/TsunamiSupport.cpp tests/fff_print/test_support_material.cpp 2>/dev/null | wc -l)
printf '  temporary instrumentation in tree: %s (must be 0 at a step boundary)\n' "$probes"

rule "Permanent diagnostics - grep these before proposing any cause"
cat <<'DIAG'
  Tsunami root selection:        chosen root does not span its target
  Tsunami coverage stall:        branch coverage loop gave up; carries no_plan_reasons
  Tsunami contact sampling failed: target could not be sampled at all
  Tsunami final coverage gate:   planning finished but support misses the target
  Tsunami fan-gap acceptance:    stalled plan accepted anyway. NOT a coverage guarantee
  Tsunami micro reach rejection: residue outside every terminal ring's micro reach

  one case:
    "$EXE" "<case name>" 2>&1 \
      | grep -E "Tsunami (root selection|coverage stall|contact sampling|final coverage|fan-gap|micro reach|planning failed)" \
      | sed 's/^.*\[warning\] //'
DIAG

rule "Suite as of 2026-08-18"
cat <<'SUITE'
  pass  Selecting Tsunami does not generate support while support is disabled     0s
  pass  Tsunami falls back for independent support-layer heights                  0s
  pass  Tsunami terminal ring feeds local tree tips into a separate interface ..   3s
  pass  Tsunami slices a solid circular footprint with an external contour root   11s
  pass  Tsunami covers every quadrant of a hollow gear overhang            32/32  26s
  pass  Tsunami micro branch reaches the hollow gear overhang              15/15  87s
  FAIL  Tsunami routes from snug overhang demand ...                  2863/2864   25s
  FAIL  Tsunami micro branch serves the snug overhang behind the column     9/10  82s
  FAIL  Tsunami keeps unobstructed zero-angle support vertically aligned     2/4    5s
  FAIL  Tsunami covers every overhang of a multi-column model ...        176/320  64s
  FAIL  Tsunami slices a corpus of complex upstream test models    does not finish
        ipadstand alone is the corpus timeout; the other five models total 12s
SUITE

rule "Build"
cat <<'BUILD'
  cmake --build build --target fff_print_tests --config Release > /tmp/build.log 2>&1
  rc=$?; [ $rc -ne 0 ] && grep -iE "error C[0-9]+|LNK" /tmp/build.log | head

  Check the exit code explicitly. Chaining the build to the run with && and
  reading only the tail makes a failed build report the previous binary's
  numbers as new ones. That happened in this session.
BUILD

if [ "${1:-}" = "--verify" ]; then
    rule "Verify - build then time the five main cases"
    cmake --build build --target fff_print_tests --config Release > /tmp/tsunami_build.log 2>&1
    if [ $? -ne 0 ]; then
        echo "  BUILD FAILED"
        grep -iE "error C[0-9]+|LNK" /tmp/tsunami_build.log | head -5
        exit 1
    fi
    echo "  BUILD OK"
    for t in "Tsunami covers every quadrant of a hollow gear overhang" \
             "Tsunami micro branch reaches the hollow gear overhang" \
             "Tsunami routes from snug overhang demand instead of expanded contact-grid fragments" \
             "Tsunami micro branch serves the snug overhang behind the column" \
             "Tsunami terminal ring feeds local tree tips into a separate interface stack"; do
        s=$(date +%s)
        r=$("$EXE" "$t" 2>&1 | grep -E "^assertions:|All tests passed" | head -1)
        printf '  %5ss  %-52.52s %s\n' "$(( $(date +%s) - s ))" "$t" "${r:-NO RESULT}"
        pkill -f fff_print_tests 2>/dev/null
    done
else
    rule "Verify"
    echo "  re-run with --verify to build and time the five main cases"
fi

rule "Next steps, in order"
cat <<'NEXT'
  1  snug micro, 0.783 mm2 - instrument plan_seeded_micro_tree for the branch owning
     one residue piece; compare emitted contacts against the disc that credited it
  2  multi-column reach limit - decide whether a target whose contour regions all sit
     beyond maximum_xy_distance should get multiple roots
  3  zero-angle - needs a design answer first: should a zero-angle configuration cover
     a 1250 mm2 target at all, or should the fixture use a smaller target?
  4  ipadstand - profile root and trunk search
  5  micro as the shipping default - costs about 3.3x macro, unattributed

  Full reasoning for each in docs/TSUNAMI_SESSION_2026-08-18.md section 8.
NEXT

rule "Two traps that cost time here"
cat <<'TRAPS'
  A counterfactual is judged by the mechanism, not by the test result. Causes stack:
  raising the sampling cap left zero-angle failing the same 2 of 4 and the cap was
  wrongly dismissed, when the cap_hit flag would have settled it immediately.

  timeout kills the wrapper, not the exe. A leftover fff_print_tests burns a core and
  corrupts the next measurement. pkill -f fff_print_tests after any capped run.
TRAPS
