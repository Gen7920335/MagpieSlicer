param(
    [string] $OutputRoot,
    [string] $ResumeFrom,
    [int] $WorkerCount = 12,
    [int] $CaseTimeoutSeconds = 600,
    [int] $RunTimeoutSeconds = 600
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\small-nozzle-90'
}
if ($WorkerCount -lt 1 -or $WorkerCount -gt 32) { throw "Invalid worker count: $WorkerCount" }

$sessionRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $sessionRoot | Out-Null
$verifier = Join-Path $PSScriptRoot 'verify_small_nozzle_geometry.ps1'
$workers = [Collections.Generic.List[object]]::new()
$previousByCase = @{}
$pendingNames = @()

if (-not [string]::IsNullOrWhiteSpace($ResumeFrom)) {
    $resumeRoots = @($ResumeFrom.Split(';') | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
    $allNames = @()
    foreach ($resumeRootValue in $resumeRoots) {
        $resumeRoot = [IO.Path]::GetFullPath($resumeRootValue)
        if (-not (Test-Path -LiteralPath $resumeRoot -PathType Container)) {
            throw "Resume session not found: $resumeRoot"
        }
        foreach ($resultFile in Get-ChildItem -LiteralPath $resumeRoot -Recurse -File -Filter 'result.json') {
            try {
                $result = Get-Content -LiteralPath $resultFile.FullName -Raw | ConvertFrom-Json
                $previousByCase[$result.Case] = $result
            } catch {
                Write-Warning "Ignoring unreadable result: $($resultFile.FullName)"
            }
        }
        $allNames += @(
            Get-ChildItem -LiteralPath $resumeRoot -Recurse -File -Filter 'case-plan.json' |
                ForEach-Object { (Get-Content -LiteralPath $_.FullName -Raw | ConvertFrom-Json).Cases.Name }
        )
    }
    $allNames = @($allNames | Sort-Object -Unique)
    if ($allNames.Count -ne 90) { throw "Resume session does not contain a 90-case plan: $($allNames.Count)" }
    $pendingNames = @($allNames | Where-Object {
        -not $previousByCase.ContainsKey($_) -or -not $previousByCase[$_].Passed
    })
    if ($pendingNames.Count -eq 0) {
        Write-Output 'No pending or failed cases to resume'
        exit 0
    }
    $WorkerCount = [Math]::Min($WorkerCount, $pendingNames.Count)
}

for ($index = 0; $index -lt $WorkerCount; ++$index) {
    $workerRoot = Join-Path $sessionRoot ("shard_{0:D2}" -f $index)
    New-Item -ItemType Directory -Force -Path $workerRoot | Out-Null
    $stdout = Join-Path $workerRoot 'worker.out.log'
    $stderr = Join-Path $workerRoot 'worker.err.log'
    $arguments = [Collections.Generic.List[string]]@(
        '-NoProfile',
        '-ExecutionPolicy', 'Bypass',
        '-File', ('"{0}"' -f $verifier),
        '-Mode', 'Full',
        '-IncludePairwise',
        '-SliceTimeoutSeconds', [string] $CaseTimeoutSeconds,
        '-OutputRoot', ('"{0}"' -f $workerRoot)
    )
    if ([string]::IsNullOrWhiteSpace($ResumeFrom)) {
        $arguments.Add('-ShardIndex')
        $arguments.Add([string] $index)
        $arguments.Add('-ShardCount')
        $arguments.Add([string] $WorkerCount)
    } else {
        $workerNames = @(
            for ($caseIndex = 0; $caseIndex -lt $pendingNames.Count; ++$caseIndex) {
                if (($caseIndex % $WorkerCount) -eq $index) { $pendingNames[$caseIndex] }
            }
        )
        $caseNamesPath = Join-Path $workerRoot 'case-names.txt'
        $workerNames | Set-Content -LiteralPath $caseNamesPath -Encoding ASCII
        $arguments.Add('-CaseNamesPath')
        $arguments.Add(('"{0}"' -f $caseNamesPath))
    }
    $process = Start-Process -FilePath 'powershell.exe' -ArgumentList $arguments -PassThru -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $workers.Add([pscustomobject]@{
        Index = $index
        Process = $process
        Root = $workerRoot
        Stdout = $stdout
        Stderr = $stderr
    })
}

$timer = [Diagnostics.Stopwatch]::StartNew()
$lastReport = -30
while (@($workers | Where-Object { -not $_.Process.HasExited }).Count -gt 0) {
    if ($timer.Elapsed.TotalSeconds -ge $RunTimeoutSeconds) {
        foreach ($worker in $workers | Where-Object { -not $_.Process.HasExited }) {
            $worker.Process.Kill()
            $worker.Process.WaitForExit()
        }
        throw "The 90-case run exceeded $RunTimeoutSeconds seconds"
    }
    if ($timer.Elapsed.TotalSeconds - $lastReport -ge 30) {
        $completedWorkers = @($workers | Where-Object { $_.Process.HasExited }).Count
        $completedCases = 0
        foreach ($worker in $workers) {
            $resultFiles = @(Get-ChildItem -LiteralPath $worker.Root -Recurse -File -Filter 'result.json' -ErrorAction SilentlyContinue)
            $completedCases += $resultFiles.Count
        }
        Write-Output ("PROGRESS elapsed={0}s workers={1}/{2} cases={3}/90" -f
            [int] $timer.Elapsed.TotalSeconds, $completedWorkers, $WorkerCount, $completedCases)
        $lastReport = $timer.Elapsed.TotalSeconds
    }
    Start-Sleep -Seconds 5
    foreach ($worker in $workers) { $worker.Process.Refresh() }
}
$timer.Stop()

$aggregateByCase = @{}
foreach ($entry in $previousByCase.GetEnumerator()) {
    if ($entry.Value.Passed) { $aggregateByCase[$entry.Key] = $entry.Value }
}
$workerErrors = [Collections.Generic.List[string]]::new()
foreach ($worker in $workers) {
    $worker.Process.Refresh()
    $workerResults = @(
        Get-ChildItem -LiteralPath $worker.Root -Recurse -File -Filter 'result.json' -ErrorAction SilentlyContinue
    )
    $workerExitCode = $worker.Process.ExitCode
    if ($null -eq $workerExitCode) { $workerExitCode = if ($workerResults.Count -gt 0) { 0 } else { 1 } }
    if ($workerExitCode -ne 0) {
        $tail = if (Test-Path -LiteralPath $worker.Stderr) {
            (Get-Content -LiteralPath $worker.Stderr -Tail 8) -join ' | '
        } else { 'no stderr log' }
        $workerErrors.Add("Shard $($worker.Index) exit ${workerExitCode}: $tail")
    }
    if ($workerResults.Count -eq 0) {
        $workerErrors.Add("Shard $($worker.Index) produced no case result files")
        continue
    }
    foreach ($resultFile in $workerResults) {
        $result = Get-Content -LiteralPath $resultFile.FullName -Raw | ConvertFrom-Json
        $aggregateByCase[[string] $result.Case] = $result
    }
}

$aggregate = @($aggregateByCase.Values)
$duplicates = @($aggregate | Group-Object Case | Where-Object Count -ne 1)
if ($aggregate.Count -ne 90) { $workerErrors.Add("Expected 90 results, found $($aggregate.Count)") }
if ($duplicates.Count -gt 0) { $workerErrors.Add("Duplicate cases: $($duplicates.Name -join ', ')") }
$failed = @($aggregate | Where-Object { -not $_.Passed })

$aggregatePath = Join-Path $sessionRoot 'aggregate-results.json'
$summaryPath = Join-Path $sessionRoot 'aggregate-summary.csv'
$runInfoPath = Join-Path $sessionRoot 'run-info.json'
$aggregate | Sort-Object Case | ConvertTo-Json -Depth 30 | Set-Content -LiteralPath $aggregatePath -Encoding UTF8
$aggregate | Sort-Object Case |
    Select-Object Case,Factor,Level,Generator,Nozzle2,Enabled,Count,Interlock,WallLoops,Overlap,
        SliceSeconds,AnalysisSeconds,DurationSeconds,Passed,@{N='Errors';E={$_.Errors -join '; '}} |
    Export-Csv -LiteralPath $summaryPath -NoTypeInformation -Encoding UTF8
[pscustomobject]@{
    WorkerCount = $WorkerCount
    DurationSeconds = [Math]::Round($timer.Elapsed.TotalSeconds, 2)
    ResultCount = $aggregate.Count
    Passed = $aggregate.Count - $failed.Count
    Failed = $failed.Count
    WorkerErrors = $workerErrors.ToArray()
} | ConvertTo-Json -Depth 10 | Set-Content -LiteralPath $runInfoPath -Encoding UTF8

Write-Output "SESSION=$sessionRoot"
Write-Output "RESULTS=$aggregatePath"
Write-Output "SUMMARY=$summaryPath"
Write-Output "DURATION_SECONDS=$([Math]::Round($timer.Elapsed.TotalSeconds,2))"
Write-Output "PASSED=$($aggregate.Count - $failed.Count) FAILED=$($failed.Count)"
foreach ($failure in $failed) {
    Write-Output "FAIL_CASE=$($failure.Case): $($failure.Errors -join '; ')"
}
foreach ($workerError in $workerErrors) { Write-Output "WORKER_ERROR=$workerError" }
if ($failed.Count -gt 0 -or $workerErrors.Count -gt 0) { exit 1 }
