param(
    [string] $SlicerPath,
    [string] $ModelPath,
    [string] $OutputRoot,
    [string] $ExistingGcode,
    [int] $ImageSize = 3000,
    [int] $SliceTimeoutSeconds = 300
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\orca-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($ModelPath)) {
    $ModelPath = Join-Path $RepoRoot 'resources\handy_models\Stanford_Bunny.drc'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\cura-bunny-support-top'
}

$SlicerPath = [IO.Path]::GetFullPath($SlicerPath)
$ModelPath = [IO.Path]::GetFullPath($ModelPath)
$OutputRoot = [IO.Path]::GetFullPath($OutputRoot)
$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $ModelPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Required file not found: $path"
    }
}

Add-Type -AssemblyName System.Drawing

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
        if ($line -eq ';LAYER_CHANGE') { $layer++; continue }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
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
        if ($line -notmatch '^G[01](?:\s|$)') { continue }

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
            if ($role -eq 'Support') {
                $segments.Add([pscustomobject]@{
                    Layer = $layer
                    X1 = [double] $x
                    Y1 = [double] $y
                    X2 = [double] $newX
                    Y2 = [double] $newY
                })
                [void] $layers.Add($layer)
            } elseif ($role -match '(?i)support interface') {
                $interfaceSegments++
            }
        }
        if ($hasX) { $x = $newX }
        if ($hasY) { $y = $newY }
        if ($hasE) { $e = $newE }
    }

    [pscustomobject]@{
        Segments = $segments
        Layers = @($layers | Sort-Object)
        InterfaceSegments = $interfaceSegments
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

function Render-SupportTop($support, [string] $path, [int] $size) {
    if ($support.Segments.Count -eq 0) { throw 'No Cura support-body extrusion was generated' }

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
    $graphics.Clear([Drawing.Color]::FromArgb(14, 17, 21))

    $titleFont = [Drawing.Font]::new('Segoe UI', 42, [Drawing.FontStyle]::Bold)
    $labelFont = [Drawing.Font]::new('Segoe UI', 24, [Drawing.FontStyle]::Regular)
    $smallFont = [Drawing.Font]::new('Segoe UI', 20, [Drawing.FontStyle]::Regular)
    $white = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(238, 242, 248))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(168, 179, 195))
    $graphics.DrawString('Stanford Bunny - Cura support top view', $titleFont, $white, 100, 60)
    $graphics.DrawString(
        "90 deg threshold | support body only | model, interface and travel excluded",
        $labelFont, $muted, 104, 130)

    $rect = [Drawing.RectangleF]::new(100, 220, $size - 200, $size - 470)
    $panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(24, 29, 35))
    $panelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(59, 68, 80), 2)
    $graphics.FillRectangle($panelBrush, $rect)
    $graphics.DrawRectangle($panelPen, $rect.X, $rect.Y, $rect.Width, $rect.Height)

    $scale = [Math]::Min(($rect.Width - 180) / $rangeX, ($rect.Height - 180) / $rangeY)
    $drawingWidth = $rangeX * $scale
    $drawingHeight = $rangeY * $scale
    $originX = $rect.X + ($rect.Width - $drawingWidth) / 2
    $originY = $rect.Y + ($rect.Height - $drawingHeight) / 2
    $minLayer = ($support.Layers | Measure-Object -Minimum).Minimum
    $maxLayer = ($support.Layers | Measure-Object -Maximum).Maximum
    $layerRange = [Math]::Max(1, $maxLayer - $minLayer)
    $lowColor = [Drawing.Color]::FromArgb(37, 205, 200)
    $highColor = [Drawing.Color]::FromArgb(255, 181, 54)

    foreach ($layer in $support.Layers) {
        $ratio = ($layer - $minLayer) / $layerRange
        $color = Blend-Color $lowColor $highColor $ratio 42
        $pen = [Drawing.Pen]::new($color, [float] [Math]::Max(1.3, 0.45 * $scale))
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

    $footerY = $size - 175
    $graphics.DrawString(
        "$($support.Layers.Count) support-body layers | $($support.Segments.Count) extrusion segments | 0 interface segments",
        $smallFont, $muted, 104, $footerY)

    $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    $panelPen.Dispose()
    $panelBrush.Dispose()
    $white.Dispose()
    $muted.Dispose()
    $smallFont.Dispose()
    $labelFont.Dispose()
    $titleFont.Dispose()
    $graphics.Dispose()
    $bitmap.Dispose()
}

if (-not [string]::IsNullOrWhiteSpace($ExistingGcode)) {
    $gcode = Get-Item -LiteralPath ([IO.Path]::GetFullPath($ExistingGcode))
    $runRoot = $gcode.DirectoryName
    $imagePath = Join-Path $runRoot 'stanford-bunny-cura-support-top.png'
} else {
$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$machinePath = Join-Path $runRoot 'machine.json'
$processPath = Join-Path $runRoot 'process.json'
$stdout = Join-Path $runRoot 'cli.out.log'
$stderr = Join-Path $runRoot 'cli.err.log'
$imagePath = Join-Path $runRoot 'stanford-bunny-cura-support-top.png'

$machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
$process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
$machineName = 'Codex Stanford Bunny Cura support'
$machine.name = $machineName
$machine.setting_id = 'cura-bunny-support'
$process.name = $machineName
$process.setting_id = 'cura-bunny-support'
$process.compatible_printers = @($machineName)
Set-JsonProperty $process 'layer_height' '0.2'
Set-JsonProperty $process 'initial_layer_print_height' '0.2'
Set-JsonProperty $process 'enable_support' '1'
Set-JsonProperty $process 'support_type' 'normal_cura(auto)'
Set-JsonProperty $process 'support_threshold_angle' '90'
Set-JsonProperty $process 'support_interface_top_layers' '0'
Set-JsonProperty $process 'support_interface_bottom_layers' '0'
Set-JsonProperty $process 'support_base_pattern' 'default'
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
    ([char]34 + $ModelPath + [char]34)
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
}

$support = Read-SupportBody $gcode.FullName
if ($support.InterfaceSegments -ne 0) {
    throw "Interface extrusion was generated despite zero interface layers: $($support.InterfaceSegments) segments"
}
Render-SupportTop $support $imagePath $ImageSize

$result = [pscustomobject]@{
    Passed = $support.Segments.Count -gt 0 -and $support.InterfaceSegments -eq 0
    Model = $ModelPath
    ThresholdAngle = 90
    SupportBodyLayers = $support.Layers.Count
    SupportBodySegments = $support.Segments.Count
    InterfaceSegments = $support.InterfaceSegments
    Gcode = $gcode.FullName
    Image = $imagePath
}
$resultPath = Join-Path $runRoot 'results.json'
$result | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $resultPath -Encoding utf8
$result | Format-List
"RESULTS=$resultPath"
