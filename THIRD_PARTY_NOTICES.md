# Third-party notices

## FFmpeg 8.1.3 — LGPL-2.1-or-later (dynamically linked)

Screen+ uses the FFmpeg libraries `avformat-62.dll`, `avcodec-62.dll`, `avutil-60.dll`,
`swscale-9.dll` and `swresample-6.dll`, built from the unmodified upstream release
<https://ffmpeg.org/releases/ffmpeg-8.1.3.tar.xz>
(SHA-256 `7138D28C96D9D3E3AF4EE3D8CAD72741F8FFB40DA90C1112235DEA3ECD3178A3`).

- Licensed under the GNU Lesser General Public License v2.1 or later; full text in
  `licenses/COPYING.LGPLv2.1`. The build contains no GPL or non-free components
  (`--enable-gpl` / `--enable-nonfree` are not used; `FFMPEG_LICENSE` = "LGPL version 2.1 or later").
- The exact configure line is recorded in `licenses/BUILDINFO.txt`; the reproducible build
  script is `scripts/build-ffmpeg.sh`. Source for the shipped FFmpeg binaries is the
  tarball above.
- The libraries are dynamically linked (delay-loaded) and may be replaced: drop in any
  ABI-compatible FFmpeg 8.x build of the same major versions next to `ScreenPlus.exe`.
- Codec patents: some formats (e.g. H.264/HEVC) may be covered by patents in some
  jurisdictions. Distributors are responsible for any required licenses.

## Microsoft Edge WebView2 SDK 1.0.4258.31 — BSD-3-Clause

`WebView2LoaderStatic.lib` (statically linked) and `WebView2.h` from the
`Microsoft.Web.WebView2` NuGet package. Copyright (C) Microsoft Corporation.
Redistribution and use in source and binary forms, with or without modification, are
permitted provided that the conditions of the BSD 3-Clause license are met (see the
`LICENSE.txt` in the NuGet package). The WebView2 *Runtime* is not redistributed; it is
part of Windows 11 and installable on Windows 10.

## yt-dlp 2026.10.21 — Unlicense; QuickJS-NG 0.17.0 — MIT (optional, separate programs)

Used only by "Import from a link" (the optional setup component, or the `tools` folder of the
portable zip). The unmodified official release binaries are run as separate processes, never
linked: `yt-dlp.exe` from <https://github.com/yt-dlp/yt-dlp/releases/tag/2026.10.21>
(SHA-256 `33121d192d706e62f684a06f80918e3f3a8ba52ac3a65213b6951c39c7d9f555`; public domain under
the Unlicense, its bundled components carry their own notices) and `qjs.exe`
(`qjs-windows-x86_64.exe`) from <https://github.com/quickjs-ng/quickjs/releases/tag/v0.17.0>
(SHA-256 `2aeabf0092c3262d6b2609824418f7dd7ed1f1df939f73b2b15645230cac0d77`; Copyright (c)
Fabrice Bellard, Charlie Gordon and the QuickJS-NG contributors, MIT License). Screen+ refuses
any file that does not match these checksums.

## ONNX Runtime 1.24.4 (DirectML build) — MIT; Depth Anything V2 Small — Apache-2.0 (optional, downloaded on request)

Used only by "Make wallpaper cooler". Nothing is bundled: on the user's request Screen+ downloads, verifies
(SHA-256) and keeps `onnxruntime.dll` from the official `Microsoft.ML.OnnxRuntime.DirectML` 1.24.4 NuGet package
(SHA-256 `57e9f11b73437bef7a309496135d4c1f96b1a8e9ddba60013fa27bfc1d788681`; Copyright (c) Microsoft
Corporation, MIT License) and the unquantized FP32 ONNX export of Depth Anything V2 Small
(<https://huggingface.co/onnx-community/depth-anything-v2-small>, SHA-256
`afb6a5c28f3b6bf1618c6e43f02073ef9dfdc70e937502d51603e57b0a1df10c`; Copyright the Depth Anything V2 authors,
Apache License 2.0). Only the ONNX Runtime C API headers are used at build time. DirectML is the copy that is
part of Windows.

## Microsoft Visual C++ Redistributable (x64) 14.44 (optional, downloaded on request)

ONNX Runtime needs it. When it is missing or older than 14.44, setup (or `screenplus vcredist install`) downloads
Microsoft's own installer from <https://aka.ms/vs/17/release/vc_redist.x64.exe> (the pinned build, SHA-256
`cc0ff0eb1dc3f5188ae6300faef32bf5beeba4bdd6e8e445a9184072096b713b`, signed by Microsoft Corporation) and runs it
after Windows' administrator prompt. It is never bundled; its own license terms apply.

## Sample wallpapers (`samples\`: the portable zip, and setup's optional download)

Aurora and Singularity are Screen+'s own. The others are Lively Wallpaper packages by the authors below; each
folder keeps its own license file and its source link (`LivelyInfo.json`).

| Folder | Wallpaper | Author | License |
| --- | --- | --- | --- |
| `0wj1biqk.f41` | Fluids | Pavel Dobryakov (Lively port: rocksdanister) | MIT |
| `4yvhquwl.pcv` | Simple System | rocksdanister | MIT |
| `abqqpcp4.hv3` | Rain | rocksdanister | CC BY-NC-SA 3.0 |
| `iqdvd4pt.jyo` | Triangles & Light | Maksim Surguy | MIT |
| `lgz4xpht.sjn` | The Hill | Yoichi Kobayashi | MIT |
| `mylulkt4.dn1` | Living Room | rocksdanister | MIT |
| `nps35xrp.5eh` | Medusae | Jay Weeks | Artistic License 2.0 |
| `ptpcxvcd.hlz` | Periodic Table | Mike Golus | MIT |
| `rtnm43pj.wyl` | Parallax.js | Matthew Wagerfield | MIT |
| `spfaevge.qza` | Music Tunnel | rocksdanister | MIT |
| `xn0quq52.bq2` | Matrix Rain Customizable | parambirs, khuong | none stated (see its `README.md`) |
| `zmijuhra.1n3` | Music TV (LQ) | rocksdanister | MIT |

## Windows SDK / system components

Direct3D 11, DXGI, Direct2D, DirectWrite, WIC, WASAPI, DWM and the Segoe fonts are part of
Windows and are not redistributed.
