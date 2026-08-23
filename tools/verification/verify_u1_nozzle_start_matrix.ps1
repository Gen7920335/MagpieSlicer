param(
    [string] $RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)

$ErrorActionPreference = "Stop"
$ProgressPreference = "SilentlyContinue"

function Set-ConfigValue {
    param(
        [pscustomobject] $Config,
        [string] $Name,
        [object] $Value
    )

    $property = $Config.PSObject.Properties[$Name]
    if ($null -eq $property) {
        $Config | Add-Member -NotePropertyName $Name -NotePropertyValue $Value
    } else {
        $property.Value = $Value
    }
}

function Save-Config {
    param(
        [pscustomobject] $Config,
        [string] $Path
    )

    [IO.File]::WriteAllText(
        $Path,
        ($Config | ConvertTo-Json -Depth 50),
        [Text.UTF8Encoding]::new($false))
}

function Test-SerializedNumber {
    param(
        [string] $Text,
        [string] $Name,
        [double] $Expected
    )

    $pattern = "(?m)^;\s*" + [regex]::Escape($Name) + "\s*=\s*([-+]?\d+(?:\.\d+)?)\s*$"
    $match = [regex]::Match($Text, $pattern)
    if (-not $match.Success) {
        return $false
    }

    $actual = [double]::Parse(
        $match.Groups[1].Value,
        [Globalization.NumberStyles]::Float,
        [Globalization.CultureInfo]::InvariantCulture)
    # Layer-height values are millimetres. Ignore JSON/G-code formatting differences only.
    return [math]::Abs($actual - $Expected) -le 1e-9
}

function Get-CaseLayerHeight {
    param(
        [pscustomobject] $Variant,
        [pscustomobject] $Case
    )

    switch ($Case.HeightMode) {
        "min" { return [double] $Variant.MinLayers[0] }
        "max" { return [double] $Variant.MaxLayers[0] }
        default { return [double] $Variant.LayerHeight }
    }
}

function Get-CaseInitialLayerHeight {
    param(
        [pscustomobject] $Variant,
        [pscustomobject] $Case
    )

    return [double] $Variant.InitialLayerHeight
}

function Invoke-MagpieSlice {
    param(
        [string] $Executable,
        [string] $Machine,
        [string] $Process,
        [string] $Filament,
        [string] $Model,
        [string] $OutputDirectory
    )

    New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null
    $stdout = Join-Path $OutputDirectory "stdout.log"
    $stderr = Join-Path $OutputDirectory "stderr.log"
    $arguments = @(
        "--load-settings", "$Machine;$Process",
        "--load-filaments", $Filament,
        "--load-filament-ids", "1",
        "--slice", "0",
        "--outputdir", $OutputDirectory,
        $Model
    )
    $timer = [Diagnostics.Stopwatch]::StartNew()
    $processHandle = Start-Process -FilePath $Executable -ArgumentList $arguments `
        -PassThru -Wait -NoNewWindow `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    $timer.Stop()
    $gcode = Get-ChildItem -LiteralPath $OutputDirectory -File -Filter "*.gcode" |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    return [pscustomobject]@{
        ExitCode = $processHandle.ExitCode
        Seconds = [math]::Round($timer.Elapsed.TotalSeconds, 3)
        GCode = if ($null -ne $gcode) { $gcode.FullName } else { $null }
        Stdout = $stdout
        Stderr = $stderr
    }
}

function Test-GCode {
    param(
        [string] $Path,
        [pscustomobject] $Variant,
        [pscustomobject] $Case
    )

    if (-not $Path -or -not (Test-Path -LiteralPath $Path)) {
        return [pscustomobject]@{ Passed = $false; Error = "No G-code generated" }
    }

    $text = [IO.File]::ReadAllText($Path)
    $firstLayerChange = $text.IndexOf(";LAYER_CHANGE", [StringComparison]::Ordinal)
    $preamble = if ($firstLayerChange -ge 0) { $text.Substring(0, $firstLayerChange) } else { $text }
    $requiredInOrder = @(
        "PRINT_START",
        "G28 X Y",
        "DEFECT_DETECT_NOODLE_FIRST",
        "G28 Z I140 J140",
        "M190 S$($Case.BedTemp)",
        "G28 Z",
        "BED_MESH_CALIBRATE PROBE_COUNT=11,11",
        "G0 X85 Y1 Z2",
        "M109 S$($Case.NozzleTemp)",
        "G1 X185 E15"
    )
    $cursor = 0
    $orderOk = $true
    foreach ($needle in $requiredInOrder) {
        $next = $preamble.IndexOf($needle, $cursor, [StringComparison]::Ordinal)
        if ($next -lt 0) {
            $orderOk = $false
            break
        }
        $cursor = $next + $needle.Length
    }

    $nozzleCsv = $Variant.Nozzles -join ","
    $hasHighTempOffset = $preamble.Contains("G28 Z Z_OFFSET -0.07") -and
        $preamble.Contains("BED_MESH_CALIBRATE PROBE_COUNT=11,11 Z_OFFSET=-0.07")
    $expectsHighTempOffset = $Case.BedType -eq "High Temp Plate"
    $brimMarkers = ([regex]::Matches($text, "(?im)^;\s*(TYPE|FEATURE):\s*.*brim")).Count
    $firstSupport = $text.IndexOf(";TYPE:Support", [StringComparison]::Ordinal)
    $firstWall = $text.IndexOf(";TYPE:Outer wall", [StringComparison]::Ordinal)
    $raftMarker = $firstSupport -ge 0 -and $firstWall -ge 0 -and $firstSupport -lt $firstWall
    $expectsBrim = [int] $Case.RaftLayers -eq 0 -and
        $Case.BrimType -ne "no_brim" -and [double] $Case.BrimWidth -gt 0
    $expectsRaft = [int] $Case.RaftLayers -gt 0
    $expectedLayerHeight = Get-CaseLayerHeight -Variant $Variant -Case $Case
    $expectedInitialLayerHeight = Get-CaseInitialLayerHeight -Variant $Variant -Case $Case

    $errors = @()
    if (-not $orderOk) { $errors += "start order/required command mismatch" }
    if ($preamble.Contains("PRINT_START  TOOL_TEMP=") -or $preamble.Contains("G0 X0 Y0 Z10")) {
        $errors += "legacy toolchanger start remains in executable preamble"
    }
    if (-not $preamble.Contains("M140 S$($Case.BedTemp)")) { $errors += "initial bed temperature mismatch" }
    if ($hasHighTempOffset -ne $expectsHighTempOffset) { $errors += "bed-type Z offset mismatch" }
    if (-not $text.Contains("; nozzle_diameter = $nozzleCsv")) { $errors += "serialized nozzle diameter mismatch" }
    if (-not $text.Contains("; printer_variant = $($Variant.Id)")) { $errors += "serialized printer variant mismatch" }
    if (-not (Test-SerializedNumber -Text $text -Name "layer_height" -Expected $expectedLayerHeight)) {
        $errors += "serialized layer height mismatch"
    }
    if (-not (Test-SerializedNumber -Text $text -Name "initial_layer_print_height" -Expected $expectedInitialLayerHeight)) {
        $errors += "serialized initial layer height mismatch"
    }
    if (-not (Test-SerializedNumber -Text $text -Name "line_width" -Expected ([double] $Variant.LineWidth))) {
        $errors += "serialized line width mismatch"
    }
    if (-not (Test-SerializedNumber -Text $text -Name "initial_layer_line_width" -Expected ([double] $Variant.InitialLineWidth))) {
        $errors += "serialized initial layer line width mismatch"
    }
    if ($null -ne $Variant.PSObject.Properties["TreeTipDiameter"] -and
        -not (Test-SerializedNumber -Text $text -Name "tree_support_tip_diameter" -Expected ([double] $Variant.TreeTipDiameter))) {
        $errors += "serialized tree support tip diameter mismatch"
    }
    if ($expectsBrim -and $brimMarkers -eq 0) { $errors += "brim requested but no brim marker" }
    if (-not $expectsBrim -and $brimMarkers -gt 0) { $errors += "brim disabled but brim marker exists" }
    if ($expectsRaft -ne $raftMarker) { $errors += "raft presence mismatch" }
    if (-not $text.Contains("CONFIG_BLOCK_END")) { $errors += "G-code is incomplete" }

    return [pscustomobject]@{
        Passed = $errors.Count -eq 0
        Error = $errors -join "; "
        BrimMarkers = $brimMarkers
        RaftMarker = $raftMarker
    }
}

$runner = Join-Path $RepoRoot "build-vulkan\src\Release\magpie-core-cli.exe"
$releaseDll = Join-Path $RepoRoot "build-vulkan\src\Release\MagpieSlicer.dll"
$baselineRoot = Join-Path $RepoRoot "build\verification\snapmaker-1to1-with-filament\20260731-080033"
$machineBaseline = Join-Path $baselineRoot "machine.json"
$processBaseline = Join-Path $baselineRoot "process.json"
$filamentBaseline = Join-Path $baselineRoot "filament.json"
$model = Join-Path $RepoRoot "tools\verification\assets\u1_adhesion_cube.stl"
$profileRoot = Join-Path $RepoRoot "resources\profiles\Snapmaker\machine"
$commonProfile = Get-Content -Raw -LiteralPath (Join-Path $profileRoot "fdm_U1.json") | ConvertFrom-Json

foreach ($required in @($runner, $releaseDll, $machineBaseline, $processBaseline, $filamentBaseline, $model)) {
    if (-not (Test-Path -LiteralPath $required -PathType Leaf)) {
        throw "Required file is missing: $required"
    }
}

$variants = @(
    [pscustomobject]@{ Id="0.2"; File="Snapmaker U1 (0.2 nozzle).json"; LayerHeight="0.10"; InitialLayerHeight="0.12"; LineWidth="0.22"; InitialLineWidth="0.25"; OverrideRoleWidths=$true },
    [pscustomobject]@{ Id="0.4"; File="Snapmaker U1 (0.4 nozzle).json"; LayerHeight="0.20"; InitialLayerHeight="0.20"; LineWidth="0.42"; InitialLineWidth="0.50"; OverrideRoleWidths=$false },
    [pscustomobject]@{ Id="0.4+0.6"; File="Snapmaker U1 (0.4+0.6 nozzle).json"; LayerHeight="0.20"; InitialLayerHeight="0.25"; LineWidth="0.42"; InitialLineWidth="0.50"; OverrideRoleWidths=$false },
    [pscustomobject]@{ Id="0.6"; File="Snapmaker U1 (0.6 nozzle).json"; LayerHeight="0.30"; InitialLayerHeight="0.30"; LineWidth="0.62"; InitialLineWidth="0.72"; OverrideRoleWidths=$true },
    [pscustomobject]@{ Id="0.8"; File="Snapmaker U1 (0.8 nozzle).json"; LayerHeight="0.40"; InitialLayerHeight="0.40"; LineWidth="0.82"; InitialLineWidth="0.82"; TreeTipDiameter="0.82"; OverrideRoleWidths=$true }
)

$cases = @(
    [pscustomobject]@{ Id="BASIC"; HeightMode="typical"; BedType="Textured PEI Plate"; BedTemp=60; NozzleTemp=220; BrimType="no_brim"; BrimWidth=0; BrimGap=0.0; RaftLayers=0 },
    [pscustomobject]@{ Id="HIGH"; HeightMode="typical"; BedType="High Temp Plate"; BedTemp=90; NozzleTemp=250; BrimType="outer_only"; BrimWidth=5; BrimGap=0.1; RaftLayers=0 },
    [pscustomobject]@{ Id="COOL"; HeightMode="typical"; BedType="Cool Plate"; BedTemp=35; NozzleTemp=180; BrimType="outer_and_inner"; BrimWidth=10; BrimGap=0.2; RaftLayers=0 },
    [pscustomobject]@{ Id="RAFT"; HeightMode="typical"; BedType="Engineering Plate"; BedTemp=70; NozzleTemp=260; BrimType="outer_only"; BrimWidth=5; BrimGap=0.1; RaftLayers=2 },
    [pscustomobject]@{ Id="MIN"; HeightMode="min"; BedType="Textured PEI Plate"; BedTemp=60; NozzleTemp=220; BrimType="no_brim"; BrimWidth=0; BrimGap=0.0; RaftLayers=0 },
    [pscustomobject]@{ Id="MAX"; HeightMode="max"; BedType="Textured PEI Plate"; BedTemp=60; NozzleTemp=220; BrimType="no_brim"; BrimWidth=0; BrimGap=0.0; RaftLayers=0 }
)

$stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$outputRoot = Join-Path $RepoRoot "build\verification\u1-nozzle-start-matrix\$stamp"
New-Item -ItemType Directory -Force -Path $outputRoot | Out-Null
$results = @()

foreach ($variant in $variants) {
    $profile = Get-Content -Raw -LiteralPath (Join-Path $profileRoot $variant.File) | ConvertFrom-Json
    $variant | Add-Member -NotePropertyName Nozzles -NotePropertyValue @($profile.nozzle_diameter)
    $variant | Add-Member -NotePropertyName MinLayers -NotePropertyValue @($profile.min_layer_height)
    $variant | Add-Member -NotePropertyName MaxLayers -NotePropertyValue @($profile.max_layer_height)
    $startProperty = $profile.PSObject.Properties["machine_start_gcode"]
    $startGcode = if ($null -ne $startProperty) { [string] $startProperty.Value } else { [string] $commonProfile.machine_start_gcode }

    foreach ($case in $cases) {
        $caseRoot = Join-Path $outputRoot "$($variant.Id)-$($case.Id)"
        New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
        $machine = Get-Content -Raw -LiteralPath $machineBaseline | ConvertFrom-Json
        $process = Get-Content -Raw -LiteralPath $processBaseline | ConvertFrom-Json
        $filament = Get-Content -Raw -LiteralPath $filamentBaseline | ConvertFrom-Json

        Set-ConfigValue $machine "printer_model" "Snapmaker U1"
        Set-ConfigValue $machine "printer_variant" $variant.Id
        Set-ConfigValue $machine "printer_settings_id" "Snapmaker U1 ($($variant.Id) nozzle)"
        Set-ConfigValue $machine "nozzle_diameter" @($profile.nozzle_diameter)
        Set-ConfigValue $machine "min_layer_height" @($profile.min_layer_height)
        Set-ConfigValue $machine "max_layer_height" @($profile.max_layer_height)
        Set-ConfigValue $machine "machine_start_gcode" $startGcode
        Set-ConfigValue $machine "curr_bed_type" $case.BedType

        $caseLayerHeight = Get-CaseLayerHeight -Variant $variant -Case $case
        $caseInitialLayerHeight = Get-CaseInitialLayerHeight -Variant $variant -Case $case
        Set-ConfigValue $process "layer_height" ([string] $caseLayerHeight)
        Set-ConfigValue $process "initial_layer_print_height" ([string] $caseInitialLayerHeight)
        Set-ConfigValue $process "line_width" $variant.LineWidth
        Set-ConfigValue $process "initial_layer_line_width" $variant.InitialLineWidth
        if ($variant.OverrideRoleWidths) {
            foreach ($widthKey in @(
                "outer_wall_line_width", "inner_wall_line_width", "sparse_infill_line_width",
                "internal_solid_infill_line_width", "support_line_width", "top_surface_line_width"
            )) {
                Set-ConfigValue $process $widthKey $variant.LineWidth
            }
        }
        if ($null -ne $variant.PSObject.Properties["TreeTipDiameter"]) {
            Set-ConfigValue $process "tree_support_tip_diameter" $variant.TreeTipDiameter
        }
        Set-ConfigValue $process "brim_type" $case.BrimType
        Set-ConfigValue $process "brim_width" ([string] $case.BrimWidth)
        Set-ConfigValue $process "brim_object_gap" ([string] $case.BrimGap)
        Set-ConfigValue $process "raft_layers" ([string] $case.RaftLayers)

        Set-ConfigValue $filament "nozzle_temperature" ([string] $case.NozzleTemp)
        Set-ConfigValue $filament "nozzle_temperature_initial_layer" ([string] $case.NozzleTemp)
        foreach ($bedKey in @("hot_plate_temp_initial_layer", "textured_plate_temp_initial_layer", "cool_plate_temp_initial_layer", "eng_plate_temp_initial_layer")) {
            if ($null -ne $filament.PSObject.Properties[$bedKey]) {
                Set-ConfigValue $filament $bedKey ([string] $case.BedTemp)
            }
        }

        $machinePath = Join-Path $caseRoot "machine.json"
        $processPath = Join-Path $caseRoot "process.json"
        $filamentPath = Join-Path $caseRoot "filament.json"
        Save-Config $machine $machinePath
        Save-Config $process $processPath
        Save-Config $filament $filamentPath

        $slice = Invoke-MagpieSlice -Executable $runner -Machine $machinePath -Process $processPath `
            -Filament $filamentPath -Model $model -OutputDirectory (Join-Path $caseRoot "gcode")
        $assessment = Test-GCode -Path $slice.GCode -Variant $variant -Case $case
        $results += [pscustomobject]@{
            Variant = $variant.Id
            Nozzles = $variant.Nozzles -join ","
            Case = $case.Id
            BedType = $case.BedType
            BedTemp = $case.BedTemp
            NozzleTemp = $case.NozzleTemp
            LayerHeight = $caseLayerHeight
            InitialLayerHeight = $caseInitialLayerHeight
            LineWidth = [double] $variant.LineWidth
            InitialLineWidth = [double] $variant.InitialLineWidth
            BrimType = $case.BrimType
            RaftLayers = $case.RaftLayers
            ExitCode = $slice.ExitCode
            Seconds = $slice.Seconds
            GCode = $slice.GCode
            Passed = $slice.ExitCode -eq 0 -and $assessment.Passed
            Error = $assessment.Error
        }
    }
}

$csv = Join-Path $outputRoot "matrix-summary.csv"
$json = Join-Path $outputRoot "matrix-summary.json"
$results | Export-Csv -LiteralPath $csv -NoTypeInformation -Encoding UTF8
[IO.File]::WriteAllText($json, ($results | ConvertTo-Json -Depth 8), [Text.UTF8Encoding]::new($false))

[pscustomobject]@{
    OutputRoot = $outputRoot
    Cases = $results.Count
    Passed = @($results | Where-Object Passed).Count
    Failed = @($results | Where-Object { -not $_.Passed }).Count
    Results = $results
} | ConvertTo-Json -Depth 8
