param(
    [string]$Root = (Split-Path -Parent $PSScriptRoot)
)

$ErrorActionPreference = 'Stop'
Set-Location -LiteralPath $Root

$failures = [System.Collections.Generic.List[string]]::new()
function Require-Match {
    param([string]$Path, [string]$Pattern, [string]$Description)
    $content = Get-Content -LiteralPath $Path -Raw
    if ($content -notmatch $Pattern) {
        $failures.Add($Description)
    }
}

Require-Match 'version.inc' 'option\(MAGPIE_SLICING_PROFILER' 'Missing profiler build option.'
Require-Match 'version.inc' 'Magpie Slicer Profiler' 'Missing isolated profiler application identity.'
Require-Match 'CMakeLists.txt' 'MAGPIE_SLICING_PROFILER=1' 'Missing profiler compile definition.'
Require-Match 'CMakeLists.txt' 'option\(MAGPIE_SLICING_TIMING[^\r\n]*OFF\)' 'Timing must remain opt-in without changing application identity.'
Require-Match 'CMakeLists.txt' 'MAGPIE_SLICING_TIMING=1' 'Missing common timing compile definition.'
Require-Match 'src/libslic3r/CMakeLists.txt' 'SlicingProfiler\.cpp' 'Profiler implementation is not linked.'
Require-Match 'src/libslic3r/PrintBase.hpp' 'begin_state_step\(false' 'Print-step timing hook is missing.'
Require-Match 'src/libslic3r/PrintBase.hpp' 'begin_state_step\(true' 'Print-object timing hook is missing.'
Require-Match 'src/libslic3r/Gpu/VulkanSlicer.cpp' 'Exact vertical intersections' 'Vertical Vulkan timing is missing.'
Require-Match 'src/libslic3r/Gpu/VulkanSlicer.cpp' 'operation_name' 'Spatial Vulkan timing is missing.'
Require-Match 'src/slic3r/GUI/BackgroundSlicingProcess.cpp' 'Generate G-code and thumbnails' 'G-code pipeline timing is missing.'
Require-Match 'src/slic3r/GUI/GLCanvas3D.cpp' 'Export timing log' 'Profiler export button is missing.'
Require-Match 'src/libslic3r/SlicingProfiler.cpp' 'magpie-slicing-profile-v1' 'Profile JSON schema is missing.'
Require-Match 'scripts/build_installer.ps1' 'rejects MAGPIE_SLICING_PROFILER=ON' 'Stable installer guard is missing.'

$guardedGuiFiles = @(
    'src/slic3r/GUI/Plater.hpp',
    'src/slic3r/GUI/Plater.cpp',
    'src/slic3r/GUI/GLCanvas3D.cpp'
)
foreach ($path in $guardedGuiFiles) {
    Require-Match $path '#ifdef MAGPIE_SLICING_TIMING' "Timing UI is not compile-time guarded in $path."
}

if ($failures.Count -gt 0) {
    $failures | ForEach-Object { Write-Error $_ }
    exit 1
}

Write-Output 'SLICING_PROFILER_CONTRACT=PASS'
Write-Output 'STABLE_DEFAULT=OFF'
Write-Output 'DEVELOPMENT_IDENTITY=Magpie Slicer Profiler'
Write-Output 'EXPORT_SCHEMA=magpie-slicing-profile-v1'
