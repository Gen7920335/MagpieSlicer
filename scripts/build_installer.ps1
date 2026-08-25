param(
    [ValidateRange(1, 2)]
    [int]$Parallel = 2,
    [string]$ShortStageRoot = "C:\MagpiePkg",
    [string]$BuildDirectory = "",
    [switch]$DevelopmentProfiler,
    [switch]$PreflightOnly
)

$ErrorActionPreference = "Stop"

# Installer builds are intentionally conservative. CMake/MSBuild project
# parallelism and MSVC's per-project /MP workers otherwise multiply each other
# and can make a 32 GiB workstation unresponsive during a full rebuild.
$currentProcess = [Diagnostics.Process]::GetCurrentProcess()
$currentProcess.PriorityClass = [Diagnostics.ProcessPriorityClass]::BelowNormal
$env:CMAKE_BUILD_PARALLEL_LEVEL = $Parallel.ToString()
$env:CL_MPCount = $Parallel.ToString()

$competingProcesses = @(
    Get-Process -Name cmake, cpack, makensis, msbuild, cl, link -ErrorAction SilentlyContinue |
        Where-Object { $_.Id -ne $currentProcess.Id }
)
if ($competingProcesses.Count -gt 0) {
    $processSummary = ($competingProcesses | ForEach-Object { "$($_.ProcessName):$($_.Id)" }) -join ", "
    throw "Another build or packaging process is already running ($processSummary). Wait for it to finish before building an installer."
}

$os = Get-CimInstance Win32_OperatingSystem
$freeMemoryBytes = [int64]$os.FreePhysicalMemory * 1KB
$minimumFreeMemoryBytes = 8GB
if ($freeMemoryBytes -lt $minimumFreeMemoryBytes) {
    throw "Installer packaging requires at least 8 GiB of free physical memory. Available: $([Math]::Round($freeMemoryBytes / 1GB, 1)) GiB."
}

$root = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildDirectory)) {
    $BuildDirectory = Join-Path $root $(if ($DevelopmentProfiler) { "build-profiler-dev" } else { "build-vulkan" })
}
$buildDir = [IO.Path]::GetFullPath($BuildDirectory)
$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$resolvedStageRoot = [IO.Path]::GetFullPath($ShortStageRoot)
$stageDir = Join-Path $resolvedStageRoot $stamp
$outputDir = Join-Path $buildDir "installer\$stamp"
Set-Location $root

$stageDrive = Get-PSDrive -Name ([IO.Path]::GetPathRoot($resolvedStageRoot).TrimEnd('\').TrimEnd(':'))
$minimumFreeDiskBytes = 20GB
if ($stageDrive.Free -lt $minimumFreeDiskBytes) {
    throw "Installer packaging requires at least 20 GiB free on $($stageDrive.Name):. Available: $([Math]::Round($stageDrive.Free / 1GB, 1)) GiB."
}

$cachePath = Join-Path $buildDir 'CMakeCache.txt'
if (-not (Test-Path -LiteralPath $cachePath -PathType Leaf)) {
    throw "CMake cache not found: $cachePath"
}
$cacheText = Get-Content -LiteralPath $cachePath -Raw
if ($cacheText -notmatch '(?m)^SLIC3R_ENABLE_VULKAN_SLICER:BOOL=ON\r?$') {
    throw "Installer packaging requires SLIC3R_ENABLE_VULKAN_SLICER=ON: $cachePath"
}
if ($cacheText -match '(?m)^MAGPIE_VULKAN_TEST_BRANDING:BOOL=ON\r?$') {
    throw "Installer packaging rejects MAGPIE_VULKAN_TEST_BRANDING=ON: $cachePath"
}
$profilerEnabled = $cacheText -match '(?m)^MAGPIE_SLICING_PROFILER:BOOL=ON\r?$'
if ($DevelopmentProfiler -and -not $profilerEnabled) {
    throw "Development profiler packaging requires MAGPIE_SLICING_PROFILER=ON: $cachePath"
}
if (-not $DevelopmentProfiler -and $profilerEnabled) {
    throw "Stable installer packaging rejects MAGPIE_SLICING_PROFILER=ON. Pass -DevelopmentProfiler for the isolated development package."
}

if ($resolvedStageRoot.Length -gt 80) {
    throw "NSIS staging root is too long ($($resolvedStageRoot.Length) characters). Use a short path such as C:\MagpiePkg."
}

$appKey = if ($DevelopmentProfiler) { "MagpieSlicerProfiler" } else { "MagpieSlicer" }
$appName = if ($DevelopmentProfiler) { "Magpie Slicer Profiler" } else { "Magpie Slicer" }
$appCommand = if ($DevelopmentProfiler) { "magpie-slicer-profiler" } else { "magpie-slicer" }
$releaseExe = Join-Path $buildDir "src\Release\$appCommand.exe"
$lockingProcesses = @(
    Get-CimInstance Win32_Process -Filter "Name = '$appCommand.exe'" -ErrorAction SilentlyContinue |
        Where-Object {
            -not [string]::IsNullOrWhiteSpace($_.ExecutablePath) -and
            [IO.Path]::GetFullPath($_.ExecutablePath) -eq [IO.Path]::GetFullPath($releaseExe)
        }
)
if ($lockingProcesses.Count -gt 0) {
    $lockingPids = ($lockingProcesses | ForEach-Object ProcessId) -join ", "
    throw "The build-tree $appName is running and locks the Release DLL. Close process ID(s): $lockingPids."
}

$started = Get-Date
Write-Output "Installer build started: $($started.ToString('s'))"
Write-Output "Parallel jobs: $Parallel"
Write-Output "MSVC compile workers: $env:CL_MPCount"
Write-Output "Process priority: $($currentProcess.PriorityClass)"
Write-Output "Free physical memory: $([Math]::Round($freeMemoryBytes / 1GB, 1)) GiB"
Write-Output "Free staging disk: $([Math]::Round($stageDrive.Free / 1GB, 1)) GiB"
Write-Output "Build directory: $buildDir"
Write-Output "Package identity: $appName ($appKey)"
Write-Output "Vulkan slicer: ON"
Write-Output "Vulkan test branding: OFF"
Write-Output "Slicing profiler: $($DevelopmentProfiler.IsPresent)"
Write-Output "Short staging directory: $stageDir"

if ($PreflightOnly) {
    Write-Output "PREFLIGHT_OK=1"
    return
}

& cmake --build $buildDir --config Release --target OrcaSlicer_app_gui --parallel $Parallel -- /p:CL_MPCount=$Parallel /nodeReuse:false
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
            $_.Name -match "^$($appKey)_Windows_Installer_.*_x64\.exe$"
        } |
        Sort-Object LastWriteTime -Descending
)
if ($installers.Count -eq 0) {
    throw "Package target succeeded but no $appName installer was found."
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
