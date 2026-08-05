param(
    [string] $SlicerPath,
    [string] $OutputRoot,
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
$ModelPath = Join-Path $RepoRoot 'tests\data\overhang.obj'
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

function Analyze-SupportGcode([string] $path) {
    $settings = @{}
    $role = ''
    $layer = -1
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $e = 0.0
    $supportLength = 0.0
    $interfaceLength = 0.0
    $supportLayers = [Collections.Generic.HashSet[int]]::new()
    $interfaceLayers = [Collections.Generic.HashSet[int]]::new()
    $interfaceAnglesByLayer = @{}
    $temperatureCommands = [Collections.Generic.List[int]]::new()
    $nonFinite = 0

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
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
                    $interfaceLength += $length
                    [void] $interfaceLayers.Add($layer)
                    if ($length -ge 1.5) {
                        if (-not $interfaceAnglesByLayer.ContainsKey($layer)) { $interfaceAnglesByLayer[$layer] = @{} }
                        $angle = [Math]::Atan2($dy, $dx) * 180.0 / [Math]::PI
                        while ($angle -lt 0) { $angle += 180.0 }
                        while ($angle -ge 180.0) { $angle -= 180.0 }
                        $bin = [int]([Math]::Round($angle / 5.0) * 5) % 180
                        $interfaceAnglesByLayer[$layer][$bin] = [double] $interfaceAnglesByLayer[$layer][$bin] + $length
                    }
                } else {
                    $supportLength += $length
                    [void] $supportLayers.Add($layer)
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
    return [pscustomobject]@{
        Settings = $settings
        SupportLength = $supportLength
        InterfaceLength = $interfaceLength
        SupportLayers = $supportLayers.Count
        InterfaceLayers = $interfaceLayers.Count
        DominantAnglesByLayer = $dominantAngles
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
    [pscustomobject]@{ Name='cura_90_sublayer_2_4'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=4; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=165; TopLayers=5 },
    [pscustomobject]@{ Name='cura_90_sublayer_clamp'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=99; SublayerPattern='grid'; SublayerAngle=45; Temperature=170; TopLayers=5 },
    [pscustomobject]@{ Name='cura_90_single_interface'; Type='normal_cura(auto)'; Angle=90; Pattern='triangles'; Sublayer=$true; Start=2; End=99; SublayerPattern='rectilinear'; SublayerAngle=30; Temperature=165; TopLayers=1 }
) | Where-Object Name -like $CaseFilter
if ($cases.Count -eq 0) { throw "No verification cases matched: $CaseFilter" }

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
    $arguments = @('--slice','0','--debug','1','--load-settings',$quotedSettings,'--load-filaments',$quotedFilaments,'--outputdir',([char]34 + $caseRoot + [char]34),([char]34 + $ModelPath + [char]34))
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
    if ($analysis) {
        if ($analysis.Settings['support_type'] -ne $case.Type) { $errors.Add('Embedded support type mismatch') }
        if ($analysis.Settings['support_threshold_angle'] -ne [string] $case.Angle) { $errors.Add('Embedded threshold mismatch') }
        if ($analysis.Settings['support_interface_pattern'] -ne $case.Pattern) { $errors.Add('Embedded interface pattern mismatch') }
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
        TemperatureSeen = if ($analysis -and $case.Temperature -gt 0) { $analysis.TemperatureCommands -contains $case.Temperature } else { $false }
        DominantAngles = if ($analysis) { ($analysis.DominantAnglesByLayer.GetEnumerator() | Sort-Object Key | ForEach-Object { "$($_.Key):$($_.Value -join '/')" }) -join ';' } else { '' }
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
