# Bare UI (beta)

The new Screen+ window, drawn with [Bare](../../../../Bare) liquid glass. It is a separate process
(`screenplus-bare-ui.exe`) and a client of the running service: data comes from the same JSON `ScreenPlus.com
<command> --json` prints, and every action is a service command. The service stays the only owner of the config and
of playback.

- **See-through.** The window is frameless; its background is the live desktop behind it
  (`bare_window_backdrop`, DXGI Desktop Duplication), and the sidebar, the page and every control are glass that
  bends it. The window is left out of screenshots and screen sharing while it does this.
- **Liquid motion.** Selections (the nav, filters, segmented choices) are a glass drop that glides between items on
  a spring, clears into a lens and stretches while it moves; switch and slider knobs turn to glass while held;
  pages slide in; toasts condense out of the glass (`sp_kit.c`). Settings → Animations: Full, Reduced, Off.
- **Fast.** A worker thread sends WM_COPYDATA requests straight to the service (`sp_ipc.c`: no process per
  request) and refreshes every 2 s, or right after an action; the window thread only draws.

Files: `live.c` (the real window), `preview.c` (sample data, no service), `screenplus_bare.c` (the pages),
`sp_kit.c` (glass controls), `sp_ipc.c` (service requests), `test_view.c` (headless filter test).

## In the app

When `screenplus-bare-ui.exe` is installed beside `ScreenPlus.exe` (a build with `-DSCREENPLUS_BARE_UI=ON`), it is
the window: the tray, a plain launch and `screenplus show` open it. `screenplus ui classic` opens the classic window
(every option lives there while this one is in beta); `screenplus ui Bare` opens this one.

Linux: the beta is Windows-only for now (no backdrop there yet); `ui classic` opens the usual window.

## Build

```sh
cmake -S . -B build -DSCREENPLUS_BARE_UI=ON -DBARE_SOURCE_DIR=/path/to/Bare
cmake --build build
```

Preview and the view test on their own:

```sh
cmake -S src/ui_bare -B build-bare-ui -DBARE_SOURCE_DIR=/path/to/Bare
cmake --build build-bare-ui
ctest --test-dir build-bare-ui
SP_BACKDROP=gen build-bare-ui/screenplus-bare-preview --page settings   # a generated picture behind the glass
```

## Covered so far

- Library: now showing (pause, sound, restart, show file), display choice, search, type filters, Favourites and
  collections, sort, apply by click, right-click menu (apply to a display, favourite, show in folder, remove),
  import a file or a folder, add from a link.
- Displays: arrangement to scale, display mode, choose a wallpaper for one display.
- Customise: live brightness, contrast, saturation, hue and gamma; Reset look.
- Store: browse with previews, search, more, get with progress.
- Settings: general, updates (Check now), appearance, animations, accent colour, playback, sound, web wallpapers,
  slideshow, library, screensaver.

Not yet: Customise's author properties, layout, upscaling and recordings; the Language setting and translations
(Bare's fonts are Latin-only so far); the screensaver page; drag to reorder. Those are in the classic window.
