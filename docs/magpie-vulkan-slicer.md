# Deterministic Vulkan slicing

The Vulkan backend is compile-time optional. Configure with
`-DSLIC3R_ENABLE_VULKAN_SLICER=ON` only when Vulkan 1.2 headers, loader and
`glslc` are available. CPU geometry remains the authoritative fallback.

Runtime mode defaults to **Auto**. Auto benchmarks the active CPU and qualified
GPU before slicing and only dispatches batches whose calibrated crossover is
expected to be faster. **On** prefers qualified Vulkan hardware, while **Off**
keeps all geometry on CPU. Production uses sampled CPU validation after startup
qualification; `MAGPIE_VULKAN_SLICER_VALIDATION=strict` enables full CPU
comparison for development verification.

## Exact geometry contract

- Coordinates remain signed 64-bit fixed point at 1e-6 mm.
- Every vertical-intersection request is rebased around its scanline and Y
  midpoint before dispatch.
- A signed 128-bit CPU preflight proves that all GPU intermediates and the
  restored global result fit in signed 64-bit values.
- Every live GPU result is checked for validity, stable ID, denominator and
  restored numerator. One mismatch discards the complete batch.

## Failure behavior

- Initialization failures can retry after a bounded delay.
- Dispatch waits are limited to 10 seconds.
- A timeout or device failure disables Vulkan for the remaining process and
  keeps the established CPU path active.
- Unsupported devices, oversized workloads and unsafe coordinates remain on
  CPU without producing a partial GPU result.

## Current scope

Only exact vertical scanline intersections and conservative tree-support AABB
candidates are backend capabilities. Polygon boolean/offset operations,
Arachne and classic wall topology, support topology, path ordering, seams,
multi-nozzle assignment and G-code generation remain CPU-authoritative.
