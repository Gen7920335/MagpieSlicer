param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

function Require-Text([string] $Path, [string] $Pattern, [string] $Description) {
    if (-not (Select-String -LiteralPath $Path -Pattern $Pattern -Quiet)) {
        throw "Missing $Description in $Path"
    }
}

$backend = Join-Path $RepoRoot 'src\libslic3r\Gpu\VulkanSlicer.cpp'
$header = Join-Path $RepoRoot 'src\libslic3r\Gpu\VulkanSlicer.hpp'
$background = Join-Path $RepoRoot 'src\slic3r\GUI\BackgroundSlicingProcess.cpp'
$rectilinear = Join-Path $RepoRoot 'src\libslic3r\Fill\FillRectilinear.cpp'
$gyroid = Join-Path $RepoRoot 'src\libslic3r\Fill\FillGyroid.cpp'
$distanceField = Join-Path $RepoRoot 'src\libslic3r\Fill\Lightning\DistanceField.cpp'
$benchmark = Join-Path $RepoRoot 'scripts\benchmark_gpu_slicing.ps1'

foreach ($path in @($backend, $header, $background, $rectilinear, $gyroid, $distanceField, $benchmark)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

Require-Text $backend 'AutomatedVerificationDispatchRecorder' 'buffered diagnostic recorder'
Require-Text $backend 'kRetainedIntersectionRequestCapacity = 64 \* 1024' 'bounded intersection retention'
Require-Text $backend 'kRetainedTreeRequestCapacity = 32 \* 1024' 'bounded tree-request retention'
Require-Text $backend 'kRetainedTreeEdgeCapacity = 128 \* 1024' 'bounded tree-edge retention'
Require-Text $backend 'required_gpu_fraction = 0\.90' 'minimum ten-percent GPU benefit'
Require-Text $backend 'kGpuPriorityMinimumIntersectionBatch = 256' 'explicit aggressive GPU-priority floor'
Require-Text $backend 'kDefaultPreferredIntersectionBatch = 4096' 'conservative Auto intersection floor'
Require-Text $backend 'compute_mode == VulkanSlicerComputeMode::Maximum \? 1' 'maximum GPU single-workload floor'
Require-Text $backend 'compute_mode == VulkanSlicerComputeMode::Priority \? 128 : 16 \* 1024' 'separate On and Auto spatial floors'
Require-Text $backend 'compute_mode == VulkanSlicerComputeMode::Maximum \? 4 \* 1024 \* 1024' 'bounded maximum GPU spatial capacity'
Require-Text $backend 'is_obvious_low_end_gpu' 'low-end hardware classification'
Require-Text $backend 'try_load_cached_dispatch_policy' 'hardware policy cache read'
Require-Text $backend 'save_cached_dispatch_policy' 'hardware policy cache write'
Require-Text $backend 'm_selected_driver_version' 'driver-specific cache invalidation'
Require-Text $backend 'm_cpu_identifier' 'CPU-specific cache invalidation'
Require-Text $backend 'first large batch cannot slip through the provisional threshold' 'post-initialization policy recheck'
Require-Text $header 'release_unused_staging_memory\(bool force = false\)' 'bounded cleanup API'
Require-Text $background 'release_unused_staging_memory\(true\)' 'forced cleanup after interruption'
Require-Text $rectilinear 'struct DeferredVerticalIntersection' 'single-pass deferred intersection mapping'
Require-Text $rectilinear 'resolve_vertical_intersections_on_cpu' 'shared exact CPU fallback'
Require-Text $rectilinear 'deferred_intersections\.push_back' 'single-pass request location capture'
Require-Text $backend 'maximum_cells_per_box = 4096' 'bounded deterministic spatial index'
Require-Text $backend 'candidate_pair_count' 'indexed candidate workload threshold'
Require-Text $gyroid 'VulkanAabbOperation::Gyroid' 'conservative Gyroid clipping broad phase'
Require-Text $gyroid 'VulkanSlicerComputeMode::Maximum' 'maximum GPU Gyroid caller-gate bypass'
Require-Text $gyroid 'expolygon\.contains\(polyline\.points\.front\(\)\)' 'Gyroid contained-path retention'
Require-Text $distanceField 'VulkanAabbOperation::DistanceField' 'distance-field spatial candidate dispatch'
Require-Text $distanceField 'seed_is_final' 'exact CPU distance fallback'
if (Select-String -LiteralPath $rectilinear -Pattern 'prepare_vulkan_vertical_intersections' -Quiet) {
    throw 'The duplicate Vulkan request-generation pass was reintroduced.'
}
Require-Text $benchmark '\[switch\] \$VerificationMode' 'separate correctness mode'
Require-Text $benchmark '\[string\] \$GpuMode = ''auto''' 'Auto benchmark default'
Require-Text $benchmark 'performance: Vulkan Auto, calibrated crossover' 'production Auto performance mode'
Require-Text $benchmark '\^vertical,\\d\+\$' 'vertical-only dispatch accounting'
Require-Text $benchmark 'acceleration_applicable = \$accelerationApplicable' 'non-applicable Auto result classification'
Require-Text $benchmark 'speedup = if \(\$accelerationApplicable\)' 'dispatch-gated speedup reporting'
Require-Text $benchmark '\[int\] \$SyntheticGridSize = 28' 'high-cardinality production benchmark model'
Require-Text $benchmark "wall_loops = '1'" 'infill-focused benchmark walls'
Require-Text $benchmark "sparse_infill_density = '75%'" 'high-cardinality benchmark infill'

$benchmarkText = Get-Content -LiteralPath $benchmark -Raw
$forceAssignment = [regex]::Match(
    $benchmarkText,
    '(?ms)\$env:MAGPIE_VULKAN_SLICER_FORCE_DISPATCH\s*=\s*''1''')
if (-not $forceAssignment.Success) {
    throw 'Verification mode no longer has an explicit force-dispatch assignment.'
}
$verificationGuard = $benchmarkText.LastIndexOf('if ($VerificationMode)', $forceAssignment.Index)
if ($verificationGuard -lt 0 -or ($forceAssignment.Index - $verificationGuard) -gt 300) {
    throw 'Force-dispatch assignment escaped the VerificationMode guard.'
}

Write-Output 'VULKAN_THROUGHPUT_CONTRACT=PASS'
