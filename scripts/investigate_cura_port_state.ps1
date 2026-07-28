$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host '=== Git status ==='
git status --short

Write-Host '=== Cura adapter diff ==='
git diff -- src/libslic3r/Fill/FillBase.hpp src/libslic3r/Fill/FillBase.cpp src/libslic3r/Fill/FillRectilinear.cpp src/libslic3r/Support/SupportCommon.hpp src/libslic3r/Support/SupportCommon.cpp src/libslic3r/Support/CuraStyleSupport.cpp

Write-Host '=== Orca Cura-style support implementation ==='
$orca = Get-Content 'src/libslic3r/Support/CuraStyleSupport.cpp'
for ($i = 1; $i -le $orca.Count; $i++) {
    if (($i -ge 1 -and $i -le 340) -or ($i -ge 500 -and $i -le 620)) {
        '{0,5}: {1}' -f $i, $orca[$i - 1]
    }
}

Write-Host '=== Cura reference roof/interface implementation ==='
$cura = Get-Content 'sandboxes/curaengine-reference/src/support.cpp'
for ($i = 1690; $i -le [Math]::Min(1890, $cura.Count); $i++) {
    '{0,5}: {1}' -f $i, $cura[$i - 1]
}

Write-Host '=== Relevant Orca contact/interface calls ==='
rg -n "make_cura_style_contact_footprints|generate_interface_layers|top_contacts|bottom_contacts|interface_id" src/libslic3r/Support/CuraStyleSupport.cpp src/libslic3r/Support/SupportCommon.cpp src/libslic3r/Support/SupportCommon.hpp
