param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $BaselineRoot = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if ([string]::IsNullOrWhiteSpace($BaselineRoot)) {
    $BaselineRoot = Join-Path $RepoRoot 'build\verification\vulkan-baseline\cpu-random-320-seed250802'
}
$errors = [Collections.Generic.List[string]]::new()

function Require-Text([string] $Path, [string] $Needle) {
    $text = Get-Content -LiteralPath $Path -Raw
    if (-not $text.Contains($Needle)) { $script:errors.Add("$Path is missing: $Needle") }
}

function Reject-Pattern([string[]] $Paths, [string] $Name, [string] $Pattern) {
    $hits = @(Select-String -Path $Paths -Pattern $Pattern)
    if ($hits.Count -gt 0) { $script:errors.Add("$Name found $($hits.Count) time(s)") }
}

$required = @(
    'src\libslic3r\Gpu\GpuExactGeometry.hpp',
    'src\libslic3r\Gpu\VulkanSlicer.hpp',
    'src\libslic3r\Gpu\VulkanSlicer.cpp',
    'src\libslic3r\Gpu\VulkanComputeSelfTest.cpp',
    'src\libslic3r\Gpu\shaders\perimeter_infill_candidates.comp',
    'src\libslic3r\Gpu\shaders\tree_support_contour_candidates.comp',
    'src\libslic3r\Fill\FillRectilinear.cpp',
    'src\libslic3r\Support\TreeSupport.cpp'
)
foreach ($relative in $required) {
    if (-not (Test-Path -LiteralPath (Join-Path $RepoRoot $relative) -PathType Leaf)) {
        $errors.Add("Missing required file: $relative")
    }
}
if ($errors.Count -gt 0) { throw ($errors -join "`n") }

$backend = Join-Path $RepoRoot 'src\libslic3r\Gpu\VulkanSlicer.cpp'
$cmake = Join-Path $RepoRoot 'src\libslic3r\CMakeLists.txt'
$fill = Join-Path $RepoRoot 'src\libslic3r\Fill\FillRectilinear.cpp'
$tree = Join-Path $RepoRoot 'src\libslic3r\Support\TreeSupport.cpp'
$appConfig = Join-Path $RepoRoot 'src\libslic3r\AppConfig.cpp'
$preferences = Join-Path $RepoRoot 'src\slic3r\GUI\Preferences.cpp'
$background = Join-Path $RepoRoot 'src\slic3r\GUI\BackgroundSlicingProcess.cpp'
$gpuFiles = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src\libslic3r\Gpu') -Recurse -File | ForEach-Object FullName)

foreach ($needle in @(
    'automated_verification_flag("MAGPIE_VULKAN_SLICER_ENABLE")',
    "kDispatchFenceTimeoutNs = 10'000'000'000ULL",
    'kInitializationRetryDelay',
    'm_device_faulted',
    'expected_numerators',
    'the entire intersection batch was discarded'
)) { Require-Text $backend $needle }
Require-Text $cmake 'option(SLIC3R_ENABLE_VULKAN_SLICER "Build the experimental deterministic Vulkan slicer backend" OFF)'
Require-Text $fill 'prepare_vulkan_vertical_intersections'
Require-Text $fill 'complete_batch_consumed'
Require-Text $tree 'VulkanTreeContourBatch gpu_tree_batch'
Require-Text $tree 'mst_line_x_layer_contour_cache.insert({ line, false })'
Require-Text $appConfig 'set("vulkan_slicer_mode", get_bool("vulkan_slicer_compute") ? "on" : "auto")'
Require-Text $background 'get("vulkan_slicer_mode")'
Require-Text $background 'set_gpu_priority_enabled(vulkan_mode == "on")'
Require-Text $backend 'return VulkanIntersectionValidationMode::Sampled'
Require-Text $backend 'normalized == "strict"'
Require-Text $preferences 'VulkanSlicerBackend::compiled_with_vulkan()'
Require-Text $background 'VulkanSlicerBackend::release_unused_staging_memory()'
Reject-Pattern @($appConfig,$preferences,$background) 'unsafe GPU-priority application setting' 'vulkan_slicer_gpu_priority'

Reject-Pattern $gpuFiles 'unbounded Vulkan fence wait' 'vkWaitForFences\([^\r\n]*UINT64_MAX'
Reject-Pattern $gpuFiles 'one-shot Vulkan initialization' 'call_once|once_flag'
Reject-Pattern $gpuFiles 'stale 4 mm tile claim' 'fits_in_tile|max_tile_radius|4 mm tile'

$coreReferences = @(Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'src\libslic3r') -Recurse -File -Include '*.cpp','*.hpp' |
    Where-Object { $_.FullName -notlike '*\Gpu\*' } |
    Select-String -Pattern 'VulkanSlicerBackend|VulkanVerticalIntersection|VulkanTreeContour')
$unexpected = @($coreReferences | Where-Object {
    $_.Path -ne $fill -and $_.Path -ne $tree
})
if ($unexpected.Count -gt 0) { $errors.Add("Unexpected Vulkan core references: $($unexpected.Count)") }

foreach ($path in @($fill,$tree)) {
    $text = Get-Content -LiteralPath $path -Raw
    $ifCount = ([regex]::Matches($text, '(?m)^#if(?:def|ndef)?\b')).Count
    $endifCount = ([regex]::Matches($text, '(?m)^#endif\b')).Count
    if ($ifCount -ne $endifCount) { $errors.Add("Preprocessor imbalance in ${path}: $ifCount/$endifCount") }
}

$manifestPath = Join-Path $BaselineRoot 'sealed-baseline-manifest.json'
if (-not (Test-Path -LiteralPath $manifestPath -PathType Leaf)) {
    $errors.Add("Missing sealed CPU baseline: $manifestPath")
} else {
    $manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    if ($manifest.case_count -lt 300 -or $manifest.total_slices -lt 600) {
        $errors.Add("CPU baseline is too small: cases=$($manifest.case_count), slices=$($manifest.total_slices)")
    }
    $gcodeCount = @(Get-ChildItem -LiteralPath $BaselineRoot -Recurse -File -Filter '*.gcode').Count
    if ($gcodeCount -lt $manifest.case_count) {
        $errors.Add("CPU baseline G-code count is incomplete: $gcodeCount/$($manifest.case_count)")
    }
}

$oldPreference = $ErrorActionPreference
$ErrorActionPreference = 'Continue'
& git -C $RepoRoot -c core.safecrlf=false diff --check 2>$null | Out-Null
$diffExitCode = $LASTEXITCODE
$ErrorActionPreference = $oldPreference
if ($diffExitCode -ne 0) { $errors.Add('git diff --check failed') }

Write-Output "CORE_REFERENCES=$($coreReferences.Count)"
Write-Output "UNEXPECTED_REFERENCES=$($unexpected.Count)"
Write-Output "ERRORS=$($errors.Count)"
$errors | ForEach-Object { Write-Output "ERROR=$_" }
if ($errors.Count -gt 0) { exit 1 }
