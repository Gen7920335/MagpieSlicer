param(
    [string]$SlicerPath,
    [string]$OutputRoot,
    [string]$ModelPath,
    [int]$SliceTimeoutSeconds = 180
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($SlicerPath)) { $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe' }
if ([string]::IsNullOrWhiteSpace($OutputRoot)) { $OutputRoot = Join-Path $RepoRoot 'build\verification\hotend-filament-matrix' }
if ([string]::IsNullOrWhiteSpace($ModelPath)) { $ModelPath = Join-Path $RepoRoot 'tests\data\test_stl\ASCII\20mmbox-LF.stl' }

$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $ModelPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

function Find-FilamentProfile([string]$Name) {
    foreach ($root in @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$Name.json" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $Name"
}

function New-FilamentProfileIndex {
    $index = @{}
    $roots = @((Join-Path $RepoRoot 'resources\profiles'), (Join-Path $env:APPDATA 'MagpieSlicer\system'))
    foreach ($root in $roots) {
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        foreach ($file in Get-ChildItem -LiteralPath $root -Recurse -File -Filter '*.json' -ErrorAction SilentlyContinue) {
            if (-not $index.ContainsKey($file.BaseName)) { $index[$file.BaseName] = $file.FullName }
        }
    }
    return $index
}

function Resolve-FilamentProfile([string]$Path, [hashtable]$Index, [Collections.Generic.HashSet[string]]$Stack) {
    $resolvedPath = (Resolve-Path -LiteralPath $Path).Path
    if (-not $Stack.Add($resolvedPath)) { throw "Filament inheritance cycle: $resolvedPath" }

    $profile = Get-Content -LiteralPath $resolvedPath -Raw | ConvertFrom-Json
    $merged = [ordered]@{}
    $parentName = [string]$profile.inherits
    if (-not [string]::IsNullOrWhiteSpace($parentName)) {
        if (-not $Index.ContainsKey($parentName)) { throw "Filament parent not found: $parentName (from $resolvedPath)" }
        $parent = Resolve-FilamentProfile $Index[$parentName] $Index $Stack
        foreach ($key in $parent.Keys) { $merged[$key] = $parent[$key] }
    }
    foreach ($property in $profile.PSObject.Properties) {
        if ($property.Name -ne 'inherits') { $merged[$property.Name] = $property.Value }
    }
    [void]$Stack.Remove($resolvedPath)
    return $merged
}

function Set-Property($Object, [string]$Name, $Value) {
    if ($null -eq $Object.PSObject.Properties[$Name]) { $Object | Add-Member -NotePropertyName $Name -NotePropertyValue $Value }
    else { $Object.$Name = $Value }
}

function Set-ToolVector($Object, [string]$Name, [string]$FirstValue, [string]$OtherValue, [int]$ToolCount) {
    $values = @($FirstValue)
    for ($i = 1; $i -lt $ToolCount; ++$i) { $values += $OtherValue }
    Set-Property $Object $Name $values
}

function Parse-FirstNumber([string]$Value) {
    if ($Value -match '-?(?:\d+(?:\.\d*)?|\.\d+)') { return [double]::Parse($Matches[0], $Invariant) }
    return [double]::NaN
}

function Get-Median([Collections.Generic.List[double]]$Values) {
    if ($Values.Count -eq 0) { return [double]::NaN }
    $ordered = @($Values.ToArray() | Sort-Object)
    $middle = [int][Math]::Floor($ordered.Count / 2)
    if (($ordered.Count % 2) -eq 1) { return [double]$ordered[$middle] }
    return ([double]$ordered[$middle - 1] + [double]$ordered[$middle]) / 2.0
}

function Analyze-Gcode([string]$Path) {
    $settings = @{}
    $role = ''
    $tool = -1
    $absoluteXY = $true
    $relativeE = $false
    $x = $null; $y = $null; $e = 0.0
    $widths = [Collections.Generic.List[double]]::new()
    $nonFinite = 0
    $positiveExtrusions = 0
    $filamentArea = [Math]::PI * 1.75 * 1.75 / 4.0
    $layerHeight = 0.08

    foreach ($line in [IO.File]::ReadLines($Path)) {
        if ($line -match '^; ([a-z][a-z0-9_]*) = (.*)$') { $settings[$Matches[1]] = $Matches[2].Trim(); continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^T(\d+)(?:\s|$)') { $tool = [int]$Matches[1]; continue }
        if ($line -eq 'G90') { $absoluteXY = $true; continue }
        if ($line -eq 'G91') { $absoluteXY = $false; continue }
        if ($line -match '^M82(?:\s|$)') { $relativeE = $false; continue }
        if ($line -match '^M83(?:\s|$)') { $relativeE = $true; continue }
        if ($line -match '^G92(?:\s|$)' -and $line -match '(?:^|\s)E(-?(?:\d+(?:\.\d*)?|\.\d+))') { $e = [double]::Parse($Matches[1], $Invariant); continue }
        if ($line -notmatch '^G[0123](?:\s|$)') { continue }
        if ($line -match '(?i)(?:^|\s)[XYZEIJKRF][+-]?(?:nan|inf(?:inity)?)(?=\s|;|$)') { $nonFinite++; continue }

        $newX = $x; $newY = $y; $newE = $e; $hasX = $false; $hasY = $false; $hasE = $false
        foreach ($token in [regex]::Matches($line, '(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))')) {
            $axis = $token.Groups[1].Value; $value = [double]::Parse($token.Groups[2].Value, $Invariant)
            switch ($axis) {
                'X' { $newX = if ($absoluteXY -or $null -eq $x) { $value } else { $x + $value }; $hasX = $true }
                'Y' { $newY = if ($absoluteXY -or $null -eq $y) { $value } else { $y + $value }; $hasY = $true }
                'E' { $newE = $value; $hasE = $true }
            }
        }
        $extrusion = if (-not $hasE) { 0.0 } elseif ($relativeE) { $newE } else { $newE - $e }
        if ($extrusion -gt 0.0000001) {
            $positiveExtrusions++
            if ($tool -eq 0 -and $role -in @('Outer wall','Inner wall') -and $null -ne $x -and $null -ne $y -and ($hasX -or $hasY)) {
                $length = [Math]::Sqrt(($newX - $x) * ($newX - $x) + ($newY - $y) * ($newY - $y))
                if ($length -ge 0.5 -and $length -le 30.0 -and $widths.Count -lt 10000) {
                    $area = $extrusion * $filamentArea / $length
                    $width = $area / $layerHeight + $layerHeight * (1.0 - [Math]::PI / 4.0)
                    if ($width -gt 0.05 -and $width -lt 2.0) { $widths.Add($width) }
                }
            }
        }
        if ($hasX) { $x = $newX }; if ($hasY) { $y = $newY }; if ($hasE) { $e = $newE }
    }

    return [pscustomobject]@{
        Settings = $settings
        MedianWallWidth = Get-Median $widths
        WallWidthSamples = $widths.Count
        NonFinite = $nonFinite
        PositiveExtrusions = $positiveExtrusions
    }
}

$nozzles = @(
    [pscustomobject]@{ Diameter = 0.15; Width = 0.17 },
    [pscustomobject]@{ Diameter = 0.20; Width = 0.22 },
    [pscustomobject]@{ Diameter = 0.40; Width = 0.45 },
    [pscustomobject]@{ Diameter = 0.60; Width = 0.66 },
    [pscustomobject]@{ Diameter = 0.80; Width = 0.88 },
    [pscustomobject]@{ Diameter = 1.00; Width = 1.10 }
)
$profileIndex = New-FilamentProfileIndex
$materials = @(
    [pscustomobject]@{ Type = 'PLA'; Name = 'Snapmaker PLA @U1' },
    [pscustomobject]@{ Type = 'PETG'; Name = 'Snapmaker PETG @U1' },
    [pscustomobject]@{ Type = 'ABS'; Name = 'Snapmaker ABS @U1' },
    [pscustomobject]@{ Type = 'TPU'; Name = 'Snapmaker TPU @U1' },
    [pscustomobject]@{ Type = 'ASA'; Name = 'Snapmaker ASA @U1' }
)
foreach ($material in $materials) {
    $sourcePath = Find-FilamentProfile $material.Name
    $flat = Resolve-FilamentProfile $sourcePath $profileIndex ([Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase))
    $flat['filament_type'] = @($material.Type)
    $material | Add-Member -NotePropertyName FlatConfig -NotePropertyValue $flat
    $material | Add-Member -NotePropertyName Temperature -NotePropertyValue (Parse-FirstNumber ([string]($flat['nozzle_temperature'] -join ';')))
    $material | Add-Member -NotePropertyName MaxVolumetricSpeed -NotePropertyValue (Parse-FirstNumber ([string]($flat['filament_max_volumetric_speed'] -join ';')))
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$results = [Collections.Generic.List[object]]::new()
$caseIndex = 0

foreach ($nozzle in $nozzles) {
    foreach ($material in $materials) {
        $caseIndex++
        $caseName = ('{0:D2}_{1}_{2:0.00}mm' -f $caseIndex, $material.Type, $nozzle.Diameter)
        $caseRoot = Join-Path $runRoot $caseName
        New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
        $machinePath = Join-Path $caseRoot 'machine.json'
        $processPath = Join-Path $caseRoot 'process.json'
        $filamentPath = Join-Path $caseRoot 'filament.json'
        $stdoutPath = Join-Path $caseRoot 'cli.out.log'
        $stderrPath = Join-Path $caseRoot 'cli.err.log'

        $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
        $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
        $toolCount = @($machine.nozzle_diameter).Count
        $diameterText = $nozzle.Diameter.ToString('0.##', $Invariant)
        $widthText = $nozzle.Width.ToString('0.##', $Invariant)
        Set-ToolVector $machine 'nozzle_diameter' $diameterText '0.4' $toolCount
        Set-ToolVector $machine 'min_layer_height' '0.04' '0.08' $toolCount
        Set-ToolVector $machine 'max_layer_height' (([Math]::Max(0.08, $nozzle.Diameter * 0.75)).ToString('0.###', $Invariant)) '0.32' $toolCount
        foreach ($key in @('toolhead_line_width','toolhead_initial_layer_line_width','toolhead_outer_wall_line_width','toolhead_inner_wall_line_width','toolhead_top_surface_line_width','toolhead_sparse_infill_line_width','toolhead_internal_solid_infill_line_width','toolhead_support_line_width','toolhead_bridge_line_width')) {
            Set-ToolVector $machine $key $widthText '0.45' $toolCount
        }
        Set-Property $process 'layer_height' '0.08'
        Set-Property $process 'initial_layer_print_height' '0.10'
        Set-Property $process 'use_smaller_nozzles_in_crisp_corners' '0'
        Set-Property $process 'wall_loops' '3'
        Set-Property $process 'sparse_infill_density' '15%'
        Set-Property $process 'sparse_infill_pattern' 'gyroid'

        $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
        $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8
        $material.FlatConfig | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $filamentPath -Encoding UTF8
        $settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
        $filamentsArg = [char]34 + ((1..$toolCount | ForEach-Object { $filamentPath }) -join ';') + [char]34
        $outputArg = [char]34 + $caseRoot + [char]34
        $modelArg = [char]34 + $ModelPath + [char]34
        $arguments = @('--slice','0','--debug','1','--load-settings',$settingsArg,'--load-filaments',$filamentsArg,'--outputdir',$outputArg,$modelArg)
        $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru -RedirectStandardOutput $stdoutPath -RedirectStandardError $stderrPath
        if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) { $handle.Kill(); throw "Slice timeout: $caseName" }
        $handle.WaitForExit()
        $handle.Refresh()
        $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
        $errors = [Collections.Generic.List[string]]::new()
        $exitCode = $handle.ExitCode
        if ($null -eq $exitCode) { $exitCode = if ($null -ne $gcode -and $gcode.Length -gt 0) { 0 } else { 1 } }
        if ($exitCode -ne 0) { $errors.Add("CLI exit $exitCode") }
        if ($null -eq $gcode -or $gcode.Length -le 0) { $errors.Add('G-code missing or empty') }

        $analysis = $null
        if ($null -ne $gcode -and $gcode.Length -gt 0) {
            $analysis = Analyze-Gcode $gcode.FullName
            if ($analysis.NonFinite -ne 0) { $errors.Add("Non-finite coordinates: $($analysis.NonFinite)") }
            if ($analysis.PositiveExtrusions -eq 0) { $errors.Add('No positive extrusion') }
            if ($analysis.WallWidthSamples -lt 10) { $errors.Add("Insufficient wall samples: $($analysis.WallWidthSamples)") }
            if ([double]::IsNaN($analysis.MedianWallWidth) -or [Math]::Abs($analysis.MedianWallWidth - $nozzle.Width) -gt [Math]::Max(0.05, $nozzle.Width * 0.25)) {
                $errors.Add("Wall width $($analysis.MedianWallWidth) does not match expected $($nozzle.Width)")
            }
            if (-not $analysis.Settings.ContainsKey('nozzle_diameter') -or [Math]::Abs((Parse-FirstNumber $analysis.Settings.nozzle_diameter) - $nozzle.Diameter) -gt 0.001) {
                $errors.Add('Serialized nozzle diameter mismatch')
            }
            if (-not $analysis.Settings.ContainsKey('filament_type') -or $analysis.Settings.filament_type -notmatch [regex]::Escape($material.Type)) {
                $errors.Add('Serialized filament type mismatch')
            }
            if (-not [double]::IsNaN($material.Temperature) -and
                (-not $analysis.Settings.ContainsKey('nozzle_temperature') -or [Math]::Abs((Parse-FirstNumber $analysis.Settings.nozzle_temperature) - $material.Temperature) -gt 0.001)) {
                $errors.Add('Serialized filament temperature mismatch')
            }
            if (-not [double]::IsNaN($material.MaxVolumetricSpeed) -and
                (-not $analysis.Settings.ContainsKey('filament_max_volumetric_speed') -or [Math]::Abs((Parse-FirstNumber $analysis.Settings.filament_max_volumetric_speed) - $material.MaxVolumetricSpeed) -gt 0.001)) {
                $errors.Add('Serialized filament volumetric speed mismatch')
            }
            if (-not $analysis.Settings.ContainsKey('toolhead_outer_wall_line_width') -or [Math]::Abs((Parse-FirstNumber $analysis.Settings.toolhead_outer_wall_line_width) - $nozzle.Width) -gt 0.001) {
                $errors.Add('Serialized hotend width mismatch')
            }
        }

        $results.Add([pscustomobject]@{
            Case = $caseName
            Nozzle = $nozzle.Diameter
            Material = $material.Type
            ExitCode = $exitCode
            GcodeBytes = if ($null -eq $gcode) { 0 } else { $gcode.Length }
            MedianWallWidth = if ($null -eq $analysis) { [double]::NaN } else { [Math]::Round($analysis.MedianWallWidth, 4) }
            Errors = ($errors -join '; ')
            Passed = $errors.Count -eq 0
        })
        Write-Output ("[{0}/30] {1}: {2}" -f $caseIndex, $caseName, $(if ($errors.Count -eq 0) { 'PASS' } else { 'FAIL ' + ($errors -join '; ') }))
    }
}

$csvPath = Join-Path $runRoot 'results.csv'
$jsonPath = Join-Path $runRoot 'results.json'
$results | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
$results | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$failed = @($results | Where-Object { -not $_.Passed })
Write-Output "RESULT total=$($results.Count) passed=$($results.Count-$failed.Count) failed=$($failed.Count) root=$runRoot"
if ($results.Count -ne 30 -or $failed.Count -ne 0) { exit 1 }
