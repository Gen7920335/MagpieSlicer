param(
    [string] $ResultsPath = 'build\verification\multinozzle-orcacube\20260722-103523\results.json',
    [int[]] $Layers = @(100, 101),
    [string] $OutputPath = 'build\verification\multinozzle-orcacube\20260722-103523\middle-section.png',
    [switch] $SeparateLayers,
    [int] $SeparateImageSize = 2400,
    [string] $SeparateOutputDirectory,
    [string] $Title = 'Orca Cube',
    [string] $Subtitle = '3 large + 4 small / 4 large + 3 small, 15% gyroid',
    [string] $SingleCase = '',
    [string] $SingleCaseLabel = 'Classic',
    [int] $ConfiguredSmallWalls = 4,
    [int] $ConfiguredLargeWalls = 3,
    [int] $LayerA = -1,
    [int] $LayerB = -1
)

$ErrorActionPreference = 'Stop'
if ($LayerA -ge 0 -and $LayerB -ge 0) {
    $Layers = @($LayerA, $LayerB)
}
Add-Type -AssemblyName System.Drawing

function Token-Value([string] $line, [char] $token) {
    $match = [regex]::Match($line, "(?:^|\s)$token(-?(?:\d+(?:\.\d*)?|\.\d+))")
    if ($match.Success) { return [double]::Parse($match.Groups[1].Value, [Globalization.CultureInfo]::InvariantCulture) }
    return $null
}

function Read-GcodeLayers([string] $path, [int[]] $wantedLayers) {
    $wanted = [Collections.Generic.HashSet[int]]::new()
    foreach ($value in $wantedLayers) { [void] $wanted.Add($value) }
    $lastWantedLayer = ($wantedLayers | Measure-Object -Maximum).Maximum
    $segments = [Collections.Generic.List[object]]::new()
    $layerZ = @{}
    $layer = -1
    $tool = 0
    $role = ''
    $x = 0.0
    $y = 0.0
    $e = 0.0
    $relativeE = $false

    foreach ($rawLine in [IO.File]::ReadLines((Resolve-Path $path))) {
        $line = $rawLine.Trim()
        if ($line -eq ';LAYER_CHANGE') {
            ++$layer
            if ($layer -gt $lastWantedLayer) { break }
            continue
        }
        if ($line -match '^;Z:([\d.]+)' -and $wanted.Contains($layer)) {
            $layerZ[$layer] = [double]::Parse($Matches[1], [Globalization.CultureInfo]::InvariantCulture)
            continue
        }
        if ($line -match '^;TYPE:(.+)$') { $role = $Matches[1].Trim(); continue }
        if ($line -match '^T(\d+)\b') { $tool = [int] $Matches[1]; continue }
        if ($line -match '^M82\b') { $relativeE = $false; continue }
        if ($line -match '^M83\b') { $relativeE = $true; continue }
        if ($line -match '^G92\b') {
            $newE = Token-Value $line 'E'
            if ($null -ne $newE) { $e = $newE }
            continue
        }
        if ($line -notmatch '^G([0123])\b') { continue }

        $command = [int] $Matches[1]
        $newX = Token-Value $line 'X'; if ($null -eq $newX) { $newX = $x }
        $newY = Token-Value $line 'Y'; if ($null -eq $newY) { $newY = $y }
        $newE = Token-Value $line 'E'
        $deltaE = 0.0
        if ($null -ne $newE) {
            $deltaE = if ($relativeE) { $newE } else { $newE - $e }
            $e = if ($relativeE) { $e + $newE } else { $newE }
        }

        $printRole = $role -in @('Outer wall', 'Inner wall', 'Sparse infill', 'Internal solid infill', 'Gap infill')
        if ($wanted.Contains($layer) -and $deltaE -gt 0.000001 -and $printRole) {
            $points = [Collections.Generic.List[object]]::new()
            $points.Add([Drawing.PointF]::new([float]$x, [float]$y))
            if ($command -in @(2, 3)) {
                $i = Token-Value $line 'I'; if ($null -eq $i) { $i = 0.0 }
                $j = Token-Value $line 'J'; if ($null -eq $j) { $j = 0.0 }
                $cx = $x + $i; $cy = $y + $j
                $radius = [Math]::Sqrt($i * $i + $j * $j)
                $startAngle = [Math]::Atan2($y - $cy, $x - $cx)
                $endAngle = [Math]::Atan2($newY - $cy, $newX - $cx)
                $sweep = $endAngle - $startAngle
                if ($command -eq 2 -and $sweep -ge 0) { $sweep -= 2 * [Math]::PI }
                if ($command -eq 3 -and $sweep -le 0) { $sweep += 2 * [Math]::PI }
                $steps = [Math]::Max(4, [int][Math]::Ceiling([Math]::Abs($sweep * $radius) / 0.20))
                for ($step = 1; $step -le $steps; ++$step) {
                    $angle = $startAngle + $sweep * $step / $steps
                    $points.Add([Drawing.PointF]::new([float]($cx + $radius * [Math]::Cos($angle)), [float]($cy + $radius * [Math]::Sin($angle))))
                }
            } else {
                $points.Add([Drawing.PointF]::new([float]$newX, [float]$newY))
            }
            $segments.Add([pscustomobject]@{ Layer=$layer; Tool=$tool; Role=$role; Points=$points.ToArray() })
        }
        $x = $newX; $y = $newY
    }
    return [pscustomobject]@{ Segments=$segments; LayerZ=$layerZ }
}

$results = Get-Content -LiteralPath $ResultsPath -Raw | ConvertFrom-Json
$cases = if ([string]::IsNullOrWhiteSpace($SingleCase)) {
    @(
        [pscustomobject]@{ Name='Classic'; Data=$results | Where-Object Case -eq 'classic_count4_interlock_015' },
        [pscustomobject]@{ Name='Arachne'; Data=$results | Where-Object Case -eq 'arachne_count4_interlock_015' }
    )
} else {
    @(
        [pscustomobject]@{ Name=$SingleCaseLabel; Data=$results | Where-Object Case -eq $SingleCase | Select-Object -First 1 }
    )
}
foreach ($case in $cases) {
    if ($null -eq $case.Data -or -not (Test-Path -LiteralPath $case.Data.Gcode)) { throw "Missing G-code for $($case.Name)" }
    $case | Add-Member Parsed (Read-GcodeLayers $case.Data.Gcode $Layers)
}

$allSegments = @($cases | ForEach-Object { $_.Parsed.Segments })
if ($allSegments.Count -eq 0) { throw 'No printable segments found in requested layers' }
$allPoints = @($allSegments | ForEach-Object { $_.Points })
$minX = ($allPoints | Measure-Object X -Minimum).Minimum
$maxX = ($allPoints | Measure-Object X -Maximum).Maximum
$minY = ($allPoints | Measure-Object Y -Minimum).Minimum
$maxY = ($allPoints | Measure-Object Y -Maximum).Maximum

$width = 1800; $height = 1020
$bitmap = [Drawing.Bitmap]::new($width, $height)
$graphics = [Drawing.Graphics]::FromImage($bitmap)
$graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
$graphics.Clear([Drawing.Color]::FromArgb(18, 20, 24))
$titleFont = [Drawing.Font]::new('Segoe UI', 19, [Drawing.FontStyle]::Bold)
$labelFont = [Drawing.Font]::new('Segoe UI', 12, [Drawing.FontStyle]::Regular)
$brush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(235, 238, 242))
$mutedBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(168, 176, 188))
$graphics.DrawString("$Title - middle layer section", $titleFont, $brush, 42, 24)
$graphics.DrawString($Subtitle, $labelFont, $mutedBrush, 44, 60)

$panelWidth = 850; $panelHeight = 410; $left = 40; $top = 105; $gapX = 30; $gapY = 45
$rangeX = [Math]::Max(0.001, $maxX - $minX); $rangeY = [Math]::Max(0.001, $maxY - $minY)
$scale = [Math]::Min(($panelWidth - 54) / $rangeX, ($panelHeight - 76) / $rangeY)
$largeColor = [Drawing.Color]::FromArgb(242, 105, 68)
$smallColor = [Drawing.Color]::FromArgb(41, 201, 196)
$infillColor = [Drawing.Color]::FromArgb(123, 132, 145)
$gapColor = [Drawing.Color]::FromArgb(239, 190, 74)

for ($caseIndex = 0; $caseIndex -lt $cases.Count; ++$caseIndex) {
    for ($layerIndex = 0; $layerIndex -lt $Layers.Count; ++$layerIndex) {
        $panelX = $left + $layerIndex * ($panelWidth + $gapX)
        $panelY = $top + $caseIndex * ($panelHeight + $gapY)
        $panelRect = [Drawing.Rectangle]::new($panelX, $panelY, $panelWidth, $panelHeight)
        $graphics.FillRectangle([Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(27, 30, 36)), $panelRect)
        $graphics.DrawRectangle([Drawing.Pen]::new([Drawing.Color]::FromArgb(58, 64, 74), 1), $panelRect)
        $layer = $Layers[$layerIndex]
        $z = $cases[$caseIndex].Parsed.LayerZ[$layer]
        $graphics.DrawString("$($cases[$caseIndex].Name) - layer $layer (Z $([Math]::Round($z, 2)) mm)", $labelFont, $brush, $panelX + 18, $panelY + 13)

        $panelSegments = @($cases[$caseIndex].Parsed.Segments | Where-Object Layer -eq $layer | Sort-Object @{Expression={ if ($_.Role -like '*infill') {0} elseif ($_.Tool -eq 0) {1} else {2} }})
        $drawingWidth = $rangeX * $scale
        $drawingHeight = $rangeY * $scale
        $originX = $panelX + ($panelWidth - $drawingWidth) / 2
        $originY = $panelY + 48 + (($panelHeight - 68) - $drawingHeight) / 2
        foreach ($segment in $panelSegments) {
            $color = if ($segment.Role -like '*infill') { $infillColor } elseif ($segment.Role -eq 'Gap infill') { $gapColor } elseif ($segment.Tool -eq 1) { $smallColor } else { $largeColor }
            $physicalWidth = if ($segment.Role -like '*infill' -or $segment.Role -eq 'Gap infill') { 0.45 } elseif ($segment.Tool -eq 1) { 0.17 } else { 0.45 }
            $pen = [Drawing.Pen]::new($color, [float][Math]::Max(1.2, $physicalWidth * $scale))
            $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round; $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round; $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
            $screenPoints = foreach ($point in $segment.Points) {
                [Drawing.PointF]::new([float]($originX + ($point.X - $minX) * $scale), [float]($originY + $drawingHeight - ($point.Y - $minY) * $scale))
            }
            if ($screenPoints.Count -ge 2) { $graphics.DrawLines($pen, [Drawing.PointF[]]$screenPoints) }
            $pen.Dispose()
        }
        foreach ($segment in @($panelSegments | Where-Object Role -in @('Outer wall', 'Inner wall'))) {
            $centerPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(220, 255, 255, 255), 0.8)
            $screenPoints = foreach ($point in $segment.Points) {
                [Drawing.PointF]::new([float]($originX + ($point.X - $minX) * $scale), [float]($originY + $drawingHeight - ($point.Y - $minY) * $scale))
            }
            if ($screenPoints.Count -ge 2) { $graphics.DrawLines($centerPen, [Drawing.PointF[]]$screenPoints) }
            $centerPen.Dispose()
        }
    }
}

$legendY = 980
foreach ($item in @(
    @{X=930; Color=$largeColor; Text='Large-nozzle wall (T0, 0.4 mm)'},
    @{X=1210; Color=$smallColor; Text='Small-nozzle wall (T1, 0.15 mm)'},
    @{X=1510; Color=$infillColor; Text='Gyroid infill'}
)) {
    $graphics.FillRectangle([Drawing.SolidBrush]::new($item.Color), $item.X, $legendY, 22, 10)
    $graphics.DrawString($item.Text, $labelFont, $mutedBrush, $item.X + 30, $legendY - 7)
}

$outputDirectory = Split-Path -Parent $OutputPath
New-Item -ItemType Directory -Force -Path $outputDirectory | Out-Null
$bitmap.Save($OutputPath, [Drawing.Imaging.ImageFormat]::Png)

$separatePaths = [Collections.Generic.List[string]]::new()
if ($SeparateLayers) {
    if ([string]::IsNullOrWhiteSpace($SeparateOutputDirectory)) {
        $SeparateOutputDirectory = Join-Path $outputDirectory 'layers-hires'
    }
    New-Item -ItemType Directory -Force -Path $SeparateOutputDirectory | Out-Null
    foreach ($case in $cases) {
        foreach ($layer in $Layers) {
            $single = [Drawing.Bitmap]::new($SeparateImageSize, $SeparateImageSize, [Drawing.Imaging.PixelFormat]::Format32bppPArgb)
            $singleGraphics = [Drawing.Graphics]::FromImage($single)
            $singleGraphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
            $singleGraphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
            $singleGraphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
            $singleGraphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
            $singleGraphics.Clear([Drawing.Color]::FromArgb(18, 20, 24))

            $singleTitleFont = [Drawing.Font]::new('Segoe UI', 34, [Drawing.FontStyle]::Bold)
            $singleLabelFont = [Drawing.Font]::new('Segoe UI', 22, [Drawing.FontStyle]::Regular)
            $singleSmallFont = [Drawing.Font]::new('Segoe UI', 17, [Drawing.FontStyle]::Regular)
            $z = $case.Parsed.LayerZ[$layer]
            $smallWallCount = if (($layer % 2) -eq 0) { $ConfiguredSmallWalls } else { [Math]::Max(1, $ConfiguredSmallWalls - 1) }
            $largeWallCount = $ConfiguredLargeWalls + ($ConfiguredSmallWalls - $smallWallCount)
            $wallOrder = "$smallWallCount small + $largeWallCount large walls"
            $singleGraphics.DrawString("$Title - $($case.Name)", $singleTitleFont, $brush, 90, 55)
            $singleGraphics.DrawString("Layer $layer  |  Z $([Math]::Round($z, 2)) mm  |  $wallOrder", $singleLabelFont, $mutedBrush, 94, 115)

            $contentRect = [Drawing.Rectangle]::new(90, 190, $SeparateImageSize - 180, $SeparateImageSize - 360)
            $singleGraphics.FillRectangle([Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(27, 30, 36)), $contentRect)
            $singleGraphics.DrawRectangle([Drawing.Pen]::new([Drawing.Color]::FromArgb(58, 64, 74), 2), $contentRect)
            $singleScale = [Math]::Min(($contentRect.Width - 110) / $rangeX, ($contentRect.Height - 110) / $rangeY)
            $singleDrawingWidth = $rangeX * $singleScale
            $singleDrawingHeight = $rangeY * $singleScale
            $singleOriginX = $contentRect.X + ($contentRect.Width - $singleDrawingWidth) / 2
            $singleOriginY = $contentRect.Y + ($contentRect.Height - $singleDrawingHeight) / 2
            $panelSegments = @($case.Parsed.Segments | Where-Object Layer -eq $layer | Sort-Object @{Expression={ if ($_.Role -like '*infill') {0} elseif ($_.Tool -eq 0) {1} else {2} }})

            foreach ($segment in $panelSegments) {
                $color = if ($segment.Role -like '*infill') { $infillColor } elseif ($segment.Role -eq 'Gap infill') { $gapColor } elseif ($segment.Tool -eq 1) { $smallColor } else { $largeColor }
                $physicalWidth = if ($segment.Role -like '*infill' -or $segment.Role -eq 'Gap infill') { 0.45 } elseif ($segment.Tool -eq 1) { 0.17 } else { 0.45 }
                $pen = [Drawing.Pen]::new($color, [float][Math]::Max(2.0, $physicalWidth * $singleScale))
                $pen.StartCap = [Drawing.Drawing2D.LineCap]::Round; $pen.EndCap = [Drawing.Drawing2D.LineCap]::Round; $pen.LineJoin = [Drawing.Drawing2D.LineJoin]::Round
                $screenPoints = foreach ($point in $segment.Points) {
                    [Drawing.PointF]::new([float]($singleOriginX + ($point.X - $minX) * $singleScale), [float]($singleOriginY + $singleDrawingHeight - ($point.Y - $minY) * $singleScale))
                }
                if ($screenPoints.Count -ge 2) { $singleGraphics.DrawLines($pen, [Drawing.PointF[]]$screenPoints) }
                $pen.Dispose()
            }
            foreach ($segment in @($panelSegments | Where-Object Role -in @('Outer wall', 'Inner wall'))) {
                $centerPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(230, 255, 255, 255), 2.0)
                $screenPoints = foreach ($point in $segment.Points) {
                    [Drawing.PointF]::new([float]($singleOriginX + ($point.X - $minX) * $singleScale), [float]($singleOriginY + $singleDrawingHeight - ($point.Y - $minY) * $singleScale))
                }
                if ($screenPoints.Count -ge 2) { $singleGraphics.DrawLines($centerPen, [Drawing.PointF[]]$screenPoints) }
                $centerPen.Dispose()
            }

            $legendY = $SeparateImageSize - 105
            $legend = @(
                @{ X=100; Color=$largeColor; Text='Large-nozzle wall (T0, 0.4 mm)' },
                @{ X=850; Color=$smallColor; Text='Small-nozzle wall (T1, 0.15 mm)' },
                @{ X=1640; Color=$infillColor; Text='15% gyroid infill' }
            )
            foreach ($item in $legend) {
                $singleGraphics.FillRectangle([Drawing.SolidBrush]::new($item.Color), $item.X, $legendY, 42, 20)
                $singleGraphics.DrawString($item.Text, $singleSmallFont, $mutedBrush, $item.X + 58, $legendY - 9)
            }

            $slug = $case.Name.ToLowerInvariant()
            $singlePath = Join-Path $SeparateOutputDirectory "$slug-layer-$layer.png"
            $single.Save($singlePath, [Drawing.Imaging.ImageFormat]::Png)
            $separatePaths.Add((Resolve-Path $singlePath).Path)
            $singleGraphics.Dispose(); $single.Dispose(); $singleTitleFont.Dispose(); $singleLabelFont.Dispose(); $singleSmallFont.Dispose()
        }
    }
}

$graphics.Dispose(); $bitmap.Dispose(); $titleFont.Dispose(); $labelFont.Dispose(); $brush.Dispose(); $mutedBrush.Dispose()
Write-Output (Resolve-Path $OutputPath)
foreach ($path in $separatePaths) { Write-Output $path }
