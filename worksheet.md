# Screen+ Linux test worksheet

For Claude, running on the user's Linux machine (or over SSH into it). Work from the top. Fix what fails, retest it, and
fill in **Results** at the bottom as you go. Written on 2026-11-06 at commit `0ce771f` (`cross-platform`), without a Linux
machine: nothing here has been run yet, so if a step itself is wrong, correct it and carry on.

## Rules

- Read the "Linux" section of `CLAUDE.md` first. Open only the files a fix needs. The user is short on tokens: keep reports brief.
- Stay on branch `cross-platform`. Do not merge `main` into it. `SHARP` comes in at part 9a and not before.
- Keep each file's line endings. `0ce771f` flipped them on `CLAUDE.md`, `cli_server.c` and `cli_main.c` (thousands of noise
  lines). Check `git diff --stat` before any commit: the numbers should match the size of the fix.
- Shared code (`src/common`, `src/ui`, `src/app`, `src/settings`) also builds on Windows with MSVC at 0 warnings. Guard Linux-only code
  with `#ifdef _WIN32`, and use `%ls` for wide strings.
- Every behaviour change also reaches the CLI where it applies (see `CLAUDE.md`, "CLI parity").
- Ask before committing or pushing. Never add a `Co-Authored-By: Claude` line.
- `sudo` needs the user's password: ask them to run those commands.
- Steps marked **USER** need the user's eyes or hands. Save them and ask for them in one batch at the end of a part.
- Never leave the machine worse than you found it: **Cleanup** at the end is required.

## 0. Setup

**Get the code.** GitHub may be hidden (the account was flagged on 2026-11-06), and GitLab may have only `main`.
Run `git log -1 --oneline cross-platform` in any existing checkout. If it is missing or older than `0ce771f`, try
`git fetch origin` and then `git fetch gitlab`. If neither has the branch, ask the user to push it to GitLab or to copy over
a bundle (`git bundle create sp.bundle cross-platform` on Windows).

**Know the machine.** Note these in Results:

```bash
cat /etc/os-release | head -3; echo "$XDG_CURRENT_DESKTOP $XDG_SESSION_TYPE"; plasmashell --version; hyprctl version 2>/dev/null | head -1
systemd-detect-virt; command -v plasmawindowed kscreen-doctor spectacle ffmpeg ffprobe yt-dlp curl
```

If it is the Arch VM (VirtualBox, `coolninja@127.0.0.1:2222`): everything is slow, and it needs 3D on for web wallpapers. The session
auto-locks after a few idle minutes and only the user can unlock it, so check `loginctl show-session "$XDG_SESSION_ID" -p LockedHint` before
trusting any screenshot. `VBoxManage screenshotpng` is stale with 3D on. A service started over SSH gets no keyboard in its
window, so have the user start it from Konsole for the typing tests.

**Over SSH**, first set the session's environment, or the service reports an unsupported desktop:

```bash
export XDG_RUNTIME_DIR=/run/user/$(id -u) WAYLAND_DISPLAY=wayland-0 XDG_CURRENT_DESKTOP=KDE
export DBUS_SESSION_BUS_ADDRESS=unix:path=$XDG_RUNTIME_DIR/bus
```

**Dependencies** (Arch names; ask the user to install what is missing):

```bash
sudo pacman -S --needed base-devel cmake ninja pkgconf sdl3 cairo pango fontconfig gtk4 gtk4-layer-shell mpv webkitgtk-6.0 libepoxy wayland wayland-protocols ffmpeg qt6-multimedia qt6-multimedia-ffmpeg qt6-webengine yt-dlp curl
```

**Protect the user's own library.** Stop any running copy, record the current wallpaper, and use a scratch data dir. Export
`SCREENPLUS_DATA_DIR` in every shell used for testing.

```bash
command -v screenplus && screenplus stop
cp ~/.config/plasma-org.kde.plasma.desktop-appletsrc ~/sp-appletsrc.backup
export SCREENPLUS_DATA_DIR=$HOME/sp-test/data
```

**Test files:**

```bash
F=$HOME/sp-test/files; mkdir -p "$F/page-plain" "$F/page-lively"; cd "$F"
ffmpeg -v error -y -f lavfi -i testsrc2=s=1920x1080:r=30:d=12 -f lavfi -i sine=f=440:d=12 -c:v libx264 -pix_fmt yuv420p -c:a aac video.mp4
cp video.mp4 "wäll paper ✓.mp4"
ffmpeg -v error -y -f lavfi -i testsrc=s=480x270:r=10:d=3 anim.gif
ffmpeg -v error -y -f lavfi -i testsrc=s=1920x1080:d=1 -frames:v 1 still.png
cat > page-plain/index.html <<'E'
<!doctype html><title>plain</title><body style="margin:0;background:#123;color:#fff;font:40px sans-serif">
<div id=o>0</div><div id=m>no pointer yet</div><script>
let n=0;setInterval(()=>{o.textContent=++n;document.body.style.background=`hsl(${n*7%360} 60% 30%)`},100);
for(const t of['mousemove','mousedown','mouseup','click'])addEventListener(t,e=>m.textContent=t+' '+e.clientX+','+e.clientY);
</script>
E
cp page-plain/index.html page-lively/index.html
ffmpeg -v error -y -i anim.gif page-lively/preview.gif
ffmpeg -v error -y -i still.png -vf scale=640:360 page-lively/thumbnail.jpg
cat > page-lively/LivelyInfo.json <<'E'
{"AppVersion":"2.0","Title":"Lively test","Thumbnail":"thumbnail.jpg","Preview":"preview.gif","Desc":"","Author":"","Type":1,"FileName":"index.html"}
E
cat > page-lively/LivelyProperties.json <<'E'
{"speed":{"max":10,"min":1,"tick":1,"text":"Speed","type":"slider","value":5}}
E
```

## 1. Build

| ID | Do | Pass |
|---|---|---|
| B1 | `cmake -S . -B build-linux -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo && cmake --build build-linux` | Builds all three: `screenplus`, `screenplus-ui.so`, `screenplus-engine`. Fix every `-Wformat` warning: a `%s`/`%ls` mismatch prints garbage or one letter. |
| B2 | Make a release-shaped folder, as `linux.yml`'s Package step does: `d=$HOME/sp-test/dist; mkdir -p $d/fonts; cp build-linux/src/linux/{screenplus,screenplus-engine,screenplus-ui.so} src/linux/{install.sh,screenplus.png} $d/; cp resources/fonts/*.ttf $d/fonts/; cp -r src/linux/kde samples LICENSE THIRD_PARTY_NOTICES.md $d/` | Folder complete |
| B3 | `$d/install.sh`, then `screenplus version` from a new shell | Prints the version (1.9.0 for a local build), `~/.local/bin/screenplus` links into `~/.local/share/screenplus-app`, the `.desktop` file and icon are in place |

After every later fix, rebuild and repeat B2 and B3. If a file under `kde/` changed, also run
`systemctl --user restart plasma-plasmashell` (the old plugin stays loaded otherwise).

## 2. CLI and service

Check exit codes as well as text: 0 ok, 1 failed, 2 bad usage, 3 not running.

| ID | Do | Pass |
|---|---|---|
| C1 | With the service stopped: `screenplus status`, `screenplus list` | Exit 3, clear message |
| C2 | `help`, `paths`, `path data`, `path nonsense`, `tools`, `logs -n 5` | Work without the service; `path nonsense` exits 2; paths are under `$SCREENPLUS_DATA_DIR` |
| C3 | `screenplus start` twice, then `status` | One service only (`pgrep -fc 'screenplus --daemon'` is 1), socket at `$XDG_RUNTIME_DIR/screenplus.sock` |
| C4 | `import` each file in `$F`, both page folders, and a relative path; then `list` and `list --json` | All listed with the right type. The name with `ä` and `✓` is intact in text and JSON. JSON parses (`python -m json.tool`). |
| C5 | `import /does/not/exist` | Non-zero exit, clear message, no empty entry |
| C6 | `apply` each type, then `next` several times | Plasma check below passes each time |
| C7 | `displays`, `mode`, `mode duplicate`, `mode span`, `mode independent`, `mode nonsense` | Sizes and Hz are right (see K5); the bad mode exits 2 or 1 without changing the mode |
| C8 | `pause`, `resume`, `toggle`, `audio on/off/toggle`, each followed by `status` | Status follows each one |
| C9 | `look`, `look set brightness 20` (take the key names from `look`), `look reset`; `props`, `props set speed 8`, `props reset` with `page-lively` showing | Values persist in `status`/`look`/`props`; `Brightness` etc. reach Plasma |
| C10 | `settings`, then `get` and `set` on one key of each kind, and a bad key and a bad value | Round-trips; survives `restart`; bad input leaves a clear error, not a changed value |
| C11 | `remove` a wallpaper that is showing | Next state is sensible; nothing is left pointing at it after `restart` |
| C12 | `startup on`, `startup`, `startup off` | `~/.config/autostart/screenplus.desktop` appears and goes |
| C13 | `screenplus` alone, then pipe in `status` and `quit`: `printf 'status\nquit\n' \| screenplus` | Prompt works, exits on `quit` and on EOF |
| C14 | `screensaver`, plus anything `help` doesn't list | Short "not on Linux" or unknown-command error, no crash |
| C15 | `logs -f` while applying, and `import` of a direct link and a video link (only if the user agrees to the network use) | Lines stream; `tools` agrees with what is installed |
| C16 | `kill -9` the service, then `screenplus status`, `start` | Stale socket doesn't block the restart |
| C17 | `stop` | Service gone, socket gone, and Plasma is back on `org.kde.image` with the user's picture, not a blank one |

**Plasma check**, for what an apply really did:

```bash
qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript 'var d=desktops()[0];d.currentConfigGroup=["Wallpaper","org.screenplus.wallpaper","General"];print(d.wallpaperPlugin+" "+d.readConfig("Kind")+" "+d.readConfig("Source"))'
journalctl --user -u plasma-plasmashell -n 30 --no-pager | grep -iE 'qml|screenplus|error'
```

Also read the Screen+ log after every part. Lines with one-letter words mean a wide string went through `%s`.

## 3. KDE wallpapers

Screenshot with `spectacle -b -n -f -o /tmp/sp.png`, then read the PNG. Take two a second apart to prove motion.

| ID | Do | Pass |
|---|---|---|
| K1 | Apply `video.mp4`, `wäll paper ✓.mp4`, `anim.gif`, `still.png`, `page-plain`, `page-lively` in turn, then back and forth quickly between GIF and image 5 times | Each shows and moves. The GIF starts after an image (the view-per-switch fix in `main.qml`). No leftover frame or black screen. |
| K2 | `audio on` with `video.mp4` | **USER**: hears the tone. `audio off` silences it. |
| K3 | `pause`, then take CPU readings: `top -b -n2 -d3 -p $(pgrep -x plasmashell) \| tail -1` | Picture freezes and CPU drops. `resume` continues. |
| K4 | **USER**: maximise a window over the desktop with a video playing | With `pause_on_fullscreen` on, CPU drops; back to normal when it closes |
| K5 | Compare `screenplus displays` with `kscreen-doctor -j`, ideally with a second or a disabled output | Right Hz on each display. See suspect S2. |
| K6 | **USER**, with `page-plain`: move and click on empty desktop | The text shows `mousemove`/`click` with sensible coordinates; desktop icons and the right-click menu still work. With the `html_mouse` setting off, no events reach the page. |
| K7 | `page-lively`: check its shim is loaded by changing `speed` with `props set`; open the wallpaper's page console if needed | `livelyPropertyListener` is called; no errors in the journal |
| K8 | `look set` brightness, contrast, saturation on a video and a page | Visible change on each |
| K9 | If there are 2+ displays: a different wallpaper on each with `apply --display N`, then each `mode` | The right one on the right display. `cfg[d.screen]` assumes Plasma's screen numbers match the `displays` order; check it. |
| K10 | `systemctl --user restart plasma-plasmashell`, then log out and in with `startup on` (**USER**) | Wallpaper comes back, service reconnects, nothing duplicated |
| K11 | Lock widgets in Plasma, then `apply` | Clear error rather than silent failure (`desktop_kde.c`) |

## 4. The window

`screenplus show`. It needs SDL3, so check that `hide` and `show` repeat cleanly over at least 5 cycles and the service survives.

| ID | Do | Pass |
|---|---|---|
| U1 | Open each page in the sidebar; screenshot each at the default size and resized small | No overlap, clipped text, missing glyphs or empty boxes. Icons are the Fluent ones (a new `GL_*` needs a line in `ui_icons.h`). |
| U2 | Check for the things not ported: auto update, sample updates, audio visualizer, now playing, stills, Lively discovery, stream links, screensaver (plus Store and depth until part 9 lands them) | None shows a dead button. `ui_pages.c` hides sections; add what's missing. |
| U3 | Thumbnails, with `$F` imported | Stills first, then animations, for video and GIF. Non-ASCII file name works. `page-lively` animates from its GIF. `page-plain`: see T1. |
| U4 | Import 30 copies of the video, scroll fast, then `hide` during thumbnailing | No freeze, and `hide` isn't held up by a running ffmpeg (`thumbs_stop` joins the worker). No ffmpeg left behind: `pgrep -c ffmpeg`. |
| U5 | **USER**: click Apply, remove, the menus and a dialog; drag and drop a file in; type in the search box (IME and accents too) | All work. Keyboard needs the service started from Konsole. |
| U6 | Trigger an info notice and a warning | Info goes by itself after 10 s; the warning stays until closed; the close button works on both |
| U7 | Change a setting in the window, read it with `settings get`, and the reverse | Both stay in step while the window is open |
| U8 | **USER**: taskbar and launcher | Screen+ icon, not a generic one. Window groups with the `org.screenplus.ScreenPlus` launcher, which opens it. |
| U9 | Hide `screenplus-ui.so` (rename it), `screenplus show`, restore it | Clear error, service still runs (CLI-only machines) |
| U10 | Check HiDPI: set a 150% scale if the user agrees | Crisp text, clicks land on the right items |

## 5. Finish page thumbnails (the unfinished work)

The last commit ("trying to fix thumbnails of apps") stopped halfway. `kde/org.screenplus.capture` is a plasmoid that records a
page, and it ships, but nothing runs it: its header names `src/linux/preview.c`, which doesn't exist, and `CMakeLists.txt`, `service.c`
and `ui_thumbs.c` know nothing about it. Pages with no preview of their own have no picture in the Library.

**First prove it can work**, by hand, before writing any C. This is the main risk: `grabToImage` on a `WebEngineView` may give
black frames, especially on the offscreen platform.

```bash
mkdir -p ~/.local/share/plasma/plasmoids ~/sp-test/cap
cp -r src/linux/kde/org.screenplus.capture ~/.local/share/plasma/plasmoids/
QT_QPA_PLATFORM=offscreen timeout 60 plasmawindowed org.screenplus.capture "$HOME/sp-test/cap" "file://$F/page-plain/index.html" "" ""; ls ~/sp-test/cap | wc -l
```

| ID | Do | Pass |
|---|---|---|
| T1a | Run the above | 41 PNGs (`000`..`039` and `done.png`), 480x270, not black, frames differ. Check `plasmawindowed` passes the extra arguments through to `Qt.application.arguments`. |
| T1b | If black or nothing | In order, try: `QT_QUICK_BACKEND=software`, `QTWEBENGINE_CHROMIUM_FLAGS=--disable-gpu`, a hidden real window in place of offscreen. If none works, use one fallback that needs no capture and tell the user. |
| T1c | Write `src/linux/preview.c` | See below |
| T1d | Build, import `page-plain` and a page from `samples/`, open the Library | Page animates in the Library within about a minute. Restart: comes from the cache, no second recording. A page that never loads or hangs is killed at the deadline and keeps a plain tile. Nothing shows on screen. |
| T1e | Update docs | `CLAUDE.md` Linux section (it says recording isn't ported) and the `ui_thumbs.c` comments |

What `preview.c` must do, matching `src/app/preview_capture.c` (read `write_previews`, `preview_request` and the constants there):

- Install the applet to `~/.local/share/plasma/plasmoids/` next to the wallpaper plugin in `desktop_kde.c`.
- Queue one page at a time; run `plasmawindowed` with `nice`; kill it at `DEADLINE_MS` (45 s). The QML has no timeout of its own.
- Pass the shim and the script from `shim_url()`/`wpapi_properties_js()`, so Lively pages see their properties.
- Turn the frames into `cache/previews/<id>.gif` and `<id>.jpg` with ffmpeg, at the paths from `preview_paths()`. Follow `frame_change`: a page
  that doesn't move gets only the still.
- Teach `source_of()` and `anim_source()` in `ui_thumbs.c` to use those files, and redraw when a recording lands. Follow `src/ui/thumbs.c`.
- Add it to `CMakeLists.txt`. Hyprland has no plasmoid, so there it stays a still unless a WebKitGTK offscreen grab in `engine_gtk.c` is cheap to add.
- Make it reach the CLI if Windows has a command for it, and say so if it doesn't apply.

## 6. Hyprland

Run this only if Hyprland is installed. If not, you can try the engine on KDE, since KWin has layer-shell, but only with the user's
agreement: it needs `plasmashell` stopped, which takes their desktop away until `systemctl --user start plasma-plasmashell`.

| ID | Do | Pass |
|---|---|---|
| H1 | `screenplus start`, `status`, `pgrep -c screenplus-engine` | One engine, one layer-shell window per display (`hyprctl layers`) |
| H2 | Repeat K1, K3, K8 | Same results; `apply` reaches the engine through SIGUSR1 |
| H3 | Plug or unplug a display (or `hyprctl keyword monitor`) | Window added or removed |
| H4 | `kill -9` the engine 7 times, waiting for each restart | Backs off, gives up after five, `restart` revives it; with the engine binary missing, `status` shows the clear error |
| H5 | Start `show` and repeat U1, U3 | Same as KDE |

## 7. Suspects from reading the code

Not confirmed: nothing has been run. Confirm or clear each one, and fix what's real.

| ID | Where | Suspicion |
|---|---|---|
| S1 | `kde/org.screenplus.capture` | Unwired; see part 5 |
| S2 | `read_output_hz`, `desktop_kde.c` | Doesn't skip outputs with `enabled:false` or `connected:false`: they can sit at pos 0,0 and give the real display at 0,0 the wrong Hz, or use up the `SP_MAX_MONITORS` slots |
| S3 | same | If `kscreen-doctor -j` gives `id`/`currentModeId` as numbers, `json_str` returns `""` for both, `strcmp` matches, and the first mode wins. Output past 256 KB is cut and parses as nothing. |
| S4 | `kde_monitors` | Hz is only reread when the layout changes, so a refresh-rate change alone stays stale until restart |
| S5 | `kde_restore` | Forces `org.kde.image`, so a user who had another wallpaper plugin doesn't get it back |
| S6 | `sp_run_read`, `run.c` | Pipes aren't close-on-exec. If two threads run it at once, one child holds the other's pipe open, and the reader waits for the wrong process to end. Watch for stalls while thumbnails run during a monitor poll. |
| S7 | `sp_run_read` | No timeout: a hung `dbus-send`, `kscreen-doctor` or ffmpeg hangs the caller for good, including the service's main loop |
| S8 | `compat.c` | Every file call turns `\` into `/`: a real Linux file name with a backslash in it can't be opened |
| S9 | `install.sh` | The `.desktop` file has no `StartupWMClass`; `Exec` breaks on paths with `"`, `$` or `` ` ``. Spaces in `$HOME` should work: test it if cheap. |
| S10 | `CMakeLists.txt` | Version stays 1.9.0, behind the 1.10.2 Windows build; `linux.yml` passes the real one only on release |

## 8. Memory safety (if time allows; slow on the VM)

Build with `-DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS='-fsanitize=address,undefined -fno-omit-frame-pointer'` into `build-asan`, run
the service in the foreground (`screenplus --daemon`), and repeat C4, C6, C10 and U1. Fix every report.

## 9. Port the Store, Make wallpaper cooler and SHARP

The user wants all three on Linux, SHARP included, because the Linux version is experimental. Nothing is ported yet: this was
written with no Linux compiler to hand. Start once parts 1 to 5 pass, so a failure here is known to be new. Do them in this order.

### 9a. Bring in SHARP

`git merge --no-commit -X renormalize SHARP`. A plain merge conflicts in `CLAUDE.md`, `THIRD_PARTY_NOTICES.md`, `cli_server.c`,
`cli_main.c` and `engine.c`, partly because `cross-platform` holds CRLF copies of some files and `SHARP` holds LF. Keep both sides'
content, with LF endings (`CLAUDE.md` says the tree is LF). Then rebuild and repeat C2 to C4 before going on. `main` stays out: all it
adds is the Hello timeout, which `SHARP` also has, and GitLab CI.

**SHARP's licence is non-commercial:** never add its weights, an export or a download link, on Linux either. The user supplies
the `.onnx`. Keep the "SHARP — not included" note in `THIRD_PARTY_NOTICES.md`.

### 9b. Shared plumbing

- **HTTP:** `src/common/webfetch.c` is WinHTTP. Add `src/linux/http_linux.c` with the same `http_*` API (`webfetch.h`), for the service.
  It must stay plain libc, so `dlopen` libcurl, with the `curl` binary as the fallback (as `url_import.c` uses it). Prefer `dlopen`: the
  Store's speed comes from one keep-alive HTTP/2 session (`http_open_shared`). Send the same `USER_AGENT` as `webfetch.c`.
- **Messages:** `WM_SP_STORE` and `WM_SP_DEPTH` go to `g_app.shellWnd`, handled in `app.c:262`. Linux has no shell window, so give
  `service.c` a small queue that its main loop drains, then calls `ui_on_config_changed()`.
- **Threads:** `CreateThread` and friends become pthreads in `compat.h`/`compat.c`, or small `#ifdef`s where that's simpler.
- **Page templates:** `depth.c:474` reads `depth.html` and `splat.html` from Windows resources. Ship them as files beside the
  program: `resources/*.html` in the B2 folder, the CMake `install`, and `linux.yml`'s Package step.
- Remove from `ui_stubs.c` what becomes real, and un-hide the sections in `ui_pages.c` (`#ifndef _WIN32` blocks).

### 9c. Store

Build `store.c` and `store_sources.c` into the service and connect them to the window. `store_sources.c` has no direct Windows ties.

- `dhash` and the WebP check use WIC (`store.c:156`, `:313`). Replace them with ffmpeg: same 16:9 centre crop, scaled to 9x8 grey, raw on stdout.
  Hashes may differ a little from Windows; `store-blocked.txt` isn't shared between machines, so that's fine.
- `url_import_start_titled` already exists on Linux. Open pages with `xdg-open`.
- Previews in `cache/store` must reach `ui_thumbs.c`, which decodes them with ffmpeg.

| ID | Do | Pass |
|---|---|---|
| ST1 | Open the Store | Cards from all six sites fill in without freezing the window. Note which sites fail. A site that answers differently to curl than to WinHTTP (403, a challenge page) gets a note, not a workaround. |
| ST2 | Search, each filter, scroll for more, retry with the network off | Same as Windows; clear error offline, recovers on retry |
| ST3 | Download a video and a picture | Progress shows, entry lands in the Library and applies. A site with no direct file opens its page in the browser. |
| ST4 | Block a card, restart, then Reset settings | Stays hidden with its near copies, then comes back |
| ST5 | Quit during a download; check `cache/store` after a restart | No crash, no stuck download, no partial file in the Library |

### 9d. Make wallpaper cooler

The child is `depth_infer.c`. It needs ONNX Runtime and libav, so make it its own binary, `screenplus-depth`, with the same arguments
and exit codes as `--depth`. The service stays plain libc and starts it where `depth.c:148` uses `CreateProcessW`, at low priority.

- **ONNX Runtime:** download the official Linux CPU build (`onnxruntime-linux-x64-<ver>.tgz` from Microsoft's GitHub releases) on request into
  `depth/ort/`, pinned by URL, size and SHA-256 as `ORT_URL` is. Unpack it with the system `tar`. `dlopen` `libonnxruntime.so`. Take the headers at
  build time, as the Windows CMake does. Match the version to Windows (1.24.4) if that release exists.
- **GPU:** DirectML is Windows-only, so guard it. Start CPU-only, with `backend.json` recording that. If the machine has an NVIDIA card,
  the CUDA build is a follow-up to raise with the user, not part of this.
- **Replace** DXGI (GPU memory for `depth_estimate`; read `/sys/class/drm/*/device/mem_info_vram_total`, else none), WIC decode and encode (use libav:
  PNG, MJPEG), and EXIF reading for SHARP's lens (parse the JPEG APP1 block). `tar.exe` becomes `tar`.
- **Codec check:** exit code 5 means the page can't play the video. On Linux it depends on the distro's QtWebEngine build, which may lack H.264.
- **File dialogs:** `depth_choose_model` and `prop_choose_file` need `ui_sdl.c`'s dialogs (`SDL_ShowOpenFileDialog`).

**Main risk: `file://`.** Windows serves pages from a virtual https host. On Linux the page, `depth*.png` and `depth*.bin` are `file://`, which can
taint WebGL textures and block `fetch`. Prove this first with `samples/abqqpcp4.hv3` (Rainy Window). Fix it in `WebView.qml`'s settings and,
on Hyprland, WebKit's `allow-file-access-from-file-urls`; if that isn't enough, use a custom scheme.

| ID | Do | Pass |
|---|---|---|
| D1 | Customise, Make wallpaper cooler on `still.png` | Downloads ONNX Runtime and the model once, shows progress, can be cancelled; the checksum is enforced |
| D2 | When it finishes | The picture has depth and follows the pointer on KDE (through `pointer.js`). No WebGL errors in the journal. Nothing draws at rest (`top`). |
| D3 | Each model in the list | Estimates look sane for CPU; switching makes the page, and going back is instant; "can't run" shows for what's too big |
| D4 | Depth on `video.mp4` | Frames made, plays with the depth moving and with sound only if Sound is on. Allow for CPU speed: time it and note it. |
| D5 | Depth strength and auto move, with `props`; pause and resume; `depth_revert` | Controls work, the page survives pause (`webglcontextlost` path), revert gives the plain wallpaper |
| D6 | `interactive_images` on, then off | Pictures convert, `WPFLAG_NOCOOL` ones are skipped, nothing downloads unless asked |
| D7 | Rainy Window: its Sound dropdown, and "Choose a file…" | Rain loops; the chosen picture gets its `depth/<picture>.png` |
| D8 | Kill `screenplus-depth` during a job; delete the `ort` folder and retry | Retries with `--crashed`, then a clear message; downloads again |
| D9 | On Hyprland, repeat D2 and D5 | Same, in WebKitGTK. `requestVideoFrameCallback` may be missing there: the page's rAF fallback must take over. |

### 9e. SHARP

Ask the user for their own `.onnx` (and `.onnx.data`). Without one, test SH1 only and mark the rest needs user. Expect the CPU to be slow and
RAM use to be high: check `free -g` first, and warn the user if the VM is short. Stop if the machine starts swapping hard.

| ID | Do | Pass |
|---|---|---|
| SH1 | `screenplus sharp`, `sharp /no/file.onnx`, `sharp` on a `.onnx` with its `.data` missing | The three messages from `cmd_sharp`; add `sharp` to `k_help` in `cli.c` |
| SH2 | `sharp <file.onnx>`, or pick it in Customise | Model is kept in `depth/models/sharp/` by hard link or copy, `path.txt` gone, works after the original is moved |
| SH3 | Make a picture cooler with SHARP | `depth-sharp.bin` written ("SPGS"); time and peak RAM noted. Pictures only: a video is refused. |
| SH4 | Look at `splat.html` on KDE and Hyprland | Program links (a shader local named like a varying fails quietly), no black gaps, moves with the pointer, no drawing at rest, Frame rate setting works |

### 9f. Finish

- `linux.yml`: add the libav dev packages, the new binary and the templates to the build and the package. `install.sh`: list the new needs.
- Update `CLAUDE.md`'s Linux section ("Not ported" and the map), the `ui_stubs.c` header and `k_help`. Check U2 again.
- **Windows must still build.** You can't test that here, so leave Windows code paths alone where you can, and tell the user to run `build.cmd` once
  back on Windows before any release.

## Cleanup (required)

```bash
screenplus startup off; screenplus stop
qdbus6 org.kde.plasmashell /PlasmaShell org.kde.PlasmaShell.evaluateScript 'print(desktops()[0].wallpaperPlugin)'
```

- The wallpaper must be what it was before. If not, restore from `~/sp-appletsrc.backup` (stop plasmashell, copy it back, start plasmashell).
- Remove `~/sp-test` and `~/sp-appletsrc.backup`. Ask before removing the install in `~/.local/share/screenplus-app`.
- If you stopped plasmashell, make sure it is running again.

## Results

Fill in as you go. Status: pass, fail, fixed, skipped (say why), or needs user.

**Machine:** Arch VM, Plasma 6 Wayland, VirtualBox with AMD-V (8 CPUs, 12 GB), one virtual display, no NVIDIA, no Hyprland.
**Commit tested:** 9b6f29d + SHARP merged, uncommitted.

| ID | Status | Notes (cause and fix, file:line) |
|---|---|---|
| B1-B3 | fixed | install.sh wasn't executable in git; 2 harmless store.c warnings left |
| C1-C17 | pass | `logs` before any log printed tail's error: fixed in cli.c |
| K1, K3 (CPU not measured), K8 not measured | pass | all six types show and move; GIF after image works |
| K5, S2, S3 | fixed | Hz right; disabled outputs now skipped; ids are strings |
| K2, K4, K6, K9, K10, K11 | needs user | sound, covering windows, pointer, 2 displays, sign-in, locked widgets |
| U1-U10 | needs user | window opens and draws, thumbnails and animations seen; no clicks possible from here |
| T1a-T1e | fixed | preview.c written; recorded GIF + JPG from a live page; ends on done.png |
| 9a | done | merge left uncommitted; conflicts kept both sides |
| ST1-ST3 | pass | all six sites list, search works, MotionBGS download lands; MoeWalls opens its page, by design |
| ST2 offline, ST4, ST5 | not tested | |
| D1, D2 | pass | 1.1 s on CPU; WebGL parallax seen on the desktop from file:// |
| D4 | pass (data only) | child ran directly: 180 frames, 126 s on the CPU; playback on the desktop not looked at |
| D3, D5-D9 | not tested | need the window, or Hyprland |
| SH1 | pass | exit 0 with the message, as on Windows |
| SH2-SH4 | needs user | their own .onnx |
| H1-H5 | skipped | Hyprland isn't installed; the engine only compiles |
| S5, S7-S10 | open | not changed |
| S6 | fixed | pipes are close-on-exec |

Second round (settings, each tested alone; pause measured by frames the page draws):

| Setting | Status | Notes |
|---|---|---|
| start_with_linux | pass, sign-in timing not measured | entries appear and go; the unit is in plasma-core.target |
| launch_to_tray | pass | window opens at --startup only when off |
| pause_on_fullscreen, pause_when_covered, paused | fixed | pages never froze: Qt won't freeze a visible page. Now 0 frames, and back |
| pause_on_battery | pass | through a stand-in power folder; the VM has no battery |
| suspend_on_lock | pass | real lock and unlock |
| audio | pass (value) | Muted follows; sound not heard |
| auto_recover | fixed | KDE: applies again when plasmashell returns; the engine stays down when off (untested, no Hyprland) |
| html_mouse | pass (value) | Mouse follows; real pointer not tried |
| html_audio | fixed | no spectrum, no capture when off; capture no longer outlives the service |
| web_fps | fixed | measured 30, 60, and uncapped at the display's 60 |
| theme | pass | dark, light, system seen in the window |
| display_mode span | fixed | checked with the values a second display gets; two real displays not tried |
| interactive_images | pass | |
| url_import | not tested | hides the link button; needs clicks |
| auto_update, auto_update_samples | partial | toggle and check run; GitHub answers 404 |

Music TV and Living Room: fixed by webhost.c (they use /js/... and ES modules, which file:// can't serve).

Added beyond the list: feed.c (audio, system, track), stream links, lock pause, colour dialog, Identify, `screenplus store`.
Auto update reached GitHub and got 404 (account hidden), so install wasn't exercised. Sample updates: not exercised.

Finish by telling the user, briefly: what was fixed, what still fails, what was skipped, what they need to check, and that nothing
was committed unless they said so. Then update `CLAUDE.md` if the facts in it changed.
