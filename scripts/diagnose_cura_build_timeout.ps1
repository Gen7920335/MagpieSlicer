$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

Write-Host '=== Active build processes ==='
Get-CimInstance Win32_Process |
    Where-Object { $_.Name -match '^(cmake|msbuild|cl|link|mspdbsrv)\.exe$' } |
    Select-Object ProcessId, Name, CreationDate, CommandLine |
    Format-List

Write-Host '=== Recent build outputs ==='
$paths = @(
    'build\src\libslic3r\Release\libslic3r.lib',
    'build\src\OrcaSlicer.dir\Release\OrcaSlicer.obj',
    'build\src\Release\OrcaSlicer.dll',
    'build\src\Release\orca-slicer.exe'
)
foreach ($path in $paths) {
    if (Test-Path $path) {
        $item = Get-Item $path
        [pscustomobject]@{
            Path = $path
            Modified = $item.LastWriteTime
            Size = $item.Length
        }
    } else {
        [pscustomobject]@{ Path = $path; Modified = $null; Size = $null }
    }
}

Write-Host '=== Latest compiler files ==='
Get-ChildItem 'build\src' -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object { $_.Extension -in '.obj', '.lib', '.dll', '.exe', '.tlog' } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 25 FullName, LastWriteTime, Length |
    Format-Table -AutoSize

Write-Host '=== Changed header dependency scope ==='
rg -l '#include "FillBase.hpp"|#include ".*/FillBase.hpp"' src/libslic3r |
    Measure-Object |
    Select-Object Count
