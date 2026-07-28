$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host '=== Stanford Bunny references ==='
rg -n -C 4 "Stanford_Bunny|handy_models|\\.drc" src resources CMakeLists.txt

Write-Host '=== Draco-related binaries ==='
Get-ChildItem build -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Name -match '(?i)(draco|decoder)' -or $_.Extension -eq '.exe' } |
    Select-Object FullName, Length, LastWriteTime |
    Format-Table -AutoSize

Write-Host '=== Import format registration ==='
rg -n -C 3 "drc|Draco|load_model|load.*mesh" src/libslic3r src/slic3r |
    Select-Object -First 240
