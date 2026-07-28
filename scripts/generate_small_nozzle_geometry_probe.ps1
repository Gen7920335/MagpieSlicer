param(
    [string] $StlPath,
    [string] $PreviewSvgPath
)

$ErrorActionPreference = 'Stop'
$Invariant = [Globalization.CultureInfo]::InvariantCulture
$RepoRoot = Split-Path -Parent $PSScriptRoot

if ([string]::IsNullOrWhiteSpace($StlPath)) {
    $StlPath = Join-Path $RepoRoot 'tests\data\small_nozzle_geometry_probe.stl'
}
if ([string]::IsNullOrWhiteSpace($PreviewSvgPath)) {
    $PreviewSvgPath = Join-Path $RepoRoot 'tests\data\small_nozzle_geometry_probe_views.svg'
}

$vertices = [Collections.Generic.List[double[]]]::new()
$triangles = [Collections.Generic.List[object]]::new()

function Add-Vertex([double] $x, [double] $y, [double] $z) {
    $vertices.Add([double[]]@($x, $y, $z))
    return $vertices.Count - 1
}

function Add-Triangle([int] $a, [int] $b, [int] $c, [string] $group) {
    $triangles.Add([pscustomobject]@{ A=$a; B=$b; C=$c; Group=$group })
}

function Add-ConvexPrism([string] $group, [object[]] $points, [double] $z0, [double] $z1) {
    if ($points.Count -lt 3) { throw "Prism '$group' needs at least three points" }
    $bottom = @($points | ForEach-Object { Add-Vertex $_[0] $_[1] $z0 })
    $top = @($points | ForEach-Object { Add-Vertex $_[0] $_[1] $z1 })

    for ($i = 1; $i -lt $points.Count - 1; ++$i) {
        Add-Triangle $bottom[0] $bottom[$i + 1] $bottom[$i] $group
        Add-Triangle $top[0] $top[$i] $top[$i + 1] $group
    }
    for ($i = 0; $i -lt $points.Count; ++$i) {
        $next = ($i + 1) % $points.Count
        Add-Triangle $bottom[$i] $bottom[$next] $top[$next] $group
        Add-Triangle $bottom[$i] $top[$next] $top[$i] $group
    }
}

function Add-Box(
    [string] $group,
    [double] $x0, [double] $y0,
    [double] $x1, [double] $y1,
    [double] $z0, [double] $z1
) {
    Add-ConvexPrism $group @(
        @($x0, $y0), @($x1, $y0), @($x1, $y1), @($x0, $y1)
    ) $z0 $z1
}

function Add-Cylinder(
    [string] $group,
    [double] $cx, [double] $cy, [double] $radius,
    [double] $z0, [double] $z1, [int] $segments
) {
    $points = @()
    for ($i = 0; $i -lt $segments; ++$i) {
        $angle = 2.0 * [Math]::PI * $i / $segments
        $points += ,@(
            ($cx + $radius * [Math]::Cos($angle)),
            ($cy + $radius * [Math]::Sin($angle))
        )
    }
    Add-ConvexPrism $group $points $z0 $z1
}

# Stable reference base.
Add-Box 'base' 0 0 120 80 0 2

# Wide body for deterministic wall-count and center-spacing measurements.
Add-Box 'straight' 7 7 44 39 2 20

# Exact convex angle probes.
Add-ConvexPrism 'angles' @(@(50,7), @(72,7), @(72,19.7017)) 2 14  # 30 degrees
Add-ConvexPrism 'angles' @(@(76,7), @(94,7), @(85,22.5885)) 2 17  # 60 degrees
Add-ConvexPrism 'angles' @(@(99,7), @(114,7), @(99,22)) 2 20       # 90 degrees

# Concave U and L assemblies. Their internal corners exercise negative-angle routing.
Add-Box 'concave' 50 29 55 57 2 18
Add-Box 'concave' 69 29 74 57 2 18
Add-Box 'concave' 50 29 74 34 2 18
Add-Box 'concave' 79 29 84 57 2 15
Add-Box 'concave' 79 29 98 34 2 15

# Radius probes for curved-detail classification.
Add-Cylinder 'curves' 106 34 7 2 20 64
Add-Cylinder 'curves' 91 48 3 2 16 48
Add-Cylinder 'curves' 104 50 1.5 2 12 32

# Connected letter-like outline: a selected detail must keep the whole stroke on one tool.
Add-Box 'letter' 50 63 52 76 2 12
Add-Box 'letter' 50 74 64 76 2 12
Add-Box 'letter' 50 68.5 61 70.5 2 12
Add-Box 'letter' 50 63 64 65 2 12

# Thin features around the intended nozzle capabilities.
$thinWidths = @(0.20, 0.40, 0.60, 0.80, 1.20)
for ($i = 0; $i -lt $thinWidths.Count; ++$i) {
    $x = 70.0 + 7.0 * $i
    Add-Box 'thin' $x 63 ($x + $thinWidths[$i]) 76 2 (8 + 2 * $i)
}

# Staggered roof levels exercise top-surface and layer-parity transitions.
Add-Box 'steps' 7 45 42 52 2 6
Add-Box 'steps' 7 52 35 59 2 10
Add-Box 'steps' 7 59 28 66 2 14
Add-Box 'steps' 7 66 21 73 2 18

function Subtract-Vector([double[]] $a, [double[]] $b) {
    return [double[]]@(
        ($a[0] - $b[0]),
        ($a[1] - $b[1]),
        ($a[2] - $b[2])
    )
}

function Cross-Vector([double[]] $a, [double[]] $b) {
    return [double[]]@(
        ($a[1]*$b[2] - $a[2]*$b[1]),
        ($a[2]*$b[0] - $a[0]*$b[2]),
        ($a[0]*$b[1] - $a[1]*$b[0])
    )
}

function Dot-Vector([double[]] $a, [double[]] $b) {
    return $a[0]*$b[0] + $a[1]*$b[1] + $a[2]*$b[2]
}

function Normalize-Vector([double[]] $value) {
    $length = [Math]::Sqrt((Dot-Vector $value $value))
    if ($length -le 1e-12) { return [double[]]@(0, 0, 0) }
    return [double[]]@(
        ($value[0]/$length),
        ($value[1]/$length),
        ($value[2]/$length)
    )
}

function Get-TriangleNormal($triangle) {
    $a = $vertices[$triangle.A]
    $b = $vertices[$triangle.B]
    $c = $vertices[$triangle.C]
    return Normalize-Vector (Cross-Vector (Subtract-Vector $b $a) (Subtract-Vector $c $a))
}

function Format-Number([double] $value) {
    return $value.ToString('0.#####', $Invariant)
}

$stlDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($StlPath))
$svgDirectory = Split-Path -Parent ([IO.Path]::GetFullPath($PreviewSvgPath))
New-Item -ItemType Directory -Force -Path $stlDirectory, $svgDirectory | Out-Null

$stl = [Collections.Generic.List[string]]::new()
$stl.Add('solid small_nozzle_geometry_probe')
foreach ($triangle in $triangles) {
    $normal = Get-TriangleNormal $triangle
    $stl.Add("  facet normal $(Format-Number $normal[0]) $(Format-Number $normal[1]) $(Format-Number $normal[2])")
    $stl.Add('    outer loop')
    foreach ($index in @($triangle.A, $triangle.B, $triangle.C)) {
        $vertex = $vertices[$index]
        $stl.Add("      vertex $(Format-Number $vertex[0]) $(Format-Number $vertex[1]) $(Format-Number $vertex[2])")
    }
    $stl.Add('    endloop')
    $stl.Add('  endfacet')
}
$stl.Add('endsolid small_nozzle_geometry_probe')
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($StlPath), $stl, [Text.UTF8Encoding]::new($false))

$groupColors = @{
    base='#d9dde2'; straight='#158f91'; angles='#e68132'; concave='#3978b8'
    curves='#4b9b57'; letter='#d25770'; thin='#9a5bb5'; steps='#66717d'
}
$views = @(
    [pscustomobject]@{ Name='Front'; Camera=[double[]]@(0,-1,0); Up=[double[]]@(0,0,1); X=20; Y=70 },
    [pscustomobject]@{ Name='Right'; Camera=[double[]]@(1,0,0); Up=[double[]]@(0,0,1); X=810; Y=70 },
    [pscustomobject]@{ Name='Top'; Camera=[double[]]@(0,0,1); Up=[double[]]@(0,1,0); X=20; Y=620 },
    [pscustomobject]@{ Name='Isometric'; Camera=[double[]]@(1,-1,0.82); Up=[double[]]@(0,0,1); X=810; Y=620 }
)

$svg = [Collections.Generic.List[string]]::new()
$svg.Add('<svg xmlns="http://www.w3.org/2000/svg" width="1580" height="1160" viewBox="0 0 1580 1160">')
$svg.Add('<rect width="1580" height="1160" fill="#f4f6f8"/>')
$svg.Add('<style>text{font-family:Segoe UI,Arial,sans-serif;fill:#20262d}.title{font-size:26px;font-weight:700}.view{font-size:20px;font-weight:650}.meta{font-size:14px;fill:#5f6974}</style>')
$svg.Add('<text class="title" x="28" y="38">Small-nozzle geometry verification probe</text>')
$svg.Add('<text class="meta" x="620" y="37">120 x 80 x 20 mm | deterministic STL | 30 / 60 / 90 degree features</text>')

foreach ($view in $views) {
    $panelWidth = 750.0
    $panelHeight = 500.0
    $camera = Normalize-Vector $view.Camera
    $right = Normalize-Vector (Cross-Vector $view.Up $camera)
    $screenUp = Normalize-Vector (Cross-Vector $camera $right)
    $center = [double[]]@(60, 40, 10)
    $projectedVertices = @()
    foreach ($vertex in $vertices) {
        $relative = Subtract-Vector $vertex $center
        $projectedVertices += ,[double[]]@(
            (Dot-Vector $relative $right),
            (Dot-Vector $relative $screenUp),
            (Dot-Vector $relative $camera)
        )
    }
    $minU = ($projectedVertices | ForEach-Object { $_[0] } | Measure-Object -Minimum).Minimum
    $maxU = ($projectedVertices | ForEach-Object { $_[0] } | Measure-Object -Maximum).Maximum
    $minV = ($projectedVertices | ForEach-Object { $_[1] } | Measure-Object -Minimum).Minimum
    $maxV = ($projectedVertices | ForEach-Object { $_[1] } | Measure-Object -Maximum).Maximum
    $scale = [Math]::Min(($panelWidth - 70) / [Math]::Max(1, $maxU-$minU), ($panelHeight - 80) / [Math]::Max(1, $maxV-$minV))
    $offsetX = $view.X + ($panelWidth - ($maxU-$minU)*$scale) / 2 - $minU*$scale
    $offsetY = $view.Y + 38 + ($panelHeight - 62 + ($maxV-$minV)*$scale) / 2 + $minV*$scale

    $svg.Add("<rect x='$($view.X)' y='$($view.Y)' width='$panelWidth' height='$panelHeight' rx='6' fill='#ffffff' stroke='#cdd3da'/>")
    $svg.Add("<text class='view' x='$($view.X + 18)' y='$($view.Y + 30)'>$($view.Name)</text>")

    $renderTriangles = foreach ($triangle in $triangles) {
        $pa = $projectedVertices[$triangle.A]
        $pb = $projectedVertices[$triangle.B]
        $pc = $projectedVertices[$triangle.C]
        [pscustomobject]@{
            Triangle=$triangle
            Depth=($pa[2]+$pb[2]+$pc[2])/3.0
            Points=@($pa,$pb,$pc)
        }
    }
    foreach ($item in ($renderTriangles | Sort-Object Depth)) {
        $normal = Get-TriangleNormal $item.Triangle
        $light = [Math]::Max(0.35, [Math]::Min(1.0, 0.42 + 0.58 * [Math]::Abs((Dot-Vector $normal $camera))))
        $baseColor = $groupColors[$item.Triangle.Group]
        $points = @($item.Points | ForEach-Object {
            "$(Format-Number ($offsetX + $_[0]*$scale)),$(Format-Number ($offsetY - $_[1]*$scale))"
        }) -join ' '
        $svg.Add("<polygon points='$points' fill='$baseColor' fill-opacity='$(Format-Number $light)' stroke='#ffffff' stroke-opacity='0.55' stroke-width='0.45'/>")
    }
}

$legend = @(
    @('straight','wall-count zone'), @('angles','30 / 60 / 90 degrees'),
    @('concave','negative corners'), @('curves','radius probes'),
    @('letter','connected outline'), @('thin','0.2-1.2 mm ribs'), @('steps','layer transitions')
)
$legendX = 24
foreach ($entry in $legend) {
    $color = $groupColors[$entry[0]]
    $svg.Add("<rect x='$legendX' y='1130' width='14' height='14' rx='2' fill='$color'/>")
    $svg.Add("<text class='meta' x='$($legendX + 20)' y='1142'>$($entry[1])</text>")
    $legendX += 205
}
$svg.Add('</svg>')
[IO.File]::WriteAllLines([IO.Path]::GetFullPath($PreviewSvgPath), $svg, [Text.UTF8Encoding]::new($false))

Write-Output "STL=$([IO.Path]::GetFullPath($StlPath))"
Write-Output "SVG=$([IO.Path]::GetFullPath($PreviewSvgPath))"
Write-Output "VERTICES=$($vertices.Count) TRIANGLES=$($triangles.Count)"
