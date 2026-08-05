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
    $OutputRoot = Join-Path $RepoRoot 'build\verification\cura-support-geometry'
}

$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

function Format-Number([double] $value) {
    $value.ToString('0.######', $Invariant)
}

function Set-JsonProperty($object, [string] $name, $value) {
    $object | Add-Member -MemberType NoteProperty -Name $name -Value $value -Force
}

function Find-FilamentProfile([string] $name) {
    foreach ($root in @(
        (Join-Path $RepoRoot 'resources\profiles'),
        (Join-Path $RepoRoot 'build\src\Release\resources\profiles')
    )) {
        if (-not (Test-Path -LiteralPath $root)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Add-Prism(
    [Collections.Generic.List[string]] $lines,
    [Collections.Generic.List[object]] $vertices,
    [double[][]] $points,
    [double] $z0,
    [double] $z1,
    [string] $name
) {
    $base = $vertices.Count
    $lines.Add("o $name")
    foreach ($point in $points) {
        $vertices.Add(@($point[0], $point[1], $z0))
        $lines.Add("v $(Format-Number $point[0]) $(Format-Number $point[1]) $(Format-Number $z0)")
    }
    foreach ($point in $points) {
        $vertices.Add(@($point[0], $point[1], $z1))
        $lines.Add("v $(Format-Number $point[0]) $(Format-Number $point[1]) $(Format-Number $z1)")
    }

    $count = $points.Count
    if ($count -le 4) {
        $bottom = 0..($count - 1) | ForEach-Object { $base + $count - $_ }
        $top = 0..($count - 1) | ForEach-Object { $base + $count + $_ + 1 }
        $lines.Add('f ' + ($bottom -join ' '))
        $lines.Add('f ' + ($top -join ' '))
    } else {
        $bottomFirst = $base + 1
        $topFirst = $base + $count + 1
        for ($index = 1; $index -lt $count - 1; ++$index) {
            $bottomA = $base + $index + 1
            $bottomB = $base + $index + 2
            $topA = $base + $count + $index + 1
            $topB = $base + $count + $index + 2
            $lines.Add("f $bottomFirst $bottomB $bottomA")
            $lines.Add("f $topFirst $topA $topB")
        }
    }
    for ($index = 0; $index -lt $count; ++$index) {
        $next = ($index + 1) % $count
        $a = $base + $index + 1
        $b = $base + $next + 1
        $c = $base + $count + $next + 1
        $d = $base + $count + $index + 1
        $lines.Add("f $a $b $c $d")
    }
}

function Write-GeometryModel([string] $path, [string] $shape) {
    $lines = [Collections.Generic.List[string]]::new()
    $vertices = [Collections.Generic.List[object]]::new()
    $centerX = 100.0
    $centerY = 100.0

    if ($shape -eq 'curved') {
        $stem = [Collections.Generic.List[object]]::new()
        $roof = [Collections.Generic.List[object]]::new()
        for ($index = 0; $index -lt 64; ++$index) {
            $angle = 2.0 * [Math]::PI * $index / 64.0
            $stem.Add(@(
                ($centerX + 8.0 * [Math]::Cos($angle)),
                ($centerY + 8.0 * [Math]::Sin($angle))
            ))
            $roof.Add(@(
                ($centerX + 30.0 * [Math]::Cos($angle)),
                ($centerY + 30.0 * [Math]::Sin($angle))
            ))
        }
        Add-Prism $lines $vertices $stem.ToArray() 0.0 20.0 'stem'
        Add-Prism $lines $vertices $roof.ToArray() 20.0 24.0 'curved_roof'
    } elseif ($shape -eq 'stepped') {
        $stem = @(
            @(95.0, 95.0), @(105.0, 95.0), @(105.0, 105.0), @(95.0, 105.0)
        )
        $roofCenter = @(
            @(70.0, 90.0), @(130.0, 90.0), @(130.0, 110.0), @(70.0, 110.0)
        )
        $roofLower = @(
            @(80.0, 80.0), @(120.0, 80.0), @(120.0, 90.0), @(80.0, 90.0)
        )
        $roofUpper = @(
            @(80.0, 110.0), @(120.0, 110.0), @(120.0, 120.0), @(80.0, 120.0)
        )
        Add-Prism $lines $vertices $stem 0.0 20.0 'stem'
        Add-Prism $lines $vertices $roofCenter 20.0 24.0 'stepped_roof_center'
        Add-Prism $lines $vertices $roofLower 20.0 24.0 'stepped_roof_lower'
        Add-Prism $lines $vertices $roofUpper 20.0 24.0 'stepped_roof_upper'
    } else {
        $stem = @(
            @(98.0, 98.0), @(102.0, 98.0), @(102.0, 102.0), @(98.0, 102.0)
        )
        $roof = @(
            @(80.0, 98.0), @(120.0, 98.0), @(120.0, 102.0), @(80.0, 102.0)
        )
        Add-Prism $lines $vertices $stem 0.0 20.0 'narrow_stem'
        Add-Prism $lines $vertices $roof 20.0 24.0 'narrow_roof'
    }
    $lines | Set-Content -LiteralPath $path -Encoding ascii
}

function Token-Value([string] $line, [char] $token) {
    $match = [regex]::Match($line, "(?:^|\s)$token(-?(?:\d+(?:\.\d*)?|\.\d+))")
    if ($match.Success) { return [double]::Parse($match.Groups[1].Value, $Invariant) }
    return $null
}

function Analyze-SupportGeometry([string] $path) {
    $layer = -1
    $role = ''
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $e = 0.0
    $pointsByLayer = @{}
    $interfaceLength = 0.0
    $nonFinite = 0

    foreach ($line in [IO.File]::ReadLines($path)) {
        if ($line -eq ';LAYER_CHANGE') { ++$layer; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -eq 'M82') { $relativeExtrusion = $false; continue }
        if ($line -eq 'M83') { $relativeExtrusion = $true; continue }
        if ($line -notmatch '^G[01](?:\s|$)') { continue }

        $tx = Token-Value $line 'X'
        $ty = Token-Value $line 'Y'
        $te = Token-Value $line 'E'
        $nextX = if ($null -eq $tx) { $x } elseif ($absoluteXY -or $null -eq $x) { $tx } else { $x + $tx }
        $nextY = if ($null -eq $ty) { $y } elseif ($absoluteXY -or $null -eq $y) { $ty } else { $y + $ty }
        $extruding = $false
        if ($null -ne $te) {
            $deltaE = if ($relativeExtrusion) { $te } else { $te - $e }
            $extruding = $deltaE -gt 0.000001
            $e = if ($relativeExtrusion) { $e + $te } else { $te }
        }

        if ($extruding -and $null -ne $x -and $null -ne $y -and $null -ne $nextX -and $null -ne $nextY) {
            $length = [Math]::Sqrt(($nextX - $x) * ($nextX - $x) + ($nextY - $y) * ($nextY - $y))
            if ([double]::IsNaN($length) -or [double]::IsInfinity($length)) {
                ++$nonFinite
            } elseif ($role -eq 'Support') {
                if (-not $pointsByLayer.ContainsKey($layer)) {
                    $pointsByLayer[$layer] = [Collections.Generic.List[object]]::new()
                }
                $pointsByLayer[$layer].Add(@($x, $y))
                $pointsByLayer[$layer].Add(@($nextX, $nextY))
            } elseif ($role -match '(?i)support interface') {
                $interfaceLength += $length
            }
        }
        if ($null -ne $nextX) { $x = $nextX }
        if ($null -ne $nextY) { $y = $nextY }
    }

    $extents = foreach ($entry in $pointsByLayer.GetEnumerator()) {
        $xs = @($entry.Value | ForEach-Object { [double] $_[0] })
        $ys = @($entry.Value | ForEach-Object { [double] $_[1] })
        [pscustomobject]@{
            Layer = [int] $entry.Key
            MinX = ($xs | Measure-Object -Minimum).Minimum
            MaxX = ($xs | Measure-Object -Maximum).Maximum
            MinY = ($ys | Measure-Object -Minimum).Minimum
            MaxY = ($ys | Measure-Object -Maximum).Maximum
            WidthX = ($xs | Measure-Object -Maximum).Maximum - ($xs | Measure-Object -Minimum).Minimum
            WidthY = ($ys | Measure-Object -Maximum).Maximum - ($ys | Measure-Object -Minimum).Minimum
        }
    }
    $extents = @($extents | Sort-Object Layer)
    $stable = @($extents | Where-Object { $_.Layer -ge 5 -and $_.Layer -le 90 })
    if ($stable.Count -eq 0) { $stable = $extents }
    [pscustomobject]@{
        SupportLayers = $extents.Count
        InterfaceLength = [Math]::Round($interfaceLength, 3)
        MinWidthX = if ($stable) { [Math]::Round(($stable.WidthX | Measure-Object -Minimum).Minimum, 3) } else { 0 }
        MaxWidthX = if ($stable) { [Math]::Round(($stable.WidthX | Measure-Object -Maximum).Maximum, 3) } else { 0 }
        MinWidthY = if ($stable) { [Math]::Round(($stable.WidthY | Measure-Object -Minimum).Minimum, 3) } else { 0 }
        MaxWidthY = if ($stable) { [Math]::Round(($stable.WidthY | Measure-Object -Maximum).Maximum, 3) } else { 0 }
        NonFinite = $nonFinite
        Extents = $extents
    }
}

$filaments = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()

foreach ($case in @(
    [pscustomobject]@{ Name='curved'; TargetX=60.0; TargetY=60.0 },
    [pscustomobject]@{ Name='stepped'; TargetX=60.0; TargetY=40.0 },
    [pscustomobject]@{ Name='narrow'; TargetX=40.0; TargetY=4.0 }
)) {
    $caseRoot = Join-Path $runRoot $case.Name
    New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
    $modelPath = Join-Path $caseRoot "$($case.Name).obj"
    $machinePath = Join-Path $caseRoot 'machine.json'
    $processPath = Join-Path $caseRoot 'process.json'
    Write-GeometryModel $modelPath $case.Name

    $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
    $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
    $machineName = "Codex Cura geometry $($case.Name)"
    $machine.name = $machineName
    $machine.setting_id = "cura-geometry-$($case.Name)"
    $process.name = $machineName
    $process.setting_id = "cura-geometry-$($case.Name)"
    $process.compatible_printers = @($machineName)
    Set-JsonProperty $process 'layer_height' '0.2'
    Set-JsonProperty $process 'initial_layer_print_height' '0.2'
    Set-JsonProperty $process 'enable_support' '1'
    Set-JsonProperty $process 'support_type' 'normal_cura(auto)'
    Set-JsonProperty $process 'support_threshold_angle' '90'
    Set-JsonProperty $process 'support_interface_top_layers' '4'
    Set-JsonProperty $process 'support_interface_bottom_layers' '0'
    Set-JsonProperty $process 'support_interface_pattern' 'rectilinear'
    Set-JsonProperty $process 'support_interface_spacing' '0'
    Set-JsonProperty $process 'support_wall_loops' '1'
    Set-JsonProperty $process 'support_expansion' '0'
    Set-JsonProperty $process 'support_on_build_plate_only' '0'
    Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '0'
    $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding utf8
    $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding utf8

    $settings = [char]34 + "$machinePath;$processPath" + [char]34
    $loadedFilaments = [char]34 + ($filaments -join ';') + [char]34
    $stdout = Join-Path $caseRoot 'cli.out.log'
    $stderr = Join-Path $caseRoot 'cli.err.log'
    $arguments = @(
        '--slice','0','--debug','1',
        '--load-settings',$settings,
        '--load-filaments',$loadedFilaments,
        '--outputdir',([char]34 + $caseRoot + [char]34),
        ([char]34 + $modelPath + [char]34)
    )
    $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
        -RedirectStandardOutput $stdout -RedirectStandardError $stderr
    if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
        $handle.Kill()
        throw "Slice timed out: $($case.Name)"
    }
    $handle.Refresh()
    $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
    $errors = [Collections.Generic.List[string]]::new()
    if ($null -eq $gcode) {
        $errors.Add('G-code was not generated')
        $analysis = $null
    } else {
        $analysis = Analyze-SupportGeometry $gcode.FullName
        if ($analysis.SupportLayers -lt 50) { $errors.Add("Too few support layers: $($analysis.SupportLayers)") }
        if ($analysis.InterfaceLength -le 0) { $errors.Add('Support interface was not generated') }
        if ($analysis.NonFinite -gt 0) { $errors.Add("Non-finite support geometry: $($analysis.NonFinite)") }
        if ($analysis.MaxWidthX -gt $case.TargetX + 1.5) { $errors.Add("X boundary protrudes: $($analysis.MaxWidthX) mm") }
        if ($analysis.MaxWidthY -gt $case.TargetY + 1.5) { $errors.Add("Y boundary protrudes: $($analysis.MaxWidthY) mm") }
        if ($analysis.MaxWidthX - $analysis.MinWidthX -gt 1.5) {
            $errors.Add("X boundary changes between straight support layers: $($analysis.MinWidthX)-$($analysis.MaxWidthX) mm")
        }
        if ($analysis.MaxWidthY - $analysis.MinWidthY -gt 1.5) {
            $errors.Add("Y boundary changes between straight support layers: $($analysis.MinWidthY)-$($analysis.MaxWidthY) mm")
        }
    }
    $results.Add([pscustomobject]@{
        Case = $case.Name
        Passed = $errors.Count -eq 0
        SupportLayers = if ($analysis) { $analysis.SupportLayers } else { 0 }
        InterfaceLength = if ($analysis) { $analysis.InterfaceLength } else { 0 }
        WidthX = if ($analysis) { "$($analysis.MinWidthX)-$($analysis.MaxWidthX)" } else { '' }
        WidthY = if ($analysis) { "$($analysis.MinWidthY)-$($analysis.MaxWidthY)" } else { '' }
        Errors = $errors -join '; '
        Gcode = if ($gcode) { $gcode.FullName } else { '' }
    })
}

$resultsPath = Join-Path $runRoot 'results.json'
$results | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath $resultsPath -Encoding utf8
$results | Format-Table Case,Passed,SupportLayers,InterfaceLength,WidthX,WidthY,Errors -AutoSize
"RESULTS=$resultsPath"
$failed = @($results | Where-Object { -not $_.Passed })
"PASSED=$($results.Count - $failed.Count) FAILED=$($failed.Count)"
if ($failed.Count -gt 0) { exit 1 }
