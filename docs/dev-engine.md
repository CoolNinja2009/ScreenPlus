# Screen- : the Screen+ dev engine (design spec, draft)

Status: draft; first code in `screen-minus/`. Branch `dev-engine`.

**Screen-** ("screen minus": Screen+ without the wallpaper projector) is the engine and editor people make Screen+
wallpapers with. Three parts:

1. **App wallpapers**: Screen+ runs exported Godot and Unity games (`.exe`) as wallpapers.
2. **Screen- projects (`.spx`)**: made in the Screen- editor, a fork of Godot 4.7, run by the Screen- runtime, with a
   very open `ScreenPlus` API (§5).
3. **HTML + JS export**: the same project exported as a web wallpaper (§7).

We do not write an engine. Godot is MIT: we fork, rebrand and ship it. Unity is closed, so it is supported only as
part 1. Pinned: Godot `4.7.2-stable` (`screen-minus/godot.pin`).

## 1. App renderer

New renderer `src/renderer/app/app_renderer.c` behind the existing vtable (`renderer.h`).

- Wallpaper type `app`: a folder with an exe, or a `.exe` dropped on the library.
- Start: `CreateProcess` suspended, inside a per-wallpaper job object (killed with the engine). Wait for its top-level
  window (process id match, up to 10 s), then strip the caption/border styles, `SetParent` it into the display's
  desktop host window and size it to the display.
- Godot exes get `--windowed --resolution WxH`; Unity exes `-screen-fullscreen 0 -screen-width W -screen-height H
  -popupwindow`. Unknown exes: none.
- Input: forwarded from `vt->input` with `PostMessage`.
- Pause: suspend the job's processes (same states as other renderers). Mute: the process's audio session.
- Crash: restart with the supervisor's backoff; after 3 crashes in a minute, show the error card.
- An app wallpaper is someone else's program and can't be scanned like a .spx (§8): first run asks, per file hash, and
  says plainly that it gets full access. The Store never offers exes.

## 2. .spx format

A zip, extension `.spx`:

```
manifest.json      required
project.pck        Godot export (runtime "screen-")        } one of the two
web/index.html     web build or a hand-written page        } (both allowed: §7)
preview.jpg        required, 16:9, ≤ 1920 wide
preview.webm       optional
models/*.onnx      optional, for the ai capability
native/<os>/*.dll|.so   optional, only with the native capability
signature.json     added on export (§8.4)
```

`manifest.json` (schema: `screen-minus/spx/manifest.schema.json`):

```json
{
  "spx": 1,
  "id": "com.author.name",
  "title": "Name",
  "author": "Author",
  "version": "1.0.0",
  "runtime": "screen-4.7.2-1",
  "entry": "project.pck",
  "capabilities": {
    "audio": true,
    "camera": true,
    "net.client": ["api.open-meteo.com", "*.example.org"],
    "fs.read": ["pictures"]
  },
  "properties": {
    "speed": { "type": "slider", "label": "Speed", "min": 0, "max": 2, "step": 0.1, "value": 1 }
  }
}
```

- `runtime`: `screen-<godot>-<n>` for packs, `web` for HTML. Screen+ keeps every runtime it has downloaded and picks
  the exact match.
- `properties`: Lively property types, so Customise draws them with existing code.
- `capabilities`: everything a pack may do beyond drawing. Undeclared means refused (§5, §8).

## 3. Runtime

- A Godot export template built from the fork with the `screenplus` module compiled in (`screen-minus/modules/`;
  a module, not a GDExtension, so packs can't swap it). Target ~30–40 MB.
- Downloaded on first use into `%LOCALAPPDATA%\ScreenPlus\runtime\<version>\`, SHA-256 and signature checked.
- Started by the engine: `screen-runtime.exe --main-pack <dir>\project.pck --rendering-driver d3d12|vulkan --
  --sp-pipe <name> --sp-display <n>` (Screen+ arguments after `--`, read with `get_cmdline_user_args`). Hosted by the
  app renderer's code, inside the sandbox (§8.3).
- Pause: pipe `pause` → tree paused, render loop off, low-processor mode with 100 ms sleeps; suspend after 2 s as
  a fallback. Nothing is drawn or processed while paused.

## 4. Protocol: the Screen+ bus

One protocol everywhere: **JSON-RPC 2.0**, framed by a 4-byte little-endian length.

| Who talks | Transport |
|---|---|
| Runtime ↔ engine | named pipe `\\.\pipe\ScreenPlus.spx.<pid>.<display>` (Linux: Unix socket in `$XDG_RUNTIME_DIR`) |
| Web build ↔ engine | `window.screenplus` in the page (WebView2 host object / Linux feed WebSocket) |
| Outside programs ↔ engine | WebSocket `ws://127.0.0.1:<port>/bus` with a token (`cache\bus.txt`), off by default (`bus_external`) |

Bulk data (audio spectrum, camera frames, depth maps, screen capture) goes through shared memory / shared GPU
textures named in the call's result, never through JSON.

Engine → runtime notifications: `sp.hello` {version, display, props, granted}, `sp.pause`, `sp.resume`, `sp.props`,
`sp.mute`, `sp.quality`, `sp.mouse`, `sp.stop`, `sp.event` {name, data} (anything an API pushes).

Runtime → engine: requests `<namespace>.<method>` (§5), notifications `sp.ready`, `sp.log`, `sp.error`.

Errors: JSON-RPC codes; `-32001` capability not granted, `-32002` blocked by policy, `-32003` user said no.

Because the bus is the API, a new capability is added in the engine only: packs and the runtime need no rebuild,
and every client (GDScript, JS, an outside Python script) gets it at once.

## 5. API: open by design

Everything is reachable; nothing is reachable silently. A pack declares what it uses, the user sees it before the
first run, and the broker enforces it at every call. In GDScript:

```gdscript
var r = await ScreenPlus.call_api("net.http", {"url": "https://api.open-meteo.com/...", "method": "GET"})
ScreenPlus.event.connect(func(name, data): ...)   # pushed events
ScreenPlus.property("speed"); ScreenPlus.property_changed.connect(...)
ScreenPlus.paused; ScreenPlus.display
```

Namespaces (each is a capability; the tier decides how loudly it is asked for):

| Namespace | What it gives | Tier |
|---|---|---|
| `sp` | lifecycle, properties, display, cursor, theme/accent colour, time of day, sun position | open |
| `audio` | system audio spectrum, level, beat, now playing (title, art, position), media keys | standard |
| `mic` | microphone level, spectrum, raw PCM ring buffer | standard (asks) |
| `camera` | list, open (shared texture), formats | standard (asks) |
| `ai` | depth (Depth Anything 2/3), segmentation, upscale, and **any ONNX model shipped in `models/`**, CPU/DirectML | standard |
| `gpu` | adapter info; Godot's `RenderingDevice` compute is always available inside the pack | open |
| `quality` | render scale, FPS cap, FSR 1/2, DLSS, XeSS | open |
| `system` | CPU/GPU/RAM/temps, battery, network up/down, idle time, notifications | standard |
| `net.client` | HTTP(S), WebSocket, TCP, UDP, to the declared hosts (`*` allowed, shown as "anywhere") | standard / powerful for `*` |
| `net.server` | listen on a local port (HTTP, WebSocket, TCP, UDP), LAN only unless declared | powerful |
| `osc`, `midi`, `serial`, `hid`, `gamepad`, `bluetooth.le` | devices and creative protocols | powerful |
| `fs.read`, `fs.write` | named folders (pictures, music, videos, documents, downloads) or a folder the user picks; never the whole disk | powerful |
| `desktop` | windows list, foreground app, desktop icon positions, other wallpapers on the bus | powerful |
| `screen` | screen capture of a display (shared texture) | powerful |
| `clipboard`, `location` | | powerful |
| `bus` | talk to other wallpapers and to outside programs on the bus | powerful |
| `process` | start a program the user picked once (path pinned in the grant) | unrestricted |
| `native` | load the pack's own DLL/.so as a GDExtension | unrestricted |

Tiers: **open** no prompt; **standard** listed on the install card; **powerful** its own line with a warning, granted
per capability; **unrestricted** typed confirmation, not available to Store packs unless the author is verified
(§8.5). Grants are revocable in Customise and `screenplus spx grants`.

## 6. Rendering quality

Customise per wallpaper, sent as `sp.quality` (the pack can also ask through `quality.*`):

- Render scale 50–100 %, FPS cap (display refresh; 30 on battery).
- Upscaler: `off`, `fsr1`, `fsr2` (Godot built-in), `dlss` (Streamline, Windows D3D12, RTX), `xess` later.
- Streamline's DLLs ship in the runtime download with NVIDIA's notice. DLSS shows only when `sl.dlss` reports support.

## 7. HTML + JS export

- **From a Godot project**: Godot's web export (wasm, Compatibility renderer, WebGL2) plus a Screen- shell page, in
  `web/`. The `ScreenPlus` singleton's web build sends the same JSON-RPC through `JavaScriptBridge` to
  `window.screenplus`, so the same scripts run.
- **Hand-written**: a pack with `"runtime": "web"` and `web/index.html` is a normal page using
  `window.screenplus.call(method, params)` and `screenplus.on(event, fn)`.
- Runs in Screen+'s existing HTML renderer (WebView2 / Qt WebEngine / WebKitGTK). Exported with a
  `LivelyInfo.json` too, so it also works in Lively and a browser (without the bus: the shim falls back to Lively's
  audio API where it can).
- The virtual host serves `web/` with COOP/COEP (threads need cross-origin isolation) and a CSP built from the
  manifest: `connect-src` = `net.client` hosts, no remote scripts, no `eval` unless `web.eval` is declared.
- Limits: no FSR 2/DLSS (WebGL2), no `native`/`process`; performance below the native runtime. WebGPU later.

## 8. Security

Static checks catch mistakes and obvious abuse; they can't prove a pack has no backdoor (GDScript and JS are Turing
complete and can hide intent). So the guarantee is the **sandbox + broker**: a pack can only do what its granted
capabilities allow, whatever its code tries. The scanner and signatures decide what the user is asked and what the
Store accepts.

### 8.1 Scanner (`screenplus spx scan <file>`, run on every import, export and Store upload)

1. **Container**: zip only; no absolute paths, `..`, symlinks or duplicate names; total size ≤ 2 GB, ratio ≤ 100:1
   per entry (zip bombs); only listed folders; file types by magic, not extension.
2. **Manifest**: schema-valid; id/version sane; every capability known.
3. **Binaries**: any PE/ELF/Mach-O/wasm outside `native/` (or `web/` for the Godot wasm, which must hash-match the
   pinned Screen- web template) → block. Files in `native/` require `native`, are listed with hashes.
4. **Pack contents**: open `project.pck` (Godot's format, encrypted packs refused), list every resource; refuse
   C# assemblies (no Mono runtime), `.gdextension` files, resources embedding scripts in binary `.res` without source.
5. **Scripts** (GDScript source/tokens, JS): build the call graph and map API use to capabilities. Uses beyond the
   declared capabilities → block. Flags (warn, block in Store): `Expression`, `str_to_var`/`bytes_to_var` with
   objects, `load()`/`ResourceLoader` of paths built at runtime or from `user://`/network, `ClassDB.instantiate`,
   `call`/`callv`/`set_script` with non-constant names, JS `eval`/`Function`/remote `import()`/`WebAssembly`
   from network bytes; strings with high entropy or long base64/hex blobs; network hosts not in the manifest; timers
   or date checks gating network or file code (time bombs).
6. **Known bad**: hash list and rules shipped and updated with Screen+ (signed rule file).
7. **Report**: pass / warn / block with each finding and the exact file and line, shown on the install card;
   `--json` for CI and the Store.

### 8.2 Broker

Every capability call goes to the engine over the bus; the engine checks the grant, the declared hosts/folders and
rate limits, then does the work itself (camera, mic, files, sockets, capture) and hands back data or a shared
handle. The pack never touches the device or the OS directly. Each call is written to the wallpaper's audit log
(`logs\spx\<id>.log`, viewable in Customise: "what did this wallpaper do").

### 8.3 Sandbox

- Windows: runtime in an **AppContainer** with no capabilities (no network, no user files, no other processes),
  low integrity, job object limits (memory 2 GB default, CPU, no child processes, no UI access beyond its window).
  The pack's folder and its `user://` are the only files it can open. `native` packs run in the same container.
- Linux: bubblewrap namespaces + Landlock + seccomp, same rules.
- Web exports: WebView2 page with the CSP above, no host objects except `screenplus`.
- `process` grants start the program from the engine, not the sandbox.

### 8.4 Signing

- Screen- creates an Ed25519 author key on first export (kept in the OS keystore). `signature.json` signs the
  manifest and the SHA-256 of every file.
- Screen+ rejects a pack whose files don't match; an update to an installed id must be signed by the same key (key
  rotation by a statement signed with the old key).
- Store packs are countersigned by the Store after review.

### 8.5 Store review

- Scanner must pass. Powerful packs get a human review; unrestricted packs only from verified authors (identity
  confirmed), with source required.
- Packs can be revoked: the signed rule file carries revoked ids/hashes, and Screen+ stops them at the next check.

## 9. Screen- editor

The Godot editor from the same fork:

- Branding: name Screen-, icon, splash, about box crediting Godot (MIT notice kept).
- New project → "Screen+ wallpaper" template; a manifest panel (title, capabilities with their tiers, properties),
  preview capture.
- The `ScreenPlus` singleton in the editor with fake data (spectrum noise, test camera) so play mode works.
- Export: **.spx (native)**, **.spx (web)**, **HTML folder**; each runs the scanner first and signs.
- Preview on desktop: `ScreenPlus.exe --preview-spx <file>`.

## 10. Parity and rules

- **CLI**: `screenplus import <file.spx|game.exe>`, `screenplus spx scan|info|grants|revoke`,
  `screenplus runtime list|install|remove`, `screenplus bus on|off`; settings `spx_fps_cap`, `spx_upscaler`,
  `spx_render_scale`, `bus_external`.
- **Linux**: KDE plays inside plasmashell and Hyprland uses layer-shell, so neither adopts another program's window.
  The runtime draws offscreen and hands frames over dmabuf (engine_gtk.c; a QML item on KDE). App wallpapers stay
  Windows-only; web .spx works on both desktops from the start.
- **Dependencies**: Godot (MIT) and Streamline are the deliberate exceptions to "no new dependencies", and live
  only in the separately downloaded runtime and editor.
- Reused: `audio_visualizer.c`, `depth_infer.c`, `wallpaper_api.c` (properties, JS shim), `supervisor.c`'s backoff,
  `desktop_host.c`.

## 11. Plan

| Phase | Work | Done when |
|---|---|---|
| 1 | `screenplus` Godot module (bus client, singleton, pause), build in CI | a template build runs a sample pack against a test pipe |
| 2 | Engine: .spx import, scanner v1 (§8.1 1–4), runtime host, bus server (`sp.*`, `audio`) | sample .spx reacts to music and Customise |
| 3 | Broker + sandbox (§8.2–8.3), signing | sample can't read outside `user://`; tampered pack refused |
| 4 | Capabilities: camera, mic, ai, net, fs, then the powerful set | each has a sample |
| 5 | HTML + JS export, web runtime | same sample runs in WebView2 and Lively |
| 6 | Quality: FSR, DLSS | upscaler switch in Customise |
| 7 | Screen- editor branding, template, export, scanner v2 (scripts) | make → export → apply from the editor |
| 8 | App wallpapers (Godot/Unity exes) | a Godot and a Unity export run, pause, mute |
| 9 | Linux runtime (dmabuf) | sample .spx on Hyprland and KDE |

## Open questions

- Store hosting for .spx (GitLab releases, or the existing store backend).
- Do we accept plain Godot `.pck` without a manifest? (Proposed: no.)
- Rebase cadence onto new Godot releases.
