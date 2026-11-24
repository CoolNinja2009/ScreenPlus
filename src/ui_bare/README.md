# Bare UI branch

This directory contains the new cross-platform Screen+ window. It uses Bare for the view and Screen+'s existing CLI for state and actions; the service remains the owner of wallpaper playback and configuration. The shipped `show` command still opens the existing UI while this branch is being completed.

## Build

From the ScreenPlus root, point `BARE_SOURCE_DIR` at the Bare checkout on its `bare-ui` branch:

```sh
cmake -S . -B build -DSCREENPLUS_BARE_UI=ON -DBARE_SOURCE_DIR=/path/to/Bare
cmake --build build --target ScreenPlusCli screenplus-bare-ui
build/ScreenPlus.com ui Bare    # Windows
# build/screenplus ui Bare      # Linux
```

The Bare UI build is optional so existing Screen+ builds do not require a Bare checkout. The CLI launches the Bare executable beside itself; `show` continues to open the original window. The process reads `status`, `list`, `settings`, `look`, and `store` through `--json`; actions use the corresponding CLI commands. The preview executable uses sample data without connecting to a running service.

For an isolated preview and headless view test:

```sh
cmake -S src/ui_bare -B build-bare-ui -DBARE_SOURCE_DIR=/path/to/Bare
cmake --build build-bare-ui --target screenplus-bare-preview screenplus-bare-view-test
ctest --test-dir build-bare-ui --output-on-failure
```

## Current coverage

- Library: search, type filters, selection, import file, pause, sound, restart, refresh, and incremental card rendering.
- Displays: show connected monitors, change display mode, and choose a wallpaper for one monitor.
- Customise: brightness, contrast, saturation, hue, and gamma for the active wallpaper.
- Store: list, search, load more, and get an item. The Store loads asynchronously; use Refresh to see newly fetched results.
- Settings: startup, launch behavior, still wallpaper, automatic updates, appearance, and motion.

The remaining controls in the existing UI, translations, and integration with the app's `show` route are still pending. The live window requests an SDL alpha-composited surface through the Bare checkout's `bare_window_transparent` API. Whether the desktop shows through and how the compositor handles blur must be verified on Windows and Linux. Bare's glass shader samples the content rendered inside its window; it cannot refract the desktop behind the window yet. Do not replace the existing UI until those controls and platform behavior are ported and visually tested.
