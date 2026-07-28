param(
    [string] $OutputPath
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $RepoRoot 'tests\data\multinozzle_feature_matrix.obj'
}

$vertices = [Collections.Generic.List[object]]::new()
$faces = [Collections.Generic.List[int[]]]::new()
$groups = [Collections.Generic.List[object]]::new()

function Add-Vertex([double] $x, [double] $y, [double] $z) {
    $vertices.Add(@($x, $y, $z))
    return $vertices.Count
}

function Add-ConvexPrism([string] $name, [object[]] $points, [double] $z0, [double] $z1) {
    if ($points.Count -lt 3) { throw "Prism '$name' needs at least three points" }
    $firstFace = $faces.Count
    $bottom = @($points | ForEach-Object { Add-Vertex $_[0] $_[1] $z0 })
    $top = @($points | ForEach-Object { Add-Vertex $_[0] $_[1] $z1 })
    for ($i = 1; $i -lt $points.Count - 1; ++$i) {
        $faces.Add(@($bottom[0], $bottom[$i + 1], $bottom[$i]))
        $faces.Add(@($top[0], $top[$i], $top[$i + 1]))
    }
    for ($i = 0; $i -lt $points.Count; ++$i) {
        $next = ($i + 1) % $points.Count
        $faces.Add(@($bottom[$i], $bottom[$next], $top[$next]))
        $faces.Add(@($bottom[$i], $top[$next], $top[$i]))
    }
    $groups.Add([pscustomobject]@{ Name=$name; First=$firstFace; Count=$faces.Count-$firstFace })
}

function Add-Box([string] $name, [double] $x0, [double] $y0, [double] $x1, [double] $y1, [double] $z0, [double] $z1) {
    Add-ConvexPrism $name @(@($x0,$y0),@($x1,$y0),@($x1,$y1),@($x0,$y1)) $z0 $z1
}

function Add-Cylinder([string] $name, [double] $cx, [double] $cy, [double] $radius, [double] $z0, [double] $z1, [int] $segments) {
    $points = @()
    for ($i = 0; $i -lt $segments; ++$i) {
        $angle = 2.0 * [Math]::PI * $i / $segments
        $points += ,@(
            ($cx + $radius * [Math]::Cos($angle)),
            ($cy + $radius * [Math]::Sin($angle))
        )
    }
    Add-ConvexPrism $name $points $z0 $z1
}

# Stable base and long straight walls for spacing/interlocking measurements.
Add-Box 'base' 35 35 185 105 0 4

# Acute and obtuse convex corners.
Add-ConvexPrism 'acute_spike' @(@(45,45),@(72,45),@(46.0,72)) 4 20
Add-ConvexPrism 'diamond_corner' @(@(78,45),@(88,55),@(78,65),@(68,55)) 4 16

# Concave U and L shapes, represented as overlapping printable solids.
Add-Box 'u_left' 92 45 97 72 4 18
Add-Box 'u_right' 112 45 117 72 4 18
Add-Box 'u_bottom' 92 45 117 50 4 18
Add-Box 'l_vertical' 123 45 128 72 4 15
Add-Box 'l_horizontal' 123 45 143 50 4 15

# Small letter-like E strokes. A full connected stroke must stay on one nozzle.
Add-Box 'letter_e_spine' 45 80 46 99 4 14
Add-Box 'letter_e_top' 45 98 56 99 4 14
Add-Box 'letter_e_mid' 45 89 54 90 4 14
Add-Box 'letter_e_bottom' 45 80 56 81 4 14

# Narrow printable features around the two nozzle capabilities.
Add-Box 'thin_022' 65 80 65.22 100 4 12
Add-Box 'thin_035' 70 80 70.35 100 4 12
Add-Box 'thin_050' 76 80 76.50 100 4 12

# Curvature and small fillet-like polygons.
Add-Cylinder 'round_r8' 95 90 8 4 20 64
Add-Cylinder 'round_r2' 112 90 2 4 16 32
Add-Cylinder 'faceted_r5' 127 90 5 4 18 8

# Short positive details that disappear at different nozzle diameters.
Add-ConvexPrism 'micro_triangle' @(@(140,82),@(149,82),@(140.4,91)) 4 13
Add-ConvexPrism 'small_hex' @(@(158,84),@(161,82),@(164,84),@(164,88),@(161,90),@(158,88)) 4 17

$directory = Split-Path -Parent ([IO.Path]::GetFullPath($OutputPath))
New-Item -ItemType Directory -Force -Path $directory | Out-Null
$lines = [Collections.Generic.List[string]]::new()
$lines.Add('# Multi-nozzle feature coverage model: convex, concave, thin, text-like, curved')
foreach ($vertex in $vertices) {
    $lines.Add(('v {0} {1} {2}' -f
        ([double]$vertex[0]).ToString('0.#####',$Invariant),
        ([double]$vertex[1]).ToString('0.#####',$Invariant),
        ([double]$vertex[2]).ToString('0.#####',$Invariant)))
}
foreach ($group in $groups) {
    $lines.Add("g $($group.Name)")
    for ($i = $group.First; $i -lt $group.First + $group.Count; ++$i) {
        $face = $faces[$i]
        $lines.Add("f $($face[0]) $($face[1]) $($face[2])")
    }
}
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($OutputPath), $lines, [Text.UTF8Encoding]::new($false))
Write-Output ([IO.Path]::GetFullPath($OutputPath))
