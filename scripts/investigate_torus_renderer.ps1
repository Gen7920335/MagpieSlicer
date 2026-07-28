$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$path = 'scripts\render_cura_torus_support_overlay.ps1'
$lines = Get-Content $path

Write-Host '=== Parameters and G-code parser ==='
for ($i = 1; $i -le $lines.Count; $i++) {
    if ($i -le 90 -or ($i -ge 300 -and $i -le 620)) {
        '{0,5}: {1}' -f $i, $lines[$i - 1]
    }
}
