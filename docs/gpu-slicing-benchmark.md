# Magpie CPU/Vulkan slicing benchmark

This benchmark measures real Magpie CLI slicing with Vulkan disabled and with
qualified Vulkan compute forced on. It does not use strict validation because
strict mode repeats GPU work on the CPU and is unsuitable for performance
measurement.

## One-click run

Run `scripts/Run-GpuSlicingBenchmark.cmd`. The default test performs one warmup
pair and five measured CPU/GPU pairs. Results are written to
`build/verification/gpu-slicing-benchmark` and the HTML report opens when the
run finishes.

The execution order alternates between CPU-first and GPU-first pairs. Reported
times use the measured median and exclude warmup runs.

## Test a real model

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File scripts\benchmark_gpu_slicing.ps1 `
  -ModelPath "C:\Models\complex-model.stl" `
  -Repeats 7 -WarmupPairs 2 -OpenReport
```

Multiple STL or 3MF files may be passed to `-ModelPath`. Use `-SlicerPath` to
select a specific installed or development executable.

## Validity rules

A result is valid only when all of the following are true:

- every CPU and GPU slice exits successfully and generates printable G-code;
- measured GPU runs record real Vulkan dispatches;
- G-code contains no non-finite coordinates;
- CPU and GPU unordered extrusion-segment and aggregate geometry hashes match.

The report includes CPU, GPU, driver, memory and active Windows power-plan
information, per-run timings, dispatch counts and request counts. JSON and CSV
copies are saved beside the HTML report.

For stable numbers, close other heavy applications and use the same Windows
power plan for the whole run. A workload with zero Vulkan dispatches is marked
invalid instead of being presented as a GPU speed result.
