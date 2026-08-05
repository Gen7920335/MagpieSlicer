param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [int] $SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\interface-underside-smoothing'
}

$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$ModelPath = Join-Path $RepoRoot 'tests\data\overhang.obj'
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $ModelPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

function Find-FilamentProfile([string] $name) {
    $roots = @(
        (Join-Path $RepoRoot 'resources\profiles'),
        (Join-Path $env:APPDATA 'MagpieSlicer\system')
    )
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Set-JsonProperty($object, [string] $name, $value) {
    $object | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

function Set-JsonArrayValue($object, [string] $name, [int] $index, [string] $value) {
    $values = @($object.$name)
    while ($values.Count -le $index) { $values += $values[-1] }
    $values[$index] = $value
    Set-JsonProperty $object $name $values
}

function Analyze-Gcode([string] $path) {
    $settings = @{}
    $role = ''
    $layer = -1
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $e = 0.0
    $nonFinite = 0
    $supportLength = 0.0
    $interfaceLength = 0.0
    $interfaces = @{}

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') {
            $settings[$Matches[1]] = $Matches[2].Trim()
            continue
        }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeExtrusion = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeExtrusion = $true; continue }
        if ($line -match '^G92(?:\s|$)') {
            if ($line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') {
                $e = [double]::Parse($Matches[1], $Invariant)
            }
            continue
        }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        if ($line -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') {
            $nonFinite++
            continue
        }

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
                    if (-not $interfaces.ContainsKey($layer)) {
                        $interfaces[$layer] = [pscustomobject]@{
                            Length = 0.0
                            MinX = [double]::PositiveInfinity
                            MinY = [double]::PositiveInfinity
                            MaxX = [double]::NegativeInfinity
                            MaxY = [double]::NegativeInfinity
                        }
                    }
                    $item = $interfaces[$layer]
                    $item.Length += $length
                    $item.MinX = [Math]::Min($item.MinX, [Math]::Min($x, $newX))
                    $item.MinY = [Math]::Min($item.MinY, [Math]::Min($y, $newY))
                    $item.MaxX = [Math]::Max($item.MaxX, [Math]::Max($x, $newX))
                    $item.MaxY = [Math]::Max($item.MaxY, [Math]::Max($y, $newY))
                } else {
                    $supportLength += $length
                }
            }
        }
        if ($hasX) { $x = $newX }
        if ($hasY) { $y = $newY }
        if ($hasE) { $e = $newE }
    }

    $layerIds = @($interfaces.Keys | Sort-Object)
    $maxConsecutive = 0
    $currentConsecutive = 0
    $previous = $null
    foreach ($id in $layerIds) {
        if ($null -ne $previous -and $id -eq $previous + 1) {
            $currentConsecutive++
        } else {
            $currentConsecutive = 1
        }
        $maxConsecutive = [Math]::Max($maxConsecutive, $currentConsecutive)
        $previous = $id
    }

    $boundsValid = $true
    foreach ($item in $interfaces.Values) {
        if ($item.MinX -lt -0.01 -or $item.MinY -lt -0.01 -or
            $item.MaxX -gt 500.0 -or $item.MaxY -gt 500.0 -or
            $item.MaxX -le $item.MinX -or $item.MaxY -le $item.MinY) {
            $boundsValid = $false
        }
    }

    [pscustomobject]@{
        Settings = $settings
        SupportLength = $supportLength
        InterfaceLength = $interfaceLength
        InterfaceLayerCount = $layerIds.Count
        MaxConsecutiveInterfaceLayers = $maxConsecutive
        BoundsValid = $boundsValid
        NonFinite = $nonFinite
    }
}

$cases = @(
    [pscustomobject]@{ Name='cura_t0_disabled'; Type='normal_cura(auto)'; Layers=0; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_t1_min'; Type='normal_cura(auto)'; Layers=1; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_t2'; Type='normal_cura(auto)'; Layers=2; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_t5'; Type='normal_cura(auto)'; Layers=5; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_t10_max'; Type='normal_cura(auto)'; Layers=10; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_spacing_020'; Type='normal_cura(auto)'; Layers=5; Spacing=0.2; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_spacing_100'; Type='normal_cura(auto)'; Layers=5; Spacing=1.0; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_rectilinear'; Type='normal_cura(auto)'; Layers=5; Spacing=0.2; Pattern='rectilinear'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_snug'; Type='normal_cura(auto)'; Layers=5; Spacing=0.2; Pattern='triangles'; Style='snug'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_grid'; Type='normal_cura(auto)'; Layers=5; Spacing=0.2; Pattern='triangles'; Style='grid'; Nozzle=0.4 },
    [pscustomobject]@{ Name='cura_nozzle_015'; Type='normal_cura(auto)'; Layers=5; Spacing=0.0; Pattern='triangles'; Style='default'; Nozzle=0.15 },
    [pscustomobject]@{ Name='prusa_regression'; Type='normal(auto)'; Layers=5; Spacing=0.2; Pattern='triangles'; Style='default'; Nozzle=0.4 },
    [pscustomobject]@{ Name='tree_regression'; Type='tree(auto)'; Layers=5; Spacing=0.2; Pattern='triangles'; Style='default'; Nozzle=0.4 }
)

$filaments = @('Snapmaker PLA @U1','Snapmaker ABS @U1','Snapmaker PETG @U1','Snapmaker TPU @U1') |
    ForEach-Object { Find-FilamentProfile $_ }
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
    $machineName = "Codex interface smoothing $($case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "interface-smoothing-$($case.Name)"
    $process.name = $machineName
    $process.setting_id = "interface-smoothing-$($case.Name)"
    $process.compatible_printers = @($machineName)

    Set-JsonArrayValue $machine 'nozzle_diameter' 1 ([string]::Format($Invariant, '{0:0.##}', $case.Nozzle))
    Set-JsonProperty $process 'enable_support' '1'
    Set-JsonProperty $process 'support_type' $case.Type
    Set-JsonProperty $process 'support_style' $case.Style
    Set-JsonProperty $process 'support_threshold_angle' '90'
    Set-JsonProperty $process 'support_interface_top_layers' ([string] $case.Layers)
    Set-JsonProperty $process 'support_interface_bottom_layers' '0'
    Set-JsonProperty $process 'support_interface_pattern' $case.Pattern
    Set-JsonProperty $process 'support_interface_spacing' ([string]::Format($Invariant, '{0:0.##}', $case.Spacing))
    Set-JsonProperty $process 'support_interface_filament' '2'
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '0'
    if ($case.Nozzle -lt 0.2) {
        Set-JsonProperty $process 'layer_height' '0.1'
        Set-JsonProperty $process 'initial_layer_print_height' '0.1'
    }
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

    $quotedSettings = [char]34 + "$machinePath;$processPath" + [char]34
    $quotedFilaments = [char]34 + ($filaments -join ';') + [char]34
    $arguments = @(
        '--slice','0','--debug','1',
        '--load-settings',$quotedSettings,
        '--load-filaments',$quotedFilaments,
        '--outputdir',([char]34 + $caseRoot + [char]34),
        ([char]34 + $ModelPath + [char]34)
    )
    $stdout = Join-Path $caseRoot 'cli.out.log'
    $stderr = Join-Path $caseRoot 'cli.err.log'
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $handle.Kill()
        throw "Slice timeout: $($case.Name)"
    }
    $handle.Refresh()

    $gcode = Get-ChildItem -LiteralPath $caseRoot -Filter '*.gcode' -File |
        Select-Object -First 1 -ExpandProperty FullName
    $errors = [Collections.Generic.List[string]]::new()
    $exitCode = $handle.ExitCode
    if ($null -eq $exitCode) { $exitCode = if ($gcode) { 0 } else { 1 } }
    if ($exitCode -ne 0) { $errors.Add("CLI exit code $exitCode") }
    if ([string]::IsNullOrWhiteSpace($gcode)) { $errors.Add('G-code was not generated') }
    $analysis = if ($gcode) { Analyze-Gcode $gcode } else { $null }

    if ($analysis) {
        if ($analysis.Settings['support_type'] -ne $case.Type) { $errors.Add('Embedded support type mismatch') }
        if ([int] $analysis.Settings['support_interface_top_layers'] -ne $case.Layers) { $errors.Add('Embedded layer count mismatch') }
        if ($analysis.Settings['support_interface_pattern'] -ne $case.Pattern) { $errors.Add('Embedded pattern mismatch') }
        if ($analysis.SupportLength -le 0) { $errors.Add('Support body extrusion was not generated') }
        if ($case.Layers -eq 0) {
            if ($analysis.InterfaceLength -gt 0.001) { $errors.Add('Interface extrusion exists with zero interface layers') }
        } else {
            if ($analysis.InterfaceLength -le 0) { $errors.Add('Interface extrusion was not generated') }
            if ($analysis.InterfaceLayerCount -lt $case.Layers) { $errors.Add('Too few interface-bearing layers') }
            if ($case.Type -eq 'normal_cura(auto)' -and
                $analysis.MaxConsecutiveInterfaceLayers -lt $case.Layers) {
                $errors.Add("Interface stack is thinner than requested: $($analysis.MaxConsecutiveInterfaceLayers) < $($case.Layers)")
            }
            if (-not $analysis.BoundsValid) { $errors.Add('Invalid interface extrusion bounds') }
        }
        if ($analysis.NonFinite -gt 0) { $errors.Add("Non-finite G-code values: $($analysis.NonFinite)") }
    }

    $results.Add([pscustomobject]@{
        Case = $case.Name
        Passed = $errors.Count -eq 0
        Type = $case.Type
        Layers = $case.Layers
        Spacing = $case.Spacing
        Pattern = $case.Pattern
        Style = $case.Style
        Nozzle = $case.Nozzle
        SupportLength = if ($analysis) { [Math]::Round($analysis.SupportLength, 2) } else { 0 }
        InterfaceLength = if ($analysis) { [Math]::Round($analysis.InterfaceLength, 2) } else { 0 }
        InterfaceLayers = if ($analysis) { $analysis.InterfaceLayerCount } else { 0 }
        MaxConsecutive = if ($analysis) { $analysis.MaxConsecutiveInterfaceLayers } else { 0 }
        Errors = $errors -join '; '
        Gcode = $gcode
    })
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding UTF8
$results | Select-Object Case,Passed,Layers,Spacing,Pattern,Style,Nozzle,SupportLength,InterfaceLength,InterfaceLayers,MaxConsecutive,Errors |
    Format-Table -AutoSize
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULTS=$resultsPath"
Write-Output "PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
