$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Show-Lines([string]$Path, [int]$First, [int]$Last) {
    $lines = Get-Content $Path
    for ($i = $First; $i -le [Math]::Min($Last, $lines.Count); $i++) {
        '{0,5}: {1}' -f $i, $lines[$i - 1]
    }
}

Write-Host '=== Cura connectLines exact source ==='
Show-Lines 'sandboxes/curaengine-reference/src/infill.cpp' 880 1135

Write-Host '=== Cura connector data types ==='
rg -n -C 5 "struct Crossing|class UnionFind|connectLines|crossings_on_line|infill_paths" sandboxes/curaengine-reference/src sandboxes/curaengine-reference/include

Write-Host '=== Orca boundary graph implementation ==='
rg -n "struct BoundaryInfillGraph|class BoundaryInfillGraph|create_boundary_infill_graph|ContourIntersectionPoint" src/libslic3r/Fill/FillBase.cpp src/libslic3r/Fill/FillBase.hpp
Show-Lines 'src/libslic3r/Fill/FillBase.cpp' 1350 1835
