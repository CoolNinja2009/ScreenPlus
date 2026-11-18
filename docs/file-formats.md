# Screen+ file formats

Screen+ uses two ZIP archives that work on Windows and Linux:

| Extension | Meaning | Contents |
| --- | --- | --- |
| `.spf` | Screen+ file | One wallpaper, its metadata and available Customise settings. Local media and generated depth assets are included; a URL wallpaper stores its link instead. |
| `.spp` | Screen+ playlist | A named collection of `.spf` files in library order, plus its slideshow interval. |

Both formats use `manifest.json` encoded as UTF-8. `version` is currently `1`. Archive paths use `/` on both platforms. The extension identifies the intended format, while the manifest's `screenplus` field distinguishes `"spf"` from `"spp"`.

## `.spf`: one wallpaper

A local wallpaper has this layout:

```text
wallpaper.spf
├── manifest.json
├── media/
│   └── wallpaper.ext             # image or video
├── thumbnail.ext                 # only if a saved thumbnail exists
└── props.json                    # only if Customise overrides exist
```

For an HTML wallpaper, `media/wallpaper/` contains the complete source folder, including relative assets. The manifest's `path` points to its entry document, for example `media/wallpaper/index.html`. An interactive depth wallpaper is an HTML wallpaper: its generated page, source media, depth maps and other generated assets travel with that folder. The model weights used to create them are not required on the receiving PC and are not included.

Example local manifest:

```json
{
  "screenplus": "spf",
  "version": 1,
  "name": "Northern Lights",
  "type": 1,
  "format": 0,
  "flags": 0,
  "volCut": 0,
  "slideSec": 0,
  "path": "media/northern-lights.jpg",
  "thumb": ""
}
```

For a URL wallpaper, no media or thumbnail is packaged. Its `path` is the original `http://` or `https://` link. If the wallpaper has Customise overrides, the manifest contains a `customisation` JSON object; otherwise that field is absent. A streamed video keeps its stream flags, including the yt-dlp flag when it needs URL resolution. Playing a URL wallpaper requires network access.

Example URL manifest:

```json
{
  "screenplus": "spf",
  "version": 1,
  "name": "Live page",
  "type": 3,
  "format": 0,
  "flags": 2,
  "volCut": 0,
  "slideSec": 0,
  "path": "https://example.com/wallpaper",
  "customisation": { "_screenplus_adjust": { "brightness": 12 } }
}
```

`type` is `1` for an image, `2` for a video and `3` for HTML. `format` is `0` for a plain wallpaper, `1` for Lively and `2` for Wallpaper Engine. `flags` uses the `WPFLAG_*` bit values in `src/settings/config.h`; URL entries keep `WPFLAG_URL` (`2`), and depth entries use `WPFLAG_DEPTH` (`16`). `volCut` is the wallpaper's volume reduction from `0` to `100`. `slideSec` is the PowerPoint slide interval field; a PowerPoint export contains its generated media, not the original presentation link.

`props.json` and `customisation` carry the saved Customise values, including look adjustments, compatible wallpaper properties and recordings when present. Export does not include the wallpaper's library ID, display assignment, use history or membership in unrelated collections. Import creates a new library entry and ID and restores the available settings.

## `.spp`: a collection and its interval

An `.spp` contains its own manifest and numbered `.spf` members:

```text
collection.spp
├── manifest.json
├── 0000.spf
├── 0001.spf
└── ...
```

Example manifest:

```json
{
  "screenplus": "spp",
  "version": 1,
  "name": "Evening",
  "count": 2,
  "intervalMinutes": 5
}
```

Members are exported in library order. On import, Screen+ adds them to the named collection, including when the collection has only one wallpaper. `intervalMinutes` is the slideshow interval, not a per-wallpaper duration. A valid imported interval replaces the receiving app's current slideshow interval; it does not turn on the slideshow or change display assignments. Each member may be a local or URL wallpaper.

## Using the formats

- Right-click a wallpaper in the Library and choose **Export as .spf**. On Windows, dragging a card to File Explorer exports it; dragging a selected group exports every selected wallpaper as a separate `.spf`.
- Right-click a collection to export its `.spp`. Export runs in the background. On Linux, dragging a card outside the window opens a save dialog; a selected group opens a folder picker.
- Drop either archive onto the Library, use the Import button, or run `screenplus import <path>`.
- From the CLI, run `screenplus export <wallpaper> <file.spf>` or `screenplus export collection:<name> <file.spp>`.

Export writes the archive through a temporary file and replaces the destination after packaging succeeds. Import validates ZIP entries before extraction: it rejects traversal paths, symlinks, encrypted entries, unsupported ZIP64 sizes and archives above its file and expanded-size limits. A recipient should still treat links and HTML code from other people as content they choose to run.
