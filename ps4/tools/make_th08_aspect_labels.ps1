# PS4 port: replace IN's Window / FullScreen choices with 16:9 / 4:3.
# Usage: make_th08_aspect_labels.ps1 <extracted title01.png> <overlay root>
#
# The labels are composed from the sheet's own glyphs rather than drawn with a system font:
# the digits come from the 0-9 row in the same style (white for the selected row, grey for the
# unselected one), and the colon is two copies of the full stop in "S.E.Vol". That keeps the
# game's rounded font, outline and glow in both states.
#
# The menu shows sprite 40 + cfg.windowed, and the PS4 renderer treats windowed as 16:9, so
# "Window" (sprites 73/74) becomes 16:9 and "FullScreen" (75/76) becomes 4:3.
#
# Get title01.png from the game's own data: extract title01.anm from th08.dat (thtk:
# `thdat -x 8 th08.dat title01.anm`, or the Linux reference build with TH08_DUMP_DIR set) and
# pass it through ps4/tools/th08_anm_dump.py.
param([string]$SourcePng, [string]$OverlayRoot)

Add-Type -AssemblyName System.Drawing
$ErrorActionPreference = 'Stop'

$outDir = Join-Path $OverlayRoot 'data/title'
New-Item -ItemType Directory -Force $outDir | Out-Null

$argb = [System.Drawing.Imaging.PixelFormat]::Format32bppArgb
$source = [System.Drawing.Bitmap]::FromFile($SourcePng)
$sheet = New-Object System.Drawing.Bitmap $source.Width, $source.Height, $argb
$graphics = [System.Drawing.Graphics]::FromImage($sheet)
$graphics.CompositingMode = 'SourceCopy'
$graphics.DrawImage($source, 0, 0, $source.Width, $source.Height)
$source.Dispose()

# Each style's rows, and the alpha that separates a glyph's outline and core ("ink") from its
# glow. The unselected style is drawn translucent: its ink peaks at alpha 136, its glow at 102.
$styles = @(
    @{ DigitRow = 256; DotRow = 128; LabelRow = 288; InkAlpha = 200 },  # selected: white
    @{ DigitRow = 320; DotRow = 192; LabelRow = 352; InkAlpha = 120 }   # unselected: grey
)
$labels = @(
    @{ Text = '16:9'; X = 224; Width = 112 },  # was "Window"
    @{ Text = '4:3';  X = 336; Width = 144 }   # was "FullScreen"
)
$gap = 2         # between the ink of neighbouring glyphs, as in the game's own words
$colonRise = 9   # the upper dot sits this far above the lower one

function Get-Crop([int]$x, [int]$y, [int]$w, [int]$h) {
    return $sheet.Clone((New-Object System.Drawing.Rectangle $x, $y, $w, $h), $argb)
}

function Get-InkBounds($bitmap, [int]$threshold) {
    $minX = $bitmap.Width; $maxX = -1
    for ($y = 0; $y -lt $bitmap.Height; $y++) {
        for ($x = 0; $x -lt $bitmap.Width; $x++) {
            if ($bitmap.GetPixel($x, $y).A -ge $threshold) {
                if ($x -lt $minX) { $minX = $x }
                if ($x -gt $maxX) { $maxX = $x }
            }
        }
    }
    if ($maxX -lt 0) { throw 'glyph without ink' }
    return @{ MinX = $minX; Width = $maxX - $minX + 1 }
}

# A copy of the glyph without its glow, to draw over every glow in the label.
function Get-InkOnly($bitmap, [int]$threshold) {
    $ink = $bitmap.Clone((New-Object System.Drawing.Rectangle 0, 0, $bitmap.Width, $bitmap.Height), $argb)
    for ($y = 0; $y -lt $ink.Height; $y++) {
        for ($x = 0; $x -lt $ink.Width; $x++) {
            if ($ink.GetPixel($x, $y).A -lt $threshold) { $ink.SetPixel($x, $y, [System.Drawing.Color]::Transparent) }
        }
    }
    return $ink
}

# The full stop after the S of "S.E.Vol" (sprite at x=288): the white core on the baseline, in
# columns 24-29 and rows 21-26. Found in the white row; the grey row has the same layout.
$dotSpriteX = 288
$sumX = 0; $sumY = 0; $count = 0
for ($y = 21; $y -le 26; $y++) {
    for ($x = 24; $x -le 29; $x++) {
        $p = $sheet.GetPixel($dotSpriteX + $x, 128 + $y)
        if ($p.A -ge 200 -and ($p.R + $p.G + $p.B) -gt 600) { $sumX += $x; $sumY += $y; $count++ }
    }
}
if ($count -eq 0) { throw 'no full stop found in S.E.Vol' }
$dotCenterX = $sumX / $count
$dotCenterY = $sumY / $count

# Core and outline only: nothing outside a small radius, so neither neighbouring letter comes
# along. It is drawn in the ink pass, over the digits' glow, like the stops in "S.E.Vol".
function Get-Dot([int]$spriteY) {
    $size = 9
    $radius = 3.4
    $dot = New-Object System.Drawing.Bitmap $size, $size, $argb
    for ($y = 0; $y -lt $size; $y++) {
        for ($x = 0; $x -lt $size; $x++) {
            $sx = [int][Math]::Round($dotCenterX - ($size - 1) / 2 + $x)
            $sy = [int][Math]::Round($dotCenterY - ($size - 1) / 2 + $y)
            $dx = $sx - $dotCenterX; $dy = $sy - $dotCenterY
            if ([Math]::Sqrt($dx * $dx + $dy * $dy) -le $radius) {
                $dot.SetPixel($x, $y, $sheet.GetPixel($dotSpriteX + $sx, $spriteY + $sy))
            } else {
                $dot.SetPixel($x, $y, [System.Drawing.Color]::Transparent)
            }
        }
    }
    return $dot
}

foreach ($style in $styles) {
    $dot = Get-Dot $style.DotRow
    foreach ($label in $labels) {
        $rect = New-Object System.Drawing.Rectangle $label.X, $style.LabelRow, $label.Width, 32

        # Glyphs and the width of their ink, to centre the label in its sprite. The white row's
        # ink decides the spacing for both styles, so the two states line up exactly.
        $pieces = @()
        foreach ($ch in $label.Text.ToCharArray()) {
            if ($ch -eq ':') {
                $pieces += @{ Colon = $true; Width = 5 }
            } else {
                $digit = [int]::Parse([string]$ch)
                $cell = Get-Crop ($digit * 32) $style.DigitRow 32 32
                $white = Get-Crop ($digit * 32) 256 32 32
                $bounds = Get-InkBounds $white 200
                $white.Dispose()
                $pieces += @{ Colon = $false; Cell = $cell; Ink = (Get-InkOnly $cell $style.InkAlpha);
                              MinX = $bounds.MinX; Width = $bounds.Width }
            }
        }
        $total = ($pieces | ForEach-Object { $_.Width } | Measure-Object -Sum).Sum + $gap * ($pieces.Count - 1)
        $start = $label.X + [int][Math]::Floor(($label.Width - $total) / 2)

        $graphics.CompositingMode = 'SourceCopy'
        $graphics.FillRectangle([System.Drawing.Brushes]::Transparent, $rect)
        $graphics.CompositingMode = 'SourceOver'
        $graphics.SetClip($rect)
        # Glow first, then ink over all of it: the game's text has one glow layer beneath the
        # letters, so a glyph's glow must never cover its neighbour's outline.
        foreach ($pass in 'glow', 'ink') {
            $cursor = $start
            foreach ($piece in $pieces) {
                if ($piece.Colon) {
                    if ($pass -eq 'ink') {
                        $x = $cursor + [int][Math]::Floor(($piece.Width - $dot.Width) / 2)
                        $lowerY = $style.LabelRow + [int][Math]::Round($dotCenterY) - 4
                        $graphics.DrawImage($dot, $x, $lowerY, $dot.Width, $dot.Height)
                        $graphics.DrawImage($dot, $x, $lowerY - $colonRise, $dot.Width, $dot.Height)
                    }
                } else {
                    $image = if ($pass -eq 'glow') { $piece.Cell } else { $piece.Ink }
                    $graphics.DrawImage($image, $cursor - $piece.MinX, $style.LabelRow, 32, 32)
                }
                $cursor += $piece.Width + $gap
            }
        }
        $graphics.ResetClip()
        foreach ($piece in $pieces) { if (-not $piece.Colon) { $piece.Cell.Dispose(); $piece.Ink.Dispose() } }
    }
    $dot.Dispose()
}

$graphics.Dispose()
$target = Join-Path $outDir 'title01.png'
$sheet.Save($target, [System.Drawing.Imaging.ImageFormat]::Png)
$sheet.Dispose()
Write-Output $target
