param(
    [string]$OutputPath = "",
    [double]$Diameter = 50.0,
    [double]$BottomZ = 10.0,
    [double]$CenterX = 135.5,
    [double]$CenterY = 136.0,
    [int]$LongitudeSegments = 96,
    [int]$LatitudeSegments = 48
)

$ErrorActionPreference = "Stop"
Set-StrictMode -Version Latest

if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $PSScriptRoot "..\build\verification\sphere-50mm-z10\Sphere_50mm_Z10.3mf"
}

$OutputPath = [System.IO.Path]::GetFullPath($OutputPath)
$outputDirectory = [System.IO.Path]::GetDirectoryName($OutputPath)
[System.IO.Directory]::CreateDirectory($outputDirectory) | Out-Null

if ($Diameter -le 0.0) {
    throw "Diameter must be greater than zero."
}
if ($LongitudeSegments -lt 8 -or $LatitudeSegments -lt 4) {
    throw "Sphere segment counts are too small."
}

$radius = $Diameter / 2.0
$centerZ = $BottomZ + $radius
$vertices = [System.Collections.Generic.List[object]]::new()
$triangles = [System.Collections.Generic.List[int[]]]::new()

function Add-Vertex {
    param([double]$X, [double]$Y, [double]$Z)
    $script:vertices.Add([pscustomobject]@{ X = $X; Y = $Y; Z = $Z })
    return $script:vertices.Count - 1
}

function Add-OutwardTriangle {
    param([int]$A, [int]$B, [int]$C)

    $va = $script:vertices[$A]
    $vb = $script:vertices[$B]
    $vc = $script:vertices[$C]

    $abx = $vb.X - $va.X
    $aby = $vb.Y - $va.Y
    $abz = $vb.Z - $va.Z
    $acx = $vc.X - $va.X
    $acy = $vc.Y - $va.Y
    $acz = $vc.Z - $va.Z

    $nx = ($aby * $acz) - ($abz * $acy)
    $ny = ($abz * $acx) - ($abx * $acz)
    $nz = ($abx * $acy) - ($aby * $acx)
    $cx = ($va.X + $vb.X + $vc.X) / 3.0
    $cy = ($va.Y + $vb.Y + $vc.Y) / 3.0
    $cz = ($va.Z + $vb.Z + $vc.Z) / 3.0

    if ((($nx * $cx) + ($ny * $cy) + ($nz * $cz)) -lt 0.0) {
        $temporary = $B
        $B = $C
        $C = $temporary
    }

    $script:triangles.Add([int[]]@($A, $B, $C))
}

$topIndex = Add-Vertex -X 0.0 -Y 0.0 -Z $radius

for ($latitude = 1; $latitude -lt $LatitudeSegments; $latitude++) {
    $phi = [Math]::PI * $latitude / $LatitudeSegments
    $ringRadius = $radius * [Math]::Sin($phi)
    $z = $radius * [Math]::Cos($phi)

    for ($longitude = 0; $longitude -lt $LongitudeSegments; $longitude++) {
        $theta = 2.0 * [Math]::PI * $longitude / $LongitudeSegments
        [void](Add-Vertex `
            -X ($ringRadius * [Math]::Cos($theta)) `
            -Y ($ringRadius * [Math]::Sin($theta)) `
            -Z $z)
    }
}

$bottomIndex = Add-Vertex -X 0.0 -Y 0.0 -Z (-$radius)

for ($longitude = 0; $longitude -lt $LongitudeSegments; $longitude++) {
    $next = ($longitude + 1) % $LongitudeSegments
    Add-OutwardTriangle -A $topIndex -B (1 + $longitude) -C (1 + $next)
}

for ($latitude = 0; $latitude -lt ($LatitudeSegments - 2); $latitude++) {
    $upperStart = 1 + ($latitude * $LongitudeSegments)
    $lowerStart = $upperStart + $LongitudeSegments

    for ($longitude = 0; $longitude -lt $LongitudeSegments; $longitude++) {
        $next = ($longitude + 1) % $LongitudeSegments
        $upper = $upperStart + $longitude
        $upperNext = $upperStart + $next
        $lower = $lowerStart + $longitude
        $lowerNext = $lowerStart + $next
        Add-OutwardTriangle -A $upper -B $lower -C $upperNext
        Add-OutwardTriangle -A $upperNext -B $lower -C $lowerNext
    }
}

$lastRingStart = 1 + (($LatitudeSegments - 2) * $LongitudeSegments)
for ($longitude = 0; $longitude -lt $LongitudeSegments; $longitude++) {
    $next = ($longitude + 1) % $LongitudeSegments
    Add-OutwardTriangle `
        -A ($lastRingStart + $longitude) `
        -B $bottomIndex `
        -C ($lastRingStart + $next)
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

if ([System.IO.File]::Exists($OutputPath)) {
    [System.IO.File]::Delete($OutputPath)
}

$invariant = [System.Globalization.CultureInfo]::InvariantCulture
$utf8WithoutBom = [System.Text.UTF8Encoding]::new($false)
$xmlSettings = [System.Xml.XmlWriterSettings]::new()
$xmlSettings.Encoding = $utf8WithoutBom
$xmlSettings.Indent = $true
$xmlSettings.CloseOutput = $false

$fileStream = [System.IO.File]::Open(
    $OutputPath,
    [System.IO.FileMode]::CreateNew,
    [System.IO.FileAccess]::ReadWrite,
    [System.IO.FileShare]::None
)

try {
    $archive = [System.IO.Compression.ZipArchive]::new(
        $fileStream,
        [System.IO.Compression.ZipArchiveMode]::Create,
        $true
    )

    try {
        $contentTypesEntry = $archive.CreateEntry("[Content_Types].xml")
        $stream = $contentTypesEntry.Open()
        try {
            $writer = [System.Xml.XmlWriter]::Create($stream, $xmlSettings)
            try {
                $writer.WriteStartDocument()
                $writer.WriteStartElement("Types", "http://schemas.openxmlformats.org/package/2006/content-types")
                $writer.WriteStartElement("Default")
                $writer.WriteAttributeString("Extension", "rels")
                $writer.WriteAttributeString("ContentType", "application/vnd.openxmlformats-package.relationships+xml")
                $writer.WriteEndElement()
                $writer.WriteStartElement("Default")
                $writer.WriteAttributeString("Extension", "model")
                $writer.WriteAttributeString("ContentType", "application/vnd.ms-package.3dmanufacturing-3dmodel+xml")
                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndDocument()
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $stream.Dispose()
        }

        $relationshipsEntry = $archive.CreateEntry("_rels/.rels")
        $stream = $relationshipsEntry.Open()
        try {
            $writer = [System.Xml.XmlWriter]::Create($stream, $xmlSettings)
            try {
                $writer.WriteStartDocument()
                $writer.WriteStartElement("Relationships", "http://schemas.openxmlformats.org/package/2006/relationships")
                $writer.WriteStartElement("Relationship")
                $writer.WriteAttributeString("Target", "/3D/3dmodel.model")
                $writer.WriteAttributeString("Id", "rel0")
                $writer.WriteAttributeString("Type", "http://schemas.microsoft.com/3dmanufacturing/2013/01/3dmodel")
                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndDocument()
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $stream.Dispose()
        }

        $modelEntry = $archive.CreateEntry("3D/3dmodel.model")
        $stream = $modelEntry.Open()
        try {
            $writer = [System.Xml.XmlWriter]::Create($stream, $xmlSettings)
            try {
                $coreNamespace = "http://schemas.microsoft.com/3dmanufacturing/core/2015/02"
                $writer.WriteStartDocument()
                $writer.WriteStartElement("model", $coreNamespace)
                $writer.WriteAttributeString("unit", "millimeter")
                $writer.WriteAttributeString("xml", "lang", "http://www.w3.org/XML/1998/namespace", "en-US")

                $writer.WriteStartElement("metadata", $coreNamespace)
                $writer.WriteAttributeString("name", "Title")
                $writer.WriteString("50 mm Sphere - 10 mm Above Bed")
                $writer.WriteEndElement()

                $writer.WriteStartElement("resources", $coreNamespace)
                $writer.WriteStartElement("object", $coreNamespace)
                $writer.WriteAttributeString("id", "1")
                $writer.WriteAttributeString("type", "model")
                $writer.WriteAttributeString("name", "Sphere 50mm Z+10mm")
                $writer.WriteStartElement("mesh", $coreNamespace)
                $writer.WriteStartElement("vertices", $coreNamespace)

                foreach ($vertex in $vertices) {
                    $writer.WriteStartElement("vertex", $coreNamespace)
                    $writer.WriteAttributeString("x", $vertex.X.ToString("R", $invariant))
                    $writer.WriteAttributeString("y", $vertex.Y.ToString("R", $invariant))
                    $writer.WriteAttributeString("z", $vertex.Z.ToString("R", $invariant))
                    $writer.WriteEndElement()
                }

                $writer.WriteEndElement()
                $writer.WriteStartElement("triangles", $coreNamespace)

                foreach ($triangle in $triangles) {
                    $writer.WriteStartElement("triangle", $coreNamespace)
                    $writer.WriteAttributeString("v1", $triangle[0].ToString($invariant))
                    $writer.WriteAttributeString("v2", $triangle[1].ToString($invariant))
                    $writer.WriteAttributeString("v3", $triangle[2].ToString($invariant))
                    $writer.WriteEndElement()
                }

                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndElement()

                $writer.WriteStartElement("build", $coreNamespace)
                $writer.WriteStartElement("item", $coreNamespace)
                $writer.WriteAttributeString("objectid", "1")
                $writer.WriteAttributeString("printable", "1")
                $writer.WriteAttributeString("auto_drop", "0")
                $translation = "1 0 0 0 1 0 0 0 1 {0} {1} {2}" -f `
                    $CenterX.ToString("R", $invariant), `
                    $CenterY.ToString("R", $invariant), `
                    $centerZ.ToString("R", $invariant)
                $writer.WriteAttributeString("transform", $translation)
                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndElement()
                $writer.WriteEndDocument()
            }
            finally {
                $writer.Dispose()
            }
        }
        finally {
            $stream.Dispose()
        }
    }
    finally {
        $archive.Dispose()
    }
}
finally {
    $fileStream.Dispose()
}

[pscustomobject]@{
    OutputPath = $OutputPath
    Vertices = $vertices.Count
    Triangles = $triangles.Count
    Bounds = "X={0}..{1}, Y={2}..{3}, Z={4}..{5}" -f `
        ($CenterX - $radius), ($CenterX + $radius), `
        ($CenterY - $radius), ($CenterY + $radius), `
        $BottomZ, ($BottomZ + $Diameter)
} | Format-List
