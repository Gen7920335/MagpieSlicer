param(
    [string] $SlicerPath,
    [string] $OutputRoot,
    [int] $SliceTimeoutSeconds = 180,
    [int] $ImageWidth = 3600,
    [double] $NozzleDiameter = 0.4
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($SlicerPath)) {
    $SlicerPath = Join-Path $RepoRoot 'build\src\Release\magpie-slicer.exe'
}
if ([string]::IsNullOrWhiteSpace($OutputRoot)) {
    $OutputRoot = Join-Path $RepoRoot 'build\verification\tree-wall-counts'
}

$BaseMachinePath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_machine.json'
$BaseProcessPath = Join-Path $RepoRoot 'sandboxes\multinozzle_test\auto_tool2_020_base1_process.json'
foreach ($path in @($SlicerPath, $BaseMachinePath, $BaseProcessPath)) {
    if (-not (Test-Path -LiteralPath $path -PathType Leaf)) { throw "Required file not found: $path" }
}

Add-Type -AssemblyName System.Drawing
Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text.RegularExpressions;

public sealed class TreeWallSegment
{
    public double X1, Y1, X2, Y2, Length;
}

public sealed class TreeWallAnalysisData
{
    public readonly Dictionary<string, string> Settings = new Dictionary<string, string>();
    public readonly List<TreeWallSegment> Segments = new List<TreeWallSegment>();
    public int SupportLayerCount;
    public int SelectedLayer = -1;
    public double SelectedZ;
    public double Length;
    public double MinX = double.MaxValue, MaxX = double.MinValue;
    public double MinY = double.MaxValue, MaxY = double.MinValue;
    public int NonFinite;
}

public static class TreeWallGcodeParser
{
    private static readonly CultureInfo Invariant = CultureInfo.InvariantCulture;
    private static readonly Regex Setting = new Regex(
        @"^; ([a-z][a-z0-9_]*) = (.*)$",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    private static bool TokenValue(string line, char token, out double value)
    {
        value = 0.0;
        for (int index = 0; index < line.Length; ++index) {
            if (line[index] != token || (index > 0 && !char.IsWhiteSpace(line[index - 1])))
                continue;
            int begin = index + 1;
            int end = begin;
            while (end < line.Length && !char.IsWhiteSpace(line[end]) && line[end] != ';')
                ++end;
            return double.TryParse(line.Substring(begin, end - begin), NumberStyles.Float, Invariant, out value);
        }
        return false;
    }

    public static TreeWallAnalysisData Parse(string path)
    {
        var result = new TreeWallAnalysisData();
        var supportLayers = new HashSet<int>();
        var zByLayer = new Dictionary<int, double>();
        int layer = -1;
        string role = "";
        bool absoluteXY = true;
        bool relativeExtrusion = false;
        double x = 0.0, y = 0.0, e = 0.0;
        bool hasX = false, hasY = false;

        foreach (string line in File.ReadLines(path)) {
            if (line == ";LAYER_CHANGE") { ++layer; continue; }
            if (line.StartsWith(";Z:", StringComparison.Ordinal)) {
                double parsed;
                if (double.TryParse(line.Substring(3), NumberStyles.Float, Invariant, out parsed))
                    zByLayer[layer] = parsed;
                continue;
            }
            if (line.StartsWith(";TYPE:", StringComparison.Ordinal)) {
                role = line.Substring(6).Trim();
                continue;
            }
            if (line.StartsWith("; ", StringComparison.Ordinal)) {
                Match match = Setting.Match(line);
                if (match.Success)
                    result.Settings[match.Groups[1].Value] = match.Groups[2].Value.Trim();
                continue;
            }
            if (line == "G90") { absoluteXY = true; continue; }
            if (line == "G91") { absoluteXY = false; continue; }
            if (line.StartsWith("M82", StringComparison.Ordinal)) { relativeExtrusion = false; continue; }
            if (line.StartsWith("M83", StringComparison.Ordinal)) { relativeExtrusion = true; continue; }
            if (line.StartsWith("G92", StringComparison.Ordinal)) {
                double parsed;
                if (TokenValue(line, 'E', out parsed)) e = parsed;
                continue;
            }
            if (!(line.StartsWith("G0 ", StringComparison.Ordinal) ||
                  line.StartsWith("G1 ", StringComparison.Ordinal) ||
                  line.StartsWith("G2 ", StringComparison.Ordinal) ||
                  line.StartsWith("G3 ", StringComparison.Ordinal)))
                continue;
            if (line.IndexOf("nan", StringComparison.OrdinalIgnoreCase) >= 0 ||
                line.IndexOf("inf", StringComparison.OrdinalIgnoreCase) >= 0) {
                ++result.NonFinite;
                continue;
            }

            double tx, ty, te;
            bool lineHasX = TokenValue(line, 'X', out tx);
            bool lineHasY = TokenValue(line, 'Y', out ty);
            bool lineHasE = TokenValue(line, 'E', out te);
            double nextX = lineHasX ? ((absoluteXY || !hasX) ? tx : x + tx) : x;
            double nextY = lineHasY ? ((absoluteXY || !hasY) ? ty : y + ty) : y;
            double deltaE = lineHasE ? (relativeExtrusion ? te : te - e) : 0.0;
            bool isSupport = (role.Equals("Support", StringComparison.OrdinalIgnoreCase) ||
                              role.IndexOf("support material", StringComparison.OrdinalIgnoreCase) >= 0) &&
                             role.IndexOf("interface", StringComparison.OrdinalIgnoreCase) < 0;

            if (deltaE > 0.000001 && hasX && hasY && isSupport) {
                double dx = nextX - x;
                double dy = nextY - y;
                double length = Math.Sqrt(dx * dx + dy * dy);
                if (length > 0.001) {
                    if (supportLayers.Add(layer)) {
                        ++result.SupportLayerCount;
                        if (result.SupportLayerCount == 5)
                            result.SelectedLayer = layer;
                    }
                    if (layer == result.SelectedLayer) {
                        var segment = new TreeWallSegment {
                            X1 = x, Y1 = y, X2 = nextX, Y2 = nextY, Length = length
                        };
                        result.Segments.Add(segment);
                        result.Length += length;
                        result.MinX = Math.Min(result.MinX, Math.Min(x, nextX));
                        result.MaxX = Math.Max(result.MaxX, Math.Max(x, nextX));
                        result.MinY = Math.Min(result.MinY, Math.Min(y, nextY));
                        result.MaxY = Math.Max(result.MaxY, Math.Max(y, nextY));
                    }
                }
            }
            if (lineHasX) { x = nextX; hasX = true; }
            if (lineHasY) { y = nextY; hasY = true; }
            if (lineHasE) e = relativeExtrusion ? e + te : te;
        }
        if (result.SelectedLayer >= 0 && zByLayer.ContainsKey(result.SelectedLayer))
            result.SelectedZ = zByLayer[result.SelectedLayer];
        return result;
    }
}
'@ -Language CSharp

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
        if (-not (Test-Path -LiteralPath $root -PathType Container)) { continue }
        $match = Get-ChildItem -LiteralPath $root -Recurse -File -Filter "$name.json" -ErrorAction SilentlyContinue |
            Select-Object -First 1
        if ($null -ne $match) { return $match.FullName }
    }
    throw "Filament profile not found: $name"
}

function Write-TreeWallProbe([string] $path) {
    $vertices = [Collections.Generic.List[string]]::new()
    $faces = [Collections.Generic.List[string]]::new()

    function Add-Box(
        [double] $minX, [double] $minY, [double] $minZ,
        [double] $maxX, [double] $maxY, [double] $maxZ)
    {
        $base = $vertices.Count + 1
        foreach ($point in @(
            @($minX,$minY,$minZ), @($maxX,$minY,$minZ), @($maxX,$maxY,$minZ), @($minX,$maxY,$minZ),
            @($minX,$minY,$maxZ), @($maxX,$minY,$maxZ), @($maxX,$maxY,$maxZ), @($minX,$maxY,$maxZ)
        )) {
            $vertices.Add("v $(Format-Number $point[0]) $(Format-Number $point[1]) $(Format-Number $point[2])")
        }
        foreach ($triangle in @(
            @(0,2,1),@(0,3,2), @(4,5,6),@(4,6,7),
            @(0,1,5),@(0,5,4), @(1,2,6),@(1,6,5),
            @(2,3,7),@(2,7,6), @(3,0,4),@(3,4,7)
        )) {
            $faces.Add("f $($base+$triangle[0]) $($base+$triangle[1]) $($base+$triangle[2])")
        }
    }

    # The overlapping pedestal prevents CLI auto-drop while the broad roof
    # provides a stable, repeatable tree-support region.
    Add-Box 97 97 0 103 103 6.2
    Add-Box 80 80 6 120 120 8
    $utf8 = [Text.UTF8Encoding]::new($false)
    [IO.File]::WriteAllLines($path, @('o tree_wall_count_probe') + $vertices + $faces, $utf8)
}

function Token-Value([string] $line, [char] $token) {
    $match = [regex]::Match($line, "(?:^|\s)$token(-?(?:\d+(?:\.\d*)?|\.\d+))")
    if ($match.Success) { return [double]::Parse($match.Groups[1].Value, $Invariant) }
    return $null
}

function Read-SupportLayers([string] $path) {
    $analysis = [TreeWallGcodeParser]::Parse($path)
    if ($analysis.SupportLayerCount -lt 5) {
        throw "Only $($analysis.SupportLayerCount) support-body layers were generated"
    }
    if ($analysis.Segments.Count -eq 0) {
        throw 'The fifth support-body layer has no extrusion segments'
    }
    return $analysis
}

function Render-StyleGrid(
    [object[]] $cases, [string] $style, [string] $path, [int] $width, [double] $nozzleDiameter) {
    $columns = 4
    $rows = 3
    $height = [int]($width * 0.72)
    $margin = 70
    $header = 130
    $gap = 28
    $panelWidth = ($width - 2 * $margin - ($columns - 1) * $gap) / $columns
    $panelHeight = ($height - $header - $margin - ($rows - 1) * $gap) / $rows
    $allSegments = @($cases | ForEach-Object { $_.Analysis.Segments })
    $allX = @($allSegments | ForEach-Object { $_.X1; $_.X2 })
    $allY = @($allSegments | ForEach-Object { $_.Y1; $_.Y2 })
    $minX = [double](($allX | Measure-Object -Minimum).Minimum)
    $maxX = [double](($allX | Measure-Object -Maximum).Maximum)
    $minY = [double](($allY | Measure-Object -Minimum).Minimum)
    $maxY = [double](($allY | Measure-Object -Maximum).Maximum)
    $rangeX = [Math]::Max(0.001, $maxX - $minX)
    $rangeY = [Math]::Max(0.001, $maxY - $minY)

    $bitmap = [Drawing.Bitmap]::new($width, $height, [Drawing.Imaging.PixelFormat]::Format32bppPArgb)
    $graphics = [Drawing.Graphics]::FromImage($bitmap)
    $graphics.SmoothingMode = [Drawing.Drawing2D.SmoothingMode]::HighQuality
    $graphics.TextRenderingHint = [Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $graphics.Clear([Drawing.Color]::FromArgb(13,16,20))
    $titleFont = [Drawing.Font]::new('Segoe UI', 42, [Drawing.FontStyle]::Bold)
    $labelFont = [Drawing.Font]::new('Segoe UI', 25, [Drawing.FontStyle]::Bold)
    $smallFont = [Drawing.Font]::new('Segoe UI', 19)
    $white = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(240,244,250))
    $muted = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(170,180,194))
    $panelBrush = [Drawing.SolidBrush]::new([Drawing.Color]::FromArgb(24,29,35))
    $panelPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(58,68,80), 2)
    $renderLineWidth = [float][Math]::Max(1.8, 6.0 * $nozzleDiameter / 0.4)
    $pathPen = [Drawing.Pen]::new([Drawing.Color]::FromArgb(36,218,196), $renderLineWidth)
    $pathPen.StartCap = [Drawing.Drawing2D.LineCap]::Round
    $pathPen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $centerPen = [Drawing.Pen]::new(
        [Drawing.Color]::FromArgb(245,248,252),
        [float][Math]::Max(0.55, $renderLineWidth * 0.28))
    $centerPen.StartCap = [Drawing.Drawing2D.LineCap]::Round
    $centerPen.EndCap = [Drawing.Drawing2D.LineCap]::Round
    $graphics.DrawString("Tree support wall loops - $style", $titleFont, $white, $margin, 35)
    $graphics.DrawString(
        "Fifth support-body layer from the bed | $(Format-Number $nozzleDiameter) mm nozzle",
        $smallFont, $muted, $margin + 3, 91)

    for ($index = 0; $index -lt $cases.Count; ++$index) {
        $case = $cases[$index]
        $column = $index % $columns
        $row = [Math]::Floor($index / $columns)
        $left = $margin + $column * ($panelWidth + $gap)
        $top = $header + $row * ($panelHeight + $gap)
        $rect = [Drawing.RectangleF]::new($left, $top, $panelWidth, $panelHeight)
        $graphics.FillRectangle($panelBrush, $rect)
        $graphics.DrawRectangle($panelPen, $rect.X, $rect.Y, $rect.Width, $rect.Height)
        $graphics.DrawString("Wall count $($case.WallCount)", $labelFont, $white, $left + 25, $top + 18)
        $graphics.DrawString(
            "Z $([Math]::Round($case.Analysis.SelectedZ,2)) mm | $([Math]::Round($case.Analysis.Length,1)) mm",
            $smallFont, $muted, $left + 27, $top + 58)
        $drawRect = [Drawing.RectangleF]::new($left + 24, $top + 95, $panelWidth - 48, $panelHeight - 120)
        $scale = [Math]::Min(($drawRect.Width - 20) / $rangeX, ($drawRect.Height - 20) / $rangeY)
        $originX = $drawRect.X + ($drawRect.Width - $rangeX * $scale) / 2
        $originY = $drawRect.Y + ($drawRect.Height - $rangeY * $scale) / 2
        foreach ($segment in $case.Analysis.Segments) {
            $x1 = $originX + ($segment.X1 - $minX) * $scale
            $y1 = $originY + ($maxY - $segment.Y1) * $scale
            $x2 = $originX + ($segment.X2 - $minX) * $scale
            $y2 = $originY + ($maxY - $segment.Y2) * $scale
            $graphics.DrawLine($pathPen, [float]$x1, [float]$y1, [float]$x2, [float]$y2)
            $graphics.DrawLine($centerPen, [float]$x1, [float]$y1, [float]$x2, [float]$y2)
        }
    }
    $bitmap.Save($path, [Drawing.Imaging.ImageFormat]::Png)
    foreach ($item in @($centerPen,$pathPen,$panelPen,$panelBrush,$muted,$white,$smallFont,$labelFont,$titleFont,$graphics,$bitmap)) {
        $item.Dispose()
    }
}

$runRoot = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd-HHmmss')
New-Item -ItemType Directory -Force -Path $runRoot | Out-Null
$modelPath = Join-Path $runRoot 'tree-wall-count-probe.obj'
Write-TreeWallProbe $modelPath
$filaments = @(
    (Find-FilamentProfile 'Snapmaker PLA @U1'),
    (Find-FilamentProfile 'Snapmaker ABS @U1'),
    (Find-FilamentProfile 'Snapmaker PETG @U1'),
    (Find-FilamentProfile 'Snapmaker TPU @U1')
)
$nozzleText = Format-Number $NozzleDiameter
$layerHeightText = Format-Number ([Math]::Min(0.2, $NozzleDiameter * 0.5333333333))

$results = [Collections.Generic.List[object]]::new()
$renderCases = @{}
foreach ($style in @('tree_slim','organic')) {
    $renderCases[$style] = [Collections.Generic.List[object]]::new()
    foreach ($wallCount in 0..10) {
        $caseRoot = Join-Path $runRoot "$style-$wallCount"
        New-Item -ItemType Directory -Force -Path $caseRoot | Out-Null
        $machinePath = Join-Path $caseRoot 'machine.json'
        $processPath = Join-Path $caseRoot 'process.json'
        $machine = Get-Content -LiteralPath $BaseMachinePath -Raw | ConvertFrom-Json
        $process = Get-Content -LiteralPath $BaseProcessPath -Raw | ConvertFrom-Json
        $name = "Tree wall verify $style $wallCount"
        $machine.name = $name
        $machine.setting_id = "tree-wall-$style-$wallCount"
        $process.name = $name
        $process.setting_id = "tree-wall-$style-$wallCount"
        $process.compatible_printers = @($name)
        $machine.nozzle_diameter = @($nozzleText,$nozzleText,$nozzleText,$nozzleText)
        foreach ($widthSetting in @(
            'toolhead_line_width',
            'toolhead_initial_layer_line_width',
            'toolhead_outer_wall_line_width',
            'toolhead_inner_wall_line_width',
            'toolhead_top_surface_line_width',
            'toolhead_sparse_infill_line_width',
            'toolhead_internal_solid_infill_line_width',
            'toolhead_support_line_width',
            'toolhead_bridge_line_width'
        )) {
            Set-JsonProperty $machine $widthSetting @($nozzleText,$nozzleText,$nozzleText,$nozzleText)
        }
        foreach ($setting in @{
            layer_height=$layerHeightText; initial_layer_print_height=$layerHeightText; enable_support='1';
            line_width=$nozzleText;
            support_type='tree(auto)'; support_style=$style; support_threshold_angle='90';
            support_interface_top_layers='0'; support_interface_bottom_layers='0';
            support_base_pattern='default'; support_base_pattern_spacing='2.5';
            support_on_build_plate_only='1'; independent_support_layer_height='0';
            tree_support_wall_count=[string]$wallCount; tree_support_with_infill='0';
            tree_support_branch_diameter='10'; tree_support_branch_diameter_organic='10';
            tree_support_tip_diameter=$nozzleText; tree_support_branch_distance='5';
            tree_support_branch_distance_organic='5'; support_line_width=$nozzleText;
            use_smaller_nozzles_in_crisp_corners='0'
        }.GetEnumerator()) {
            Set-JsonProperty $process $setting.Key $setting.Value
        }
        $machine | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $machinePath -Encoding UTF8
        $process | ConvertTo-Json -Depth 100 | Set-Content -LiteralPath $processPath -Encoding UTF8

        $settingsArg = [char]34 + "$machinePath;$processPath" + [char]34
        $filamentsArg = [char]34 + ($filaments -join ';') + [char]34
        $stdout = Join-Path $caseRoot 'cli.out.log'
        $stderr = Join-Path $caseRoot 'cli.err.log'
        $arguments = @(
            '--slice','0','--debug','1',
            '--load-settings',$settingsArg,
            '--load-filaments',$filamentsArg,
            '--outputdir',([char]34 + $caseRoot + [char]34),
            ([char]34 + $modelPath + [char]34)
        )
        $handle = Start-Process -FilePath $SlicerPath -ArgumentList $arguments -NoNewWindow -PassThru `
            -RedirectStandardOutput $stdout -RedirectStandardError $stderr
        if (-not $handle.WaitForExit($SliceTimeoutSeconds * 1000)) {
            $handle.Kill()
            throw "Slice timeout: $style wall count $wallCount"
        }
        $gcode = Get-ChildItem -LiteralPath $caseRoot -File -Filter '*.gcode' | Select-Object -First 1
        if ($null -eq $gcode) {
            throw "G-code was not generated for $style wall count $wallCount. $(Get-Content $stderr -Raw)"
        }
        $analysis = Read-SupportLayers $gcode.FullName
        $embedded = $analysis.Settings['tree_support_wall_count']
        $embeddedNozzles = @($analysis.Settings['nozzle_diameter'] -split ',' | ForEach-Object {
            [double]::Parse($_.Trim(), $Invariant)
        })
        $embeddedSupportWidth = [double]::Parse($analysis.Settings['support_line_width'].TrimEnd('%'), $Invariant)
        $nozzleApplied = $embeddedNozzles.Count -eq 4 -and
            @($embeddedNozzles | Where-Object { [Math]::Abs($_ - $NozzleDiameter) -gt 0.000001 }).Count -eq 0
        $supportWidthApplied = [Math]::Abs($embeddedSupportWidth - $NozzleDiameter) -le 0.000001
        $passed = $embedded -eq [string]$wallCount -and $analysis.NonFinite -eq 0 -and
            $analysis.Segments.Count -gt 0 -and $analysis.Length -gt 0 -and
            $nozzleApplied -and $supportWidthApplied
        $case = [pscustomobject]@{
            Style=$style
            WallCount=$wallCount
            EmbeddedValue=$embedded
            NozzleDiameter=$NozzleDiameter
            EmbeddedNozzles=($embeddedNozzles -join ',')
            EmbeddedSupportWidth=$embeddedSupportWidth
            SelectedLayer=$analysis.SelectedLayer
            SelectedZ=[Math]::Round($analysis.SelectedZ,3)
            SegmentCount=$analysis.Segments.Count
            ExtrusionLength=[Math]::Round($analysis.Length,3)
            SupportLayerCount=$analysis.SupportLayerCount
            NonFinite=$analysis.NonFinite
            Passed=$passed
            Gcode=$gcode.FullName
            Analysis=$analysis
        }
        $results.Add($case)
        $renderCases[$style].Add($case)
    }
}

$errors = [Collections.Generic.List[string]]::new()
foreach ($style in @('tree_slim','organic')) {
    $cases = @($results | Where-Object Style -eq $style | Sort-Object WallCount)
    foreach ($case in $cases) {
        if (-not $case.Passed) { $errors.Add("$style count $($case.WallCount) failed basic G-code validation") }
    }
    $explicit = @($cases | Where-Object WallCount -gt 0)
    $distinctLengths = @($explicit | ForEach-Object { [Math]::Round($_.ExtrusionLength,1) } | Sort-Object -Unique)
    if ($distinctLengths.Count -lt 6) {
        $errors.Add("$style produced only $($distinctLengths.Count) distinct fifth-layer path lengths for counts 1..10")
    }
    if ($explicit[-1].ExtrusionLength -le $explicit[0].ExtrusionLength * 1.5) {
        $errors.Add("$style count 10 did not add enough fifth-layer wall extrusion over count 1")
    }
    for ($index = 1; $index -lt $explicit.Count; ++$index) {
        # Organic branches may saturate at the available branch width. A topology
        # rebuild at saturation can change total path length slightly without
        # removing a printable wall, so reject only a material (>1%) regression.
        if ($explicit[$index].ExtrusionLength -lt $explicit[$index - 1].ExtrusionLength * 0.99) {
            $errors.Add("$style fifth-layer extrusion decreased from count $($explicit[$index-1].WallCount) to $($explicit[$index].WallCount)")
        }
    }
    $imagePath = Join-Path $runRoot "$style-fifth-support-layer.png"
    Render-StyleGrid $renderCases[$style] $style $imagePath $ImageWidth $NozzleDiameter
}

$serializable = $results | Select-Object Style,WallCount,EmbeddedValue,NozzleDiameter,EmbeddedNozzles,EmbeddedSupportWidth,SelectedLayer,SelectedZ,SegmentCount,ExtrusionLength,SupportLayerCount,NonFinite,Passed,Gcode
$jsonPath = Join-Path $runRoot 'results.json'
$csvPath = Join-Path $runRoot 'results.csv'
$serializable | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $jsonPath -Encoding UTF8
$serializable | Export-Csv -LiteralPath $csvPath -NoTypeInformation -Encoding UTF8
$serializable | Format-Table Style,WallCount,EmbeddedValue,SelectedZ,SegmentCount,ExtrusionLength,Passed -AutoSize
Write-Output "RUN_ROOT=$runRoot"
Write-Output "RESULTS=$jsonPath"
Write-Output "SLIM_IMAGE=$(Join-Path $runRoot 'tree_slim-fifth-support-layer.png')"
Write-Output "ORGANIC_IMAGE=$(Join-Path $runRoot 'organic-fifth-support-layer.png')"
Write-Output "PASSED=$($results.Count - @($results | Where-Object {-not $_.Passed}).Count) FAILED=$(@($results | Where-Object {-not $_.Passed}).Count)"
if ($errors.Count -gt 0) {
    $errors | ForEach-Object { Write-Error $_ }
    exit 1
}
