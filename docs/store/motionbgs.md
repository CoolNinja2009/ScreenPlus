# MotionBGs (motionbgs.com) as a store source

Reverse-engineered from the live site on 2026-10-27. There is no JSON API: the site is server-rendered (Django,
htmx, behind Cloudflare), so each "endpoint" below is an HTML page or htmx fragment plus the anchors to parse.
The markup is minified with unquoted attributes (`href=/slug`, `class=ttl`); match those forms.

## Access
- Base `https://motionbgs.com`. Plain GETs work without cookies, Referer or a browser User-Agent, and no
  Cloudflare challenge was hit. Send a real User-Agent anyway and cache: pages carry `Cache-Control: max-age=86400`.
- robots.txt allows everything except `/ajax/`, `/admin/` and `/a2/`, none of which are needed.
- The page's htmx config adds an `X-CSRFToken` header; GETs don't need it.
- The wallpapers are third-party art: link back to the source page and download on demand rather than rehosting.

## Wallpaper card
Every listing and search page is a grid of up to 36 cards:

```
<a title="Wallhack Berserk live wallpaper" href=/wallhack-berserk> <figure><picture>
  <source … srcset=/i/c/546x308/media/10331/wallhack-berserk.3840x2160.jpg.webp type=image/webp> …
  <img alt="…" height=205 src=/i/c/364x205/media/10331/wallhack-berserk.3840x2160.jpg width=364>
</picture></figure> <span class=ttl>Wallhack Berserk</span> <span class=frm> 4K </span> </a>
```

| Field | From |
| --- | --- |
| slug | `href=/<slug>`; the detail page is `https://motionbgs.com/<slug>` |
| id | the number after `/media/`; also the download id |
| title | `<span class=ttl>` text (HTML-escaped) |
| source resolution | `<W>x<H>` in the poster file name (`3840x2160` = 4K) |
| badge | `<span class=frm>` text, e.g. `4K` |
| thumbnail | `src=` (364x205 JPEG); the `<source>`s add `.webp` and the 2x `546x308` |

- Mobile cards (`/mobile/` listings) link to `/mobile/<slug>`, use `235x418` crops of `<file>.720x1280.jpg` and
  have no badge.
- GIF cards (`/gifs/` listings) differ: `<a class=gf_ttl href=/<slug>> <Title> Gif Wallpaper </a>`, preview
  `src=/media/<id>/<file>_312.gif` (+ `_312.webp`), badges like `<a class=badge href=/<slug>#download_gif>800x450 gif</a>`.

## Search
`GET /search?q=<url-encoded query>[&page=<n>]`

- A query that names a tag answers `302 Location: /tag:<slug>/` (`naruto` → `/tag:naruto-art/`, `night` →
  `/tag:night/`). Follow it: the result is a tag listing (below), paged as one.
- Otherwise `200` with up to 36 cards. A next page exists only if the page links `href=/search?q=<q>&page=<n+1>`.
  A `page` past the end returns the last page again (no 404), so stop on the missing link, never on the status.
- No results: zero cards and an `<h1>` "No Results Found for '<q>'". Stop words (`a`, `the`) match nothing.
- The order is the server's relevance. There is no sort or filter parameter.

## Listings (categories)
| Listing | Page 1 | Page n | Pages on 2026-10-27 |
| --- | --- | --- | --- |
| Latest | `/` (grid under the featured rows) | `/<n>/` | 271 |
| Latest, fragment | `/hx2/latest/1/` | `/hx2/latest/<n>/` | |
| 4K | `/4k/` | `/4k/<n>/` | 216 |
| Mobile | `/mobile/` | `/mobile/<n>/` | 132 (mobile cards) |
| GIF | `/gifs/` | `/gifs/<n>/` | GIF cards |
| Tag | `/tag:<slug>/` | `/tag:<slug>/<n>/` | 320 tags |

- A next page exists only if the page links `href=/<listing>/<n+1>/`. Past the end, `/`, `/4k/` and `/mobile/`
  return the last page, while tags and `/gifs/` return 404.
- `/hx2/latest/<n>/` is the home page's infinite scroll: just the cards (~28 KB against ~50 KB for a full page).
  Its next page is `hx-get=/hx2/latest/<n+1>/`, missing on the last one. No other listing has a fragment.
- The site orders listings (newest or recently updated first) and has no sort parameter. Sort client-side by
  `uploadDate` (detail page), `lastmod` (sitemap) or id.

## Catalogue: `GET /sitemap.xml`
One 2.2 MB file of `<url><loc>…</loc><lastmod>YYYY-MM-DD</lastmod>…</url>`: 9882 wallpapers (`/<slug>`), 4722
mobile pages (`/mobile/<slug>`) and 320 tags (`/tag:<slug>/`, the full category list), plus the listing roots
and info pages. Use it to seed categories or a full index, and `lastmod` to refresh.

Hierarchy: a detail page's breadcrumb gives parent → child (Anime → Berserk), and the nav menu lists top
categories with child tags (`<li class=col-span-2><a href=/tag:games/>Games</a> <ul><li><a href=/tag:…/>…`).

## Wallpaper: `GET /<slug>`
| Field | Parse |
| --- | --- |
| title | `<h1><span><Title></span> Live Wallpaper</h1>`, or `og:title` minus " Live Wallpaper" |
| id | any `/media/<id>/` or `/dl/<variant>/<id>` |
| poster | `<meta content=https://motionbgs.com/media/<id>/<file>.<W>x<H>.jpg property=og:image>` (full size) |
| preview video | `<source src=/media/<id>/<file>.960x540.mp4 type=video/mp4>` (= `og:video`) |
| duration, upload date | `application/ld+json` `VideoObject`: `"duration": "PT16.8S"`, `"uploadDate": "2026-10-20T00:00:00+00:00"`; its `width`/`height` are the preview's |
| categories | `BreadcrumbList` JSON-LD: the items between the home page and the wallpaper are `/tag:<slug>/` with names |
| downloads | `<section class=dl>`: per variant `<a href=/dl/<variant>/<id> …>` then `<span class=font-bold>4K</span> Wallpaper (23.9Mb)` and `<div class=text-xs>3840x2160 mp4 file</div>` |
| mobile version | `<a title="Live Wallpaper For Mobile" href=/mobile/<slug>>` |
| GIF version | a `/dl/gif/<id>` link, under `id=download_gif` |

`GET /mobile/<slug>` is the phone version of the same page: preview `<file>.304x540.mp4` and one `mhd` download.

### Download variants
| Variant | URL | File (example) |
| --- | --- | --- |
| `4k` | `/dl/4k/<id>/` | `<file>.3840x2160.mp4` |
| `hd` | `/dl/hd/<id>/` | `<file>.1920x1080.mp4` |
| `mhd` | `/dl/mhd/<id>/` | `<file>.720x1280.mp4` (mobile) |
| `gif` | `/dl/gif/<id>/` | `<file>_800.gif` (800x450) |

- `/dl/<variant>/<id>` 301s to the trailing-slash form, which returns the file itself (`video/mp4` or
  `image/gif`, `Content-Disposition: attachment; filename="<file>.3840x2160.mp4"`) and honours byte ranges (206).
  No Referer or cookie is needed.
- The same files are static at `/media/<id>/<file>.<W>x<H>.mp4`, which suits streaming.
- Page sizes are MiB with one decimal ("23.9Mb" = 25,073,912 bytes). For exact bytes without downloading, GET
  with `Range: bytes=0-0` and read the total from `Content-Range: bytes 0-0/<total>`.
- List only the variants a page links: the 4K listing holds 7754 of the 9882 wallpapers, about half have a
  mobile version, and GIFs are rarer still.

### Images
`/i/c/<W>x<H>/media/<id>/<poster file>` is a resized crop of the poster. The site uses 364x205, 546x308, 960x540,
235x418 (mobile) and 48x48; other sizes work too (640x360 tested). Append `.webp` for WebP.
`/media/<id>/<poster file>` is the full-size JPEG.

Fetch the `.webp` crops. Browsers request those, so Cloudflare holds them (about 0.1 s, `cf-cache-status: HIT`).
The `.jpg` crops are mostly misses that the origin resizes (0.4–1.2 s each). Fall back to JPEG only where
Windows can't decode WebP.

## URLs without the detail page
A card gives the id, the file stem and the source `<W>x<H>`, so the preview `/media/<id>/<file>.960x540.mp4`
and any thumbnail can be built from it directly. Take the stem from the HTML rather than assuming it equals the
slug, and read the detail page for sizes, duration and which variants exist.
