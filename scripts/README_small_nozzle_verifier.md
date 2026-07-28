# Small-nozzle geometry verifier

The verifier slices `tests/data/small_nozzle_geometry_probe.stl` with isolated
machine and process profiles, then checks the generated G-code rather than CLI
success alone.

## Modes

```powershell
# Parser and geometric-oracle self-test, no slicing.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode SelfTest

# Six Classic/Arachne regression cases.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Smoke

# Twenty high-risk functional cases.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Core

# Thirty-two interlocking cases. Each case checks eight consecutive layers,
# wall ownership order, one tool boundary, constant total walls, and the
# N/M <-> N+1/M-1 period for Classic and Arachne.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Interlock

# Stanford Bunny differential: Classic/Arachne with interlocking off and on.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Bunny

# 66 OFAT cases: baseline + 9 binary + 28 three-level factors.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Full

# Add 24 deterministic high-risk interaction cases.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode Full -IncludePairwise

# Run all 90 cases in isolated parallel shards and merge the reports.
powershell -ExecutionPolicy Bypass -File scripts\run_small_nozzle_90.ps1

# Generate the auditable case plan without running the slicer.
powershell -ExecutionPolicy Bypass -File scripts\verify_small_nozzle_geometry.ps1 -Mode PlanOnly
```

`-CaseFilter` runs a subset by case name. `-SlicerPath`, `-ModelPath`, and
`-OutputRoot` override the default paths.

## Assertions

- No invalid tool commands, non-finite coordinates, out-of-bed extrusion, or
  excessive tool changes.
- The disabled mode emits no detail-tool walls.
- Small and large tools use distinct measured widths.
- A horizontal section through the probe's straight body has the requested
  small- and large-wall counts.
- Adjacent interlocking layers alternate `large N / small M` and
  `large N+1 / small M-1`.
- Small/large center spacing matches the configured physical overlap.
- Straight-body wall paths are closed on each tool.
- Top, bottom, solid-infill, and sparse-infill paths stay on the base tool.
- Large-nozzle override ranges use only the requested tool; overlapping ranges
  use the last matching entry.
- Numeric and percentage small-wall speed overrides constrain measured G-code
  feed rates.
- Feature-zone metrics are recorded separately for straight walls, angles,
  concave corners, curves, connected lettering, thin ribs, and Z steps.

Each run writes `case-plan.json`, `results.json`, `summary.csv`, and one
directory per case containing the exact profiles, CLI logs, G-code, and
case-level result.
