param(
    [Parameter(Mandatory = $true)]
    [string] $ReferenceRun,
    [Parameter(Mandatory = $true)]
    [string] $CandidateRun,
    [string[]] $CpuEnvelopeRuns = @(),
    [string] $DispatchDiagnosticsRoot = "",
    [switch] $PassCpuFallbackWithoutDispatch,
    [string] $OutputPath = "",
    [ValidateRange(0.01, 0.25)]
    [double] $CoverageGridMm = 0.05,
    [ValidateRange(1, 5000)]
    [int] $MinimumCaseCount = 300
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$ReferenceRun = [IO.Path]::GetFullPath($ReferenceRun)
$CandidateRun = [IO.Path]::GetFullPath($CandidateRun)
$CpuEnvelopeRuns = @($CpuEnvelopeRuns | ForEach-Object { [IO.Path]::GetFullPath($_) })
if (-not [string]::IsNullOrWhiteSpace($DispatchDiagnosticsRoot)) {
    $DispatchDiagnosticsRoot = [IO.Path]::GetFullPath($DispatchDiagnosticsRoot)
}
$analyzerSource = Join-Path $PSScriptRoot 'lib\GcodeGeometryAnalyzer.cs'
if (-not (Test-Path -LiteralPath $analyzerSource -PathType Leaf)) { throw "Missing analyzer source: $analyzerSource" }
Add-Type -Path $analyzerSource
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $CandidateRun 'baseline-comparison.json'
}

function Get-TextSha256([string] $Text) {
    $bytes = [Text.Encoding]::UTF8.GetBytes($Text)
    $sha = [Security.Cryptography.SHA256]::Create()
    try {
        return ([BitConverter]::ToString($sha.ComputeHash($bytes))).Replace('-', '').ToLowerInvariant()
    } finally {
        $sha.Dispose()
    }
}

function Get-Parameter([string] $Command, [char] $Name) {
    $match = [regex]::Match($Command, "(?:^|\s)$Name(-?(?:\d+(?:\.\d*)?|\.\d+))(?=\s|$)", 'IgnoreCase')
    if (-not $match.Success) { return $null }
    return [double]::Parse($match.Groups[1].Value, $Invariant)
}

function Get-GcodeGeometry([string] $Path, [double] $GridMm, [bool] $IncludeCoverage) {
    $segments = [Collections.Generic.List[string]]::new()
    $coverage = if ($IncludeCoverage) { [Collections.Generic.HashSet[string]]::new([StringComparer]::Ordinal) } else { $null }
    $groupLength = @{}
    $groupExtrusion = @{}
    [double] $x = 0; [double] $y = 0; [double] $z = 0; [double] $e = 0
    $xyzAbsolute = $true
    $eAbsolute = $true
    $tool = 0
    $layer = -1
    $role = 'unknown'
    $positiveSegments = 0

    foreach ($raw in [IO.File]::ReadLines($Path)) {
        $line = $raw.Trim()
        if ($line -match '^;\s*(?:LAYER:|layer num/total_layer_count:)\s*(-?\d+)') {
            $layer = [int]$Matches[1]
        } elseif ($line -match '^;\s*LAYER_CHANGE') {
            ++$layer
        }
        if ($line -match '^;\s*(?:TYPE|FEATURE):\s*(.+?)\s*$') { $role = $Matches[1].Trim().ToLowerInvariant() }

        $command = ($line -split ';', 2)[0].Trim()
        if ($command.Length -eq 0) { continue }
        if ($command -match '^G90(?:\s|$)') { $xyzAbsolute = $true; continue }
        if ($command -match '^G91(?:\s|$)') { $xyzAbsolute = $false; continue }
        if ($command -match '^M82(?:\s|$)') { $eAbsolute = $true; continue }
        if ($command -match '^M83(?:\s|$)') { $eAbsolute = $false; continue }
        if ($command -match '^T(\d+)(?:\s|$)') { $tool = [int]$Matches[1]; continue }
        if ($command -match '^G92(?:\s|$)') {
            $value = Get-Parameter $command 'X'; if ($null -ne $value) { $x = $value }
            $value = Get-Parameter $command 'Y'; if ($null -ne $value) { $y = $value }
            $value = Get-Parameter $command 'Z'; if ($null -ne $value) { $z = $value }
            $value = Get-Parameter $command 'E'; if ($null -ne $value) { $e = $value }
            continue
        }
        if ($command -notmatch '^G[01](?:\s|$)') { continue }

        $nx = $x; $ny = $y; $nz = $z; $ne = $e
        $value = Get-Parameter $command 'X'; if ($null -ne $value) { $nx = if ($xyzAbsolute) { $value } else { $x + $value } }
        $value = Get-Parameter $command 'Y'; if ($null -ne $value) { $ny = if ($xyzAbsolute) { $value } else { $y + $value } }
        $value = Get-Parameter $command 'Z'; if ($null -ne $value) { $nz = if ($xyzAbsolute) { $value } else { $z + $value } }
        $value = Get-Parameter $command 'E'
        [double] $deltaE = 0
        if ($null -ne $value) {
            if ($eAbsolute) { $ne = $value; $deltaE = $ne - $e }
            else { $deltaE = $value; $ne = $e + $value }
        }

        $dx = $nx - $x; $dy = $ny - $y
        $length = [Math]::Sqrt($dx * $dx + $dy * $dy)
        if ($deltaE -gt 1e-8 -and $length -gt 1e-8) {
            ++$positiveSegments
            $group = '{0}|{1}|{2}|{3}' -f $layer, $tool, $role, $nz.ToString('0.0000', $Invariant)
            $a = '{0},{1}' -f $x.ToString('0.0000', $Invariant), $y.ToString('0.0000', $Invariant)
            $b = '{0},{1}' -f $nx.ToString('0.0000', $Invariant), $ny.ToString('0.0000', $Invariant)
            if ([StringComparer]::Ordinal.Compare($a, $b) -gt 0) { $swap = $a; $a = $b; $b = $swap }
            $segments.Add(('{0}|{1}|{2}|{3}' -f $group, $a, $b, $deltaE.ToString('0.000000', $Invariant)))
            if (-not $groupLength.ContainsKey($group)) { $groupLength[$group] = 0.0; $groupExtrusion[$group] = 0.0 }
            $groupLength[$group] += $length
            $groupExtrusion[$group] += $deltaE

            if ($IncludeCoverage) {
                $samples = [Math]::Max(1, [int][Math]::Ceiling($length / $GridMm))
                for ($sample = 0; $sample -le $samples; ++$sample) {
                    $t = $sample / [double]$samples
                    $gx = [int][Math]::Round(($x + $dx * $t) / $GridMm)
                    $gy = [int][Math]::Round(($y + $dy * $t) / $GridMm)
                    [void]$coverage.Add(('{0}|{1}|{2}' -f $group, $gx, $gy))
                }
            }
        }
        $x = $nx; $y = $ny; $z = $nz; $e = $ne
    }

    $metricLines = foreach ($key in ($groupLength.Keys | Sort-Object)) {
        '{0}|L={1}|E={2}' -f $key, $groupLength[$key].ToString('0.001', $Invariant), $groupExtrusion[$key].ToString('0.0001', $Invariant)
    }
    [pscustomobject]@{
        segment_sha256 = Get-TextSha256 (($segments | Sort-Object) -join "`n")
        coverage_sha256 = if ($IncludeCoverage) { Get-TextSha256 (($coverage | Sort-Object) -join "`n") } else { '' }
        metrics_sha256 = Get-TextSha256 ($metricLines -join "`n")
        positive_segments = $positiveSegments
        coverage_cells = if ($IncludeCoverage) { $coverage.Count } else { 0 }
    }
}

function Read-Results([string] $Run) {
    $path = Join-Path $Run 'results.json'
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Missing results file: $path" }
    return (Get-Content -LiteralPath $path -Raw | ConvertFrom-Json)
}

$reference = @(Read-Results $ReferenceRun)
$candidate = @(Read-Results $CandidateRun)
if ($reference.Count -ne $candidate.Count -or $reference.Count -lt $MinimumCaseCount) {
    throw "Baseline sizes are invalid: reference=$($reference.Count), candidate=$($candidate.Count)"
}
$candidateById = @{}; foreach ($item in $candidate) { $candidateById[$item.id] = $item }
$cpuEnvelopeResults = [Collections.Generic.List[object]]::new()
foreach ($run in $CpuEnvelopeRuns) {
    $items = @(Read-Results $run)
    if ($items.Count -ne $reference.Count) {
        throw "CPU envelope size is invalid: run=$run reference=$($reference.Count) envelope=$($items.Count)"
    }
    $byId = @{}; foreach ($item in $items) { $byId[$item.id] = $item }
    $cpuEnvelopeResults.Add([pscustomobject]@{ run=$run; by_id=$byId })
}
$planPath = Join-Path $CandidateRun 'case-plan.json'
if (-not (Test-Path -LiteralPath $planPath -PathType Leaf)) { throw "Missing case plan: $planPath" }
$planDocument = Get-Content -LiteralPath $planPath -Raw | ConvertFrom-Json
$planCases = if ($null -ne $planDocument.cases) { @($planDocument.cases) } else { @($planDocument) }
$caseById = @{}; foreach ($case in $planCases) { $caseById[[string]$case.id] = $case }
$rows = [Collections.Generic.List[object]]::new()
$index = 0
foreach ($left in $reference) {
    ++$index
    if (-not $candidateById.ContainsKey($left.id)) { throw "Candidate is missing $($left.id)" }
    $right = $candidateById[$left.id]
    $mode = 'exact-motion'
    $passed = $left.settings_sha256 -eq $right.settings_sha256 -and $left.motion_sha256 -eq $right.motion_sha256
    $candidateDispatched = $null
    if (-not [string]::IsNullOrWhiteSpace($DispatchDiagnosticsRoot)) {
        $diagnosticsPath = Join-Path (Join-Path $DispatchDiagnosticsRoot $left.id) 'vulkan-dispatch.csv'
        $candidateDispatched = Test-Path -LiteralPath $diagnosticsPath -PathType Leaf
        if ($candidateDispatched) {
            $candidateDispatched = (Get-Item -LiteralPath $diagnosticsPath).Length -gt 0
        }
    }
    $leftGeometry = $null; $rightGeometry = $null
    $coverageDifferenceRatio = $null
    $lengthDifferenceRatio = $null
    $extrusionDifferenceRatio = $null
    $coverageLimitApplied = $null
    $metricLimitApplied = $null
    if (-not $passed -and $left.settings_sha256 -eq $right.settings_sha256 -and
        $PassCpuFallbackWithoutDispatch -and $candidateDispatched -eq $false -and [bool]$right.passed) {
        $mode = 'cpu-fallback-no-vulkan-work'
        $passed = $true
    }
    if (-not $passed -and $left.settings_sha256 -eq $right.settings_sha256) {
        $leftGcode = Get-ChildItem -LiteralPath (Join-Path $ReferenceRun $left.id) -File -Filter '*.gcode' | Select-Object -First 1
        $rightGcode = Get-ChildItem -LiteralPath (Join-Path $CandidateRun $left.id) -File -Filter '*.gcode' | Select-Object -First 1
        if ($null -ne $leftGcode -and $null -ne $rightGcode) {
            $leftGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($leftGcode.FullName, $CoverageGridMm, $false)
            $rightGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($rightGcode.FullName, $CoverageGridMm, $false)
            if ($leftGeometry.SegmentSha256 -eq $rightGeometry.SegmentSha256) {
                $mode = 'unordered-segments'; $passed = $true
            } else {
                $leftGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($leftGcode.FullName, $CoverageGridMm, $true)
                $rightGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($rightGcode.FullName, $CoverageGridMm, $true)
                $coverageDifferenceRatio = $leftGeometry.CoverageDifferenceRatio($rightGeometry)
                $lengthDifferenceRatio = if ($leftGeometry.TotalLength -gt 0) { [Math]::Abs($leftGeometry.TotalLength - $rightGeometry.TotalLength) / $leftGeometry.TotalLength } else { 0 }
                $extrusionDifferenceRatio = if ($leftGeometry.TotalExtrusion -gt 0) { [Math]::Abs($leftGeometry.TotalExtrusion - $rightGeometry.TotalExtrusion) / $leftGeometry.TotalExtrusion } else { 0 }
                if ($leftGeometry.CoverageSha256 -eq $rightGeometry.CoverageSha256 -and
                    $leftGeometry.MetricsSha256 -eq $rightGeometry.MetricsSha256) {
                    $mode = 'coverage-and-metrics'; $passed = $true
                } else {
                    $mode = 'geometry-mismatch'
                    $pattern = [string]$caseById[[string]$left.id].sparse_infill_pattern
                    $coverageLimit = if ($pattern -eq 'lightning') { 0.02 } else { 0.002 }
                    $metricLimit = if ($pattern -eq 'lightning') { 0.002 } else { 0.0001 }
                    foreach ($envelope in $cpuEnvelopeResults) {
                        if (-not $envelope.by_id.ContainsKey($left.id)) {
                            throw "CPU envelope is missing $($left.id): $($envelope.run)"
                        }
                        $envelopeItem = $envelope.by_id[$left.id]
                        if ($envelopeItem.settings_sha256 -ne $left.settings_sha256) {
                            throw "CPU envelope settings differ for $($left.id): $($envelope.run)"
                        }
                        $envelopeGcode = Get-ChildItem -LiteralPath (Join-Path $envelope.run $left.id) -File -Filter '*.gcode' | Select-Object -First 1
                        if ($null -eq $envelopeGcode) {
                            throw "CPU envelope G-code is missing for $($left.id): $($envelope.run)"
                        }
                        $envelopeGeometry = [Magpie.Verification.GcodeGeometryAnalyzer]::Analyze($envelopeGcode.FullName, $CoverageGridMm, $true)
                        $envelopeCoverage = $leftGeometry.CoverageDifferenceRatio($envelopeGeometry)
                        $envelopeLength = if ($leftGeometry.TotalLength -gt 0) { [Math]::Abs($leftGeometry.TotalLength - $envelopeGeometry.TotalLength) / $leftGeometry.TotalLength } else { 0 }
                        $envelopeExtrusion = if ($leftGeometry.TotalExtrusion -gt 0) { [Math]::Abs($leftGeometry.TotalExtrusion - $envelopeGeometry.TotalExtrusion) / $leftGeometry.TotalExtrusion } else { 0 }
                        $coverageLimit = [Math]::Max($coverageLimit, $envelopeCoverage * 1.01 + 1e-9)
                        $metricLimit = [Math]::Max($metricLimit, [Math]::Max($envelopeLength, $envelopeExtrusion) * 1.01 + 1e-9)
                    }
                    $coverageLimitApplied = $coverageLimit
                    $metricLimitApplied = $metricLimit
                    if ($coverageDifferenceRatio -le $coverageLimit -and
                        $lengthDifferenceRatio -le $metricLimit -and
                        $extrusionDifferenceRatio -le $metricLimit) {
                        $mode = 'cpu-nondeterminism-envelope'; $passed = $true
                    }
                }
            }
        }
    }
    $rows.Add([pscustomobject][ordered]@{
        id = $left.id
        passed = $passed
        comparison = $mode
        settings_equal = $left.settings_sha256 -eq $right.settings_sha256
        motion_equal = $left.motion_sha256 -eq $right.motion_sha256
        coverage_difference_ratio = $coverageDifferenceRatio
        length_difference_ratio = $lengthDifferenceRatio
        extrusion_difference_ratio = $extrusionDifferenceRatio
        candidate_dispatched = $candidateDispatched
        coverage_limit_applied = $coverageLimitApplied
        metric_limit_applied = $metricLimitApplied
        infill_pattern = [string]$caseById[[string]$left.id].sparse_infill_pattern
        reference_geometry = $leftGeometry
        candidate_geometry = $rightGeometry
    })
    if ($index % 20 -eq 0) { Write-Output "PROGRESS=$index/$($reference.Count)" }
}

$failed = @($rows | Where-Object { -not $_.passed })
$summary = [pscustomobject][ordered]@{
    schema = 1
    reference_run = $ReferenceRun
    candidate_run = $CandidateRun
    case_count = $rows.Count
    passed = $rows.Count - $failed.Count
    failed = $failed.Count
    exact_motion = @($rows | Where-Object comparison -eq 'exact-motion').Count
    unordered_segments = @($rows | Where-Object comparison -eq 'unordered-segments').Count
    coverage_and_metrics = @($rows | Where-Object comparison -eq 'coverage-and-metrics').Count
    cpu_nondeterminism_envelope = @($rows | Where-Object comparison -eq 'cpu-nondeterminism-envelope').Count
    cpu_fallback_no_vulkan_work = @($rows | Where-Object comparison -eq 'cpu-fallback-no-vulkan-work').Count
    geometry_mismatch = @($rows | Where-Object comparison -eq 'geometry-mismatch').Count
    rows = @($rows)
}
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $OutputPath -Encoding UTF8
Write-Output "RESULT=$OutputPath"
Write-Output "PASSED=$($summary.passed) FAILED=$($summary.failed) EXACT=$($summary.exact_motion) SEGMENTS=$($summary.unordered_segments) COVERAGE=$($summary.coverage_and_metrics)"
if ($failed.Count -gt 0) { exit 1 }
