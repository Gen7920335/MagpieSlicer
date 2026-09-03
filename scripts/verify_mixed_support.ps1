param(
    [string] $SlicerPath,
    [string] $SupportTestPath,
    [string] $OutputRoot,
    [string] $ModelPath,
    [ValidateRange(0, 90)]
    [int] $SupportAngle = 60,
    [ValidateRange(0, 100)]
    [int] $CoverageThreshold = 80,
    [ValidateSet(0, 1)]
    [int] $SelectiveMerge = 1,
    [int] $SliceTimeoutSeconds = 240
)

$ErrorActionPreference = 'Stop'
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build-vulkan\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot ('build\verification\mixed-support-' + (Get-Date -Format 'yyyyMMdd-HHmmss'))
}
$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'

foreach ($required in @($SlicerPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file not found: $required"
    }
}

if ([string]::IsNullOrWhiteSpace($SupportTestPath)) {
    $SupportTestPath = @(
        (Join-Path $RepoRoot 'build-vulkan\tests\fff_print\Release\fff_print_tests.exe'),
        (Join-Path $RepoRoot 'build\tests\fff_print\Release\fff_print_tests.exe')
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf } | Select-Object -First 1
}
if ([string]::IsNullOrWhiteSpace($SupportTestPath) -or
    -not (Test-Path -LiteralPath $SupportTestPath -PathType Leaf)) {
    throw 'A current fff_print_tests.exe is required for independent Mixed channel and wall-loop verification.'
}
$SupportTestPath = [IO.Path]::GetFullPath($SupportTestPath)
$nativeOutput = & $SupportTestPath '[MixedIndependent]' --reporter compact 2>&1
$nativeExitCode = $LASTEXITCODE
$nativeOutput | ForEach-Object { Write-Output $_ }
if ($nativeExitCode -ne 0) {
    throw "Independent Mixed channel/wall-loop tests failed with exit code $nativeExitCode"
}
Write-Output 'NATIVE_MIXED_CHANNEL_AND_WALL_TESTS=PASSED'

function Find-FilamentProfile([string] $name) {
    foreach ($profileRoot in @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))) {
        if (-not (Test-Path -LiteralPath $profileRoot -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $profileRoot -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Set-JsonProperty($object, [string] $property, $value) {
    if ($null -eq $object.PSObject.Properties[$property]) {
        $object | Add-Member -NotePropertyName $property -NotePropertyValue $value
    } else {
        $object.$property = $value
    }
}

$cases = @(
    [pscustomobject]@{ Name='overhang_prusa_organic'; Model='tests\data\overhang.obj'; Normal='prusa'; Tree='organic' },
    [pscustomobject]@{ Name='frog_prusa_hybrid'; Model='tests\data\frog_legs.obj'; Normal='prusa'; Tree='tree_hybrid' },
    [pscustomobject]@{ Name='ipadstand_cura_strong'; Model='tests\data\ipadstand.obj'; Normal='cura'; Tree='tree_strong' },
    [pscustomobject]@{ Name='bunny_cura_organic'; Model='resources\handy_models\Stanford_Bunny.drc'; Normal='cura'; Tree='organic' }
)
$filaments = @('Snapmaker PLA @U1','Snapmaker ABS @U1','Snapmaker PETG @U1','Snapmaker TPU @U1') |
    ForEach-Object { Find-FilamentProfile $_ }
$results = [Collections.Generic.List[object]]::new()
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null

foreach ($case in $cases) {
    $modelPath = if ([string]::IsNullOrWhiteSpace($ModelPath)) {
        Join-Path $RepoRoot $case.Model
    } else {
        [IO.Path]::GetFullPath($ModelPath)
    }
    if (-not (Test-Path -LiteralPath $modelPath -PathType Leaf)) { throw "Model not found: $modelPath" }
    $caseRoot = Join-Path $OutputRoot $case.Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
    $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
    $identity = "Magpie Mixed verify $($case.Name)"
    $machine.name = $identity
    $machine.setting_id = "mixed-$($case.Name)"
    $process.name = $identity
    $process.setting_id = "mixed-$($case.Name)"
    $process.compatible_printers = @($identity)
    Set-JsonProperty $process 'enable_support' '1'
    Set-JsonProperty $process 'support_type' 'mixed(auto)'
    Set-JsonProperty $process 'mixed_normal_support_generator' $case.Normal
    Set-JsonProperty $process 'mixed_tree_support_style' $case.Tree
    Set-JsonProperty $process 'mixed_normal_coverage_threshold' "$CoverageThreshold%"
    Set-JsonProperty $process 'mixed_selective_merge' ([string] $SelectiveMerge)
    Set-JsonProperty $process 'support_threshold_angle' ([string] $SupportAngle)
    Set-JsonProperty $process 'support_interface_top_layers' '2'
    Set-JsonProperty $process 'support_interface_bottom_layers' '0'
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '0'
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $quotedSettings = [char]34 + "$machinePath;$processPath" + [char]34
    $quotedFilaments = [char]34 + ($filaments -join ';') + [char]34
    $arguments = @('--slice','0','--debug','1','--load-settings',$quotedSettings,'--load-filaments',$quotedFilaments,
        '--outputdir',([char]34 + $caseRoot + [char]34),([char]34 + $modelPath + [char]34))
    $stdout = Join-Path $caseRoot 'cli.out.log'
    $stderr = Join-Path $caseRoot 'cli.err.log'
    $processHandle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $processHandle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $processHandle.Kill()
        throw "Slice timeout: $($case.Name)"
    }
    $processHandle.Refresh()
    $gcodePath = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' |
        Select-Object -First 1 -ExpandProperty FullName
    $errors = [Collections.Generic.List[string]]::new()
    $exitCode = $processHandle.ExitCode
    if ($null -eq $exitCode) { $exitCode = if ($gcodePath) { 0 } else { 1 } }
    if ($exitCode -ne 0) { $errors.Add("CLI exit code $exitCode") }
    if ([string]::IsNullOrWhiteSpace($gcodePath)) {
        $errors.Add('G-code was not generated')
        $gcode = ''
    } else {
        $gcode = Get-Content -LiteralPath $gcodePath -Raw
    }
    if ($gcode -notmatch '(?m)^; support_type = mixed\(auto\)$') { $errors.Add('Mixed support type was not embedded') }
    if ($gcode -notmatch "(?m)^; mixed_normal_coverage_threshold = $CoverageThreshold%$") {
        $errors.Add('Coverage threshold was not embedded')
    }
    if ($gcode -notmatch "(?m)^; mixed_selective_merge = $SelectiveMerge$") {
        $errors.Add('Selective merge was not embedded')
    }
    if ($gcode -notmatch "(?m)^; support_threshold_angle = $SupportAngle$") { $errors.Add('Support angle was not embedded') }
    if ($gcode -notmatch '(?im)(?:^;TYPE:Support|; support material)') { $errors.Add('Support extrusion was not generated') }
    if ($gcode -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') {
        $errors.Add('Non-finite G-code value was generated')
    }
    $results.Add([pscustomobject]@{
        Case = $case.Name
        Normal = $case.Normal
        Tree = $case.Tree
        CoverageThreshold = $CoverageThreshold
        SelectiveMerge = $SelectiveMerge
        GcodeBytes = if ($gcodePath) { (Get-Item -LiteralPath $gcodePath).Length } else { 0 }
        Passed = $errors.Count -eq 0
        Errors = $errors -join '; '
        Gcode = $gcodePath
    })
}

$resultsPath = Join-Path $OutputRoot 'results.json'
$results | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Format-Table Case,Normal,Tree,GcodeBytes,Passed,Errors -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
