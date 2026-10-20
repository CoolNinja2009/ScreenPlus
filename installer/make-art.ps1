# Regenerates the installer artwork from resources\app.ico:
#   wizard.bmp  164x314  welcome / finish page banner
#   header.bmp  150x57   inner page header (right-aligned)
# Usage: powershell -ExecutionPolicy Bypass -File installer\make-art.ps1
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
# app.ico stores PNG frames, which System.Drawing.Icon cannot decode: load the largest one directly.
$ico = [IO.File]::ReadAllBytes((Join-Path $here '..\resources\app.ico'))
$icon = $null; $best = 0
for ($i = 0; $i -lt [BitConverter]::ToUInt16($ico, 4); $i++) {
    $e = 6 + 16 * $i
    $w = if ($ico[$e] -eq 0) { 256 } else { [int]$ico[$e] }
    $size = [BitConverter]::ToInt32($ico, $e + 8); $off = [BitConverter]::ToInt32($ico, $e + 12)
    if ($w -gt $best -and $ico[$off] -eq 0x89) {
        $best = $w
        $icon = [System.Drawing.Image]::FromStream((New-Object IO.MemoryStream (, [byte[]]$ico[$off..($off + $size - 1)])))
    }
}
if (-not $icon) { throw 'app.ico has no PNG frame' }

function New-Canvas([int]$w, [int]$h) {
    $b = New-Object System.Drawing.Bitmap $w, $h, ([System.Drawing.Imaging.PixelFormat]::Format24bppRgb)
    $g = [System.Drawing.Graphics]::FromImage($b)
    $g.SmoothingMode = 'AntiAlias'; $g.InterpolationMode = 'HighQualityBicubic'; $g.PixelOffsetMode = 'HighQuality'
    return $b, $g
}
function RoundRect([float]$x, [float]$y, [float]$w, [float]$h, [float]$r) {
    $p = New-Object System.Drawing.Drawing2D.GraphicsPath
    $d = 2 * $r
    $p.AddArc($x, $y, $d, $d, 180, 90); $p.AddArc($x + $w - $d, $y, $d, $d, 270, 90)
    $p.AddArc($x + $w - $d, $y + $h - $d, $d, $d, 0, 90); $p.AddArc($x, $y + $h - $d, $d, $d, 90, 90)
    $p.CloseFigure(); return $p
}
function White([int]$a) { New-Object System.Drawing.SolidBrush ([System.Drawing.Color]::FromArgb($a, 255, 255, 255)) }

# ---- wizard.bmp: Screen+ blue with stacked translucent screens (cf. the VLC cone)
$bmp, $g = New-Canvas 164 314
$all = New-Object System.Drawing.Rectangle 0, 0, 164, 314
$g.FillRectangle((New-Object System.Drawing.Drawing2D.LinearGradientBrush $all,
    ([System.Drawing.Color]::FromArgb(47, 149, 255)), ([System.Drawing.Color]::FromArgb(10, 75, 196)), 90.0), $all)
$g.FillPath((White 26), (RoundRect -64 118 220 150 22))
$g.FillPath((White 38), (RoundRect -40 160 180 122 18))
$g.FillPath((White 52), (RoundRect -18 198 140 92 14))
$g.FillRectangle((White 52), 40, 290, 22, 12)
$g.FillPath((White 52), (RoundRect 18 300 66 10 4))
$g.FillPath((White 255), (RoundRect 98 24 46 46 9))
$g.DrawImage($icon, (New-Object System.Drawing.Rectangle 102, 28, 38, 38))
$bmp.Save((Join-Path $here 'wizard.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()

# ---- header.bmp: white strip with the app icon on the right
$bmp, $g = New-Canvas 150 57
$g.Clear([System.Drawing.Color]::White)
$g.DrawImage($icon, (New-Object System.Drawing.Rectangle 104, 8, 40, 40))
$bmp.Save((Join-Path $here 'header.bmp'), [System.Drawing.Imaging.ImageFormat]::Bmp)
$g.Dispose(); $bmp.Dispose()
Write-Host "Wrote wizard.bmp and header.bmp to $here"
