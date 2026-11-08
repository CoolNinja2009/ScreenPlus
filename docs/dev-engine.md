# Screen+ dev engine (design spec, draft)

Status: draft, nothing built. Branch `dev-engine`.

Two parts:

1. **App wallpapers**: run exported Godot and Unity games (`.exe`) as wallpapers.
2. **Screen+ projects (`.spx`)**: wallpapers made in **Screen+ Studio**, a fork of the Godot editor, run by a
   Screen+ build of the Godot runtime, with a `ScreenPlus` API for audio, camera, microphone, depth, upscaling.

We do not write an engine. Godot is MIT: we fork, rebrand and ship it. Unity is closed, so it is supported only as
part 1.

## 1. App renderer

New renderer `src/renderer/app/app_renderer.c` behind the existing vtable (`renderer.h`).

- Wallpaper type `app`: a folder with an exe, or a `.exe` dropped on the library.
- Start: `CreateProcess` suspended, inside a per-wallpaper job object (killed with the engine). Wait for its top-level
  window (process id match, up to 10 s), then strip the caption/border styles, `SetParent` it into the display's
  desktop host window and size it to the display.
- Godot exes get `--windowed --resolution WxH`; Unity exes `-screen-fullscreen 0 -screen-width W -screen-height H
  -popupwindow`. Unknown exes: none.
- Input: the desktop host already gets mouse input (`vt->input`); forward it to the child with `PostMessage`.
- Pause: `NtSuspendProcess`/`NtResumeProcess` on the job's processes (same states as other renderers: fullscreen,
  occlusion, battery, lock). Mute: the process's audio session (`ISimpleAudioVolume`).
- Crash: restart with the supervisor's backoff; after 3 crashes in a minute, show the error card.
- Multi-display: one process per display (`app.spanned` setting later).
- Linux: X11 `XReparentWindow`; Wayland can't, so app wallpapers report unsupported there.

**Trust**: an app wallpaper is a program. First run of each exe shows "This wallpaper is a program and can do anything
a program can. Run it?" (remembered per file hash). Workshop/store items never auto-run exes.

## 2. .spx format

A zip (store or deflate), extension `.spx`:

```
manifest.json      required
project.pck        required, Godot export (resources + scripts)
preview.jpg        required, 16:9, ≤ 1920 wide
preview.webm       optional, short loop
LICENSE            optional
```

`manifest.json`:

```json
{
  "spx": 1,
  "id": "com.author.name",
  "title": "Name",
  "author": "Author",
  "version": "1.0.0",
  "runtime": "4.5-sp1",
  "permissions": ["audio", "microphone", "camera", "depth", "network"],
  "properties": {
    "speed": { "type": "slider", "label": "Speed", "min": 0, "max": 2, "step": 0.1, "value": 1 },
    "tint":  { "type": "color",  "label": "Tint", "value": "#ffffff" }
  }
}
```

- `runtime`: the Screen+ runtime build the pack was exported for. Screen+ keeps every runtime it has downloaded and
  picks the exact match; packs are not loaded by a different minor version.
- `properties`: same types as Lively properties (`slider`, `color`, `checkbox`, `dropdown`, `textbox`, `file`), so
  Customise draws them with existing code.
- `permissions`: shown before first run; anything not listed is refused by the API (see Trust).

## 3. Runtime

- A Godot export template built from our fork with the `ScreenPlus` module compiled in (a module, not a GDExtension
  DLL, so packs can't swap it). Editor-only code stripped. Target ~30–40 MB.
- Downloaded on first `.spx` use into `%LOCALAPPDATA%\ScreenPlus\runtime\<version>\` with a SHA-256 check, like ONNX
  Runtime. Not in the installer.
- Started by the engine: `ScreenPlusRuntime.exe --main-pack <dir>\project.pck --sp-pipe <name> --sp-display <n>
  --rendering-driver d3d12|vulkan`. Hosted by the app renderer's code (same reparenting, job, pause, crash rules).
- Pause is cooperative first (pipe `pause` → the runtime stops processing and drawing), then suspend as a fallback
  after 2 s.

## 4. Pipe protocol

Named pipe `\\.\pipe\ScreenPlus.spx.<pid>.<display>` (Linux: Unix socket in `$XDG_RUNTIME_DIR`). Messages: 4-byte
little-endian length, then one JSON object `{"t": "<type>", ...}`. Bulk data (audio, frames) goes through shared
memory, not the pipe.

Engine → runtime:

| `t` | Fields | Meaning |
|---|---|---|
| `hello` | `version`, `display` {w,h,hz,scale}, `props` | first message; runtime answers `ready` |
| `pause` / `resume` | `reason` | stop/start processing |
| `props` | `{key: value}` | Customise changed a property |
| `mute` | `on` | |
| `quality` | `scale`, `upscaler`, `fps_cap` | see §6 |
| `mouse` | `x`, `y`, `buttons` | desktop cursor (also when the window has no input) |
| `stop` | | quit within 2 s or be killed |

Runtime → engine:

| `t` | Fields | Meaning |
|---|---|---|
| `ready` | | first frame drawn; engine shows the window |
| `log` | `level`, `msg` | goes to the engine log |
| `error` | `msg` | shown on the error card |
| `request` | `perm` | API asked for a permission not yet granted |

Shared memory (created by the engine, names passed in `hello`):

- `audio`: system loopback, 64-band spectrum + RMS, written ~60 Hz by the engine's existing audio capture.
- `depth`: latest depth map for a requested source (§5).

## 5. ScreenPlus API (GDScript singleton `ScreenPlus`)

```gdscript
ScreenPlus.paused            # bool
ScreenPlus.property(name)    # current value; signal property_changed(name, value)
ScreenPlus.display           # {size, refresh, scale, index}
ScreenPlus.cursor            # desktop cursor position in window coords

# audio (perm "audio"): system sound
ScreenPlus.audio_spectrum()  # PackedFloat32Array(64), 0..1
ScreenPlus.audio_level()     # float

# microphone (perm "microphone")
ScreenPlus.mic_start(); ScreenPlus.mic_stop()
ScreenPlus.mic_level(); ScreenPlus.mic_spectrum()

# camera (perm "camera")
ScreenPlus.cameras()                   # [{id, name, sizes}]
ScreenPlus.camera_open(id, size)       # returns a Texture2D updated each frame
ScreenPlus.camera_close(id)

# depth (perm "depth"): Screen+'s ONNX Runtime + Depth Anything models, run in the engine
ScreenPlus.depth_from(texture, model := "da2-small")  # async; returns Texture2D (R32F), signal depth_ready
ScreenPlus.depth_live(camera_id)                      # continuous depth of a camera feed

# AI upscale (perm none): offline, for assets at load time
ScreenPlus.upscale(texture, factor := 2)               # async; model chosen by Screen+
```

- Camera and microphone are captured by the engine (Media Foundation / WASAPI; PipeWire on Linux), not by Godot,
  so the permission check sits outside the pack and the LED/privacy indicators behave.
- Depth and upscale reuse `depth_infer.c`'s model loading; one ONNX session per model, shared across displays.
- Network: Godot's HTTP is disabled in the runtime unless `network` is granted.

## 6. Rendering quality

Exposed in Customise per wallpaper, sent as `quality`:

- **Render scale** 50–100 %, **FPS cap** (default: display refresh, 30 on battery).
- **Upscaler**: `off`, `fsr1`, `fsr2` (Godot built-in, Forward+), `dlss` (NVIDIA RTX only), `xess` later.
- DLSS: NVIDIA Streamline SDK linked into the runtime (Windows, D3D12). The SDK is free but closed: its DLLs ship in
  the runtime download with NVIDIA's notice in `THIRD_PARTY_NOTICES.md`. Shown only when `sl.dlss` reports support.
- Frame generation: out of scope (latency, and wallpapers rarely need it).

## 7. Screen+ Studio

The Godot editor, from the same fork:

- Branding: name, icon, splash, about box crediting Godot (MIT notice kept).
- New project → "Screen+ wallpaper" template: a scene, a `manifest.json` editor panel (title, permissions,
  properties), preview capture button.
- `ScreenPlus` singleton available in the editor with fake data (spectrum noise, test camera) so scripts run in play
  mode.
- **Export .spx**: an export platform that builds the `.pck` with our template and zips it with the manifest and
  preview.
- **Preview on desktop**: sends the pack to the running Screen+ (`ScreenPlus.exe --preview-spx <file>`) to show it on
  the real wallpaper.

Download separately from Screen+ (it is a full editor, ~100 MB); linked from the app's "Make your own" button.

## 8. Trust

GDScript is not sandboxed: a pack can read files. Mitigations, in order of effort:

1. Runtime build with `FileAccess`/`DirAccess` limited to the pack and a per-wallpaper `user://` folder, `OS.execute`,
   `OS.shell_open` and native GDExtension loading removed. Scripts only reach the system through `ScreenPlus`.
2. Runtime process runs at low integrity (Windows) with a restricted token; no network unless granted.
3. Store packs are scanned and signed; sideloaded packs show the permission prompt.

## 9. Plan

| Phase | Work | Done when |
|---|---|---|
| 1 | App renderer (Godot/Unity exes), trust prompt | a Godot and a Unity export run, pause, mute, survive a crash |
| 2 | Fork + runtime build, `.spx` loader, pipe (`hello`/`pause`/`props`/`stop`), audio | a sample .spx reacts to music and Customise |
| 3 | Camera, microphone, depth, upscale APIs | depth-from-camera sample works on Windows |
| 4 | Quality: render scale, FPS cap, FSR 1/2, then DLSS | upscaler switch in Customise |
| 5 | Studio: branding, template, export, desktop preview | make → export → apply without leaving Studio |
| 6 | Sandbox (§8.1–2), Linux runtime | sample pack can't read outside `user://` |

## 10. Parity and rules

- **CLI**: `screenplus import <file.spx|game.exe>` (existing import, new types), `screenplus spx info <file>`,
  `screenplus runtime list|install|remove`, `screenplus trust list|revoke`; quality keys in `k_settings`
  (`spx_fps_cap`, `spx_upscaler`, `spx_render_scale`).
- **Linux**: KDE plays wallpapers inside plasmashell (QML) and Hyprland uses layer-shell windows, so neither can adopt
  another program's window. Plan: the runtime draws offscreen and hands frames over dmabuf (Hyprland: engine_gtk.c
  imports it; KDE: a QML item fed from the service). App wallpapers (other people's exes) stay unsupported on Linux.
  The `.spx` runtime ships for Linux in phase 6.
- **Dependencies**: Godot (MIT) and Streamline (NVIDIA licence) are the deliberate exceptions to "no new
  third-party dependencies". Both live only in the separately downloaded runtime, never in ScreenPlus.exe.
- **No work while paused**: the runtime must draw nothing and process nothing on `pause`; measure by frames drawn,
  as for pages.
- Existing pieces reused: `audio_visualizer.c` (spectrum), `depth_infer.c` (models), `wallpaper_api.c` (property
  types for Customise), `supervisor.c`'s backoff, `desktop_host.c`'s windows.

## Open questions

- Which Godot version to pin (4.5 stable at fork time), and how often to rebase.
- Do we accept plain Godot `.pck` files without a manifest? (Proposed: no.)
- Store hosting for .spx packs (GitLab releases, or the existing store backend).
- Name of the runtime exe and of Studio.
