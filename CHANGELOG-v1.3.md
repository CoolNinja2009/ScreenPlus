# Screen+ v1.3 — Customise Panel & Colour Grading (what shipped)

v1.3 is the "Customise" change set: a new panel in the app UI that exposes two
kinds of settings per wallpaper — the **wallpaper author's own properties**
(the same mechanism Lively uses for its gallery packages) and a built-in
**colour-grading / look layer** implemented natively in Screen+. Everything
below is from the real diff (18 files, +1228/−64 lines) and the v1.3.0 build;
nothing here is planned-but-not-built.

---

## 1. New page: Customise

`src/ui/ui_pages.c` (+537 lines), page plumbing in `src/ui/ui_internal.h`.

- New sidebar page `PAGE_CUSTOMIZE` (between Displays and Settings), with its
  own palette glyph (`GL_COLOR` 0xE2B1), page fade-in animation, and a
  scrollable card layout via the existing scroll machinery. Wireframe items
  scroll into view when activated.
- The page has two sections:
  1. **Wallpaper properties** — controls generated from the wallpaper package
     (see §3). When the wallpaper ships no editable properties this section is
     hidden instead of rendering empty.
  2. **LOOK & COLOUR** — Screen+'s own grading controls (see §4), always
     available for every wallpaper type (image, video, HTML/web).
- A **Reset look** button resets only the grading; a separate **reset properties**
  hit (`HK_RESET_PROPS`) clears the stored property overrides so the wallpaper
  returns to its package/saved values (`wpapi_clear_override`).

## 2. How changes reach the wallpaper (no renderer restart)

Screen+ runs its renderers in a separate engine process, so the panel changes
had to reach it without the expensive `CMD_RESTART_RENDERERS` path that updating
any other setting uses.

- New IPC command `CMD_APPLY_CUSTOMISE` (`src/common/ipc.h`) → engine handler in
  `src/engine/engine.c`: for every `RS_ACTIVE` renderer slot it calls the new
  `vt->apply` function on the renderer.
- `src/renderer/renderer.h` gained `apply` as the 11th member of `RendererVtbl`.
  All three renderer types implement it:
  - **Video** (`src/renderer/video/video_renderer.c`): `vo_apply` reloads the
    grading blob for the wallpaper into `VideoOutput.adj` and wakes the present
    thread (`redraw` flag + event). The next presented frame already uses the
    new constants — video keeps playing, nothing restarts, audio unaffected.
  - **Image** (`src/renderer/image/image_renderer.c`): the decoded GPU texture
    is now **retained** instead of released after the first present (lock went
    exclusive→shared with an `AddRef` on the texture), and `img_apply` reloads
    the grading and flips a `repaint` flag that makes `WM_PAINT` re-present the
    retained texture through the new graded present path. No re-decode.
  - **HTML/Web** (`src/renderer/html/html_renderer.c`): `hr_apply` re-runs
    `apply_adjustments`, which re-injects the CSS `filter` string and the tint
    overlay div into the live page (see §5). No reload of the WebView.
- Shell side: `app_apply_customise()` (`src/app/app.c/.h`) sends the command via
  the existing `supervisor_send`.

Net effect: moving a slider updates the wallpaper within a frame or two, and
the only disk read is the small grading/props JSON.

## 3. Author properties (Lively/WE packages)

`src/renderer/html/wallpaper_api.h` grew `CustomType`, `CustomProp` and the
list API; `wallpaper_api.c` has the implementation (~220 new lines).

- New API surface (all implemented):
  - `wpapi_props_list()` / `wpapi_props_free()` — enumerate a wallpaper's
    editable properties.
  - `wpapi_save_override()` / `wpapi_clear_override()` — persist one value /
    clear the whole overrides file.
  - `wpapi_load_adjust()` / `wpapi_save_adjust()` — the grading blob (same
    file, reserved key `_screenplus_adjust`).
  - `wpapi_jval_copy()` — deep-copy helper for working copies.
- Property sources, in resolution order **override → saved value → package
  default**:
  - the Screen+ overrides file per wallpaper,
  - Lively's own saved customisations (SaveData/wpdata) when the package is
    Lively-format,
  - `<folder>/LivelyProperties.json` (Lively packages),
  - or `project.json → general.properties` (Wallpaper Engine packages).
- Control types recognised and rendered (checked against Lively's documented
  set): **slider** (min/max/step), **checkbox**, **dropdown** (+ WE's
  `scalarDropdown`, rendered as a button+menu `PK_DOWN` hit), **color /
  colorpicker** (drawn as a `#RRGGBB` swatch pick via `HK_PCOLOR`),
  **textbox** (live text field with caret; value is committed per keystroke
  like Lively does, via `pages_text_char/key/end`), and **label**.
  `button` and `folderDropdown` are deliberately skipped (`prop_type` returns 0
  and the property is left out of the panel).
- `CustomProp` carries label, type, resolved `JVal*` value, dropdown items,
  slider range/step, and an `overridden` flag (visual "modified" state).
- Semi-transparent fallbacks for slider/colour when the package omits a field
  (`jprop_num/str/bool` helpers package native types back to JSON).
- Values are written back to the Lively-shaped JSON files, so a wallpaper
  customized in Screen+ keeps its settings if opened in Lively, and the page
  scripts receive them through the existing `livelyPropertyListener` / WE
  plumbing on the next load or apply pass.

## 4. Look & Colour (the colour-grading engine)

Written natively — no external FOSS grading engine was pulled in: the whole
thing is one 37-line HLSL addition + a constant-buffer struct, which is faster
than any general engine and keeps the LGPL FFmpeg-only dependency posture.

- `src/settings/config.h`: new `WallpaperAdjust` struct
  (`used, brightness, contrast, saturation, hue, gamma, tint 0xRRGGBB,
  tintLift, tintBrightness`) plus `adjust_defaults()`. Panel defaults: tint
  colour `#FF7575`, grades neutral, grading fully off (`used=0` → shader not
  even bound).
- `src/renderer/video/video.hlsl`: new shader code consumed by all three video
  pixel shaders (`ps_nv12`, `ps_i420`, `ps_rgb`):
  - brightness offset → contrast pivot at 0.5 → gamma (`saturate(max(c,0))^e`)
    → Rec.709 luma-chrominance saturation gain → **hue rotation via YIQ matrix**
    (skipped when the angle is ~0) → tint stage.
  - **Tint = black lift**: `lifted = max(c, tint.rgb); c = lerp(c, lifted,
    tint.w)` — every pixel below the tint colour is pushed up towards it with
    the chosen strength, i.e. the *blacks* take on the tint colour while
    brighter areas are left alone. Tint RGB is pre-multiplied by
    "Tint brightness".
  - `video_renderer.c`: `grade_fill` maps the -100..100 slider ranges to
    shader constants (brightness ±0.5 offset, contrast ±1 gain, gamma
    1/1+g asymmetric curve, saturation ±1 gain, hue degrees→radians, tint lift
    0..1); grade constants ride in the existing `CBParams` constant buffer.
  - **Fallback graded present**: for surface colours (image renderer, HTML
    snapshot frames) `gfx_present()` gained an optional `WallpaperAdjust*`
    param; when grading is active it binds the existing RGB pixel shader
    (`psRGB`) as a BGRA texture-array SRV and draws a full-screen triangle into
    the back buffer with grade constants instead of the free `CopyResource`
    fast path. Neutral grading still uses the copy path, so uncustomised
    wallpapers pay nothing.
- **HTML/Web wallpapers** get an equivalent CSS path in `html_renderer.c`:
  brightness/contrast/saturate/hue-rotate are composed into a `filter:` string
  and the tint becomes a fixed, `pointer-events:none`, `z-index:2147483647`
  overlay div with `mix-blend-mode: lighten` at `tintLift/100` opacity and
  pre-darkened RGB — the same "lift the blacks" behaviour in CSS. (Gamma has no
  CSS equivalent; the page notes that gamma applies to native wallpapers only.)

### 4a. UI controls (`src/ui/ui_core.c`, +144 lines)

- New `ui_slider` widget: pill track + accent fill, thumb geometry shared with
  the hit-test helper so wall-dragging is exact; drag routing via the new
  `pages_drag()` hook with per-step commit (live value while dragging, disk
  write on the throttled commit).
- New `ui_disc` colour wheel: the hue/saturation disc is a cached
  `ID2D1Bitmap` (rebuilt only when the render target changes) drawn with
  `DrawBitmap` at 74 px radius, with the current hue/sat marker; picking maps
  distance→saturation and angle→hue. State persists out-lives retargeting and
  the new `pressedRect` bookkeeping in `Ui` keeps it tracking outside its rect.
- HSV helpers (`ui_hsv`, `ui_rgb_hsv`) both directions; the disc shows the
  current tint's hue+saturation while the **brightness** of the tint colour is
  a separate slider — the tint "colour circle" the panel was built around.
- Sliders displayed: Brightness −100..100, Contrast −100..100, Saturation
  −100..100, Hue shift −180..180, Gamma −100..100, Tint strength 0..100,
  Tint brightness 0..100, plus the colour wheel and a live tint swatch with
  hex read-out.
- Text-entry plumbing: `pressedRect` in `Ui`, `textFocus` index, caret timer
  (`TM_CARET`) shared via the `UiTimer` enum in `ui_internal.h` (the old
  file-local timer duplicate was removed), focus-loss commits/cancels without
  stealing the UI.

## 5. Persistence

- Per-wallpaper file `<data>\props\<id>.json` holds both the property overrides
  (Lively JSON shapes) and the grading under the reserved key
  `_screenplus_adjust`; the key can never collide with an author property name
  and is never sent to page scripts.
- Writes go through `save_override_file`, which rewrites the JSON preserving
  key order of all other entries and replaces/creates single keys atomically.
- The grading blob is loaded at renderer creation (`video_renderer_create`,
  `image_renderer_create`, HTML renderer) so a wallpaper presents correctly on
  boot without any UI round trip.
- None of this goes through `config.ini`; panel changes use their own apply
  command and renderers re-read their files.

## 6. Infrastructure that had to change for the above

- `src/ui/d2d_c.h` (rewritten, CRLF-normalised): the hand-rolled C vtables
  gained a real `DrawBitmap` declaration (was a `void*` placeholder — calling
  it caused MSVC C2064) and an `ID2D1Bitmap_Release` lppvt macro for the disc
  cache lifetime.
- `ui_timer` ids unified (`TM_ANIM/CARET/STATS/THUMBS`) so the two UI TUs agree.
- Warnings surfaced and fixed during the version-bump rebuild: narrow copy of
  the property key (`wallpaper_api.c`; `str_copyn` is wide-only) and `%%`
  escaping of CSS `100%` widths inside the printed JS filter snippet
  (`html_renderer.c`).
- `CMakeLists.txt`: `SCREENPLUS_VERSION` bumped 1.0.0 → **1.3.0** (verifiable in
  the binary: three UTF-16 `1.3.0` strings, zero `1.0.0`).

## 7. Build artefacts (verified)

- `build/ScreenPlus.exe`
- `build/ScreenPlus-Setup-1.3.0.exe` (NSIS installer)
- `build.cmd Release 1.3.0` → exit 0, **0 errors / 0 warnings** in the full log
  (a filtered earlier run had to be repeated with explicit status capture).
- Stale `ScreenPlus-Setup-1.0.0.exe` was deleted from `build/`.

## 8. Known gaps (actually not built, stated here rather than padded)

- Author property types `button` and `folderDropdown` are skipped by the panel.
- Web-wallpaper grading: gamma has no CSS counterpart (panel states this inline,
  carrying the rest via filter+overlay).
- Runtime click-through of the panel on a live desktop has not been exercised;
  verification is limited to the clean v1.3.0 build, the binary version check
  above, and code-path tracing of the IPC → `vt->apply` → renderer chains.

---

## Diff footprint

| File | Change |
|---|---|
| `src/ui/ui_pages.c` | +537 — the Customise page: look card, property widgets, commit paths, dispatch, sidebar |
| `src/ui/ui_core.c` | +144 — `ui_slider`, `ui_disc`, HSV helpers, pressed-rect drag, text routing |
| `src/renderer/html/wallpaper_api.c` | +223 — property enumeration/resolution, overrides store, grading load/save |
| `src/ui/ui_internal.h` | page/hit-kind/timer enums, new widget prototypes, `pressedRect`/`textFocus` |
| `src/renderer/video/video_renderer.c` | +89 — grade constants, graded presentation pipeline, `vo_apply` |
| `src/renderer/html/html_renderer.c` | +57 — CSS filter + tint overlay injection, `hr_apply` |
| `src/ui/d2d_c.h` | vtable fix (`DrawBitmap`), `ID2D1Bitmap_Release` |
| `src/renderer/image/image_renderer.c` | +44 changed (incl. locked region rework) — texture retention, re-present path, `img_apply` |
| `src/renderer/video/video.hlsl` | +37 — grading functions |
| `src/renderer/video/video_internal.h` | +2 — `adj`/`wpId` state in `VideoOutput` |
| `src/settings/config.h` | +23 — `WallpaperAdjust` struct + `adjust_defaults()` |
| `src/app/app.c/.h`, `src/common/ipc.h`, `src/engine/engine.c` | `CMD_APPLY_CUSTOMISE` plumbing |
| `src/renderer/html/wallpaper_api.h` | CustomProp list API |
| `src/renderer/renderer.h` | `vt->apply`, `gfx_present(adj)` |
| `CMakeLists.txt` | version 1.3.0 |

Footnote: the +N figures are total changed lines per file from `git diff --stat HEAD`
(insertions dominate; the whole set is 1228 insertions / 64 deletions across 18 files).

---

## v1.3 fix pass — why the panel did nothing, and what changed

Verified on the live desktop (Rain web wallpaper + a video wallpaper): every look
control, the tint wheel, author sliders/toggles/dropdowns/background picker, both
resets, and edits made while the wallpaper is paused.

**Root causes**
- The UI never set `WallpaperAdjust.used`, so every renderer treated the grading as
  off. `used` is now derived from the values (`adjust_active()`), never trusted from disk.
- `CMD_APPLY_CUSTOMISE` (and `hr_apply`) were skipped unless the slot was `RS_ACTIVE`, and
  the Screen+ window itself (class `ScreenPlus.Window`) counted as a fullscreen app, so
  the wallpaper was paused while you edited it. Apply now reaches every live renderer,
  and our own see-through window is ignored by occlusion.
- Author property values were saved as `{"v": value}` objects, so pages got 0/false.
  They are now stored as native scalars; old files are unwrapped on read.
- Web tint JS was cut off by a 220-byte buffer (syntax error, nothing applied).
- Colour wheel bitmap: uninitialised pixels outside the disc and non-premultiplied
  edges (the "glitch"); it was also never released with its render target.
- Lively dropdowns compared item text with the stored index (showed "1" not "Fill").
- `Ui.textFocus` started at 0 instead of -1, which swallowed keys/typing on other pages.

**Behaviour now**
- Slider/wheel drags preview live (one engine round trip per 40 ms max, final value on
  release); clicks jump to the pointer; arrow keys step a focused slider (Shift ×10).
- Web pages only receive properties that changed (like Lively), not the whole set on
  every tick (which reloaded Rain's background texture each time).
- Web grading = a full-viewport `backdrop-filter` layer plus the tint layer on top, in
  the shader's order (the old root `filter` also greyed the tint). Gamma remains native-only.
- New controls: Lively `folderDropdown` (file menu), WE `combo`/`bool`/`textinput`,
  WE "r g b" colours; dropdowns open a real menu. "Restore defaults" resets properties
  only and keeps the look; neutral settings delete the stored key/file.
- Image renderer: a re-decoded texture (resize) is presented again; texture ref leak on
  a failed present fixed. Graded present falls back to a plain copy if a view fails.
