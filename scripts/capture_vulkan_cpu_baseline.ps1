param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $VerificationRoot = "",
    [string] $OutputPath = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if ([string]::IsNullOrWhiteSpace($VerificationRoot)) {
    $VerificationRoot = Get-ChildItem -LiteralPath (Join-Path $RepoRoot 'build\verification') `
        -Directory -Filter 'release-readiness-*' |
        Sort-Object LastWriteTimeUtc -Descending |
        Select-Object -First 1 -ExpandProperty FullName
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $RepoRoot 'build\verification\vulkan-baseline\cpu-baseline.json'
}
$VerificationRoot = [IO.Path]::GetFullPath($VerificationRoot)
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

$summaryPath = Join-Path $VerificationRoot 'summary.json'
$slicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
foreach ($required in @($summaryPath, $slicerPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

$summary = @(Get-Content -LiteralPath $summaryPath -Raw | ConvertFrom-Json)
$failed = @($summary | Where-Object { -not $_.Passed -or [int]$_.ExitCode -ne 0 })
if ($failed.Count -gt 0) {
    throw "Cannot capture a CPU baseline from a failed verification run"
}
if (-not ($summary.Name -contains 'small-nozzle-full')) {
    throw "The selected verification run is not a Full release-readiness run"
}

$roots = [Collections.Generic.Dictionary[string,string]]::new([StringComparer]::OrdinalIgnoreCase)
Get-ChildItem -LiteralPath $VerificationRoot -Filter '*.stdout.log' -File | ForEach-Object {
    $job = $_.BaseName -replace '\.stdout$', ''
    foreach ($line in Get-Content -LiteralPath $_.FullName) {
        if ($line -notmatch '^(?:PLAN|RESULTS|SUMMARY|RUN_ROOT)=(.+)$') { continue }
        $path = $matches[1].Trim()
        if (-not (Test-Path -LiteralPath $path)) { continue }
        $root = if (Test-Path -LiteralPath $path -PathType Leaf) { Split-Path -Parent $path } else { $path }
        $roots["$job|$root"] = $root
    }
}

$entries = [Collections.Generic.List[object]]::new()
$seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
foreach ($item in $roots.GetEnumerator() | Sort-Object Key) {
    $separator = $item.Key.IndexOf('|')
    $job = $item.Key.Substring(0, $separator)
    $root = $item.Value
    Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Extension -in '.gcode', '.json', '.csv' } |
        Sort-Object FullName |
        ForEach-Object {
            if (-not $seen.Add($_.FullName)) { return }
            $relative = $_.FullName.Substring($root.Length).TrimStart('\','/') -replace '\\','/'
            $entries.Add([pscustomobject]@{
                job = $job
                path = $relative
                bytes = $_.Length
                sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant()
            })
        }
}

if (@($entries | Where-Object { $_.path -like '*.gcode' }).Count -eq 0) {
    throw "No G-code files were discovered from the verification logs"
}

$head = (& git -C $RepoRoot rev-parse HEAD).Trim()
$manifest = [ordered]@{
    schema = 1
    kind = 'magpie-vulkan-cpu-baseline'
    git_head = $head
    slicer = [ordered]@{
        path = 'build/src/Release/magpie-slicer.exe'
        bytes = (Get-Item -LiteralPath $slicerPath).Length
        sha256 = (Get-FileHash -LiteralPath $slicerPath -Algorithm SHA256).Hash.ToLowerInvariant()
    }
    verification_cases = @($summary | ForEach-Object {
        [ordered]@{ name=$_.Name; seconds=[double]$_.Seconds; passed=[bool]$_.Passed }
    })
    artifacts = @($entries | Sort-Object job,path,sha256)
}

New-Item -ItemType Directory -Force -Path (Split-Path -Parent $OutputPath) | Out-Null
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "BASELINE=$OutputPath"
Write-Output "CASES=$($manifest.verification_cases.Count)"
Write-Output "ARTIFACTS=$($manifest.artifacts.Count)"
Write-Output "GCODE=$(@($manifest.artifacts | Where-Object path -like '*.gcode').Count)"
