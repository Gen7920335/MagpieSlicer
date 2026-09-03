param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [string] $ModelPath,
    [ValidateRange(-1, 90)]
    [int] $SupportAngleOverride = -1,
    [ValidateRange(-1, 359)]
    [int] $PatternAngleOverride = -1,
    [ValidateRange(-1, 10)]
    [int] $NormalSupportWallCountOverride = -1,
    [int] $SliceTimeoutSeconds = 180,
    [string] $CaseFilter = '*'
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\support-features'
}

$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepoRoot 'tests\data\overhang.obj'
}
$ModelPath = [IO.Path]::GetFullPath($ModelPath)
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($requiredPath in @($SlicerPath, $ModelPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $requiredPath -PathType Leaf)) { throw "Required file not found: $requiredPath" }
}

function Find-FilamentProfile([string] $name) {
    foreach ($root in @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue | Select-Object -First 1
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

function Resolve-LineWidth($value, [double] $nozzle) {
    $text = [string] $value
    if ($text.EndsWith('%')) {
        return $nozzle * [double]::Parse($text.TrimEnd('%'), $Invariant) / 100.0
    }
    return [double]::Parse($text, $Invariant)
}

function Analyze-SupportGcode([string] $path) {
    $settings = @{}
    $role = ''
    $tool = -1
    $layer = -1
    $absoluteXY = $true
    $relativeExtrusion = $false
    $currentWidth = $null
    $x = $null
    $y = $null
    $e = 0.0
    $supportLength = 0.0
    $interfaceLength = 0.0
    $supportLayers = [Collections.Generic.HashSet[int]]::new()
    $interfaceLayers = [Collections.Generic.HashSet[int]]::new()
    $interfaceAnglesByLayer = @{}
    $supportAnglesByLayer = @{}
    $supportTools = [Collections.Generic.HashSet[int]]::new()
    $interfaceTools = [Collections.Generic.HashSet[int]]::new()
    $supportWidths = [Collections.Generic.HashSet[double]]::new()
    $interfaceWidths = [Collections.Generic.HashSet[double]]::new()
    $temperatureCommands = [Collections.Generic.List[int]]::new()
    $nonFinite = 0

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^;WIDTH:(\d+(?:\.\d*)?|\.\d+)$') {
            $width = [double]::Parse($Matches[1], $Invariant)
            $currentWidth = $width
            if ($role -match '(?i)support') {
                if ($role -match '(?i)interface') { [void] $interfaceWidths.Add($width) }
                else { [void] $supportWidths.Add($width) }
            }
            continue
        }
        if ($line -match '^T(\d+)(?:\s|$)') { $tool = [int]$Matches[1]; continue }
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') { $settings[$Matches[1]] = $Matches[2].Trim(); continue }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeExtrusion = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeExtrusion = $true; continue }
        if ($line -match '^G92(?:\s|$)') {
            if ($line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') { $e = [double]::Parse($Matches[1], $Invariant) }
            continue
        }
        if ($line -match '^M10[49].*(?:^|\s)S(\d+(?:\.\d*)?)') {
            $temperatureCommands.Add([int][Math]::Round([double]::Parse($Matches[1], $Invariant)))
        }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        if ($line -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') { $nonFinite++; continue }

        $newX = $x
        $newY = $y
        $newE = $e
        $hasX = $false
        $hasY = $false
        $hasE = $false
        foreach ($token in [regex]::Matches($line, '(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))')) {
            $axis = $token.Groups[1].Value
            $value = [double]::Parse($token.Groups[2].Value, $Invariant)
            switch ($axis) {
                'X' { $newX = if ($absoluteXY -or $null -eq $x) { $value } else { $x + $value }; $hasX = $true }
                'Y' { $newY = if ($absoluteXY -or $null -eq $y) { $value } else { $y + $value }; $hasY = $true }
                'E' { $newE = $value; $hasE = $true }
            }
        }
        $extrusion = if (-not $hasE) { 0.0 } elseif ($relativeExtrusion) { $newE } else { $newE - $e }
        if ($extrusion -gt 0.0000001 -and $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)) {
            $dx = $newX - $x
            $dy = $newY - $y
            $length = [Math]::Sqrt($dx * $dx + $dy * $dy)
            if ($length -gt 0.001 -and $role -match '(?i)support') {
                if ($role -match '(?i)interface') {
                    if ($null -ne $currentWidth) { [void] $interfaceWidths.Add($currentWidth) }
                    $interfaceLength += $length
                    [void] $interfaceLayers.Add($layer)
                    if ($tool -ge 0) { [void] $interfaceTools.Add($tool) }
                    if ($length -ge 1.5) {
                        if (-not $interfaceAnglesByLayer.ContainsKey($layer)) { $interfaceAnglesByLayer[$layer] = @{} }
                        $angle = [Math]::Atan2($dy, $dx) * 180.0 / [Math]::PI
                        while ($angle -lt 0) { $angle += 180.0 }
                        while ($angle -ge 180.0) { $angle -= 180.0 }
                        $bin = [int]([Math]::Round($angle / 5.0) * 5) % 180
                        $interfaceAnglesByLayer[$layer][$bin] = [double] $interfaceAnglesByLayer[$layer][$bin] + $length
                    }
                } else {
                    if ($null -ne $currentWidth) { [void] $supportWidths.Add($currentWidth) }
                    $supportLength += $length
                    [void] $supportLayers.Add($layer)
                    if ($tool -ge 0) { [void] $supportTools.Add($tool) }
                    if ($length -ge 1.5) {
                        if (-not $supportAnglesByLayer.ContainsKey($layer)) { $supportAnglesByLayer[$layer] = @{} }
                        $angle = [Math]::Atan2($dy, $dx) * 180.0 / [Math]::PI
                        while ($angle -lt 0) { $angle += 180.0 }
                        while ($angle -ge 180.0) { $angle -= 180.0 }
                        $bin = [int]([Math]::Round($angle / 5.0) * 5) % 180
                        $supportAnglesByLayer[$layer][$bin] = [double] $supportAnglesByLayer[$layer][$bin] + $length
                    }
                }
            }
        }
        if ($hasX) { $x = $newX }
        if ($hasY) { $y = $newY }
        if ($hasE) { $e = $newE }
    }

    $dominantAngles = @{}
    foreach ($entry in $interfaceAnglesByLayer.GetEnumerator()) {
        $dominantAngles[$entry.Key] = @($entry.Value.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3 | ForEach-Object { [int] $_.Key } | Sort-Object)
    }
    $dominantSupportAngles = @{}
    foreach ($entry in $supportAnglesByLayer.GetEnumerator()) {
        $dominantSupportAngles[$entry.Key] = @($entry.Value.GetEnumerator() | Sort-Object Value -Descending | Select-Object -First 3 | ForEach-Object { [int] $_.Key } | Sort-Object)
    }
    return [pscustomobject]@{
        Settings = $settings
        SupportLength = $supportLength
        InterfaceLength = $interfaceLength
        SupportLayers = $supportLayers.Count
        InterfaceLayers = $interfaceLayers.Count
        SupportTools = @($supportTools | Sort-Object)
        InterfaceTools = @($interfaceTools | Sort-Object)
        SupportWidths = @($supportWidths | Sort-Object)
        InterfaceWidths = @($interfaceWidths | Sort-Object)
        DominantAnglesByLayer = $dominantAngles
        DominantSupportAnglesByLayer = $dominantSupportAngles
        TemperatureCommands = @($temperatureCommands)
        NonFinite = $nonFinite
    }
}

$cases = @(
    [pscustomobject]@{ Name='prusa_70_triangles'; Type='normal(auto)'; Angle=70; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='prusa_90_triangles'; Type='normal(auto)'; Angle=90; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='cura_70_triangles'; Type='normal_cura(auto)'; Angle=70; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='cura_90_triangles'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='tree_70_triangles'; Type='tree(auto)'; Angle=70; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='tree_90_triangles'; Type='tree(auto)'; Angle=90; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5 },
    [pscustomobject]@{ Name='resin_default'; Type='resin(auto)'; ResinTree='default'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=2 },
    [pscustomobject]@{ Name='resin_branching'; Type='resin(auto)'; ResinTree='branching'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=2 },
    [pscustomobject]@{ Name='prusa_materials'; Type='normal(auto)'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=2; InterfaceFilament=3 },
    [pscustomobject]@{ Name='cura_materials'; Type='normal_cura(auto)'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=2; InterfaceFilament=3 },
    [pscustomobject]@{ Name='tree_materials'; Type='tree(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=2; InterfaceFilament=3 },
    [pscustomobject]@{ Name='tree_slim_materials_control'; Type='tree(auto)'; Style='tree_slim'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=2; InterfaceFilament=3 },
    [pscustomobject]@{ Name='tree_slim_040_060_control'; Type='tree(auto)'; Style='tree_slim'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=3 },
    [pscustomobject]@{ Name='tree_slim_same_040_control'; Type='tree(auto)'; Style='tree_slim'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=1 },
    [pscustomobject]@{ Name='tree_slim_material3_040_control'; Type='tree(auto)'; Style='tree_slim'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=3; InterfaceNozzleDiameter=0.4; InterfaceLineWidth=0.45 },
    [pscustomobject]@{ Name='tree_organic_same_control'; Type='tree(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=1 },
    [pscustomobject]@{ Name='tree_organic_040_060_control'; Type='tree(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=3 },
    [pscustomobject]@{ Name='tree_organic_material3_040_control'; Type='tree(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=3; InterfaceNozzleDiameter=0.4; InterfaceLineWidth=0.45 },
    [pscustomobject]@{ Name='tree_slim_040_020_control'; Type='tree(auto)'; Style='tree_slim'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=2 },
    [pscustomobject]@{ Name='tree_organic_040_020_control'; Type='tree(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=1; InterfaceFilament=2 },
    [pscustomobject]@{ Name='mixed_materials'; Type='mixed(auto)'; Style='organic'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; SupportFilament=2; InterfaceFilament=3; Mixed=$true },
    [pscustomobject]@{ Name='resin_materials'; Type='resin(auto)'; ResinTree='branching'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=2; SupportFilament=2; InterfaceFilament=3 },
    [pscustomobject]@{ Name='cura_lowtemp_no_tower'; Type='normal_cura(auto)'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; LowTemp=$true; Tower=$false },
    [pscustomobject]@{ Name='cura_lowtemp_with_tower'; Type='normal_cura(auto)'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; LowTemp=$true; Tower=$true },
    [pscustomobject]@{ Name='cura_disabled_lowtemp_with_tower'; Type='normal_cura(auto)'; Angle=60; Pattern='triangles'; Sublayer=$false; Start=2; End=2; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=0; TopLayers=5; LowTemp=$false; Tower=$true },
    [pscustomobject]@{ Name='cura_90_sublayer_2_4'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=4; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=165; TopLayers=5 },
    [pscustomobject]@{ Name='cura_90_sublayer_clamp'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=99; SublayerPattern='grid'; SublayerAngle=45; Temperature=170; TopLayers=5 },
    [pscustomobject]@{ Name='cura_90_single_interface'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=99; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=165; TopLayers=1 }
) | Where-Object Name -like $CaseFilter
if ($cases.Count -eq 0) { throw "No verification cases matched: $CaseFilter" }
if ($SupportAngleOverride -ge 0) {
    foreach ($case in $cases) {
        $case.Angle = $SupportAngleOverride
        $case.Name = "$($case.Name)_angle_$SupportAngleOverride"
    }
}
if ($PatternAngleOverride -ge 0) {
    foreach ($case in $cases) {
        $case | Add-Member -NotePropertyName PatternAngle -NotePropertyValue $PatternAngleOverride -Force
        $case.Name = "$($case.Name)_pattern_$PatternAngleOverride"
    }
}
if ($NormalSupportWallCountOverride -ge 0) {
    foreach ($case in $cases) {
        $case | Add-Member -NotePropertyName NormalSupportWallCount -NotePropertyValue $NormalSupportWallCountOverride -Force
        $case.Name = "$($case.Name)_walls_$NormalSupportWallCountOverride"
    }
}

$filaments = @('Snapmaker PLA @U1','Snapmaker ABS @U1','Snapmaker PETG @U1','Snapmaker TPU @U1') | ForEach-Object { Find-FilamentProfile $_ }
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()

foreach ($case in $cases) {
    $caseRoot = Join-Path $runRoot $case.Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
    $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
    $machineName = "Codex support verify $($case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "support-$($case.Name)"
    $process.name = $machineName
    $process.setting_id = "support-$($case.Name)"
    $process.compatible_printers = @($machineName)
    Set-JsonProperty $process 'enable_support' '1'
    Set-JsonProperty $process 'support_type' $case.Type
    Set-JsonProperty $process 'support_threshold_angle' ([string] $case.Angle)
    if ($null -ne $case.PSObject.Properties['PatternAngle']) {
        Set-JsonProperty $process 'support_angle' ([string] $case.PatternAngle)
    }
    if ($null -ne $case.PSObject.Properties['NormalSupportWallCount']) {
        Set-JsonProperty $process 'support_wall_count' ([string] $case.NormalSupportWallCount)
    }
    if ($null -ne $case.PSObject.Properties['Style']) {
        Set-JsonProperty $process 'support_style' $case.Style
    }
    if ($null -ne $case.PSObject.Properties['SupportFilament']) {
        Set-JsonProperty $process 'layer_height' '0.16'
        Set-JsonProperty $process 'initial_layer_print_height' '0.16'
        Set-JsonProperty $process 'support_filament' ([string]$case.SupportFilament)
        Set-JsonProperty $process 'support_interface_filament' ([string]$case.InterfaceFilament)
    }
    if ($null -ne $case.PSObject.Properties['InterfaceNozzleDiameter']) {
        $interfaceTool = [int]$case.InterfaceFilament - 1
        $machine.nozzle_diameter[$interfaceTool] = [string]$case.InterfaceNozzleDiameter
        $machine.toolhead_support_line_width[$interfaceTool] = [string]$case.InterfaceLineWidth
    }
    if ($null -ne $case.PSObject.Properties['Mixed']) {
        Set-JsonProperty $process 'mixed_normal_support_generator' 'prusa'
        Set-JsonProperty $process 'mixed_tree_support_style' 'organic'
        Set-JsonProperty $process 'mixed_normal_coverage_threshold' '80%'
        Set-JsonProperty $process 'mixed_selective_merge' '1'
    }
    if ($case.Type -eq 'resin(auto)') {
        Set-JsonProperty $process 'resin_support_tree_type' $case.ResinTree
        Set-JsonProperty $process 'resin_support_points_density_relative' '100'
    }
    if ($null -ne $case.PSObject.Properties['LowTemp']) {
        Set-JsonProperty $process 'single_nozzle_low_temperature_interface' $(if ($case.LowTemp) { '1' } else { '0' })
        Set-JsonProperty $process 'support_interface_temperature' '170'
        Set-JsonProperty $process 'support_interface_heating_time' '5'
        Set-JsonProperty $process 'support_interface_temperature_drop_tower' $(if ($case.Tower) { '1' } else { '0' })
        Set-JsonProperty $process 'support_interface_temperature_drop_tower_x' '70'
        Set-JsonProperty $process 'support_interface_temperature_drop_tower_y' '70'
    }
    Set-JsonProperty $process 'support_interface_pattern' $case.Pattern
    Set-JsonProperty $process 'support_interface_top_layers' ([string] $case.TopLayers)
    Set-JsonProperty $process 'support_interface_bottom_layers' '0'
    Set-JsonProperty $process 'support_interface_spacing' '0'
    Set-JsonProperty $process 'support_interface_sublayer_pattern' $(if ($case.Sublayer) { '1' } else { '0' })
    Set-JsonProperty $process 'support_interface_sublayer_start_layer' ([string] $case.Start)
    Set-JsonProperty $process 'support_interface_sublayer_end_layer' ([string] $case.End)
    Set-JsonProperty $process 'support_interface_sublayer_pattern_type' $case.SublayerPattern
    Set-JsonProperty $process 'support_interface_sublayer_angle' ([string] $case.SublayerAngle)
    Set-JsonProperty $process 'support_interface_sublayer_temperature' ([string] $case.Temperature)
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '0'
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $quotedSettings = [char]34 + "$machinePath;$processPath" + [char]34
    $quotedFilaments = [char]34 + ($filaments -join ';') + [char]34
    $arguments = @('--slice','0','--debug','1','--filament-map','1,2,3,4','--load-settings',$quotedSettings,'--load-filaments',$quotedFilaments,'--outputdir',([char]34 + $caseRoot + [char]34),([char]34 + $ModelPath + [char]34))
    $stdout = Join-Path $caseRoot 'cli.out.log'
    $stderr = Join-Path $caseRoot 'cli.err.log'
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) { $handle.Kill(); throw "Slice timeout: $($case.Name)" }
    $handle.Refresh()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -Filter '*.gcode' -File | Select-Object -First 1 -ExpandProperty FullName
    $errors = [Collections.Generic.List[string]]::new()
    $exitCode = $handle.ExitCode
    if ($null -eq $exitCode) { $exitCode = if ($gcode) { 0 } else { 1 } }
    if ($exitCode -ne 0) { $errors.Add("CLI exit code $exitCode") }
    if ([string]::IsNullOrWhiteSpace($gcode)) { $errors.Add('G-code was not generated') }
    $analysis = if ($gcode) { Analyze-SupportGcode $gcode } else { $null }
    $gcodeText = if ($gcode -and $null -ne $case.PSObject.Properties['LowTemp']) {
        [IO.File]::ReadAllText($gcode)
    } else { '' }
    if ($analysis) {
        if ($analysis.Settings['support_type'] -ne $case.Type) { $errors.Add('Embedded support type mismatch') }
        if ($analysis.Settings['support_threshold_angle'] -ne [string] $case.Angle) { $errors.Add('Embedded threshold mismatch') }
        if ($null -ne $case.PSObject.Properties['PatternAngle'] -and
            $analysis.Settings['support_angle'] -ne [string] $case.PatternAngle) {
            $errors.Add('Embedded support pattern angle mismatch')
        }
        if ($null -ne $case.PSObject.Properties['NormalSupportWallCount'] -and
            $analysis.Settings['support_wall_count'] -ne [string] $case.NormalSupportWallCount) {
            $errors.Add('Embedded normal support wall count mismatch')
        }
        if ($analysis.Settings['support_interface_pattern'] -ne $case.Pattern) { $errors.Add('Embedded interface pattern mismatch') }
        if ($case.Type -eq 'resin(auto)' -and $analysis.Settings['resin_support_tree_type'] -ne $case.ResinTree) {
            $errors.Add('Embedded Resin tree type mismatch')
        }
        if ($null -ne $case.PSObject.Properties['SupportFilament']) {
            $expectedSupportTool = [int]$case.SupportFilament - 1
            $expectedInterfaceTool = [int]$case.InterfaceFilament - 1
            if ($analysis.SupportTools -notcontains $expectedSupportTool) { $errors.Add("Support body did not use tool $expectedSupportTool") }
            if ($analysis.InterfaceTools -notcontains $expectedInterfaceTool) { $errors.Add("Support interface did not use tool $expectedInterfaceTool") }
            $expectedSupportWidth = Resolve-LineWidth $machine.toolhead_support_line_width[$expectedSupportTool] ([double]$machine.nozzle_diameter[$expectedSupportTool])
            $expectedInterfaceWidth = Resolve-LineWidth $machine.toolhead_support_line_width[$expectedInterfaceTool] ([double]$machine.nozzle_diameter[$expectedInterfaceTool])
            if (@($analysis.SupportWidths | Where-Object { [Math]::Abs($_ - $expectedSupportWidth) -le 0.001 }).Count -eq 0) {
                $errors.Add("Support body did not emit configured width $expectedSupportWidth mm; observed $($analysis.SupportWidths -join ',')")
            }
            if (@($analysis.InterfaceWidths | Where-Object { [Math]::Abs($_ - $expectedInterfaceWidth) -le 0.001 }).Count -eq 0) {
                $errors.Add("Support interface did not emit configured width $expectedInterfaceWidth mm; observed $($analysis.InterfaceWidths -join ',')")
            }
        }
        if ($null -ne $case.PSObject.Properties['LowTemp']) {
            $towerExpected = $case.LowTemp -and $case.Tower
            $lowTemperatureSeen = $gcodeText.Contains('; low-temperature support interface begin')
            $interfaceTemperatureSeen = [regex]::IsMatch($gcodeText, '(?m)^M10[49].*\sS170(?:\s|;|$)')
            if ($lowTemperatureSeen -ne [bool]$case.LowTemp) { $errors.Add('Low-temperature interface activation mismatch') }
            if ($interfaceTemperatureSeen -ne [bool]$case.LowTemp) { $errors.Add('Low-temperature interface command mismatch') }
            $towerSeen = $gcodeText.Contains('; temperature drop tower layer')
            if ($towerSeen -ne $towerExpected) { $errors.Add('Temperature drop tower activation mismatch') }
            $brimLines = [regex]::Matches($gcodeText, '(?m)^; temperature drop tower brim line$').Count
            if ($towerExpected -and $brimLines -ne 5) { $errors.Add("Temperature drop tower brim count was $brimLines") }
            if (-not $towerExpected -and $brimLines -ne 0) { $errors.Add('Disabled temperature drop tower emitted a brim') }
        }
        if ($analysis.SupportLength -le 0) { $errors.Add('Support body extrusion was not generated') }
        if ($analysis.InterfaceLength -le 0) { $errors.Add('Support interface extrusion was not generated') }
        if ($analysis.NonFinite -gt 0) { $errors.Add("Non-finite G-code values: $($analysis.NonFinite)") }
        if ($case.Sublayer -and $case.TopLayers -gt 1 -and $analysis.TemperatureCommands -notcontains $case.Temperature) {
            $errors.Add("Sublayer temperature command S$($case.Temperature) was not generated")
        }
        if ($case.TopLayers -le 1 -and $analysis.TemperatureCommands -contains $case.Temperature) {
            $errors.Add('Sublayer temperature was active with only one interface layer')
        }
    }
    $results.Add([pscustomobject]@{
        Case = $case.Name
        Type = $case.Type
        Angle = $case.Angle
        Sublayer = $case.Sublayer
        SupportLength = if ($analysis) { [Math]::Round($analysis.SupportLength,2) } else { 0 }
        InterfaceLength = if ($analysis) { [Math]::Round($analysis.InterfaceLength,2) } else { 0 }
        SupportLayers = if ($analysis) { $analysis.SupportLayers } else { 0 }
        InterfaceLayers = if ($analysis) { $analysis.InterfaceLayers } else { 0 }
        SupportTools = if ($analysis) { $analysis.SupportTools -join ',' } else { '' }
        InterfaceTools = if ($analysis) { $analysis.InterfaceTools -join ',' } else { '' }
        SupportWidths = if ($analysis) { $analysis.SupportWidths -join ',' } else { '' }
        InterfaceWidths = if ($analysis) { $analysis.InterfaceWidths -join ',' } else { '' }
        TemperatureSeen = if ($analysis -and $case.Temperature -gt 0) { $analysis.TemperatureCommands -contains $case.Temperature } else { $false }
        DominantAngles = if ($analysis) { ($analysis.DominantAnglesByLayer.GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key):$($_.Value -join '/')" }) -join ';' } else { '' }
        DominantSupportAngles = if ($analysis) { ($analysis.DominantSupportAnglesByLayer.GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key):$($_.Value -join '/')" }) -join ';' } else { '' }
        Passed = $errors.Count -eq 0
        Errors = $errors -join '; '
        Gcode = $gcode
    })
}

foreach ($type in @('prusa','cura','tree')) {
    $case70 = $results | Where-Object Case -eq "${type}_70_triangles"
    $case90 = $results | Where-Object Case -eq "${type}_90_triangles"
    if ($case70 -and $case90 -and $case70.Passed -and $case90.Passed -and $case90.SupportLength -lt $case70.SupportLength * 0.98) {
        $case90.Passed = $false
        $case90.Errors = '90 degree threshold generated less support than 70 degrees'
    }
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Select-Object Case,Passed,SupportLength,InterfaceLength,SupportLayers,InterfaceLayers,TemperatureSeen,Errors | Format-Table -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
