$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$stamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$destination = Join-Path $repo "backups\pre-cura-component-join-$stamp"
$files = @(
    'src\libslic3r\Fill\FillBase.cpp',
    'src\libslic3r\Fill\FillBase.hpp',
    'src\libslic3r\Fill\FillRectilinear.cpp',
    'src\libslic3r\Support\CuraStyleSupport.cpp',
    'src\libslic3r\Support\SupportCommon.cpp',
    'src\libslic3r\Support\SupportCommon.hpp'
)

New-Item -ItemType Directory -Path $destination -Force | Out-Null
foreach ($file in $files) {
    $target = Join-Path $destination $file
    New-Item -ItemType Directory -Path (Split-Path -Parent $target) -Force | Out-Null
    Copy-Item -LiteralPath (Join-Path $repo $file) -Destination $target
}

git diff -- $files | Set-Content -Path (Join-Path $destination 'working.diff') -Encoding utf8
Write-Host "Backup: $destination"
