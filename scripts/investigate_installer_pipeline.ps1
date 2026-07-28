$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
Set-Location $root

Write-Output "=== Repository ==="
git status --short
git branch --show-current

Write-Output "=== Release binaries ==="
Get-Item `
    ".\build\src\Release\orca-slicer.exe", `
    ".\build\src\Release\OrcaSlicer.dll" |
    Select-Object FullName, Length, LastWriteTime

Write-Output "=== Packaging files ==="
rg --files |
    rg "(?i)(installer|package|packaging|nsis|cpack|wix|inno|setup|portable|build_release)" |
    Select-Object -First 160

Write-Output "=== Packaging commands ==="
$searchFiles = @(
    (rg --files -g "README*" -g "*.md" -g "*.bat" -g "*.cmd" -g "*.ps1" -g "*.nsi" -g "*.wxs" -g "CMakeLists.txt")
)
if ($searchFiles.Count -gt 0) {
    $searchFiles |
        ForEach-Object {
            rg -n -i `
                "cpack|nsis|wix|makensis|installer|setup\.exe|package target|cmake --build.*package" `
                -- $_ 2>$null
        } |
    Select-Object -First 240
}

Write-Output "=== Root files and directories ==="
Get-ChildItem -Force |
    Select-Object Mode, Name

Write-Output "=== Existing installers ==="
$candidateRoots = @(".\build", ".\package", ".\installer", ".\dist", ".\release") |
    Where-Object { Test-Path -LiteralPath $_ }
Get-ChildItem -Path $candidateRoots `
    -Recurse -File -ErrorAction SilentlyContinue |
    Where-Object {
        $_.Name -match "(?i)(setup|installer).*\.(exe|msi)$" -or
        $_.Extension -eq ".msi"
    } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 30 FullName, Length, LastWriteTime
