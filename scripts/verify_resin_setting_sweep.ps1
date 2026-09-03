param(
    [string] $TestExecutable = (Join-Path $PSScriptRoot '..\build-vulkan\tests\fff_print\Release\fff_print_tests.exe'),
    [string] $OutputRoot = (Join-Path $PSScriptRoot '..\build\verification\resin-setting-sweep'),
    [int] $CaseTimeoutSeconds = 30,
    [long] $MemoryLimitBytes = 4GB
)

$ErrorActionPreference = 'Stop'
$TestExecutable = [IO.Path]::GetFullPath($TestExecutable)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if (-not (Test-Path -LiteralPath $TestExecutable -PathType Leaf)) {
    throw "Test executable not found: $TestExecutable"
}

$mutations = @(
    'head_front_diameter', 'head_width', 'pillar_diameter',
    'small_pillar_diameter_percent', 'max_bridges_on_pillar',
    'max_weight_on_model', 'pillar_connection_mode', 'buildplate_only',
    'pillar_widening_factor', 'base_diameter', 'base_height',
    'base_safety_distance', 'critical_angle', 'max_bridge_length',
    'max_pillar_link_distance', 'object_elevation'
)
$cases = [Collections.Generic.List[string]]::new()
foreach ($strategy in @('default', 'branching')) {
    $prefix = if ($strategy -eq 'branching') { 'resin_branching_support_' } else { 'resin_support_' }
    foreach ($mutation in $mutations) {
        $cases.Add("${strategy}:${prefix}${mutation}")
    }
}
$cases.Add('common:resin_support_points_density_relative')
$cases.Add('common:resin_support_enforcers_only')

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()

foreach ($case in $cases) {
    $safeName = $case -replace '[^A-Za-z0-9._-]', '_'
    $stdout = Join-Path $runRoot "$safeName.out.log"
    $stderr = Join-Path $runRoot "$safeName.err.log"
    $previousFilter = $env:MAGPIE_RESIN_SWEEP_FILTER
    $env:MAGPIE_RESIN_SWEEP_FILTER = $case
    try {
        $process = Start-Process -FilePath $TestExecutable `
            -ArgumentList @('[ResinSettingSweep]', '--reporter', 'console') `
            -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    } finally {
        $env:MAGPIE_RESIN_SWEEP_FILTER = $previousFilter
    }

    $deadline = (Get-Date).AddSeconds($CaseTimeoutSeconds)
    $timedOut = $false
    $memoryExceeded = $false
    $peakWorkingSet = 0L
    while (-not $process.HasExited) {
        $process.Refresh()
        $peakWorkingSet = [Math]::Max($peakWorkingSet, [long] $process.WorkingSet64)
        if ($peakWorkingSet -gt $MemoryLimitBytes) {
            $memoryExceeded = $true
            break
        }
        if ((Get-Date) -ge $deadline) {
            $timedOut = $true
            break
        }
        Start-Sleep -Milliseconds 250
    }
    if (-not $process.HasExited -and ($timedOut -or $memoryExceeded)) {
        $process.Kill()
        $process.WaitForExit()
    }
    elseif ($process.HasExited) {
        # Start-Process may report HasExited before the redirected streams and
        # ExitCode property are finalized. WaitForExit is still required here.
        $process.WaitForExit()
    }
    $process.Refresh()
    if ($timedOut -or $memoryExceeded) {
        $exitCode = -1
    } elseif ($null -ne $process.ExitCode) {
        $exitCode = [int] $process.ExitCode
    } else {
        # Some Windows PowerShell 5.1 hosts leave ExitCode unset for a process
        # started with both output streams redirected. In that case accept only
        # Catch2's unambiguous success footer and an empty stderr stream.
        $stdoutText = Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue
        $stderrText = Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue
        $exitCode = if ($stdoutText -match 'All tests passed' -and [string]::IsNullOrWhiteSpace($stderrText)) { 0 } else { 125 }
    }
    $results.Add([pscustomobject]@{
        Case = $case
        Passed = $exitCode -eq 0
        ExitCode = $exitCode
        TimedOut = $timedOut
        MemoryExceeded = $memoryExceeded
        PeakWorkingSetMB = [Math]::Round($peakWorkingSet / 1MB, 1)
        Stdout = $stdout
        Stderr = $stderr
    })
    Write-Output ("{0}: passed={1} timeout={2} memoryExceeded={3} peakMB={4}" -f `
        $case, ($exitCode -eq 0), $timedOut, $memoryExceeded, [Math]::Round($peakWorkingSet / 1MB, 1))
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
