$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$nsisDir = Join-Path $buildDir "_CPack_Packages\win64\NSIS"
$log = Join-Path $nsisDir "NSISOutput.log"
$script = Join-Path $nsisDir "project.nsi"
$makensis = "C:\Program Files (x86)\NSIS\makensis.exe"

Write-Output "=== NSIS executable ==="
Write-Output "Exists=$(Test-Path -LiteralPath $makensis)"
if (Test-Path -LiteralPath $makensis) {
    Get-Item -LiteralPath $makensis |
        Select-Object FullName, Length, LastWriteTime, VersionInfo
}
Write-Output "=== NSIS output tail ==="
if (-not (Test-Path -LiteralPath $log)) {
    throw "NSIS output log is missing: $log"
}
Get-Content -LiteralPath $log -Tail 160

Write-Output "=== Generated script ==="
if (Test-Path -LiteralPath $script) {
    $item = Get-Item -LiteralPath $script
    Write-Output "Path=$($item.FullName)"
    Write-Output "Size=$($item.Length)"
    Select-String -LiteralPath $script -Pattern `
        '^(OutFile|InstallDir|File |SetCompressor|SetCompressorDictSize|Name )' |
        Select-Object -First 80 |
        ForEach-Object Line
}

Write-Output "=== Staging payload ==="
$payload = Join-Path $nsisDir "OrcaSlicerTrInterface_Windows_Installer_V2.5.0-dev_x64"
if (Test-Path -LiteralPath $payload) {
    $files = @(Get-ChildItem -LiteralPath $payload -Recurse -File)
    $bytes = ($files | Measure-Object Length -Sum).Sum
    Write-Output "Files=$($files.Count)"
    Write-Output "Bytes=$bytes"
    $files |
        Sort-Object Length -Descending |
        Select-Object -First 20 FullName, Length
}
