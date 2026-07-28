$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

function Show-Lines([string]$Path, [int]$First, [int]$Last) {
    $lines = Get-Content $Path
    for ($i = $First; $i -le [Math]::Min($Last, $lines.Count); $i++) {
        '{0,5}: {1}' -f $i, $lines[$i - 1]
    }
}

Write-Host '=== CuraStyleSupport contact and generate ==='
Show-Lines 'src/libslic3r/Support/CuraStyleSupport.cpp' 330 660

Write-Host '=== Support layer data structures ==='
rg -n -C 5 "struct SupportGeneratorLayer|class SupportGeneratorLayer|enum class SupportLayerType|make_contact_layers_from_footprints" src/libslic3r/Support src/libslic3r

Write-Host '=== Configuration fields used by normal support interface ==='
rg -n "interface_top_layers|interface_bottom_layers|support_top_z_distance|support_bottom_z_distance|interface_spacing|support_interface_spacing|support_interface_pattern|support_interface_loop_pattern" src/libslic3r/Support src/libslic3r/PrintConfig.*

Write-Host '=== Cura area-generation core ==='
Show-Lines 'sandboxes/curaengine-reference/src/support.cpp' 1010 1385
