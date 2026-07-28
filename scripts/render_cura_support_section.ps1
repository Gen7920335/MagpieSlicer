param(
    [Parameter(Mandatory = $true)]
    [string] $Gcode,
    [int] $Layer = 50,
    [int] $ImageSize = 3000,
    [double] $MinCoordinate = 80,
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$gcodePath = (Resolve-Path $Gcode).Path
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path (Split-Path -Parent $gcodePath) "cura-support-section-layer-$Layer.png"
}
$OutputPath = [IO.Path]::GetFullPath($OutputPath)

Add-Type -AssemblyName System.Drawing

$currentLayer = -1
$role = ''
$absoluteXY = $true
$relativeExtrusion = $false
$x = $null
$y = $null
$e = 0.0
$segments = [Collections.Generic.List[object]]::new()
$supportPathOpen = $false
$supportPathCount = 0

foreach ($line in [IO.File]::ReadLines($gcodePath)) {
    if ($line -eq ';LAYER_CHANGE') {
        $supportPathOpen = $false
        $currentLayer++
        if ($currentLayer -gt $Layer) { break }
        continue
    }
    if ($line -match '^;TYPE:(.+)$') {
        $role = $Matches[1].Trim()
        if ($role -ne 'Support') { $supportPathOpen = $false }
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
    if (
        $currentLayer -eq $Layer -and
        $role -eq 'Support' -and
        $extrusion -gt 0.0000001 -and
        $null -ne $x -and $null -ne $y -and
        ($hasX -or $hasY) -and
        $x -gt $MinCoordinate -and $newX -gt $MinCoordinate -and
        $y -gt $MinCoordinate -and $newY -gt $MinCoordinate
    ) {
        if (-not $supportPathOpen) {
            $supportPathCount++
            $supportPathOpen = $true
        }
        $segments.Add([pscustomobject]@{
            X1 = [double] $x
            Y1 = [double] $y
            X2 = [double] $newX
            Y2 = [double] $newY
        })
    } elseif ($currentLayer -eq $Layer -and $role -eq 'Support' -and ($hasX -or $hasY) -and $extrusion -le 0.0000001) {
        $supportPathOpen = $false
    }
    if ($hasX) { $x = $newX }
    if ($hasY) { $y = $newY }
    if ($hasE) { $e = $newE }
}

if ($segments.Count -eq 0) {
    throw "No support-body extrusion found on layer $Layer"
}

$allX = @($segments | ForEach-Object { $_.X1; $_.X2 })
$allY = @($segments | ForEach-Object { $_.Y1; $_.Y2 })
$minX = ($allX | Measure-Object -Minimum).Minimum
$maxX = ($allX | Measure-Object -Maximum).Maximum
$minY = ($allY | Measure-Object -Minimum).Minimum
$maxY = ($allY | Measure-Object -Maximum).Maximum
$rangeX = [Math]::Max(0.001, $maxX - $minX)
$rangeY = [Math]::Max(0.001, $maxY - $minY)

$bitmap = [Drawing.Bitmap]::new($ImageSize, $ImageSize, [Drawing.Imaging.PixelFormat]::Format32bppPArgb)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
$graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
$graphics.Clear([Drawing.Color]::FromArgb(14, 17, 21))

$titleFont = [Drawing.Font]::new('Segoe UI', 42, [Drawing.FontStyle]::Bold)
$labelFont = [Drawing.Font]::new('Segoe UI', 24, [Drawing.FontStyle]::Regular)
$whiteBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(238, 242, 248))
$mutedBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(166, 177, 193))
$graphics.DrawString('Cura-style support cross-section', $titleFont, $whiteBrush, 100, 60)
$graphics.DrawString(
    "Layer $Layer | support body only | interface and travel excluded",
    $labelFont, $mutedBrush, 104, 130)

$rect = [Drawing.RectangleF]::new(100, 220, $ImageSize - 200, $ImageSize - 440)
$panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(24, 29, 35))
$panelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(59, 68, 80), 2)
$graphics.FillRectangle($panelBrush, $rect)
$graphics.DrawRectangle($panelPen, $rect.X, $rect.Y, $rect.Width, $rect.Height)

$scale = [Math]::Min(($rect.Width - 180) / $rangeX, ($rect.Height - 180) / $rangeY)
$drawingWidth = $rangeX * $scale
$drawingHeight = $rangeY * $scale
$originX = $rect.X + ($rect.Width - $drawingWidth) / 2
$originY = $rect.Y + ($rect.Height - $drawingHeight) / 2
$extrusionWidthPx = [float] [Math]::Max(7, 0.45 * $scale)

$extrusionPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(255, 187, 62), $extrusionWidthPx)
$extrusionPen.StartCap = [Drawing.Drawing2D.LineCap]::Round
$extrusionPen.EndCap = [Drawing.Drawing2D.LineCap]::Round
foreach ($segment in $segments) {
    $px1 = $originX + ($segment.X1 - $minX) * $scale
    $py1 = $originY + ($maxY - $segment.Y1) * $scale
    $px2 = $originX + ($segment.X2 - $minX) * $scale
    $py2 = $originY + ($maxY - $segment.Y2) * $scale
    $graphics.DrawLine($extrusionPen, [float] $px1, [float] $py1, [float] $px2, [float] $py2)
}

$footer = "$supportPathCount native Cura ZigZag paths | $($segments.Count) extrusion segments | 0.45 mm line width"
$graphics.DrawString($footer, $labelFont, $mutedBrush, 104, $ImageSize - 160)

$bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)

$extrusionPen.Dispose()
$panelPen.Dispose()
$panelBrush.Dispose()
$whiteBrush.Dispose()
$mutedBrush.Dispose()
$labelFont.Dispose()
$titleFont.Dispose()
$graphics.Dispose()
$bitmap.Dispose()

[pscustomobject]@{
    Passed = $true
    Layer = $Layer
    Paths = $supportPathCount
    Segments = $segments.Count
    Bounds = "$([Math]::Round($rangeX, 3)) x $([Math]::Round($rangeY, 3)) mm"
    Image = $OutputPath
}
