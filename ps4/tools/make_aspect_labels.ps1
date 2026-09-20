# PS4 port: the "Mode: Full Screen / Window" option means "4:3 / 16:9" on PS4.
# Redraws those two labels in the (translated) title04.png / title04s.png textures.
# Usage: make_aspect_labels.ps1 <dir with title04.png + title04s.png> <out dir>
param([string]$SourceDir, [string]$OutDir)

Add-Type -AssemblyName System.Drawing
New-Item -ItemType Directory -Force $OutDir | Out-Null

# Sprite rects from title04.anm: 13 = "Full Screen", 14 = "Window" (x=256 wraps to 0).
$labels = @(
    @{ Text = '4:3';  Rect = New-Object System.Drawing.Rectangle 80, 160, 176, 32 },
    @{ Text = '16:9'; Rect = New-Object System.Drawing.Rectangle 0, 192, 112, 32 }
)

function Get-FontFamily {
    foreach ($name in 'Arial Rounded MT Bold', 'Arial Black', 'Arial') {
        $f = New-Object System.Drawing.FontFamily $name -ErrorAction SilentlyContinue
        if ($f -and $f.Name -eq $name) { return $f }
    }
    return [System.Drawing.FontFamily]::GenericSansSerif
}

function Update-Texture([string]$name, [bool]$highlighted) {
    $src = Join-Path $SourceDir $name
    $orig = [System.Drawing.Bitmap]::FromFile($src)
    $bmp = New-Object System.Drawing.Bitmap $orig.Width, $orig.Height, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.DrawImage($orig, 0, 0, $orig.Width, $orig.Height)
    $orig.Dispose()

    $g.SmoothingMode = 'AntiAlias'
    $g.CompositingMode = 'SourceCopy'
    foreach ($l in $labels) { $g.FillRectangle([System.Drawing.Brushes]::Transparent, $l.Rect) }
    $g.CompositingMode = 'SourceOver'

    $family = Get-FontFamily
    foreach ($l in $labels) {
        $r = $l.Rect
        $path = New-Object System.Drawing.Drawing2D.GraphicsPath
        $fmt = New-Object System.Drawing.StringFormat
        $fmt.LineAlignment = 'Center'
        $path.AddString($l.Text, $family, [int][System.Drawing.FontStyle]::Bold, 24,
            (New-Object System.Drawing.RectangleF ($r.X + 4), $r.Y, ($r.Width - 4), $r.Height), $fmt)
        $outline = New-Object System.Drawing.Pen ([System.Drawing.Color]::FromArgb(255, 20, 20, 20)), 4
        $outline.LineJoin = 'Round'
        $g.DrawPath($outline, $path)
        if ($highlighted) {
            $fill = New-Object System.Drawing.Drawing2D.LinearGradientBrush $r, ([System.Drawing.Color]::FromArgb(255, 255, 250, 150)), ([System.Drawing.Color]::FromArgb(255, 240, 150, 20)), 90
        } else {
            $fill = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
        }
        $g.FillPath($fill, $path)
    }
    $g.Dispose()
    $bmp.Save((Join-Path $OutDir $name), [System.Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}

Update-Texture 'title04.png' $false
Update-Texture 'title04s.png' $true
Write-Output "aspect labels written to $OutDir"
