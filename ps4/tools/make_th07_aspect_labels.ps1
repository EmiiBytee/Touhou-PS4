# PS4 port: replace PCB's Window / Fullscreen choices with 16:9 / 4:3.
# Usage: make_th07_aspect_labels.ps1 <extracted title01.png> <overlay root>
param([string]$SourcePng, [string]$OverlayRoot)

Add-Type -AssemblyName System.Drawing
$outDir = Join-Path $OverlayRoot 'data/title'
New-Item -ItemType Directory -Force $outDir | Out-Null

$source = [System.Drawing.Bitmap]::FromFile($SourcePng)
$bitmap = New-Object System.Drawing.Bitmap $source.Width, $source.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
$graphics = [System.Drawing.Graphics]::FromImage($bitmap)
$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
$source.Dispose()

$graphics.SmoothingMode = 'AntiAlias'
$graphics.CompositingMode = 'SourceCopy'
$labels = @(
    @{ Text = '16:9'; Rect = New-Object System.Drawing.Rectangle 224, 288, 112, 32 },
    @{ Text = '4:3';  Rect = New-Object System.Drawing.Rectangle 336, 288, 144, 32 },
    @{ Text = '16:9'; Rect = New-Object System.Drawing.Rectangle 224, 352, 112, 32 },
    @{ Text = '4:3';  Rect = New-Object System.Drawing.Rectangle 336, 352, 144, 32 }
)
foreach ($label in $labels) {
    $graphics.FillRectangle([System.Drawing.Brushes]::Transparent, $label.Rect)
}
$graphics.CompositingMode = 'SourceOver'

$family = $null
foreach ($name in 'Arial Rounded MT Bold', 'Arial Black', 'Arial') {
    try {
        $candidate = New-Object System.Drawing.FontFamily $name
        if ($candidate.Name -eq $name) {
            $family = $candidate
            break
        }
    } catch {}
}
if (-not $family) {
    $family = [System.Drawing.FontFamily]::GenericSansSerif
}

foreach ($label in $labels) {
    $rect = $label.Rect
    $path = New-Object System.Drawing.Drawing2D.GraphicsPath
    $format = New-Object System.Drawing.StringFormat
    $format.Alignment = 'Center'
    $format.LineAlignment = 'Center'
    $path.AddString($label.Text, $family, [int][System.Drawing.FontStyle]::Bold, 24,
        (New-Object System.Drawing.RectangleF $rect.X, $rect.Y, $rect.Width, $rect.Height), $format)
    $outline = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 24, 24, 24)), 3
    $outline.LineJoin = 'Round'
    $graphics.DrawPath($outline, $path)
    $graphics.FillPath([System.Drawing.Brushes]::White, $path)
    $outline.Dispose()
    $path.Dispose()
    $format.Dispose()
}

$graphics.Dispose()
$target = Join-Path $outDir 'title01.png'
$bitmap.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
$bitmap.Dispose()
Write-Output $target
