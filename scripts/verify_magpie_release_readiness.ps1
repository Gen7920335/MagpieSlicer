param(
    [string] $RepoRoot = (Split-Path -Parent $PSScriptRoot),
    [string] $SlicerPath = "",
    [ValidateSet('Core', 'Full')]
    [string] $Mode = 'Core',
    [int] $StepTimeoutSeconds = 600,
    [string] $OutputRoot = "",
    [string] $TestBuildDirectory = ""
)

$ErrorActionPreference = 'Stop'
Set-StrictMode -Version Latest

$RepoRoot = [IO.Path]::GetFullPath($RepoRoot)
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot ('build\verification\release-readiness-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

$stepRunner = Join-Path $OutputRoot 'invoke-step.ps1'
@'
param(
    [Parameter(Mandatory=$true)][string] $SpecPath,
    [Parameter(Mandatory=$true)][string] $ExitCodePath
)
$ErrorActionPreference = 'Continue'
$spec = Get-Content -LiteralPath $SpecPath -Raw | ConvertFrom-Json
$arguments = @($spec.Arguments | ForEach-Object { [string] $_ })
& ([string] $spec.FilePath) @arguments
$code = if ($null -eq $LASTEXITCODE) { 0 } else { [int] $LASTEXITCODE }
[IO.File]::WriteAllText($ExitCodePath, [string] $code)
exit $code
'@ | Set-Content -LiteralPath $stepRunner -Encoding UTF8

function Assert-File([string] $Path) {
    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file not found: $Path"
    }
}

function Invoke-CheckedStep {
    param(
        [string] $Name,
        [string] $FilePath,
        [string[]] $Arguments,
        [string] $RequiredOutput = '',
        [int] $TimeoutSeconds = 0
    )

    $stdout = Join-Path $OutputRoot ($Name + '.stdout.log')
    $stderr = Join-Path $OutputRoot ($Name + '.stderr.log')
    $specPath = Join-Path $OutputRoot ($Name + '.command.json')
    $exitCodePath = Join-Path $OutputRoot ($Name + '.exitcode')
    @{ FilePath=$FilePath; Arguments=@($Arguments) } |
        ConvertTo-Json -Depth 4 |
        Set-Content -LiteralPath $specPath -Encoding UTF8
    $startInfo = @{
        FilePath = 'powershell.exe'
        ArgumentList = @('-NoProfile','-ExecutionPolicy','Bypass','-File',$stepRunner,'-SpecPath',$specPath,'-ExitCodePath',$exitCodePath)
        WorkingDirectory = $RepoRoot
        PassThru = $true
        NoNewWindow = $true
        RedirectStandardOutput = $stdout
        RedirectStandardError = $stderr
    }
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $process = Start-Process @startInfo
    $effectiveTimeoutSeconds = if ($TimeoutSeconds -gt 0) { $TimeoutSeconds } else { $StepTimeoutSeconds }
    if (-not $process.WaitForExit($effectiveTimeoutSeconds * 1000)) {
        & taskkill.exe /PID $process.Id /T /F 2>$null | Out-Null
        return [pscustomobject]@{ Name=$Name; Passed=$false; ExitCode=124; Seconds=[math]::Round($timer.Elapsed.TotalSeconds, 1); Detail='timeout' }
    }
    $combined = ((Get-Content -LiteralPath $stdout -Raw -ErrorAction SilentlyContinue) + "`n" +
                 (Get-Content -LiteralPath $stderr -Raw -ErrorAction SilentlyContinue))
    $exitCode = if (Test-Path -LiteralPath $exitCodePath) {
        [int](Get-Content -LiteralPath $exitCodePath -Raw)
    } else {
        125
    }
    $passed = $exitCode -eq 0
    if ($passed -and -not [string]::IsNullOrWhiteSpace($RequiredOutput)) {
        $passed = $combined -match $RequiredOutput
    }
    $detail = if ($passed) { 'passed' } elseif ($exitCode -ne 0) { "exit $exitCode" } else { 'required output missing' }
    [pscustomobject]@{ Name=$Name; Passed=$passed; ExitCode=$exitCode; Seconds=[math]::Round($timer.Elapsed.TotalSeconds, 1); Detail=$detail }
}

function Test-RuntimeLocalizationCatalog {
    $catalogPath = Join-Path $RepoRoot 'resources\i18n\ko\OrcaSlicer.mo'
    # Keep this Windows PowerShell 5.1 script ASCII-only: a UTF-8 file without
    # BOM is otherwise decoded through the active ANSI code page.
    $koNormalWalls = -join ([char[]] @(0xC77C,0xBC18,0x20,0xC11C,0xD3EC,0xD2B8,0x20,0xBCBD,0x20,0xB8E8,0xD504))
    $koTreeWalls = -join ([char[]] @(0xD2B8,0xB9AC,0x20,0xC11C,0xD3EC,0xD2B8,0x20,0xBCBD,0x20,0xB8E8,0xD504))
    $koCuraJoin = 'Cura ' + (-join ([char[]] @(0xC11C,0xD3EC,0xD2B8,0x20,0xC5F0,0xACB0,0x20,0xAC70,0xB9AC)))
    $requiredStrings = @(
        'Normal support wall loops', $koNormalWalls,
        'Tree support wall loops', $koTreeWalls,
        'Cura support join distance', $koCuraJoin
    )
    if (-not (Test-Path -LiteralPath $catalogPath -PathType Leaf)) {
        return [pscustomobject]@{ Name='korean-runtime-catalog'; Passed=$false; ExitCode=2; Seconds=0; Detail='runtime MO missing' }
    }
    $catalog = [IO.File]::ReadAllText($catalogPath, [Text.Encoding]::UTF8)
    $missing = @($requiredStrings | Where-Object { -not $catalog.Contains($_) })
    [pscustomobject]@{
        Name = 'korean-runtime-catalog'
        Passed = $missing.Count -eq 0
        ExitCode = if ($missing.Count -eq 0) { 0 } else { 1 }
        Seconds = 0
        Detail = if ($missing.Count -eq 0) { 'passed' } else { 'missing: ' + ($missing -join ', ') }
    }
}

Assert-File $SlicerPath
$expectedName = 'magpie-slicer.exe'
if ([IO.Path]::GetFileName($SlicerPath) -ne $expectedName) {
    throw "Release verification requires $expectedName, got $([IO.Path]::GetFileName($SlicerPath))"
}

if ([string]::IsNullOrWhiteSpace($TestBuildDirectory)) {
    $slicerBuildRoot = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $SlicerPath))
    $matchingTestRoot = Join-Path $slicerBuildRoot 'tests'
    $TestBuildDirectory = if (Test-Path -LiteralPath $matchingTestRoot -PathType Container) {
        $matchingTestRoot
    } else {
        Join-Path $RepoRoot 'build\tests'
    }
}
$testRoot = [IO.Path]::GetFullPath($TestBuildDirectory)
$jobs = @(
    @{ Name='preset-roundtrip'; File=(Join-Path $testRoot 'libslic3r\Release\libslic3r_tests.exe'); Args=@('[Preset][Roundtrip],[Preset][Project][MultiNozzle]', '--reporter', 'compact'); Required='All tests passed' },
    @{ Name='multinozzle-config'; File=(Join-Path $testRoot 'fff_print\Release\fff_print_tests.exe'); Args=@('[MultiFilament][Config],[Flow][MultiNozzleWalls]', '--reporter', 'compact'); Required='All tests passed' },
    @{ Name='multinozzle-toolpaths'; File=(Join-Path $testRoot 'fff_print\Release\fff_print_tests.exe'); Args=@('[MultiFilament][MultiNozzleWalls]', '--reporter', 'compact'); Required='All tests passed' },
    @{ Name='support-unit'; File=(Join-Path $testRoot 'fff_print\Release\fff_print_tests.exe'); Args=@('[SupportMaterial]~[MixedBunnyWorstCase]', '--reporter', 'compact'); Required='assertions:\s+\d+.*passed' },
    @{ Name='mixed-worst-case'; File=(Join-Path $testRoot 'fff_print\Release\fff_print_tests.exe'); Args=@('[MixedBunnyWorstCase]', '--reporter', 'compact'); Required='All tests passed'; Timeout=180 },
    @{ Name='mixed-support'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_mixed_support.ps1'),'-SlicerPath',$SlicerPath,'-SupportTestPath',(Join-Path $testRoot 'fff_print\Release\fff_print_tests.exe')); Required='FAILED=0' },
    @{ Name='snapmaker-unit'; File=(Join-Path $testRoot 'slic3rutils\Release\slic3rutils_tests.exe'); Args=@('[SnapmakerMonitor]', '--reporter', 'compact'); Required='All tests passed' },
    @{ Name='small-nozzle-core'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_small_nozzle_geometry.ps1'),'-Mode','Core','-SlicerPath',$SlicerPath,'-NativeAnalyzerPath',(Join-Path (Split-Path -Parent (Split-Path -Parent $SlicerPath)) 'dev-utils\Release\small-nozzle-gcode-verifier.exe')); Required='FAILED=0' },
    @{ Name='support-features'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_support_features.ps1'),'-SlicerPath',$SlicerPath); Required='FAILED=0' },
    @{ Name='cura-regressions'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_cura_port_regressions.ps1'),'-SlicerPath',$SlicerPath); Required='verify_support_features.ps1' },
    @{ Name='tree-wall-counts'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_tree_support_wall_counts.ps1'),'-SlicerPath',$SlicerPath); Required='FAILED=0' }
)

if ($Mode -eq 'Full') {
    $jobs += @(
        @{ Name='small-nozzle-full'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_small_nozzle_geometry.ps1'),'-Mode','Full','-SlicerPath',$SlicerPath,'-NativeAnalyzerPath',(Join-Path (Split-Path -Parent (Split-Path -Parent $SlicerPath)) 'dev-utils\Release\small-nozzle-gcode-verifier.exe')); Required='FAILED=0' },
        @{ Name='small-nozzle-speed-classic'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_small_nozzle_wall_speed.ps1'),'-WallGenerator','classic','-SlicerPath',$SlicerPath); Required='FAILED=0' },
        @{ Name='small-nozzle-speed-arachne'; File='powershell.exe'; Args=@('-NoProfile','-ExecutionPolicy','Bypass','-File',(Join-Path $RepoRoot 'scripts\verify_small_nozzle_wall_speed.ps1'),'-WallGenerator','arachne','-SlicerPath',$SlicerPath); Required='FAILED=0' }
    )
}

foreach ($job in $jobs) {
    if ($job.File -ne 'powershell.exe') { Assert-File $job.File }
}

$results = @(Test-RuntimeLocalizationCatalog)
$results += foreach ($job in $jobs) {
    $timeout = if ($job.ContainsKey('Timeout')) { [int] $job.Timeout } else { 0 }
    Invoke-CheckedStep -Name $job.Name -FilePath $job.File -Arguments $job.Args -RequiredOutput $job.Required -TimeoutSeconds $timeout
}
$results | Export-Csv -LiteralPath (Join-Path $OutputRoot 'summary.csv') -NoTypeInformation -Encoding UTF8
$results | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $OutputRoot 'summary.json') -Encoding UTF8
$results | Format-Table -AutoSize

$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$OutputRoot"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
