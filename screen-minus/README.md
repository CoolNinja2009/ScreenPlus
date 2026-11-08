# Screen-

The Screen+ dev engine: a fork of Godot (pinned in `godot.pin`) with the `screenplus` module, which connects a
wallpaper project to the Screen+ bus. Design: `../docs/dev-engine.md`.

| Path | What |
|---|---|
| `godot.pin` | Godot tag we build from |
| `modules/screenplus/` | the `ScreenPlus` singleton: JSON-RPC client, pause, properties, events (`screenplus.*`), pipe/socket transport (`sp_pipe.*`) |
| `spx/manifest.schema.json` | `.spx` manifest schema |
| `tools/fake_engine.py` | stands in for the engine's side of the bus to test a runtime build |
| `build.sh` | clones the pinned Godot and builds the editor or runtime with our modules (CI) |

Build (CI; needs Python 3, Git, and the platform's Godot toolchain):

```bash
./build.sh windows template_release
./build.sh windows editor
```

Test a runtime against the fake engine:

```bash
python -I tools/fake_engine.py out/screen-runtime.exe path/to/project.pck --grant audio
```

In a pack:

```gdscript
func _ready():
    ScreenPlus.audio_frame.connect(func(spectrum, level): $Bars.update(spectrum))
    ScreenPlus.property_changed.connect(func(n, v): if n == "speed": speed = v)
    var r = await ScreenPlus.call_api("system.stats").done   # [result, error]
```

Status: phase 1 (bus client). The Screen+ engine side, scanner, sandbox and editor branding are not built yet.
