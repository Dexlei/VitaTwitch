# Generates the LiveArea images (8-bit indexed PNG, as the Vita requires).
param([string]$Out = (Join-Path $PSScriptRoot "..\sce_sys"))
Add-Type -AssemblyName System.Drawing

function New-Asset([int]$w, [int]$h, [string]$path, [float]$fontSize, [string]$text) {
    $bmp = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format32bppArgb)
    $g = [System.Drawing.Graphics]::FromImage($bmp)
    $g.SmoothingMode = 'AntiAlias'
    $g.TextRenderingHint = 'AntiAliasGridFit'
    $g.Clear([System.Drawing.Color]::FromArgb(255, 100, 65, 165))
    $brush = New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::White)
    # simple "screen" glyph
    $pen = New-Object System.Drawing.Pen ([System.Drawing.Color]::White), ([Math]::Max(3, $h / 28))
    $rw = $w * 0.5; $rh = $h * 0.32
    $rx = ($w - $rw) / 2; $ry = $h * 0.14
    $g.DrawRectangle($pen, $rx, $ry, $rw, $rh)
    $g.FillRectangle($brush, $rx + $rw * 0.30, $ry + $rh * 0.30, $rw * 0.08, $rh * 0.38)
    $g.FillRectangle($brush, $rx + $rw * 0.58, $ry + $rh * 0.30, $rw * 0.08, $rh * 0.38)
    $font = New-Object System.Drawing.Font "Segoe UI", $fontSize, ([System.Drawing.FontStyle]::Bold), ([System.Drawing.GraphicsUnit]::Pixel)
    $sf = New-Object System.Drawing.StringFormat
    $sf.Alignment = 'Center'
    $g.DrawString($text, $font, $brush, (New-Object System.Drawing.RectangleF 0, ($h * 0.58), $w, ($h * 0.4)), $sf)
    $g.Dispose()
    $idx = $bmp.Clone((New-Object System.Drawing.Rectangle 0, 0, $w, $h), [System.Drawing.Imaging.PixelFormat]::Format8bppIndexed)
    New-Item -ItemType Directory -Force (Split-Path $path) | Out-Null
    $idx.Save($path, [System.Drawing.Imaging.ImageFormat]::Png)
    $idx.Dispose(); $bmp.Dispose()
}

New-Asset 128 128 (Join-Path $Out "icon0.png") 26 "Twitch"
New-Asset 840 500 (Join-Path $Out "livearea\contents\bg.png") 90 "VitaTwitch"
New-Asset 280 158 (Join-Path $Out "livearea\contents\startup.png") 34 "VitaTwitch"
