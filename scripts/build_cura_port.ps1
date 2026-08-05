$ErrorActionPreference = 'Stop'

$repo = Split-Path -Parent $PSScriptRoot
Set-Location $repo

$started = Get-Date
Write-Host "Build started: $started"

cmake --build build --config Release --target OrcaSlicer -- /m
if ($LASTEXITCODE -ne 0) {
    throw "OrcaSlicer build failed with exit code $LASTEXITCODE"
}

$exe = Join-Path $repo 'build\src\Release\magpie-slicer.exe'
if (-not (Test-Path $exe)) {
    throw "Expected executable not found: $exe"
}

$item = Get-Item $exe
Write-Host "Build completed in $([Math]::Round(((Get-Date) - $started).TotalSeconds, 1)) seconds"
Write-Host "Executable: $($item.FullName)"
Write-Host "Modified: $($item.LastWriteTime)"
Write-Host "Size: $($item.Length)"
