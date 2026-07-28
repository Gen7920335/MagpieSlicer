$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Output "=== Tools ==="
foreach ($name in @("cmake", "cpack", "makensis")) {
    $command = Get-Command $name -ErrorAction SilentlyContinue
    if ($command) {
        Write-Output "$name=$($command.Source)"
    } else {
        Write-Output "$name=MISSING"
    }
}
Write-Output "=== CPack configuration ==="
$cpackConfig = Join-Path $root "build\CPackConfig.cmake"
Write-Output "CPackConfigExists=$(Test-Path -LiteralPath $cpackConfig)"
if (Test-Path -LiteralPath $cpackConfig) {
    Select-String -LiteralPath $cpackConfig -Pattern `
        'CPACK_PACKAGE_FILE_NAME|CPACK_PACKAGE_INSTALL_DIRECTORY|CPACK_PACKAGE_EXECUTABLES|CPACK_GENERATOR' |
        ForEach-Object Line
}

Write-Output "=== Build cache identity ==="
$cache = Join-Path $root "build\CMakeCache.txt"
if (-not (Test-Path -LiteralPath $cache)) {
    throw "Build CMakeCache.txt is missing."
}
Select-String -LiteralPath $cache -Pattern `
    '^(CMAKE_GENERATOR|CMAKE_BUILD_TYPE|SLIC3R_APP_NAME|SLIC3R_APP_KEY|SLIC3R_APP_CMD|SoftFever_VERSION|CMAKE_INSTALL_PREFIX):' |
    ForEach-Object Line

Write-Output "=== Release dependency sample ==="
$releaseDir = Join-Path $root "build\src\Release"
$required = @(
    "orca-slicer.exe",
    "OrcaSlicer.dll"
)
foreach ($name in $required) {
    $path = Join-Path $releaseDir $name
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Required release file is missing: $path"
    }
    $item = Get-Item -LiteralPath $path
    Write-Output "$name=$($item.Length) bytes; $($item.LastWriteTime.ToString('s'))"
}
