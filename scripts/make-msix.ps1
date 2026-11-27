# Packs a built tree (build.cmd) into the Microsoft Store package: <Out>\ScreenPlus-<ver>-x64.msix, unsigned (the
# Store signs what it publishes). The manifest is installer\msix\AppxManifest.xml.
#   scripts\make-msix.ps1 -Version 2.7.1 [-Build build] [-Out store] [-LayoutOnly]
# The identity is Partner Center's (Screenplus, Store ID 9MTSPM0ZNF19, Product identity page); MSSTORE_IDENTITY_NAME,
# MSSTORE_PUBLISHER and MSSTORE_PUBLISHER_NAME override it. -LayoutOnly leaves <Out>\layout for
# `Add-AppxPackage -Register <Out>\layout\AppxManifest.xml` (Developer Mode) without packing.
param(
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Build = 'build',
    [string]$Out = 'store',
    [switch]$LayoutOnly
)
$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

if ($Version -notmatch '^\d+\.\d+\.\d+$') { throw "The Store needs major.minor.patch, not '$Version'" }
$root = Split-Path $PSScriptRoot
$identity = if ($env:MSSTORE_IDENTITY_NAME) { $env:MSSTORE_IDENTITY_NAME } else { 'CoolNinja2009.Screenplus' }
$publisher = if ($env:MSSTORE_PUBLISHER) { $env:MSSTORE_PUBLISHER } else { 'CN=2334936A-0022-4B10-8809-143375E1C1DC' }
$publisherName = if ($env:MSSTORE_PUBLISHER_NAME) { $env:MSSTORE_PUBLISHER_NAME } else { 'CoolNinja2009' }

$kit = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin' -Directory |
    Where-Object { $_.Name -match '^10\.' -and (Test-Path (Join-Path $_.FullName 'x64\makeappx.exe')) } |
    Sort-Object { [version]$_.Name } | Select-Object -Last 1
if (-not $kit) { throw 'makeappx.exe not found: install the Windows SDK' }
$makeappx = Join-Path $kit.FullName 'x64\makeappx.exe'
$makepri = Join-Path $kit.FullName 'x64\makepri.exe'

# ---- layout: what the installer ships, minus the screensaver (the package folder is read-only)
$layout = Join-Path $Out 'layout'
if (Test-Path $layout) { Remove-Item $layout -Recurse -Force }
New-Item -ItemType Directory -Force $layout, "$layout\Assets", "$layout\samples" | Out-Null
Copy-Item "$Build\ScreenPlus.exe", "$Build\ScreenPlus.com", "$Build\*.dll" $layout
if (Test-Path "$Build\screenplus-bare-ui.exe") { Copy-Item "$Build\screenplus-bare-ui.exe" $layout }
Copy-Item "$Build\licenses" $layout -Recurse
# Every sample program the build made (one <Name>.spf each, unpacked into samples\<Name> by cmake/sample_apps.cmake):
# a new sample needs nothing here. The package is read-only and the Store signs it, so nothing is downloaded later.
$spfs = @(Get-ChildItem "$Build\*.spf")
if (-not $spfs) { throw "no sample programs (*.spf) in ${Build}: build with -DSCREENPLUS_BARE_UI=ON" }
foreach ($spf in $spfs) {
    $s = "$Build\samples\$($spf.BaseName)"
    if (-not (Test-Path "$s\LivelyInfo.json")) { throw "$s is missing or isn't a sample program (cmake/sample_apps.cmake)" }
    Copy-Item $s "$layout\samples" -Recurse
}

# ---- manifest
$xml = (Get-Content "$root\installer\msix\AppxManifest.xml" -Raw).
    Replace('@IDENTITY_NAME@', [Security.SecurityElement]::Escape($identity)).
    Replace('@PUBLISHER@', [Security.SecurityElement]::Escape($publisher)).
    Replace('@PUBLISHER_NAME@', [Security.SecurityElement]::Escape($publisherName)).
    Replace('@VERSION@', "$Version.0")   # the Store keeps the fourth number for itself
[IO.File]::WriteAllText((Join-Path (Resolve-Path $layout) 'AppxManifest.xml'), $xml, (New-Object Text.UTF8Encoding $false))

# ---- images from the .ico files, at every scale Windows asks for (resources.pri maps them)
$images = @{}
function Get-IcoImage([string]$Ico) {   # the largest frame; 256 px frames are PNG, which Icon.ToBitmap can't read
    if ($images[$Ico]) { return $images[$Ico] }
    $b = [IO.File]::ReadAllBytes((Join-Path $root "resources\$Ico"))
    $best = -1; $bestW = 0
    for ($i = 0; $i -lt [BitConverter]::ToUInt16($b, 4); $i++) {
        $w = $b[6 + 16 * $i]; if ($w -eq 0) { $w = 256 }
        if ($w -gt $bestW) { $bestW = $w; $best = $i }
    }
    $size = [BitConverter]::ToInt32($b, 6 + 16 * $best + 8)
    $off = [BitConverter]::ToInt32($b, 6 + 16 * $best + 12)
    if ($b[$off] -eq 0x89) {
        $img = [Drawing.Image]::FromStream([IO.MemoryStream]::new($b, $off, $size))
    } else {
        $img = (New-Object Drawing.Icon (Join-Path $root "resources\$Ico"), $bestW, $bestW).ToBitmap()
    }
    $images[$Ico] = $img
    return $img
}

function Save-Logo([string]$Ico, [string]$Name, [int]$W, [int]$H, [double]$Fill = 1.0) {
    $src = Get-IcoImage $Ico
    $bmp = New-Object Drawing.Bitmap $W, $H
    $g = [Drawing.Graphics]::FromImage($bmp)
    $g.InterpolationMode = 'HighQualityBicubic'
    $g.PixelOffsetMode = 'HighQuality'
    $side = [int]([Math]::Min($W, $H) * $Fill)
    $g.DrawImage($src, [int](($W - $side) / 2), [int](($H - $side) / 2), $side, $side)
    $g.Dispose()
    $bmp.Save((Join-Path (Resolve-Path "$layout\Assets") $Name), [Drawing.Imaging.ImageFormat]::Png)
    $bmp.Dispose()
}
foreach ($scale in 100, 125, 150, 200, 400) {
    $f = $scale / 100
    Save-Logo app.ico "Square150x150Logo.scale-$scale.png" ([int](150 * $f)) ([int](150 * $f)) 0.66
    Save-Logo app.ico "Wide310x150Logo.scale-$scale.png" ([int](310 * $f)) ([int](150 * $f)) 0.66
    Save-Logo app.ico "Square44x44Logo.scale-$scale.png" ([int](44 * $f)) ([int](44 * $f))
    Save-Logo app.ico "StoreLogo.scale-$scale.png" ([int](50 * $f)) ([int](50 * $f))
    Save-Logo spf.ico "SpfFile.scale-$scale.png" ([int](44 * $f)) ([int](44 * $f))
    Save-Logo spp.ico "SppFile.scale-$scale.png" ([int](44 * $f)) ([int](44 * $f))
}
foreach ($size in 16, 24, 32, 48, 256) {   # taskbar, Start and the file list, without the tile plate
    Save-Logo app.ico "Square44x44Logo.targetsize-$size.png" $size $size
    Save-Logo app.ico "Square44x44Logo.targetsize-${size}_altform-unplated.png" $size $size
}

$pri = Join-Path $Out 'priconfig.xml'
& $makepri createconfig /cf $pri /dq en-US /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri createconfig failed ($LASTEXITCODE)" }
& $makepri new /pr $layout /cf $pri /mn "$layout\AppxManifest.xml" /of "$layout\resources.pri" /o | Out-Null
if ($LASTEXITCODE -ne 0) { throw "makepri new failed ($LASTEXITCODE)" }
Remove-Item $pri

if ($LayoutOnly) { Write-Host "Layout: $layout"; exit 0 }

# ---- package
$msix = Join-Path $Out "ScreenPlus-$Version-x64.msix"
& $makeappx pack /d $layout /p $msix /o
if ($LASTEXITCODE -ne 0) { throw "makeappx failed ($LASTEXITCODE)" }
Remove-Item $layout -Recurse -Force
Write-Host "Built $msix"
