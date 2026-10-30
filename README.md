<div align="center">

<img src="docs/media/icon.png" width="80" alt="">

# Screen+

Live wallpapers for Windows.

[![Release](https://img.shields.io/github/v/release/CoolNinja2009/ScreenPlus?style=flat-square&color=0a84ff&label=release)](https://github.com/CoolNinja2009/ScreenPlus/releases/latest)
[![Downloads](https://img.shields.io/github/downloads/CoolNinja2009/ScreenPlus/total?style=flat-square&color=0a84ff)](https://github.com/CoolNinja2009/ScreenPlus/releases)
[![License: MIT](https://img.shields.io/badge/license-MIT-2ea44f?style=flat-square)](LICENSE)

</div>

![Screen+ library](docs/media/library.gif)

Screen+ sets videos, GIFs, pictures and web pages as your desktop wallpaper. It is a native Win32 app written in C
and pauses rendering when the wallpaper is hidden. Most Lively Wallpaper wallpapers work without changes.

**[Download](https://github.com/CoolNinja2009/ScreenPlus/releases/latest)** for Windows 10 or 11 (64-bit).

## Features

- Video, GIF, image and web wallpapers
- Hardware video decoding (H.264, HEVC, VP9, AV1) with a software fallback
- Lively and Wallpaper Engine web and video wallpapers, including audio visualisers
- Wallpapers from YouTube and other video links
- A store that searches seven wallpaper sites at once
- Per-monitor wallpapers, or one wallpaper duplicated or spanned across monitors
- Screensaver mode
- Make wallpaper cooler: turns a picture into a 3D wallpaper that follows the mouse
- Colour grading and wallpaper properties
- Pauses for fullscreen apps, covered desktops, the lock screen and battery power
- Starts the moment you sign in (a Task Scheduler logon task, not the delayed startup-apps list)
- No admin rights needed, portable version available
- Command-line control

## Install

Run `ScreenPlus-Setup-<version>.exe` from the [latest release](https://github.com/CoolNinja2009/ScreenPlus/releases/latest).
**Default** adds the sample wallpapers and your Lively library; **Clean** starts empty. Installing over an existing
copy updates it and keeps your library.

Installed copies update themselves from GitHub (**Settings > Auto update**). The portable zip does not.

The installer is not code-signed yet, so SmartScreen may warn you: click **More info**, then **Run anyway**.
Web wallpapers need the [WebView2 Runtime](https://go.microsoft.com/fwlink/p/?LinkId=2124703), which is included
with Windows 11.

Silent install: `ScreenPlus-Setup-<version>.exe /S [/DEFAULT | /CLEAN] [/NOURL] [/VCRUN] [/D=<folder>]`

## Usage

- **Library**: import files or a folder, or drag them onto the window. For a web wallpaper, import the folder that
  contains `index.html`. The link button adds a video, image or web page from a URL.
- **Store**: search MotionBGS, Lively, MoeWalls, wallsflow, DesktopHut, backiee and Wallhaven. Sites that don't allow
  direct downloads open in your browser.
- **Displays**: choose Independent, Duplicate or Span.
- **Customise**: adjust colours and the wallpaper's own properties. **Make wallpaper cooler** downloads about 110 MB
  the first time and needs the Visual C++ Redistributable 14.44 or newer.
- **Settings**: startup, screensaver, pausing, audio and updates.

| Type | Formats |
| --- | --- |
| Video | MP4, M4V, MOV, MKV, WebM, AVI, WMV, MPG, TS, M2TS, GIF |
| Image | JPG, PNG, BMP, GIF, TIFF, WebP, HEIC, AVIF, JXR |
| Web | HTML folders, Lively and Wallpaper Engine web wallpapers |
| Link | [Sites supported by yt-dlp](https://github.com/yt-dlp/yt-dlp/blob/master/supportedsites.md), direct video and image links, web pages |

Application, Unity, Godot and Wallpaper Engine scene wallpapers are not supported.

## Wallpaper API

Web wallpapers get the Lively and Wallpaper Engine APIs: `livelyPropertyListener`, `livelyAudioListener`,
`livelyCurrentTrack`, `livelySystemInformation`, `livelyWallpaperPlaybackChanged`, `wallpaperRegisterAudioListener`
and `wallpaperPropertyListener`. Pages run in WebView2 at `https://wallpaper.screenplus.example/` and receive mouse
input but not keyboard input.

## Command line

```text
screenplus status | list [filter] | displays
screenplus apply <wallpaper> [--display N]
screenplus import <path|link>... | remove <wallpaper>
screenplus pause | resume | next | restart | audio [on|off]
screenplus look | props | settings [set <key> <value>]
screenplus screensaver | update | tools | vcredist
screenplus start | show | hide | stop | logs [-f]
screenplus help [command]
```

Add `--json` for machine-readable output.

## Data

Settings, logs and downloads are stored in `%LOCALAPPDATA%\ScreenPlus`. Set `SCREENPLUS_DATA_DIR` to use a
different folder.

## Building

Requirements: Visual Studio 2022 Build Tools (C++ and Windows SDK), [MSYS2](https://www.msys2.org) UCRT64 with
NASM, and [NSIS 3](https://nsis.sourceforge.io) for the installer.

```bash
scripts/build-ffmpeg.sh    # once, from an MSYS2 UCRT64 shell
```

```bat
build.cmd Release
```

The app and installer are written to `build\`. GitHub Actions builds and releases every push to `main`.

## License

MIT, see [LICENSE](LICENSE). Third-party components are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md):

- [FFmpeg](https://ffmpeg.org) (LGPL-2.1-or-later, dynamically linked)
- [WebView2 SDK](https://developer.microsoft.com/microsoft-edge/webview2/) (BSD-3-Clause)
- [yt-dlp](https://github.com/yt-dlp/yt-dlp) (Unlicense) and [QuickJS-NG](https://github.com/quickjs-ng/quickjs) (MIT), optional
- [ONNX Runtime](https://github.com/microsoft/onnxruntime) (MIT) and [Depth Anything V2 Small](https://huggingface.co/onnx-community/depth-anything-v2-small) (Apache-2.0), downloaded on first use
- [NSIS](https://nsis.sourceforge.io) for the installer

Wallpapers in the screenshots belong to their authors. Screen+ is not affiliated with Lively Wallpaper, Wallpaper
Engine or the Store's sites.
