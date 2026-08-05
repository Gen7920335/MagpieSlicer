param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [int] $SliceTimeoutSeconds = 180,
    [int] $ImageSize = 3000,
    [double] $BottomClearance = 0
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\cura-torus-overlay'
}

$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

Add-Type -AssemblyName System.Drawing
Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

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

function Write-TorusObj([string] $path, [double] $bottomClearance) {
    $majorSegments = 128
    $minorSegments = 48
    $majorRadius = 30.0
    $minorRadius = 10.0
    $centerX = 100.0
    $centerY = 100.0
    $centerZ = 10.0 + $bottomClearance
    $lines = [Collections.Generic.List[string]]::new()
    $lines.Add('o cura_support_torus')

    for ($major = 0; $major -lt $majorSegments; ++$major) {
        $u = 2.0 * [Math]::PI * $major / $majorSegments
        for ($minor = 0; $minor -lt $minorSegments; ++$minor) {
            $v = 2.0 * [Math]::PI * $minor / $minorSegments
            $radial = $majorRadius + $minorRadius * [Math]::Cos($v)
            $x = $centerX + $radial * [Math]::Cos($u)
            $y = $centerY + $radial * [Math]::Sin($u)
            $z = $centerZ + $minorRadius * [Math]::Sin($v)
            $lines.Add("v $(Format-Number $x) $(Format-Number $y) $(Format-Number $z)")
        }
    }

    for ($major = 0; $major -lt $majorSegments; ++$major) {
        $nextMajor = ($major + 1) % $majorSegments
        for ($minor = 0; $minor -lt $minorSegments; ++$minor) {
            $nextMinor = ($minor + 1) % $minorSegments
            $a = $major * $minorSegments + $minor + 1
            $b = $nextMajor * $minorSegments + $minor + 1
            $c = $nextMajor * $minorSegments + $nextMinor + 1
            $d = $major * $minorSegments + $nextMinor + 1
            $lines.Add("f $a $b $c")
            $lines.Add("f $a $c $d")
        }
    }
    if ($bottomClearance -gt 0) {
        $anchorBase = $majorSegments * $minorSegments
        $lines.Add('v 50 50 0')
        $lines.Add('v 50.4 50 0')
        $lines.Add('v 50.4 50.4 0')
        $lines.Add('v 50 50.4 0')
        $lines.Add("v 50 50 $(Format-Number $bottomClearance)")
        $lines.Add("v 50.4 50 $(Format-Number $bottomClearance)")
        $lines.Add("v 50.4 50.4 $(Format-Number $bottomClearance)")
        $lines.Add("v 50 50.4 $(Format-Number $bottomClearance)")
        $a = $anchorBase + 1
        $b = $anchorBase + 2
        $c = $anchorBase + 3
        $d = $anchorBase + 4
        $e = $anchorBase + 5
        $f = $anchorBase + 6
        $g = $anchorBase + 7
        $h = $anchorBase + 8
        @(
            "f $a $c $b", "f $a $d $c",
            "f $e $f $g", "f $e $g $h",
            "f $a $b $f", "f $a $f $e",
            "f $b $c $g", "f $b $g $f",
            "f $c $d $h", "f $c $h $g",
            "f $d $a $e", "f $d $e $h"
        ) | ForEach-Object { $lines.Add($_) }
    }
    $lines | Set-Content -LiteralPath $path -Encoding ascii
}

function Write-TorusProject([string] $path, [string] $workDirectory, [double] $bottomClearance) {
    $templatePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\gui_matrix_20260722\classic_wc1.3mf'
    if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
        throw "3MF template not found: $templatePath"
    }

    $projectRoot = Join-Path $workDirectory 'project-source'
    New-Item -ItemType Directory -Force -Path $projectRoot | Out-Null
    [IO.Compression.ZipFile]::ExtractToDirectory($templatePath, $projectRoot)

    $majorSegments = 128
    $minorSegments = 48
    $majorRadius = 30.0
    $minorRadius = 10.0
    $modelLines = [Collections.Generic.List[string]]::new()
    $modelLines.Add('<?xml version="1.0" encoding="UTF-8"?>')
    $modelLines.Add('<model unit="millimeter" xml:lang="en-US" xmlns="http://schemas.microsoft.com/3dmanufacturing/core/2015/02" xmlns:BambuStudio="http://schemas.bambulab.com/package/2021" xmlns:p="http://schemas.microsoft.com/3dmanufacturing/production/2015/06" requiredextensions="p">')
    $modelLines.Add(' <metadata name="BambuStudio:3mfVersion">1</metadata>')
    $modelLines.Add(' <resources>')
    $modelLines.Add('  <object id="65537" p:UUID="00010000-81cb-4c03-9d28-80fed5dfa1dc" type="model">')
    $modelLines.Add('   <mesh>')
    $modelLines.Add('    <vertices>')
    for ($major = 0; $major -lt $majorSegments; ++$major) {
        $u = 2.0 * [Math]::PI * $major / $majorSegments
        for ($minor = 0; $minor -lt $minorSegments; ++$minor) {
            $v = 2.0 * [Math]::PI * $minor / $minorSegments
            $radial = $majorRadius + $minorRadius * [Math]::Cos($v)
            $x = $radial * [Math]::Cos($u)
            $y = $radial * [Math]::Sin($u)
            $z = $minorRadius * [Math]::Sin($v)
            $modelLines.Add("     <vertex x=`"$(Format-Number $x)`" y=`"$(Format-Number $y)`" z=`"$(Format-Number $z)`"/>")
        }
    }
    $modelLines.Add('    </vertices>')
    $modelLines.Add('    <triangles>')
    for ($major = 0; $major -lt $majorSegments; ++$major) {
        $nextMajor = ($major + 1) % $majorSegments
        for ($minor = 0; $minor -lt $minorSegments; ++$minor) {
            $nextMinor = ($minor + 1) % $minorSegments
            $a = $major * $minorSegments + $minor
            $b = $nextMajor * $minorSegments + $minor
            $c = $nextMajor * $minorSegments + $nextMinor
            $d = $major * $minorSegments + $nextMinor
            $modelLines.Add("     <triangle v1=`"$a`" v2=`"$b`" v3=`"$c`"/>")
            $modelLines.Add("     <triangle v1=`"$a`" v2=`"$c`" v3=`"$d`"/>")
        }
    }
    $modelLines.Add('    </triangles>')
    $modelLines.Add('   </mesh>')
    $modelLines.Add('  </object>')
    $modelLines.Add(' </resources>')
    $modelLines.Add(' <build/>')
    $modelLines.Add('</model>')
    $objectModelPath = Join-Path $projectRoot '3D\Objects\OrcaCube_v2.drc_1.model'
    $utf8WithoutBom = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllText($objectModelPath, ($modelLines -join "`n") + "`n", $utf8WithoutBom)

    $centerZ = 10.0 + $bottomClearance
    $centerZText = Format-Number $centerZ
    $rootModelPath = Join-Path $projectRoot '3D\3dmodel.model'
    $rootModel = Get-Content -LiteralPath $rootModelPath -Raw
    $rootModel = $rootModel.Replace(
        'transform="1 0 0 0 1 0 0 0 1 135.5 136 15"',
        "transform=`"1 0 0 0 1 0 0 0 1 135.5 136 $centerZText`"")
    [IO.File]::WriteAllText($rootModelPath, $rootModel, $utf8WithoutBom)

    $modelSettingsPath = Join-Path $projectRoot 'Metadata\model_settings.config'
    $modelSettings = Get-Content -LiteralPath $modelSettingsPath -Raw
    $modelSettings = $modelSettings.Replace(
        '<metadata key="source_offset_z" value="15"/>',
        "<metadata key=`"source_offset_z`" value=`"$centerZText`"/>")
    $modelSettings = $modelSettings.Replace(
        'transform="1 0 0 0 1 0 0 0 1 0 0 15"',
        "transform=`"1 0 0 0 1 0 0 0 1 0 0 $centerZText`"")
    [IO.File]::WriteAllText($modelSettingsPath, $modelSettings, $utf8WithoutBom)

    [IO.Compression.ZipFile]::CreateFromDirectory(
        $projectRoot,
        $path,
        [IO.Compression.CompressionLevel]::Optimal,
        $false)
}

function Set-ProjectBottomClearance([string] $path, [double] $bottomClearance) {
    $utf8WithoutBom = [Text.UTF8Encoding]::new($false)
    $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Update)
    try {
        $rootEntry = $zip.Entries |
            Where-Object { $_.FullName -match '(^|[\\/])3dmodel\.model$' -and $_.FullName -notmatch 'Objects' } |
            Select-Object -First 1
        if ($null -eq $rootEntry) { throw 'Root 3MF model entry was not found' }
        $reader = [IO.StreamReader]::new($rootEntry.Open())
        try { $rootModel = $reader.ReadToEnd() } finally { $reader.Dispose() }
        $clearanceText = Format-Number $bottomClearance
        $rootModel = [regex]::Replace(
            $rootModel,
            '(<item\b[^>]*\btransform=")([^"]+)("[^>]*>)',
            {
                param($match)
                $values = @($match.Groups[2].Value -split '\s+')
                if ($values.Count -ne 12) { throw "Unexpected build transform: $($match.Groups[2].Value)" }
                $values[11] = $clearanceText
                $match.Groups[1].Value + ($values -join ' ') + $match.Groups[3].Value
            },
            1)
        $rootModel = $rootModel.Replace('auto_drop="1"', 'auto_drop="0"')
        $rootEntryName = $rootEntry.FullName
        $rootEntry.Delete()
        $newRootEntry = $zip.CreateEntry($rootEntryName, [IO.Compression.CompressionLevel]::Optimal)
        $writer = [IO.StreamWriter]::new($newRootEntry.Open(), $utf8WithoutBom)
        try { $writer.Write($rootModel) } finally { $writer.Dispose() }

        $settingsEntry = $zip.Entries |
            Where-Object { $_.FullName -match 'model_settings\.config$' } |
            Select-Object -First 1
        if ($null -ne $settingsEntry) {
            $reader = [IO.StreamReader]::new($settingsEntry.Open())
            try { $modelSettings = $reader.ReadToEnd() } finally { $reader.Dispose() }
            $modelSettings = [regex]::Replace(
                $modelSettings,
                '(<assemble_item\b[^>]*\btransform=")([^"]+)(")',
                {
                    param($match)
                    $values = @($match.Groups[2].Value -split '\s+')
                    if ($values.Count -eq 12) { $values[11] = $clearanceText }
                    $match.Groups[1].Value + ($values -join ' ') + $match.Groups[3].Value
                },
                1)
            $settingsEntryName = $settingsEntry.FullName
            $settingsEntry.Delete()
            $newSettingsEntry = $zip.CreateEntry($settingsEntryName, [IO.Compression.CompressionLevel]::Optimal)
            $writer = [IO.StreamWriter]::new($newSettingsEntry.Open(), $utf8WithoutBom)
            try { $writer.Write($modelSettings) } finally { $writer.Dispose() }
        }
    } finally {
        $zip.Dispose()
    }
}

function Token-Value([string] $line, [char] $token) {
    $match = [regex]::Match($line, "(?:^|\s)$token(-?(?:\d+(?:\.\d*)?|\.\d+))")
    if ($match.Success) { return [double]::Parse($match.Groups[1].Value, $Invariant) }
    return $null
}

function Read-SupportBody([string] $path) {
    $layer = -1
    $role = ''
    $absoluteXY = $true
    $relativeExtrusion = $false
    $x = $null
    $y = $null
    $e = 0.0
    $segments = [Collections.Generic.List[object]]::new()
    $layers = [Collections.Generic.HashSet[int]]::new()
    $interfaceSegments = 0

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
            if ($role -eq 'Support') {
                $segments.Add([pscustomobject]@{
                    Layer = $layer
                    X1 = [double] $x
                    Y1 = [double] $y
                    X2 = [double] $nextX
                    Y2 = [double] $nextY
                })
                [void] $layers.Add($layer)
            } elseif ($role -match '(?i)support interface') {
                ++$interfaceSegments
            }
        }
        if ($null -ne $nextX) { $x = $nextX }
        if ($null -ne $nextY) { $y = $nextY }
    }

    [pscustomobject]@{
        Segments = $segments
        Layers = @($layers | Sort-Object)
        InterfaceSegmentsExcluded = $interfaceSegments
    }
}

function Blend-Color([Drawing.Color] $low, [Drawing.Color] $high, [double] $ratio, [int] $alpha) {
    $ratio = [Math]::Max(0.0, [Math]::Min(1.0, $ratio))
    [Drawing.Color]::FromArgb(
        $alpha,
        [int] [Math]::Round($low.R + ($high.R - $low.R) * $ratio),
        [int] [Math]::Round($low.G + ($high.G - $low.G) * $ratio),
        [int] [Math]::Round($low.B + ($high.B - $low.B) * $ratio)
    )
}

function Render-Overlay($support, [string] $path, [int] $size) {
    if ($support.Segments.Count -eq 0) { throw 'No Cura support-body segments were found' }
    $allX = @($support.Segments | ForEach-Object { $_.X1; $_.X2 })
    $allY = @($support.Segments | ForEach-Object { $_.Y1; $_.Y2 })
    $minX = ($allX | Measure-Object -Minimum).Minimum
    $maxX = ($allX | Measure-Object -Maximum).Maximum
    $minY = ($allY | Measure-Object -Minimum).Minimum
    $maxY = ($allY | Measure-Object -Maximum).Maximum
    $rangeX = [Math]::Max(0.001, $maxX - $minX)
    $rangeY = [Math]::Max(0.001, $maxY - $minY)

    $bitmap = [Drawing.Bitmap]::new($size, $size, [Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([Drawing.Color]::FromArgb(15, 18, 22))

    $titleFont = [Drawing.Font]::new('Segoe UI', 42, [Drawing.FontStyle]::Bold)
    $labelFont = [Drawing.Font]::new('Segoe UI', 24, [Drawing.FontStyle]::Regular)
    $smallFont = [Drawing.Font]::new('Segoe UI', 20, [Drawing.FontStyle]::Regular)
    $white = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(238, 242, 248))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(170, 180, 195))
    $graphics.DrawString('Cura-style torus support overlay', $titleFont, $white, 100, 65)
    $graphics.DrawString(
        "$($support.Layers.Count) support-body layers overlaid | interface excluded",
        $labelFont, $muted, 104, 130)

    $rect = [Drawing.RectangleF]::new(100, 220, $size - 200, $size - 470)
    $panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(25, 29, 35))
    $panelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(58, 66, 78), 2)
    $graphics.FillRectangle($panelBrush, $rect)
    $graphics.DrawRectangle($panelPen, $rect.X, $rect.Y, $rect.Width, $rect.Height)
    $scale = [Math]::Min(($rect.Width - 150) / $rangeX, ($rect.Height - 150) / $rangeY)
    $drawingWidth = $rangeX * $scale
    $drawingHeight = $rangeY * $scale
    $originX = $rect.X + ($rect.Width - $drawingWidth) / 2
    $originY = $rect.Y + ($rect.Height - $drawingHeight) / 2
    $minLayer = ($support.Layers | Measure-Object -Minimum).Minimum
    $maxLayer = ($support.Layers | Measure-Object -Maximum).Maximum
    $layerRange = [Math]::Max(1, $maxLayer - $minLayer)
    $lowColor = [Drawing.Color]::FromArgb(33, 210, 205)
    $highColor = [Drawing.Color]::FromArgb(255, 181, 54)

    foreach ($layer in $support.Layers) {
        $ratio = ($layer - $minLayer) / $layerRange
        $color = Blend-Color $lowColor $highColor $ratio 28
        $pen = [Drawing.Pen]::new($color, [float] [Math]::Max(1.2, 0.46 * $scale))
        $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round
        $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round
        foreach ($segment in @($support.Segments | Where-Object Layer -eq $layer)) {
            $px1 = $originX + ($segment.X1 - $minX) * $scale
            $py1 = $originY + ($maxY - $segment.Y1) * $scale
            $px2 = $originX + ($segment.X2 - $minX) * $scale
            $py2 = $originY + ($maxY - $segment.Y2) * $scale
            $graphics.DrawLine($pen, [float] $px1, [float] $py1, [float] $px2, [float] $py2)
        }
        $pen.Dispose()
    }

    $legendY = $size - 175
    $legendPen = [Drawing.Pen]::new($lowColor, 18)
    $graphics.DrawLine($legendPen, 115, $legendY + 13, 210, $legendY + 13)
    $graphics.DrawString("Low support layer $minLayer", $smallFont, $muted, 235, $legendY - 8)
    $legendPen.Dispose()
    $legendPen = [Drawing.Pen]::new($highColor, 18)
    $graphics.DrawLine($legendPen, 950, $legendY + 13, 1045, $legendY + 13)
    $graphics.DrawString("High support layer $maxLayer", $smallFont, $muted, 1070, $legendY - 8)
    $graphics.DrawString(
        "$($support.InterfaceSegmentsExcluded) interface extrusion segments excluded",
        $smallFont, $muted, $size - 930, $legendY - 8)
    $legendPen.Dispose()

    $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $smallFont.Dispose()
    $labelFont.Dispose()
    $titleFont.Dispose()
    $white.Dispose()
    $muted.Dispose()
    $panelBrush.Dispose()
    $panelPen.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$sourceObjPath = Join-Path $runRoot 'torus.obj'
$modelPath = $sourceObjPath
$machinePath = Join-Path $runRoot 'machine.json'
$processPath = Join-Path $runRoot 'process.json'
$stdout = Join-Path $runRoot 'cli.out.log'
$stderr = Join-Path $runRoot 'cli.err.log'
$overlayPath = Join-Path $runRoot 'cura-torus-support-body-overlay.png'
Write-TorusObj $sourceObjPath $BottomClearance

$machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
$process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
$machineName = 'Codex Cura torus support overlay'
$machine.name = $machineName
$machine.setting_id = 'cura-torus-overlay'
$process.name = $machineName
$process.setting_id = 'cura-torus-overlay'
$process.compatible_printers = @($machineName)
Set-JsonProperty $process 'layer_height' '0.2'
Set-JsonProperty $process 'initial_layer_print_height' '0.2'
Set-JsonProperty $process 'enable_support' '1'
Set-JsonProperty $process 'support_type' 'normal_cura(auto)'
Set-JsonProperty $process 'support_threshold_angle' '70'
Set-JsonProperty $process 'support_interface_top_layers' '4'
Set-JsonProperty $process 'support_interface_bottom_layers' '0'
Set-JsonProperty $process 'support_interface_pattern' 'rectilinear'
Set-JsonProperty $process 'support_interface_spacing' '0'
Set-JsonProperty $process 'support_base_pattern' 'default'
Set-JsonProperty $process 'tree_support_wall_count' '0'
Set-JsonProperty $process 'support_expansion' '0'
Set-JsonProperty $process 'support_on_build_plate_only' '0'
Set-JsonProperty $process 'use_smaller_nozzles_in_crisp_corners' '0'
$machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding utf8
$process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding utf8

$filaments = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)
$settings = [char]34 + "$machinePath;$processPath" + [char]34
$loadedFilaments = [char]34 + ($filaments -join ';') + [char]34
$arguments = @(
    '--slice','0','--debug','1',
    '--load-settings',$settings,
    '--load-filaments',$loadedFilaments,
    '--outputdir',([char]34 + $runRoot + [char]34),
    ([char]34 + $modelPath + [char]34)
)
$handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr
if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
    $handle.Kill()
    throw "Slice timed out after $SliceTimeoutSeconds seconds"
}
$handle.Refresh()
$gcode = Get-ChildItem -LiteralPath $runRoot -File -Filter '*.gcode' | Select-Object -First 1
if ($null -eq $gcode) {
    $errorText = Get-Content -LiteralPath $stderr -Raw
    throw "G-code was not generated. $errorText"
}

$support = Read-SupportBody $gcode.FullName
if ($BottomClearance -ge 20 -and $support.Layers.Count -lt 80) {
    throw "Floating model was not preserved: only $($support.Layers.Count) support-body layers were generated"
}
Render-Overlay $support $overlayPath $ImageSize
$result = [pscustomobject]@{
    Passed = $support.Segments.Count -gt 0
    SupportBodyLayers = $support.Layers.Count
    SupportBodySegments = $support.Segments.Count
    InterfaceSegmentsExcluded = $support.InterfaceSegmentsExcluded
    Angle = 70
    BottomClearance = $BottomClearance
    Gcode = $gcode.FullName
    Model = $modelPath
    Image = $overlayPath
}
$resultPath = Join-Path $runRoot 'results.json'
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding utf8
$result | Format-List
"RESULTS=$resultPath"
