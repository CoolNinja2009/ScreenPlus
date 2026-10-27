# Store sources

The Store (`src/app/store.c`) merges these sites into one feed. Each one is an entry in `src/app/store_sources.c`
with a listing/search address per filter, a card parser, an optional wallpaper-page parser, and preview and file
addresses. MotionBGS is mapped in detail in [motionbgs.md](motionbgs.md). Sites whose Download opens their page
are explained in [sites-without-one-click-download.md](sites-without-one-click-download.md). Checked 2026-10-27.

| Site | Kind | Latest / search / category | Filters on the site | Preview | Download |
| --- | --- | --- | --- | --- | --- |
| MotionBGS | live | `/hx2/latest/<n>/`, `/search?q=&page=`, `/tag:<tag>/<n>/` (a search naming a tag redirects there), `/4k/<n>/` | 4K listing | `/i/c/364x205/media/<id>/<poster>.webp` | `/dl/<4k\|hd>/<id>/` from its page |
| Lively | live | `/live-wallpapers/[<category>/]<n>/` (36 a page; no search: cards carry `data-search` keywords) | categories, `4k` | `cdn…/library/<slug>-320\|640.webp` | `cdn.livelywallpaper.app/wallpapers/<slug>/download.mp4` (size on the card) |
| MoeWalls | live | `/page/<n>/`, `/page/<n>/?s=`, `/category/<slug>/page/<n>/` (16 a page) | none (resolution on each card) | `…-thumb-364x205.jpg` / `728x410` | opens the page |
| wallsflow | live | `/live-wallpapers/[<category>/]page/<n>/`, `index.php?do=search&subaction=search&story=&search_start=<n>` (9 a page) | none (resolution and size on its page) | `cloud.wallsflow.com/posts/…/thumbs/….webp` | opens the page |
| backiee | still, to 8K | `/latest`, `/popular-wallpapers`, `/categories/<slug>`, `/search/<q>` + `?page=&resolution=hd\|4k\|5k\|8k&sort=date\|rating\|downloads` (24 a page) | resolution, sort, category | `/static/wallpapers/456x257/<image>.webp` | `/static/wallpapers/<W>x<H>/<image>.jpg`, sizes up to the JSON-LD original |
| Wallhaven | still | `api/v1/search?q=&categories=&purity=100&sorting=&atleast=&ratios=landscape&page=` (JSON, 24 a page) | everything; safe for work forced (`purity=100`, `"sfw"` checked) | `thumbs.original` | `w.wallhaven.cc/full/<id[0..1]>/wallhaven-<id>.<ext>` |

- **Common categories** (Anime, Games, Nature, Cars, Space, Animals, Fantasy, City, Abstract, Movies & TV, Sport,
  Technology, Minimal) map to each site's own category where it has one. Otherwise the site searches the
  category's word, unless something was typed: few sites can combine both, so the typed words win.
- **Resolution** goes to the sites that filter by it (backiee, Wallhaven, MotionBGS's 4K listing); the Store
  hides known smaller cards from the rest. Sites whose best is below the filter (5K and 8K are backiee and
  Wallhaven only) aren't asked.
- **Sort** (Best match, Latest, Popular, Most downloaded) goes to backiee and Wallhaven; the others keep their own
  order, which is newest first.
- **Copies:** each preview's 64-bit difference hash, taken from its centre 16:9 crop, decides. Hashes within 5
  bits, or within 12 bits when the titles share half their words, of the same kind (live or still) are one
  wallpaper. The copy with more pixels, then the larger file, stays.
- **Bot walls:** all six use Cloudflare only as a CDN with its passive script, so WinHTTP gets the real pages.
  mylivewallpapers.com sits behind a Sucuri JavaScript challenge and is left out.
