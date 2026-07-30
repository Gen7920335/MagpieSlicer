param(
    [int]$Parallel = [Math]::Max(1, [Environment]::ProcessorCount),
    [string]$ShortStageRoot = "C:\MagpiePkg"
)

$ErrorActionPreference = "Stop"

$root = Split-Path -Parent $PSScriptRoot
$buildDir = Join-Path $root "build"
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$resolvedStageRoot = [IO.Path]::GetFullPath($ShortStageRoot)
$stageDir = Join-Path $resolvedStageRoot $stamp
$outputDir = Join-Path $buildDir "installer\$stamp"
Set-Location $root

if ($resolvedStageRoot.Length -gt 80) {
    throw "NSIS staging root is too long ($($resolvedStageRoot.Length) characters). Use a short path such as C:\MagpiePkg."
}

$releaseExe = Join-Path $buildDir "src\Release\magpie-slicer.exe"
$lockingProcesses = @(
    Get-CimInstance Win32_Process -Filter "Name = 'magpie-slicer.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_.ExecutablePath) -and
            [IO.Path]::GetFullPath($_.ExecutablePath) -eq [IO.Path]::GetFullPath($releaseExe)
        }
)
if ($lockingProcesses.Count -gt 0) {
    $lockingPids = ($lockingProcesses | ForEach-Object ProcessId) -join ", "
    throw "The build-tree Magpie Slicer is running and locks the Release DLL. Close process ID(s): $lockingPids."
}

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
    $nsisLog = Join-Path $stageDir "_CPack_Packages\win64\NSIS\NSISOutput.log"
    if (Test-Path -LiteralPath $nsisLog) {
        $pathError = Select-String -LiteralPath $nsisLog -Pattern "Can't open output file|File: failed opening file" -ErrorAction SilentlyContinue |
            Select-Object -Last 1 -ExpandProperty Line
        if ($pathError) {
            throw "CPack NSIS generation failed. NSIS could not open a staged file, usually because its path is too long: $pathError"
        }
    }
    throw "CPack NSIS generation failed with exit code $LASTEXITCODE. See $nsisLog."
}

$installers = @(
    Get-ChildItem -LiteralPath $stageDir -File -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Name -match '^MagpieSlicer_Windows_Installer_.*_x64\.exe$'
        } |
        Sort-Object LastWriteTime -Descending
)
if ($installers.Count -eq 0) {
    throw "Package target succeeded but no Magpie Slicer installer was found."
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
