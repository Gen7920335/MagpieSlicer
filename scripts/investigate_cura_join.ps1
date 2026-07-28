$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Show-Lines([string]$Path, [int]$First, [int]$Last) {
    $lines = Get-Content $Path
    for ($i = $First; $i -le [Math]::Min($Last, $lines.Count); $i++) {
        '{0,5}: {1}' -f $i, $lines[$i - 1]
    }
}

Write-Host '=== Current Orca area adapter ==='
Show-Lines 'src/libslic3r/Support/CuraStyleSupport.cpp' 1 370

Write-Host '=== Cura join ==='
Show-Lines 'sandboxes/curaengine-reference/src/support.cpp' 470 640

Write-Host '=== Polygon operation declarations ==='
rg -n "Polygons offset\\(|Polygons union_\\(|Polygons diff\\(|Polygons intersection\\(" src/libslic3r/ClipperUtils.hpp src/libslic3r/ClipperUtils.cpp

Write-Host '=== Config mapping ==='
rg -n "support_expansion|support_threshold_angle|support_object_xy_distance|support_top_z_distance|support_bottom_z_distance|support_interface_top_layers|support_interface_bottom_layers" src/libslic3r/PrintConfig.hpp src/libslic3r/PrintConfig.cpp src/libslic3r/Support/SupportParameters.*
