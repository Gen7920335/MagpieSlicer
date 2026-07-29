param(
    [string] $BuildRoot,
    [string] $OutputRoot,
    [string] $PackageName = 'MagpieSlicer',
    [int] $SmokeTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($BuildRoot)) { $BuildRoot = Join-Path $RepoRoot 'build' }
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $BuildRoot 'portable' }

$BuildRoot = [IO.Path]::GetFullPath($BuildRoot)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$releaseRoot = Join-Path $BuildRoot 'src\Release'
$sourceLauncher = Join-Path $releaseRoot 'magpie-slicer.exe'
$sourceDll = Join-Path $releaseRoot 'MagpieSlicer.dll'
$verifyScript = Join-Path $PSScriptRoot 'verify_multinozzle_orcacube.ps1'
foreach ($requiredPath in @($sourceLauncher, $sourceDll, $verifyScript)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) { throw "Required file not found: $requiredPath" }
}

New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$stageRoot = [IO.Path]::GetFullPath((Join-Path $OutputRoot $PackageName))
$outputPrefix = $OutputRoot.TrimEnd('\') + '\'
if (-not $stageRoot.StartsWith($outputPrefix, [StringComparison]::OrdinalIgnoreCase)) {
    throw "Refusing to replace staging directory outside output root: $stageRoot"
}
if (Test-Path -LiteralPath $stageRoot) { Remove-Item -LiteralPath $stageRoot -Recurse -Force }

& cmake --install $BuildRoot --config Release --prefix $stageRoot
if ($LASTEXITCODE -ne 0) { throw "CMake install failed with exit code $LASTEXITCODE" }

foreach ($developmentDirectory in @('include', 'lib')) {
    $path = Join-Path $stageRoot $developmentDirectory
    if (Test-Path -LiteralPath $path) { Remove-Item -LiteralPath $path -Recurse -Force }
}

$stageLauncher = Join-Path $stageRoot 'magpie-slicer.exe'
$stageDll = Join-Path $stageRoot 'MagpieSlicer.dll'
$stageResources = Join-Path $stageRoot 'resources'
foreach ($requiredPath in @($stageLauncher, $stageDll, $stageResources)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) { throw "Portable staging file not found: $requiredPath" }
}

$sourceHash = (Get-FileHash -LiteralPath $sourceDll -Algorithm SHA256).Hash
$stageHash = (Get-FileHash -LiteralPath $stageDll -Algorithm SHA256).Hash
if ($sourceHash -ne $stageHash) { throw 'Staged MagpieSlicer.dll does not match the Release build' }
$resourceCount = @(Get-ChildItem -LiteralPath $stageResources -Recurse -File).Count
if ($resourceCount -lt 100) { throw "Portable resources are incomplete: $resourceCount files" }

$smokeRoot = Join-Path $BuildRoot 'verification\portable-smoke'
& powershell -NoProfile -ExecutionPolicy Bypass -File $verifyScript `
    -SlicerPath $stageLauncher `
    -OutputRoot $smokeRoot `
    -SliceTimeoutSeconds $SmokeTimeoutSeconds `
    -CaseFilter 'classic_count1_015'
if ($LASTEXITCODE -ne 0) { throw "Portable slicing smoke test failed with exit code $LASTEXITCODE" }

$branch = (& git -C $RepoRoot branch --show-current 2>$null)
$commit = (& git -C $RepoRoot rev-parse HEAD 2>$null)
$buildInfo = [ordered]@{
    package = $PackageName
    created_utc = (Get-Date).ToUniversalTime().ToString('o')
    branch = [string] $branch
    commit = [string] $commit
    magpie_slicer_dll_sha256 = $stageHash
    resource_files = $resourceCount
    smoke_test = 'classic_count1_015 passed'
}
$buildInfo | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $stageRoot 'build-info.json') -Encoding UTF8

$dateStamp = Get-Date -Format 'yyyyMMdd-HHmmss'
$archivePath = Join-Path $OutputRoot "$PackageName-$dateStamp-portable.zip"
$temporaryArchive = Join-Path $OutputRoot "$PackageName-$dateStamp-partial.zip"
if (Test-Path -LiteralPath $temporaryArchive) { Remove-Item -LiteralPath $temporaryArchive -Force }
Push-Location $OutputRoot
try {
    & tar.exe -a -c -f $temporaryArchive $PackageName
    if ($LASTEXITCODE -ne 0) { throw "ZIP creation failed with exit code $LASTEXITCODE" }
} finally {
    Pop-Location
}
Move-Item -LiteralPath $temporaryArchive -Destination $archivePath

$archiveEntries = @(& tar.exe -tf $archivePath)
if ($LASTEXITCODE -ne 0) { throw "ZIP listing failed with exit code $LASTEXITCODE" }
foreach ($requiredEntry in @(
    "$PackageName/magpie-slicer.exe",
    "$PackageName/MagpieSlicer.dll",
    "$PackageName/resources/",
    "$PackageName/build-info.json"
)) {
    if (-not ($archiveEntries | Where-Object { $_ -eq $requiredEntry -or $_.StartsWith($requiredEntry) })) {
        throw "Required ZIP entry not found: $requiredEntry"
    }
}

$archive = Get-Item -LiteralPath $archivePath
[pscustomobject]@{
    Archive = $archive.FullName
    SizeBytes = $archive.Length
    Stage = $stageRoot
    DllSha256 = $stageHash
    ResourceFiles = $resourceCount
    SmokeTest = 'passed'
} | Format-List
