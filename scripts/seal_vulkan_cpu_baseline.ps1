param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [Parameter(Mandatory = $true)]
    [string] $ReferenceRun,
    [Parameter(Mandatory = $true)]
    [string] $ValidatedRunsCsv,
    [string] $Destination = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
$ReferenceRun = [IO.Path]::GetFullPath($ReferenceRun)
$ValidatedRuns = @($ValidatedRunsCsv -split ';' | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
if ($ValidatedRuns.Count -eq 0) { throw 'At least one validated run is required' }
if ([string]::IsNullOrWhiteSpace($Destination)) {
    $Destination = Join-Path $RepoRoot 'build\verification\vulkan-baseline\cpu-random-320-seed250802'
}
$Destination = [IO.Path]::GetFullPath($Destination)
New-Item -ItemType Directory -Force -Path $Destination | Out-Null

foreach ($name in @('case-plan.json','results.json','summary.csv','baseline-manifest.json','baseline-comparison.json')) {
    $source = Join-Path $ReferenceRun $name
    if (Test-Path -LiteralPath $source -PathType Leaf) {
        Copy-Item -LiteralPath $source -Destination (Join-Path $Destination $name) -Force
    }
}

$results = Get-Content -LiteralPath (Join-Path $ReferenceRun 'results.json') -Raw | ConvertFrom-Json
if ($results.Count -lt 300) { throw "Reference run has only $($results.Count) cases" }
foreach ($result in $results) {
    $sourceCase = Join-Path $ReferenceRun $result.id
    $destinationCase = Join-Path $Destination $result.id
    New-Item -ItemType Directory -Force -Path $destinationCase | Out-Null
    foreach ($name in @('machine.json','process.json')) {
        $source = Join-Path $sourceCase $name
        if (Test-Path -LiteralPath $source -PathType Leaf) {
            Copy-Item -LiteralPath $source -Destination (Join-Path $destinationCase $name) -Force
        }
    }
    $gcode = Get-ChildItem -LiteralPath $sourceCase -File -Filter '*.gcode' | Select-Object -First 1
    if ($null -eq $gcode) { throw "Reference G-code is missing for $($result.id)" }
    $target = Join-Path $destinationCase $gcode.Name
    if (-not (Test-Path -LiteralPath $target -PathType Leaf)) {
        New-Item -ItemType HardLink -Path $target -Target $gcode.FullName | Out-Null
    }
}

$validation = foreach ($run in $ValidatedRuns) {
    $fullRun = [IO.Path]::GetFullPath($run)
    $comparison = Join-Path $fullRun 'baseline-comparison.json'
    if (-not (Test-Path -LiteralPath $comparison -PathType Leaf)) { throw "Missing comparison: $comparison" }
    $document = Get-Content -LiteralPath $comparison -Raw | ConvertFrom-Json
    if ($document.failed -ne 0 -or $document.case_count -lt 300) { throw "Comparison did not pass: $comparison" }
    [ordered]@{
        run = $fullRun
        comparison_sha256 = (Get-FileHash -LiteralPath $comparison -Algorithm SHA256).Hash.ToLowerInvariant()
        cases = $document.case_count
        passed = $document.passed
    }
}

$manifest = [ordered]@{
    schema = 1
    kind = 'magpie-vulkan-sealed-cpu-baseline'
    seed = 250802
    case_count = $results.Count
    slicing_runs = 1 + $ValidatedRuns.Count
    total_slices = $results.Count * (1 + $ValidatedRuns.Count)
    reference_run = $ReferenceRun
    reference_results_sha256 = (Get-FileHash -LiteralPath (Join-Path $ReferenceRun 'results.json') -Algorithm SHA256).Hash.ToLowerInvariant()
    validation = @($validation)
    coverage_grid_mm = 0.05
    note = 'Lightning and rare CPU ordering variance use the measured nondeterminism envelope in compare_vulkan_cpu_baselines.ps1.'
}
$manifestPath = Join-Path $Destination 'sealed-baseline-manifest.json'
$manifest | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $manifestPath -Encoding UTF8
Write-Output "BASELINE=$Destination"
Write-Output "CASES=$($manifest.case_count) RUNS=$($manifest.slicing_runs) TOTAL_SLICES=$($manifest.total_slices)"
Write-Output "MANIFEST=$manifestPath"
