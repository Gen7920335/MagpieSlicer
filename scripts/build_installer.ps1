param(
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),
    [string]$ShortStageRoot = "C:\OrcaPkg"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$stageDir = Join-Path ([IO.Path]::GetFullPath($ShortStageRoot)) $stamp
$outputDir = Join-Path $buildDir "installer\$stamp"
Set-Location $root

$started = Get-Date
Write-Output "Installer build started: $($started.ToString('s'))"
Write-Output "Parallel jobs: $Parallel"
Write-Output "Short staging directory: $stageDir"

& cmake --build $buildDir --config Release --target OrcaSlicer_app_gui --parallel $Parallel
if ($LASTEXITCODE -ne 0) {
    throw "Release application build failed with exit code $LASTEXITCODE."
}

New-Item -ItemType Directory -Path $stageDir -Force | Out-Null
& cpack -G NSIS -C Release --config (Join-Path $buildDir "CPackConfig.cmake") -B $stageDir
if ($LASTEXITCODE -ne 0) {
    throw "CPack NSIS generation failed with exit code $LASTEXITCODE."
}

$installers = @(
    Get-ChildItem -LiteralPath $stageDir -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match '^OrcaSlicerTrInterface_Windows_Installer_.*_x64\.exe$'
        } |
        Sort-Object LastWriteTime -Descending
)
if ($installers.Count -eq 0) {
    throw "Package target succeeded but no TrInterface installer was found."
}

$stageInstaller = $installers[0]
New-Item -ItemType Directory -Path $outputDir -Force | Out-Null
$installerPath = Join-Path $outputDir $stageInstaller.Name
Copy-Item -LiteralPath $stageInstaller.FullName -Destination $installerPath
$installer = Get-Item -LiteralPath $installerPath
$hash = Get-FileHash -LiteralPath $installer.FullName -Algorithm SHA256
$elapsed = (Get-Date) - $started

Write-Output "INSTALLER_PATH=$($installer.FullName)"
Write-Output "INSTALLER_SIZE=$($installer.Length)"
Write-Output "INSTALLER_SHA256=$($hash.Hash)"
Write-Output "ELAPSED_SECONDS=$([Math]::Round($elapsed.TotalSeconds, 1))"
