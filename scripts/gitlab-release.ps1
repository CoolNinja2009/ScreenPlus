# Runs inside the GitLab CI job (see .gitlab-ci.yml): tools, FFmpeg, build, package, release.
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$api = "$env:CI_API_V4_URL/projects/$env:CI_PROJECT_ID"
$auth = @{ 'JOB-TOKEN' = $env:CI_JOB_TOKEN }
$publish = $env:CI_PIPELINE_SOURCE -ne 'merge_request_event' -and
    ($env:CI_COMMIT_TAG -or $env:CI_COMMIT_BRANCH -eq $env:CI_DEFAULT_BRANCH)

function Invoke-Native([string]$What, [scriptblock]$Run) {
    & $Run
    if ($LASTEXITCODE -ne 0) { throw "$What failed ($LASTEXITCODE)" }
}

function Test-Url([string]$Url) {
    try {
        Invoke-WebRequest $Url -Headers $auth -Method Head -UseBasicParsing | Out-Null
        return $true
    } catch { return $false }
}

# The tag if this is a tag pipeline, else the next patch after the newest tag, or the CMakeLists.txt
# version when that is higher.
function Get-Version([string]$Tag, [string[]]$Tags, [string]$CMake) {
    if ($Tag) { $ver = $Tag.TrimStart('v') }
    else {
        if ($CMake -notmatch '(?m)^set\(SCREENPLUS_VERSION "(\d+\.\d+\.\d+)"') { throw 'No version in CMakeLists.txt' }
        $base = [version]$Matches[1]
        $found = @($Tags | Where-Object { $_ -match '^v\d+\.\d+\.\d+$' } | ForEach-Object { [version]$_.Substring(1) } | Sort-Object)
        if ($found.Count -eq 0 -or $base -gt $found[-1]) { $ver = "$base" }
        else { $l = $found[-1]; $ver = "$($l.Major).$($l.Minor).$($l.Build + 1)" }
    }
    if ($ver -notmatch '^\d+\.\d+\.\d+$') { throw "Version '$ver' must be major.minor.patch" }
    return $ver
}

function Send-File([string]$Path, [string]$Url) {
    Invoke-RestMethod $Url -Method Put -InFile $Path -Headers $auth -ContentType 'application/octet-stream' | Out-Null
}

# Making a release creates its tag, and that starts a tag pipeline: nothing is left for it to do.
if ($env:CI_COMMIT_TAG -and (Test-Url "$api/releases/$env:CI_COMMIT_TAG")) {
    Write-Host "$env:CI_COMMIT_TAG is already released"
    exit 0
}

$ver = Get-Version $env:CI_COMMIT_TAG @(git tag -l) (Get-Content CMakeLists.txt -Raw)
Write-Host "Building Screen+ $ver"

# ---- tools
$vswhere = 'C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe'
$vs = if (Test-Path $vswhere) {
    & $vswhere -version '[17.0,18.0)' -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
}
if (-not $vs) {
    Invoke-Native 'Visual Studio install' {
        choco install visualstudio2022buildtools -y --no-progress --package-parameters `
            '--add Microsoft.VisualStudio.Workload.VCTools --includeRecommended --passive'
    }
}
Invoke-Native 'Tools install' { choco install nsis ninja -y --no-progress }
$env:PATH = "$env:PATH;C:\ProgramData\chocolatey\bin"

# ---- FFmpeg: compiled once per revision of build-ffmpeg.sh, then kept in the package registry
$hash = (Get-FileHash scripts\build-ffmpeg.sh -Algorithm SHA256).Hash.Substring(0, 16).ToLower()
$ffmpegUrl = "$api/packages/generic/ffmpeg-$hash/1.0.0/ffmpeg.zip"
$ffmpegZip = "$env:TEMP\ffmpeg-$hash.zip"
$hit = $true
try { Invoke-WebRequest $ffmpegUrl -Headers $auth -OutFile $ffmpegZip -UseBasicParsing }
catch { $hit = $false }
if ($hit) {
    Expand-Archive $ffmpegZip -DestinationPath third_party -Force
    Write-Host "Using prebuilt ffmpeg-$hash"
} else {
    Write-Host "No prebuilt ffmpeg-$hash yet: building FFmpeg once"
    Invoke-Native 'MSYS2 install' { choco install msys2 -y --no-progress }
    $bash = 'C:\tools\msys64\usr\bin\bash.exe'
    $env:MSYSTEM = 'UCRT64'
    $env:CHERE_INVOKING = '1'
    Invoke-Native 'MSYS2 packages' {
        & $bash -lc 'pacman -S --noconfirm --needed make diffutils tar xz curl mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-nasm'
    }
    Invoke-Native 'FFmpeg build' { & $bash -lc 'bash ./scripts/build-ffmpeg.sh' }
    if ($publish) {
        Compress-Archive -Path third_party\ffmpeg -DestinationPath $ffmpegZip -Force
        Send-File $ffmpegZip $ffmpegUrl
    }
}

# ---- app and installer
Invoke-Native 'Build' { cmd.exe /c "build.cmd Release $ver" }

# ---- package
$setup = "build\ScreenPlus-Setup-$ver.exe"
if (-not (Test-Path $setup)) { throw "Installer was not built: $setup" }
$stage = 'dist\ScreenPlus'
New-Item -ItemType Directory -Force $stage | Out-Null
Copy-Item build\ScreenPlus.exe, build\*.dll $stage
Copy-Item build\licenses, build\samples $stage -Recurse
# Self-contained like the rest of the zip: the URL-import tools, fetched and SHA-256 verified by the app.
$env:SCREENPLUS_DATA_DIR = "$env:TEMP\sp-data"
Invoke-Native 'URL-import tools' { & build\ScreenPlus.com tools install }
Copy-Item "$env:SCREENPLUS_DATA_DIR\tools" $stage -Recurse
New-Item -ItemType Directory -Force release | Out-Null
Copy-Item $setup, "build\ScreenPlus-$ver-samples.zip", "build\ScreenPlus-$ver-samples.json" release\
Compress-Archive -Path "$stage\*" -DestinationPath "release\ScreenPlus-$ver-win-x64-portable.zip"
Get-ChildItem release -File | ForEach-Object {
    "$((Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower())  $($_.Name)"
} | Set-Content -Encoding ascii release\SHA256SUMS.txt
Get-ChildItem release

if (-not $publish) { exit 0 }

# ---- release: the files go to the package registry, the release links to them
$links = @(Get-ChildItem release -File | ForEach-Object {
    $url = "$api/packages/generic/screenplus/$ver/$($_.Name)"
    Send-File $_.FullName $url
    @{ name = $_.Name; url = $url; direct_asset_path = "/$($_.Name)"; link_type = 'package' }
})
$body = @{
    name = "Screen+ $ver"
    tag_name = "v$ver"
    ref = $env:CI_COMMIT_SHA
    description = "Built from $env:CI_COMMIT_SHORT_SHA."
    assets = @{ links = $links }
} | ConvertTo-Json -Depth 6
Invoke-RestMethod "$api/releases" -Method Post -Headers $auth -ContentType 'application/json; charset=utf-8' `
    -Body ([Text.Encoding]::UTF8.GetBytes($body)) | Out-Null
Write-Host "Released v$ver"
exit 0
