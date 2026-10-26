# Screen+ — working notes

Native Windows live-wallpaper app (C17, Win32, MSVC) that plays video, GIF, image, web and link wallpapers
behind the desktop icons, aiming at Lively Wallpaper compatibility. Use this map instead of exploring the
repository; open only the files a change touches. README.md is the user-facing documentation.

## Build
- `cmd.exe /c "<repo>\build.cmd" Release <x.y.z>`. Call it by full path; PowerShell can't resolve it relatively.
  It needs VS 2022 Build Tools + the Windows SDK. FFmpeg is prebuilt in `third_party/ffmpeg`; only
  `scripts/build-ffmpeg.sh` (MSYS2 UCRT64) rebuilds it. NSIS produces `build\ScreenPlus-Setup-<ver>.exe`.
- `build\` gets `ScreenPlus.exe` (GUI), `ScreenPlus.com` (terminal CLI), FFmpeg DLLs, `samples\` and `licenses\`.
  Keep the build at 0 warnings (/W4).
- There is no unit-test suite. `tests/desktop_probe.c` and `tests/sysinfo_test.c` are validation helpers.
- CI (`.github/workflows/release.yml`) releases every push to main as the next patch version, or as
  `SCREENPLUS_VERSION` in CMakeLists.txt when that is higher; a `vX.Y.Z` tag releases that exact version.
  The portable zip carries every `samples\` wallpaper and the URL tools. The installer ships only Aurora
  and downloads the tools.
- The local makensis is a trimmed copy (CMakeCache `MAKENSIS`). MUI_PAGE_COMPONENTS fails with it, so use
  nsDialogs custom pages.

## Processes and IPC
- **Shell**: `ScreenPlus.exe`, `src/app`. Runs the tray and UI window, owns `config.ini` and supervises the
  engine (`supervisor.c`: job object, backoff, safe mode). It is single-instance (mutex
  `Local\ScreenPlus.Instance.v1`); a second launch forwards its switch and exits.
- **Engine**: `ScreenPlus.exe --engine`, `src/engine/engine.c`. Owns the desktop-hosted windows, renderers,
  the per-monitor pause state machine (fullscreen, occlusion, battery, lock) and recovery. It reads
  `config.ini` itself and reloads on command.
- **Preview helper**: `ScreenPlus.exe --capture-preview <id>` (`src/app/preview_capture.c`) records HTML
  previews into `cache\previews`.
- **Child processes**: WebView2 browser processes, and yt-dlp (+ qjs) for link wallpapers.
- **IPC** (`src/common/ipc.h`) is WM_COPYDATA:
  - `IPC_ENGINE_HELLO` / `IPC_ENGINE_STATUS`: engine → shell.
  - `IPC_ENGINE_CMD`: shell → engine, `CMD_*`, sent with `supervisor_send`.
  - `IPC_SHELL_CMD`: second instance → shell.
  - `IPC_CLI_REQUEST` / `IPC_CLI_REPLY`: JSON between `ScreenPlus.com` and the shell.

## Source map
| Path | Contents |
| --- | --- |
| `src/main.c` | Mode dispatch: shell, `--engine`, `--capture-preview` |
| `src/app/app.c` | Shell window proc (`shell_proc`), `app_notice`, apply/pause/audio, setup handoff (`apply_setup_choice`) |
| `src/app/library.c` | Imports (files, folders, Lively/WE packages, Lively auto-discovery), remove, missing check |
| `src/app/url_import.c` | Link button job on a worker thread: direct downloads, yt-dlp streams/downloads, live pages |
| `src/app/cli_server.c` | Every CLI command that needs the app; `k_settings` table of settings keys |
| `src/app/startup.c`, `supervisor.c` | HKCU Run entry; engine process lifecycle |
| `src/cli/cli_main.c` | `ScreenPlus.com`: local commands (help, version, start, logs, paths, path, tools), IPC for the rest |
| `src/engine/` | `engine.c` (slots, state machine), `occlusion.c`, `audio_visualizer.c` (system-audio spectrum), `now_playing.c` (media session), `sysinfo.c` |
| `src/desktop/desktop_host.c` | Hosting behind the icons: Progman (24H2+), WorkerW, or fallback |
| `src/renderer/renderer.h` | Renderer vtable (set_state, resize, set_audio, paint, describe, destroy, wants, input, feed, on_timer, apply); all calls on the engine window thread |
| `src/renderer/video/` | `video_renderer.c`, `video_audio.c`, `video_stream.c`, `ffmpeg_thumb.c` (UI thumbnails), `video.hlsl` (NV12/I420/RGB + grading); details below |
| `src/renderer/html/` | `html_renderer.c` (WebView2), `wallpaper_api.c` (Lively/WE JS APIs, author properties); details below |
| `src/renderer/image/image_renderer.c` | WIC decode; presents on paint only |
| `src/ui/ui_core.c` | Window, Direct2D/DirectWrite primitives, input, focus, caret, tooltip |
| `src/ui/ui_pages.c` | Library, Displays, Customise and Settings pages; `pages_activate`, `pages_context`, `pages_tooltip` |
| `src/ui/thumbs.c`, `d2d_c.h`, `dwrite_c.h` | Thumbnail worker thread; hand-written C COM declarations |
| `src/settings/config.c` | `config.ini` (UTF-8 INI, atomic write + `.bak`), `WallpaperEntry`, `Settings` |
| `src/common/` | `util` (bounded `str_*`, paths, UTF-8, StrBuf), `log`, `paths` (data dirs), `json` (small DOM), `ipc.h`, `webfetch` (WinHTTP + pinned tool install; the CLI uses it too), `ytdlp` (run/resolve yt-dlp, stream address cache) |
| `installer/screenplus.nsi` | Per-user NSIS installer: install type page and an optional-features page (link tools) |
| `resources/` | Icon, `.rc`, manifest (comctl32 v6, PerMonitorV2) |

Renderer details:
- **Video**: one D3D11 swapchain per display and one `VideoSource` per path, shared across displays. Decode
  and present run on their own threads; decoding uses D3D11VA with a software fallback (`SCREENPLUS_HWDEC`).
  `video_stream.c` handles stream wallpapers.
- **HTML**: one WebView2 environment is shared. Folders are mapped to the virtual host
  `https://wallpaper.screenplus.example/`, and URL pages load directly. Hidden pages are suspended with a
  low memory target.

## UI model (immediate mode)
- `ui_invalidate` redraws on demand; there are no retained widgets. Drawing registers hit rects with
  `ui_hit(HIT_ID(kind, data), rect, focusable, cursor)`; `ui_button`, `ui_chip` and similar do this for you.
- To add a control: add an `HK_*` kind in `ui_internal.h`, draw it in `ui_pages.c` and handle it in
  `pages_activate`.
- Text fields use `u->textFocus` with `cus.edit` (`edit_begin`, `pages_text_char/key/end`, `edit_commit`). The
  search box has its own buffer, `u->search`.
- Glyphs are Segoe Fluent/MDL2 codepoints (`GL_*`), colours come from `u->t`, and units are DIPs.
- Closing the window to the tray destroys it and its GPU resources. State that must survive lives in
  statics or `g_app`.

## Data and settings
- Data lives in `%LOCALAPPDATA%\ScreenPlus\`, or in `SCREENPLUS_DATA_DIR` when that is set:

  | Path | Contents |
  | --- | --- |
  | `config.ini` | Settings and library |
  | `props\<id>.json` | Customise: author property overrides, plus grading under `_screenplus_adjust` |
  | `logs\` | `screenplus.log`, `engine.log`, `capture.log`, `crash\` minidumps |
  | `cache\` | `previews\`, `yt-dlp\`, `streams\` (stream addresses and thumbnails) |
  | `WebView2\` | WebView2 user data |
  | `downloads\` | Link imports; deleted with their entry |
  | `tools\` | `yt-dlp.exe`, `qjs.exe` |
- `WallpaperEntry` fields:
  - `type`: image, video or html.
  - `path`: a file, the HTML entry document, or an http(s) URL.
  - `format`: plain, lively or we.
  - `flags`: `WPFLAG_URL` (http(s) path), `WPFLAG_YTDLP` (a stream that yt-dlp resolves), `WPFLAG_AUDIO`,
    `WPFLAG_SYSINFO`.
  - `thumb`.
- To add a setting:
  1. Add the field to `Settings` in `config.h`.
  2. Add its default, parse and save in `config.c`.
  3. Add a `k_settings` row in `cli_server.c`.
  4. Add a `TOG_*` in `ui_internal.h`, its row on the Settings page, and a case in `toggle_setting`.
- Customise changes never restart a renderer: `app_apply_customise()` sends `CMD_APPLY_CUSTOMISE`, and the
  engine calls `vt->apply` so each renderer re-reads its props and grading.
- Setup hands choices to the app through `HKCU\Software\ScreenPlus`: `SetupMode` (default/clean) and
  `UrlImport` (0/1). Both are applied once, at the next start.

## Link wallpapers
- The pinned tools (URL, size, SHA-256) are `k_urlTools` in `webfetch.c`. To update, change all three (yt-dlp
  publishes SHA2-256SUMS; GitHub lists asset digests). They are re-verified before use and re-downloaded on
  a mismatch.
- FFmpeg is built without network code. Streams therefore read through `video_stream.c`'s AVIOContext:
  4 MB HTTP ranges, 8 slots (32 MB), the first chunk pinned, 4 chunks prefetched, idle while paused.
- Stream addresses are cached as `cache\streams\<fnv32(link)>.url` and re-resolved for the same format id
  shortly before `expire=`, or on a 403.
- Automatic mode downloads videos of up to 10 minutes and 300 MB once and streams longer ones. A page with no
  video becomes a live HTML wallpaper.
- yt-dlp.exe is a PyInstaller one-file build. It ignores `PYTHON*` variables, so pass `--encoding utf-8`.
  Killing its launcher leaks `%TEMP%\_MEI*`, so stop its children first (`stop_tree` in `ytdlp.c`).

## Conventions
- C17 with MSVC `/W4 /utf-8` and zero warnings. Use `wchar_t` throughout, and UTF-8 only at file, JSON and
  IPC boundaries.
- Use the bounded helpers (`str_copy`, `str_printf`, `str_cat`, `path_join`, `SP_COUNTOF`); never unbounded
  copies.
- Log with `LOGI/LOGW/LOGE/LOGD`. The CLI target doesn't link `log.c`, so shared code (`webfetch.c`) must
  not log.
- Each file opens with a design comment; other comments are short and say why. Match the surrounding
  density.
- No new third-party dependencies, no polling or busy loops, and no work while paused: RAM and power matter.
- `small` is a Windows macro, so don't use it as a name. The working tree uses LF line endings.

## Testing a change
- Check the logs (`ScreenPlus.com logs -f`) and state (`ScreenPlus.com status --json`, `settings`, `look`,
  `props`). This avoids screenshots.
- The user's installed copy (`%LOCALAPPDATA%\Programs\ScreenPlus`) is usually running, and only one
  instance can run at a time. To try a build:
  1. Compare `ScreenPlus.com version` first and never downgrade.
  2. Run `ScreenPlus.com stop`.
  3. Run or copy in the build.
  4. Restart the installed copy afterwards.
  A dev build whose config has `start_with_windows=1` rewrites `HKCU\...\Run\ScreenPlus`; restore it.
- To test logic without stopping the user's app, compile a scratch harness with `cl`. Link the real `.c`
  files, stub the `app_*`/`ui_*` hooks, and point `SCREENPLUS_DATA_DIR` at a scratch folder.
- `ScreenPlus.exe --capture-preview <id>` renders an HTML wallpaper offscreen.
- `docs/media/library.gif` (880×572, 40 frames, 100 ms) is made by running the dev build on demo data,
  capturing with `PrintWindow`, and assembling with Pillow.
- Shell gotcha: the Bash tool collapses `\\` to `\`. Write scripts that contain backslashes with the Write
  tool.

## Known gaps (vs Lively)
- There are no application, Unity or Godot wallpapers; they are reported as unsupported.
- Missing features: spanning one wallpaper across monitors, per-app pause rules, a screensaver mode, and
  in-app update notices.
- Customise skips author property types `button` and `label`. Web grading has no gamma, because CSS has no
  equivalent.
- Streams need a connection. HLS-only sites can't stream, and content that needs a sign-in is never
  imported.
