param(
    [string] $Executable = "build/src/Release/magpie-slicer.exe",
    [string] $Model = "tests/data/overhang.obj",
    [string] $BaseCase = "build/verification/support-post-lowtemp-fix-20260728-213109/20260728-213110/cura_90_triangles",
    [string] $OutputRoot = "",
    [switch] $ReuseExisting
)

$ErrorActionPreference = "Stop"

$Executable = (Resolve-Path $Executable).Path
$Model = (Resolve-Path $Model).Path
$BaseCase = (Resolve-Path $BaseCase).Path
if (-not $OutputRoot) {
    $OutputRoot = Join-Path (Resolve-Path "build/verification").Path ("cura-solid-support-raft-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
New-Item -ItemType Directory -Force -Path $OutputRoot | Out-Null
$tracePath = Join-Path $OutputRoot "trace.log"
Set-Content $tracePath ("START " + (Get-Date -Format o))
function Write-Trace([string] $Message) {
    Add-Content $tracePath ((Get-Date -Format o) + " " + $Message)
}

$cases = @(
    @{ Name = "cura_off_10";  Type = "normal_cura(auto)"; Toggle = "0"; Density = "10%" },
    @{ Name = "cura_on_10";   Type = "normal_cura(auto)"; Toggle = "1"; Density = "10%" },
    @{ Name = "cura_off_60";  Type = "normal_cura(auto)"; Toggle = "0"; Density = "60%" },
    @{ Name = "cura_on_60";   Type = "normal_cura(auto)"; Toggle = "1"; Density = "60%" },
    @{ Name = "cura_off_100"; Type = "normal_cura(auto)"; Toggle = "0"; Density = "100%" },
    @{ Name = "cura_on_100";  Type = "normal_cura(auto)"; Toggle = "1"; Density = "100%" },
    @{ Name = "prusa_off_10"; Type = "normal(auto)";      Toggle = "0"; Density = "10%" },
    @{ Name = "prusa_on_10";  Type = "normal(auto)";      Toggle = "1"; Density = "10%" }
)

function Wait-GCodeComplete([string] $Path, [int] $TimeoutSeconds = 180) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastLength = -1L
    $stableSamples = 0
    while ((Get-Date) -lt $deadline) {
        if (Test-Path $Path) {
            $length = (Get-Item $Path).Length
            if ($length -gt 0 -and $length -eq $lastLength) {
                $stableSamples++
            } else {
                $stableSamples = 0
                $lastLength = $length
            }
            if ($stableSamples -ge 3) {
                $tail = Get-Content $Path -Tail 12
                if ($tail -match "CONFIG_BLOCK_END") {
                    return
                }
            }
        }
        Start-Sleep -Milliseconds 500
    }
    throw "G-code output did not complete within $TimeoutSeconds seconds: $Path"
}

Add-Type -TypeDefinition @'
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;

public sealed class GCodeLayerStats
{
    public int Layer { get; set; }
    public int Segments { get; set; }
    public double PathLength { get; set; }
    public double Extrusion { get; set; }
    public double BBoxArea { get; set; }
    public int Scanlines { get; set; }
    public double MedianGap { get; set; }
}

public static class GCodeFirstLayers
{
    private sealed class Accumulator
    {
        public int Segments;
        public double Length;
        public double Extrusion;
        public double MinX = double.PositiveInfinity;
        public double MaxX = double.NegativeInfinity;
        public double MinY = double.PositiveInfinity;
        public double MaxY = double.NegativeInfinity;
        public readonly List<double> Horizontal = new List<double>();
        public readonly List<double> Vertical = new List<double>();
    }

    private static readonly Regex ParameterRegex = new Regex(
        @"(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))",
        RegexOptions.Compiled | RegexOptions.CultureInvariant);

    public static GCodeLayerStats[] Analyze(string path)
    {
        var stats = new[] { new Accumulator(), new Accumulator() };
        var layer = -1;
        var role = string.Empty;
        var absoluteXY = true;
        var absoluteE = false;
        var x = 0.0;
        var y = 0.0;
        var e = 0.0;

        foreach (var line in File.ReadLines(path))
        {
            if (line == ";LAYER_CHANGE")
            {
                layer++;
                if (layer > 1)
                    break;
                role = string.Empty;
                continue;
            }
            if (line.StartsWith(";TYPE:", StringComparison.Ordinal))
            {
                role = line.Substring(6).Trim();
                continue;
            }
            if (line == "G90" || line.StartsWith("G90 ", StringComparison.Ordinal)) { absoluteXY = true; continue; }
            if (line == "G91" || line.StartsWith("G91 ", StringComparison.Ordinal)) { absoluteXY = false; continue; }
            if (line == "M82" || line.StartsWith("M82 ", StringComparison.Ordinal)) { absoluteE = true; continue; }
            if (line == "M83" || line.StartsWith("M83 ", StringComparison.Ordinal)) { absoluteE = false; continue; }
            if (line == "G92" || line.StartsWith("G92 ", StringComparison.Ordinal))
            {
                foreach (Match match in ParameterRegex.Matches(line))
                {
                    var value = double.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
                    switch (match.Groups[1].Value[0])
                    {
                    case 'X': x = value; break;
                    case 'Y': y = value; break;
                    case 'E': e = value; break;
                    }
                }
                continue;
            }
            if (!(line == "G0" || line.StartsWith("G0 ", StringComparison.Ordinal) ||
                  line == "G1" || line.StartsWith("G1 ", StringComparison.Ordinal)))
                continue;

            var nextX = x;
            var nextY = y;
            var nextE = e;
            var hasE = false;
            foreach (Match match in ParameterRegex.Matches(line))
            {
                var value = double.Parse(match.Groups[2].Value, CultureInfo.InvariantCulture);
                switch (match.Groups[1].Value[0])
                {
                case 'X': nextX = absoluteXY ? value : x + value; break;
                case 'Y': nextY = absoluteXY ? value : y + value; break;
                case 'E': hasE = true; nextE = absoluteE ? value : e + value; break;
                }
            }

            var deltaE = nextE - e;
            var deltaX = nextX - x;
            var deltaY = nextY - y;
            var length = Math.Sqrt(deltaX * deltaX + deltaY * deltaY);
            if ((layer == 0 || layer == 1) && role == "Support" && hasE && deltaE > 0.0000001 && length > 0)
            {
                var stat = stats[layer];
                stat.Segments++;
                stat.Length += length;
                stat.Extrusion += deltaE;
                stat.MinX = Math.Min(stat.MinX, Math.Min(x, nextX));
                stat.MaxX = Math.Max(stat.MaxX, Math.Max(x, nextX));
                stat.MinY = Math.Min(stat.MinY, Math.Min(y, nextY));
                stat.MaxY = Math.Max(stat.MaxY, Math.Max(y, nextY));
                if (length > 2 && Math.Abs(deltaX) > 5 * Math.Abs(deltaY))
                    stat.Horizontal.Add(Math.Round((y + nextY) / 2, 3));
                if (length > 2 && Math.Abs(deltaY) > 5 * Math.Abs(deltaX))
                    stat.Vertical.Add(Math.Round((x + nextX) / 2, 3));
            }
            x = nextX;
            y = nextY;
            e = nextE;
        }

        return new[] { BuildResult(1, stats[0]), BuildResult(2, stats[1]) };
    }

    private static GCodeLayerStats BuildResult(int layer, Accumulator stat)
    {
        var coordinates = (stat.Horizontal.Count >= stat.Vertical.Count ? stat.Horizontal : stat.Vertical)
            .Distinct().OrderBy(value => value).ToArray();
        var gaps = new List<double>();
        for (var index = 1; index < coordinates.Length; index++)
        {
            var gap = coordinates[index] - coordinates[index - 1];
            if (gap > 0.05)
                gaps.Add(gap);
        }
        gaps.Sort();
        var medianGap = gaps.Count == 0 ? 0.0 :
            gaps.Count % 2 == 1 ? gaps[gaps.Count / 2] :
            (gaps[gaps.Count / 2 - 1] + gaps[gaps.Count / 2]) / 2;
        var area = double.IsInfinity(stat.MinX) ? 0.0 : (stat.MaxX - stat.MinX) * (stat.MaxY - stat.MinY);
        return new GCodeLayerStats {
            Layer = layer,
            Segments = stat.Segments,
            PathLength = Math.Round(stat.Length, 3),
            Extrusion = Math.Round(stat.Extrusion, 4),
            BBoxArea = Math.Round(area, 3),
            Scanlines = coordinates.Length,
            MedianGap = Math.Round(medianGap, 3)
        };
    }
}
'@
Write-Trace "ANALYZER_READY"

function Get-Median([double[]] $Values) {
    if (-not $Values -or $Values.Count -eq 0) {
        return 0.0
    }
    $sorted = @($Values | Sort-Object)
    $middle = [int] ($sorted.Count / 2)
    if ($sorted.Count % 2) {
        return [double] $sorted[$middle]
    }
    return ([double] $sorted[$middle - 1] + [double] $sorted[$middle]) / 2
}

function Analyze-GCode([string] $Path) {
    $layer = -1
    $role = ""
    $absoluteXY = $true
    $absoluteE = $false
    $x = 0.0
    $y = 0.0
    $e = 0.0
    $stats = @{
        0 = [ordered] @{ Segments = 0; Length = 0.0; Extrusion = 0.0; MinX = [double]::PositiveInfinity; MaxX = [double]::NegativeInfinity; MinY = [double]::PositiveInfinity; MaxY = [double]::NegativeInfinity; H = @(); V = @() }
        1 = [ordered] @{ Segments = 0; Length = 0.0; Extrusion = 0.0; MinX = [double]::PositiveInfinity; MaxX = [double]::NegativeInfinity; MinY = [double]::PositiveInfinity; MaxY = [double]::NegativeInfinity; H = @(); V = @() }
    }

    foreach ($line in [System.IO.File]::ReadLines($Path)) {
        if ($line -eq ";LAYER_CHANGE") {
            $layer++
            if ($layer -gt 1) {
                break
            }
            $role = ""
            continue
        }
        if ($line -match "^;TYPE:(.+)$") {
            $role = $matches[1].Trim()
            continue
        }
        if ($line -match "^G90(?:\s|$)") { $absoluteXY = $true; continue }
        if ($line -match "^G91(?:\s|$)") { $absoluteXY = $false; continue }
        if ($line -match "^M82(?:\s|$)") { $absoluteE = $true; continue }
        if ($line -match "^M83(?:\s|$)") { $absoluteE = $false; continue }
        if ($line -match "^G92(?:\s|$)") {
            foreach ($match in [regex]::Matches($line, "(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))")) {
                $value = [double] $match.Groups[2].Value
                switch ($match.Groups[1].Value) {
                    "X" { $x = $value }
                    "Y" { $y = $value }
                    "E" { $e = $value }
                }
            }
            continue
        }
        if ($line -notmatch "^G[01](?:\s|$)") {
            continue
        }

        $nextX = $x
        $nextY = $y
        $nextE = $e
        $hasE = $false
        foreach ($match in [regex]::Matches($line, "(?:^|\s)([XYE])(-?(?:\d+(?:\.\d*)?|\.\d+))")) {
            $value = [double] $match.Groups[2].Value
            switch ($match.Groups[1].Value) {
                "X" { $nextX = if ($absoluteXY) { $value } else { $x + $value } }
                "Y" { $nextY = if ($absoluteXY) { $value } else { $y + $value } }
                "E" { $hasE = $true; $nextE = if ($absoluteE) { $value } else { $e + $value } }
            }
        }

        $deltaE = $nextE - $e
        $deltaX = $nextX - $x
        $deltaY = $nextY - $y
        $length = [Math]::Sqrt($deltaX * $deltaX + $deltaY * $deltaY)
        if (($layer -eq 0 -or $layer -eq 1) -and $role -eq "Support" -and $hasE -and $deltaE -gt 0.0000001 -and $length -gt 0) {
            $stat = $stats[$layer]
            $stat.Segments++
            $stat.Length += $length
            $stat.Extrusion += $deltaE
            $stat.MinX = [Math]::Min($stat.MinX, [Math]::Min($x, $nextX))
            $stat.MaxX = [Math]::Max($stat.MaxX, [Math]::Max($x, $nextX))
            $stat.MinY = [Math]::Min($stat.MinY, [Math]::Min($y, $nextY))
            $stat.MaxY = [Math]::Max($stat.MaxY, [Math]::Max($y, $nextY))
            if ($length -gt 2 -and [Math]::Abs($deltaX) -gt 5 * [Math]::Abs($deltaY)) {
                $stat.H += [Math]::Round(($y + $nextY) / 2, 3)
            }
            if ($length -gt 2 -and [Math]::Abs($deltaY) -gt 5 * [Math]::Abs($deltaX)) {
                $stat.V += [Math]::Round(($x + $nextX) / 2, 3)
            }
        }
        $x = $nextX
        $y = $nextY
        $e = $nextE
    }

    $output = @()
    foreach ($id in 0, 1) {
        $stat = $stats[$id]
        $coordinates = if ($stat.H.Count -ge $stat.V.Count) { @($stat.H | Sort-Object -Unique) } else { @($stat.V | Sort-Object -Unique) }
        $gaps = @()
        for ($index = 1; $index -lt $coordinates.Count; $index++) {
            $gap = [double] $coordinates[$index] - [double] $coordinates[$index - 1]
            if ($gap -gt 0.05) {
                $gaps += $gap
            }
        }
        $area = if ([double]::IsInfinity($stat.MinX)) { 0.0 } else { ($stat.MaxX - $stat.MinX) * ($stat.MaxY - $stat.MinY) }
        $output += [pscustomobject] @{
            Layer = $id + 1
            Segments = $stat.Segments
            PathLength = [Math]::Round($stat.Length, 3)
            Extrusion = [Math]::Round($stat.Extrusion, 4)
            BBoxArea = [Math]::Round($area, 3)
            Scanlines = $coordinates.Count
            MedianGap = [Math]::Round((Get-Median $gaps), 3)
        }
    }
    return $output
}

$results = @()
foreach ($case in $cases) {
    Write-Trace ("CASE_START " + $case.Name)
    $caseDirectory = Join-Path $OutputRoot $case.Name
    New-Item -ItemType Directory -Force -Path $caseDirectory | Out-Null
    $machinePath = Join-Path $caseDirectory "machine.json"
    $processPath = Join-Path $caseDirectory "process.json"
    $gcodePath = Join-Path $caseDirectory "plate_1.gcode"
    $stopwatch = [Diagnostics.Stopwatch]::StartNew()
    if (-not $ReuseExisting) {
        Copy-Item (Join-Path $BaseCase "machine.json") $machinePath
        $process = Get-Content (Join-Path $BaseCase "process.json") -Raw | ConvertFrom-Json
        $process.support_type = $case.Type
        $process | Add-Member -NotePropertyName "cura_solid_support_raft" -NotePropertyValue $case.Toggle -Force
        $process | Add-Member -NotePropertyName "raft_first_layer_density" -NotePropertyValue $case.Density -Force
        $process | Add-Member -NotePropertyName "raft_layers" -NotePropertyValue "0" -Force
        $process | ConvertTo-Json -Depth 100 | Set-Content $processPath -Encoding utf8
        $arguments = @("--slice", "0", "--load-settings", $machinePath, "--load-settings", $processPath, "--outputdir", $caseDirectory, $Model)
        Start-Process -FilePath $Executable -ArgumentList $arguments -RedirectStandardOutput (Join-Path $caseDirectory "cli.out.log") -RedirectStandardError (Join-Path $caseDirectory "cli.err.log") | Out-Null
        Wait-GCodeComplete $gcodePath
    } elseif (-not (Test-Path $gcodePath)) {
        throw "Missing existing G-code: $gcodePath"
    }
    $stopwatch.Stop()

    $header = Get-Content $gcodePath -TotalCount 4 | Where-Object { $_ -match "^; generated by " }
    Write-Trace ("ANALYZE_START " + $case.Name)
    $layers = [GCodeFirstLayers]::Analyze($gcodePath)
    Write-Trace ("ANALYZE_DONE " + $case.Name)
    $results += [pscustomobject] @{
        Case = $case.Name
        SupportType = $case.Type
        Toggle = $case.Toggle
        Density = $case.Density
        Seconds = [Math]::Round($stopwatch.Elapsed.TotalSeconds, 2)
        Header = $header
        Layer1 = $layers[0]
        Layer2 = $layers[1]
        GCode = $gcodePath
    }
    Write-Output ("SLICED {0} in {1:N2}s | L1={2}mm/{3} lines | L2={4}mm/{5} lines" -f $case.Name, $stopwatch.Elapsed.TotalSeconds, $layers[0].PathLength, $layers[0].Scanlines, $layers[1].PathLength, $layers[1].Scanlines)
}

Write-Trace "WRITE_RESULTS"
$results | ForEach-Object {
    [pscustomobject] @{
        Case = $_.Case
        SupportType = $_.SupportType
        Toggle = $_.Toggle
        Density = $_.Density
        Header = $_.Header
        Layer1PathLength = $_.Layer1.PathLength
        Layer1Segments = $_.Layer1.Segments
        Layer1Scanlines = $_.Layer1.Scanlines
        Layer1MedianGap = $_.Layer1.MedianGap
        Layer2PathLength = $_.Layer2.PathLength
        Layer2Segments = $_.Layer2.Segments
        Layer2Scanlines = $_.Layer2.Scanlines
        Layer2MedianGap = $_.Layer2.MedianGap
        GCode = $_.GCode
    }
} | Export-Csv (Join-Path $OutputRoot "results.csv") -NoTypeInformation -Encoding utf8
$failures = @()
if (@($results | Where-Object { $_.Header -notmatch "^; generated by OrcaSlicer " }).Count) {
    $failures += "A generated G-code has a non-compatible producer header."
}
foreach ($density in "10%", "60%", "100%") {
    $suffix = $density.TrimEnd("%")
    $off = $results | Where-Object { $_.Case -eq "cura_off_$suffix" }
    $on = $results | Where-Object { $_.Case -eq "cura_on_$suffix" }
    if ($on.Layer1.PathLength -le $off.Layer1.PathLength * 1.25) {
        $failures += "Cura ON did not densify layer 1 at $density."
    }
    if ([Math]::Abs($on.Layer2.PathLength - $off.Layer2.PathLength) -gt [Math]::Max(0.5, $off.Layer2.PathLength * 0.005)) {
        $failures += "The Cura toggle changed layer 2 at $density."
    }
}
$onLengths = @($results | Where-Object { $_.Case -match "^cura_on_" } | ForEach-Object { $_.Layer1.PathLength })
if (($onLengths | Measure-Object -Maximum).Maximum - ($onLengths | Measure-Object -Minimum).Minimum -gt 0.5) {
    $failures += "The forced solid Cura layer depends on the legacy density value."
}
$prusaOff = $results | Where-Object { $_.Case -eq "prusa_off_10" }
$prusaOn = $results | Where-Object { $_.Case -eq "prusa_on_10" }
if ([Math]::Abs($prusaOff.Layer1.PathLength - $prusaOn.Layer1.PathLength) -gt 0.5 -or
    [Math]::Abs($prusaOff.Layer2.PathLength - $prusaOn.Layer2.PathLength) -gt 0.5) {
    $failures += "The Cura-only toggle affected non-Cura support."
}

Write-Output "RESULT_ROOT=$OutputRoot"
$results | ForEach-Object {
    [pscustomobject] @{
        Case = $_.Case
        L1Length = $_.Layer1.PathLength
        L1Segments = $_.Layer1.Segments
        L1Scanlines = $_.Layer1.Scanlines
        L1Gap = $_.Layer1.MedianGap
        L2Length = $_.Layer2.PathLength
        L2Segments = $_.Layer2.Segments
        L2Scanlines = $_.Layer2.Scanlines
        L2Gap = $_.Layer2.MedianGap
    }
} | Format-Table -AutoSize

if ($failures.Count) {
    Write-Trace ("FAILED " + ($failures -join " | "))
    throw ($failures -join "`n")
}
Write-Trace "PASS"
Write-Output "RUNTIME_VALIDATION=PASS"
