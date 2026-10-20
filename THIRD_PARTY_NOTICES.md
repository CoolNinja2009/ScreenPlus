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

## Windows SDK / system components

Direct3D 11, DXGI, Direct2D, DirectWrite, WIC, WASAPI, DWM and the Segoe fonts are part of
Windows and are not redistributed.
