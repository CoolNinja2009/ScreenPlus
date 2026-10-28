# Sites without one-click download

Checked on 2026-10-27. The Store lists, searches and filters these sites like the others, but their Download
button opens the wallpaper's page in the browser, where one click finishes the download. Fetching the file
directly would mean defeating protections the sites added on purpose, and Screen+ doesn't do that.

## MoeWalls (moewalls.com): live wallpapers, opens the page

What the Store does: latest, search (`/?s=`), category pages, and the resolution on each card. Download opens
the wallpaper's page.

Why no one-click download:

- **No file address in the page.** The button is `<a id="moe-download" data-id="<post id>" data-url="<token>" href="#">`.
  `data-url` holds an encrypted, URL-encoded token (`dF9DZYIr0W…`) that the site's JavaScript turns into the real
  link only when the button is clicked.
- **robots.txt bans the download path.** `Disallow: /download*` applies to every automated client.
- **No side door.** The WordPress REST API (`/wp-json/wp/v2/…`) answers 403.
- Cloudflare's passive bot-detection script runs on every page. The page also mentions a CAPTCHA (the download
  endpoint itself was not requested).

What it uses against automated downloads: per-wallpaper encrypted download tokens that only its own JavaScript
resolves (anti-scraping and anti-hotlinking), plus a robots.txt ban on its download endpoint.

## wallsflow (wallsflow.com): live wallpapers, opens the page

What the Store does: latest, search (DataLife Engine's `do=search`), category pages, and each wallpaper's page
for its resolution and size. Download opens the wallpaper's page.

Why no one-click download:

- **A gated download page, not a file.** The button leads to `/download/<file id>.html` (the file id differs from
  the wallpaper's id). That page starts a PHP session (`PHPSESSID` cookie) and loads the site's CAPTCHA settings
  (`captcha_type = '4'`).
- **A quota for anonymous visitors.** The site says "Log in or register to download without limits".
- robots.txt also bans `/engine/go.php` (the CMS's outbound redirect script).
- The full 4K file (3840×2160, 68–97 MB) is publicly streamed from `play.wallsflow.com` as the card's hover
  preview. Downloading it from there would sidestep the CAPTCHA and the quota, so the Store doesn't.

What it uses against automated downloads: a CAPTCHA-gated, session-tracked download page with a per-visitor
download limit, and accounts for unlimited downloads.

## Left out entirely

- **mylivewallpapers.com.** A Sucuri/CloudProxy firewall (`Server: Sucuri/Cloudproxy`) answers every
  non-browser request with HTTP 307 and a "You are being redirected…" JavaScript challenge. Even its listings
  can't be read without defeating that bot protection.
- **backiee's live wallpapers.** backiee's web pages offer no download for them; they point to backiee's
  Microsoft Store app. The pages play the original video, but no download is intended there, so the Store
  takes only backiee's still pictures.

## Not protections, but worth knowing

- **backiee:** every picture's page lists 1080p, 4K, 5K and 8K, but only sizes up to the original exist (the
  rest return 404). The Store reads the original size from the page's JSON-LD (`"width"`, `"height"`) and
  downloads the largest real file. The site's own Download button does the same with `data-file-url`.
- **DesktopHut** links the file itself from its Download button (`/files/…`, served by the web server). Its
  robots.txt bans only `/download/` (the mobile crop job, with a 60 s wait) and `/track-download/` (a counter),
  which the Store never asks for.
- **Six sites sit behind Cloudflare** (all but DesktopHut), but only as a CDN with its passive script: plain requests (curl and
  WinHTTP) get the real pages, with no challenge.
