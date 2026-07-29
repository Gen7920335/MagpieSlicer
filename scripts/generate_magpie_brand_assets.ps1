param(
    [Parameter(Mandatory = $true)]
    [string] $SourceDirectory
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$root = Split-Path -Parent $PSScriptRoot
$imageDirectory = Join-Path $root 'resources\images'
$webImageDirectory = Join-Path $root 'resources\web\image'
$roundedSourcePath = Join-Path $SourceDirectory '1-Photo-1.jpg'
$transparentSourcePath = Join-Path $SourceDirectory '2-Photo-2.jpg'
$circleSourcePath = Join-Path $SourceDirectory '3-Photo-3.jpg'

foreach ($path in @($roundedSourcePath, $transparentSourcePath, $circleSourcePath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) {
        throw "Brand source image not found: $path"
    }
}

function New-ArgbBitmap([int] $Width, [int] $Height)
{
    return [Drawing.Bitmap]::new($Width, $Height, [Drawing.Imaging.PixelFormat]::Format32bppArgb)
}

function New-RoundedPath([Drawing.RectangleF] $Rectangle, [float] $Radius)
{
    $path = [Drawing.Drawing2D.GraphicsPath]::new()
    $diameter = $Radius * 2
    $path.AddArc($Rectangle.X, $Rectangle.Y, $diameter, $diameter, 180, 90)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Y, $diameter, $diameter, 270, 90)
    $path.AddArc($Rectangle.Right - $diameter, $Rectangle.Bottom - $diameter, $diameter, $diameter, 0, 90)
    $path.AddArc($Rectangle.X, $Rectangle.Bottom - $diameter, $diameter, $diameter, 90, 90)
    $path.CloseFigure()
    return $path
}

function New-ClippedSource([string] $Path, [string] $Shape)
{
    $source = [Drawing.Bitmap]::FromFile($Path)
    $output = New-ArgbBitmap $source.Width $source.Height
    $graphics = [Drawing.Graphics]::FromImage($output)
    $graphics.Clear([Drawing.Color]::Transparent)

    if ($Shape -eq 'rounded') {
        $clip = New-RoundedPath ([Drawing.RectangleF]::new(50, 50, 1180, 1180)) 190
    } else {
        $clip = [Drawing.Drawing2D.GraphicsPath]::new()
        $clip.AddEllipse(24, 24, 1232, 1232)
    }

    $graphics.SetClip($clip)
    $graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
    $graphics.Dispose()
    $clip.Dispose()
    $source.Dispose()
    return $output
}

function New-TransparentSymbol([string] $Path)
{
    $source = [Drawing.Bitmap]::FromFile($Path)
    $output = New-ArgbBitmap $source.Width $source.Height
    $graphics = [Drawing.Graphics]::FromImage($output)
    $graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
    $graphics.Dispose()
    $source.Dispose()

    $rectangle = [Drawing.Rectangle]::new(0, 0, $output.Width, $output.Height)
    $data = $output.LockBits(
        $rectangle,
        [Drawing.Imaging.ImageLockMode]::ReadWrite,
        [Drawing.Imaging.PixelFormat]::Format32bppArgb
    )
    $bytes = [byte[]]::new([Math]::Abs($data.Stride) * $data.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)

    for ($y = 0; $y -lt $data.Height; ++$y) {
        for ($x = 0; $x -lt $data.Width; ++$x) {
            $index = $y * $data.Stride + $x * 4
            $blue = [int] $bytes[$index]
            $green = [int] $bytes[$index + 1]
            $red = [int] $bytes[$index + 2]
            $maximum = [Math]::Max($red, [Math]::Max($green, $blue))

            if ($maximum -le 3) {
                $alpha = 0
            } elseif ($maximum -ge 32) {
                $alpha = 255
            } else {
                $alpha = [int] [Math]::Round(($maximum - 3) * 255 / 29)
            }

            $bytes[$index + 3] = [byte] $alpha
            if ($alpha -gt 0 -and $alpha -lt 255) {
                $bytes[$index] = [byte] [Math]::Min(255, [Math]::Round($blue * 255 / $alpha))
                $bytes[$index + 1] = [byte] [Math]::Min(255, [Math]::Round($green * 255 / $alpha))
                $bytes[$index + 2] = [byte] [Math]::Min(255, [Math]::Round($red * 255 / $alpha))
            }
        }
    }

    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
    $output.UnlockBits($data)
    return $output
}

function New-Grayscale([Drawing.Bitmap] $Source)
{
    $output = New-ArgbBitmap $Source.Width $Source.Height
    $graphics = [Drawing.Graphics]::FromImage($output)
    $graphics.DrawImage($Source, 0, 0, $Source.Width, $Source.Height)
    $graphics.Dispose()

    $rectangle = [Drawing.Rectangle]::new(0, 0, $output.Width, $output.Height)
    $data = $output.LockBits(
        $rectangle,
        [Drawing.Imaging.ImageLockMode]::ReadWrite,
        [Drawing.Imaging.PixelFormat]::Format32bppArgb
    )
    $bytes = [byte[]]::new([Math]::Abs($data.Stride) * $data.Height)
    [Runtime.InteropServices.Marshal]::Copy($data.Scan0, $bytes, 0, $bytes.Length)

    for ($y = 0; $y -lt $data.Height; ++$y) {
        for ($x = 0; $x -lt $data.Width; ++$x) {
            $index = $y * $data.Stride + $x * 4
            $gray = [int] [Math]::Round(
                $bytes[$index + 2] * 0.299 +
                $bytes[$index + 1] * 0.587 +
                $bytes[$index] * 0.114
            )
            $bytes[$index] = [byte] $gray
            $bytes[$index + 1] = [byte] $gray
            $bytes[$index + 2] = [byte] $gray
        }
    }

    [Runtime.InteropServices.Marshal]::Copy($bytes, 0, $data.Scan0, $bytes.Length)
    $output.UnlockBits($data)
    return $output
}

function Get-ResizedPngBytes([Drawing.Bitmap] $Source, [int] $Width, [int] $Height)
{
    $output = New-ArgbBitmap $Width $Height
    $graphics = [Drawing.Graphics]::FromImage($output)
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.CompositingQuality = [Drawing.Drawing2D.CompositingQuality]::HighQuality
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.PixelOffsetMode = [Drawing.Drawing2D.PixelOffsetMode]::HighQuality
    $graphics.DrawImage($Source, [Drawing.Rectangle]::new(0, 0, $Width, $Height))
    $graphics.Dispose()

    $stream = [IO.MemoryStream]::new()
    $output.Save($stream, [Drawing.Imaging.ImageFormat]::Png)
    $output.Dispose()
    $bytes = $stream.ToArray()
    $stream.Dispose()
    return $bytes
}

function Save-Png([Drawing.Bitmap] $Source, [string] $Path, [int] $Width, [int] $Height)
{
    [IO.File]::WriteAllBytes($Path, (Get-ResizedPngBytes $Source $Width $Height))
}

function Save-Ico([Drawing.Bitmap] $Source, [string] $Path, [int[]] $Sizes)
{
    $entries = foreach ($size in $Sizes) {
        @{
            Size = $size
            Data = [byte[]] (Get-ResizedPngBytes $Source $size $size)
        }
    }

    $stream = [IO.MemoryStream]::new()
    $writer = [IO.BinaryWriter]::new($stream)
    $writer.Write([uint16] 0)
    $writer.Write([uint16] 1)
    $writer.Write([uint16] $entries.Count)
    $offset = 6 + 16 * $entries.Count

    foreach ($entry in $entries) {
        $dimension = if ($entry.Size -ge 256) { 0 } else { $entry.Size }
        $writer.Write([byte] $dimension)
        $writer.Write([byte] $dimension)
        $writer.Write([byte] 0)
        $writer.Write([byte] 0)
        $writer.Write([uint16] 1)
        $writer.Write([uint16] 32)
        $writer.Write([uint32] $entry.Data.Length)
        $writer.Write([uint32] $offset)
        $offset += $entry.Data.Length
    }

    foreach ($entry in $entries) {
        $writer.Write([byte[]] $entry.Data)
    }

    $writer.Flush()
    [IO.File]::WriteAllBytes($Path, $stream.ToArray())
    $writer.Dispose()
    $stream.Dispose()
}

function Get-BigEndianUInt32([int] $Value)
{
    return [byte[]] @(
        (($Value -shr 24) -band 255),
        (($Value -shr 16) -band 255),
        (($Value -shr 8) -band 255),
        ($Value -band 255)
    )
}

function Save-Icns([Drawing.Bitmap] $Source, [string] $Path)
{
    $specification = @(
        @('icp4', 16),
        @('icp5', 32),
        @('icp6', 64),
        @('ic07', 128),
        @('ic08', 256),
        @('ic09', 512),
        @('ic10', 1024)
    )
    $chunks = foreach ($entry in $specification) {
        @{
            Type = [string] $entry[0]
            Data = Get-ResizedPngBytes $Source ([int] $entry[1]) ([int] $entry[1])
        }
    }
    $totalLength = 8
    foreach ($chunk in $chunks) {
        $totalLength += 8 + $chunk.Data.Length
    }

    $stream = [IO.MemoryStream]::new()
    $ascii = [Text.Encoding]::ASCII
    $stream.Write($ascii.GetBytes('icns'), 0, 4)
    $lengthBytes = Get-BigEndianUInt32 $totalLength
    $stream.Write($lengthBytes, 0, 4)

    foreach ($chunk in $chunks) {
        $typeBytes = $ascii.GetBytes($chunk.Type)
        $stream.Write($typeBytes, 0, 4)
        $chunkLengthBytes = Get-BigEndianUInt32 (8 + $chunk.Data.Length)
        $stream.Write($chunkLengthBytes, 0, 4)
        $stream.Write($chunk.Data, 0, $chunk.Data.Length)
    }

    [IO.File]::WriteAllBytes($Path, $stream.ToArray())
    $stream.Dispose()
}

function New-Wordmark([Drawing.Bitmap] $Icon, [int] $Width, [int] $Height, [bool] $Dark)
{
    $scale = 4
    $output = New-ArgbBitmap ($Width * $scale) ($Height * $scale)
    $graphics = [Drawing.Graphics]::FromImage($output)
    $graphics.Clear([Drawing.Color]::Transparent)
    $graphics.InterpolationMode = [Drawing.Drawing2D.InterpolationMode]::HighQualityBicubic
    $padding = [int] ($Height * 0.12 * $scale)
    $iconSize = $Height * $scale - 2 * $padding
    $graphics.DrawImage($Icon, [Drawing.Rectangle]::new($padding, $padding, $iconSize, $iconSize))

    $textColor = if ($Dark) {
        [Drawing.Color]::FromArgb(245, 245, 245)
    } else {
        [Drawing.Color]::FromArgb(48, 48, 54)
    }
    $brush = [Drawing.SolidBrush]::new($textColor)
    $fontSize = [single] ($Height * 0.30 * $scale)
    $primaryFont = [Drawing.Font]::new(
        'Segoe UI Semibold',
        $fontSize,
        [Drawing.FontStyle]::Bold,
        [Drawing.GraphicsUnit]::Pixel
    )
    $secondaryFont = [Drawing.Font]::new(
        'Segoe UI',
        $fontSize * 0.70,
        [Drawing.FontStyle]::Regular,
        [Drawing.GraphicsUnit]::Pixel
    )
    $textX = [single] ($Height * $scale * 1.02)
    $graphics.DrawString('MAGPIE', $primaryFont, $brush, $textX, [single] ($Height * $scale * 0.18))
    $graphics.DrawString('SLICER', $secondaryFont, $brush, $textX, [single] ($Height * $scale * 0.53))
    $primaryFont.Dispose()
    $secondaryFont.Dispose()
    $brush.Dispose()
    $graphics.Dispose()
    return $output
}

function Save-EmbeddedSvg(
    [Drawing.Bitmap] $Source,
    [string] $Path,
    [double] $Width,
    [double] $Height
)
{
    $pixelWidth = [Math]::Max(1, [int] [Math]::Round($Width))
    $pixelHeight = [Math]::Max(1, [int] [Math]::Round($Height))
    $base64 = [Convert]::ToBase64String(
        (Get-ResizedPngBytes $Source $pixelWidth $pixelHeight)
    )
    $svg = '<svg xmlns="http://www.w3.org/2000/svg" width="{0}" height="{1}" viewBox="0 0 {0} {1}"><image width="{0}" height="{1}" href="data:image/png;base64,{2}"/></svg>{3}' -f
        $Width, $Height, $base64, [Environment]::NewLine
    [IO.File]::WriteAllText($Path, $svg, [Text.UTF8Encoding]::new($false))
}

$rounded = New-ClippedSource $roundedSourcePath 'rounded'
$symbol = New-TransparentSymbol $transparentSourcePath
$circle = New-ClippedSource $circleSourcePath 'circle'
$grayscale = New-Grayscale $symbol

$pngAssets = @{
    'MagpieSlicer.png' = @($rounded, 154, 154)
    'MagpieSlicer_128px.png' = @($rounded, 128, 128)
    'MagpieSlicer_154.png' = @($rounded, 154, 154)
    'MagpieSlicer_192px.png' = @($rounded, 192, 192)
    'MagpieSlicer_32px.png' = @($rounded, 32, 32)
    'MagpieSlicer_64.png' = @($rounded, 64, 64)
    'MagpieSlicer-mac_128px.png' = @($rounded, 128, 128)
    'MagpieSlicer_192px_transparent.png' = @($symbol, 192, 192)
    'MagpieSlicer_192px_grayscale.png' = @($grayscale, 192, 192)
    'MagpieSlicerTitle.png' = @($rounded, 154, 154)
    'MagpieSlicer_154_title.png' = @($rounded, 184, 184)
}
foreach ($name in $pngAssets.Keys) {
    $asset = $pngAssets[$name]
    Save-Png $asset[0] (Join-Path $imageDirectory $name) $asset[1] $asset[2]
}

Save-Png $rounded (Join-Path $webImageDirectory 'logo.png') 154 154
Save-Png $symbol (Join-Path $webImageDirectory 'logo2.png') 339 406
Save-Ico $symbol (Join-Path $imageDirectory 'MagpieSlicer.ico') @(16, 24, 32, 48, 64, 128, 256)
Save-Ico $rounded (Join-Path $imageDirectory 'MagpieSlicerTitle.ico') @(16, 24, 32, 48, 64, 128, 256)
Save-Ico $rounded (Join-Path $imageDirectory 'MagpieSlicer-mac_256px.ico') @(32, 64, 128, 256)
Save-Ico $symbol (Join-Path $imageDirectory 'MagpieSlicer-gcodeviewer.ico') @(16, 24, 32, 48, 64, 128, 256)
Save-Icns $rounded (Join-Path $imageDirectory 'MagpieSlicer.icns')
Save-Icns $rounded (Join-Path $root 'resources\MagpieSlicer.icns')

Save-EmbeddedSvg $rounded (Join-Path $imageDirectory 'MagpieSlicer.svg') 64 64
Save-EmbeddedSvg $rounded (Join-Path $imageDirectory 'MagpieSlicer_gradient.svg') 1024 1024
Save-EmbeddedSvg $circle (Join-Path $imageDirectory 'MagpieSlicer_gradient_circle.svg') 1024 1024
Save-EmbeddedSvg $symbol (Join-Path $imageDirectory 'MagpieSlicer_gradient_narrow.svg') 814.987 1023.9927
Save-EmbeddedSvg $grayscale (Join-Path $imageDirectory 'MagpieSlicer_gray.svg') 1024 1024

$aboutLight = New-Wordmark $symbol 560 125 $false
$aboutDark = New-Wordmark $symbol 560 125 $true
Save-EmbeddedSvg $aboutLight (Join-Path $imageDirectory 'MagpieSlicer_about.svg') 560 125
Save-EmbeddedSvg $aboutDark (Join-Path $imageDirectory 'MagpieSlicer_about_dark.svg') 560 125
$aboutLight.Dispose()
$aboutDark.Dispose()

$horizontalLight = New-Wordmark $symbol 214 80 $false
$horizontalDark = New-Wordmark $symbol 214 80 $true
Save-EmbeddedSvg $horizontalLight (Join-Path $imageDirectory 'MagpieSlicer_horizontal_light.svg') 214 80
Save-EmbeddedSvg $horizontalDark (Join-Path $imageDirectory 'MagpieSlicer_horizontal_dark.svg') 214 80
$horizontalLight.Dispose()
$horizontalDark.Dispose()

Save-EmbeddedSvg $symbol (Join-Path $imageDirectory 'splash_logo.svg') 480 480
Save-EmbeddedSvg $symbol (Join-Path $imageDirectory 'splash_logo_dark.svg') 480 480
Save-EmbeddedSvg $symbol (Join-Path $imageDirectory 'studio_logo.svg') 814.987 1023.9927

$preview = New-ArgbBitmap 1400 520
$previewGraphics = [Drawing.Graphics]::FromImage($preview)
$previewGraphics.Clear([Drawing.Color]::FromArgb(235, 235, 238))
$previewGraphics.DrawImage($rounded, [Drawing.Rectangle]::new(40, 40, 400, 400))
$previewGraphics.DrawImage($symbol, [Drawing.Rectangle]::new(500, 40, 400, 400))
$previewGraphics.DrawImage($circle, [Drawing.Rectangle]::new(960, 40, 400, 400))
$previewGraphics.Dispose()
$previewPath = Join-Path $root 'build\branding-preview.png'
$preview.Save($previewPath, [Drawing.Imaging.ImageFormat]::Png)
$preview.Dispose()

$rounded.Dispose()
$symbol.Dispose()
$circle.Dispose()
$grayscale.Dispose()

Write-Output "BRAND_ASSET_PREVIEW=$previewPath"
Get-ChildItem -LiteralPath $imageDirectory -File |
    Where-Object Name -Like 'MagpieSlicer*' |
    Sort-Object Name |
    Select-Object Name, Length
