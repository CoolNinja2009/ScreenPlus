/* screenplus-engine for macOS: the counterpart of engine_gtk.c. One borderless window per display at the desktop
 * window level (above the system wallpaper, below the icons, on every Space), showing a wallpaper through
 * AVFoundation (video), NSImageView (pictures, GIFs), WKWebView (pages, and videos AVFoundation can't open: WebM,
 * MKV) or a program of its own (app wallpapers). Like the other engines it reads config.ini itself; the service sends
 * SIGUSR1 to reload it and SIGTERM to stop.
 *  - Look and Sharp upscaling: Core Image on each frame (AVVideoComposition) only while the look isn't neutral; the
 *    filters read the slot's current values under a lock, so a recording changes them without a new composition.
 *  - Pause: the service's pause, app rules, battery, a full-screen (or with pause_when_covered, zoomed) window over
 *    the display, the lock screen, sleeping displays, and macOS saying the window can't be seen (occlusion).
 *  - html_mouse: the icons sit above us and take the pointer, so a global mouse monitor watches it (no permission
 *    needed for mouse events) and, while it is over the bare desktop, raises the events in the page (pointer.js). */
#import <Cocoa/Cocoa.h>
#import <AVFoundation/AVFoundation.h>
#import <CoreImage/CoreImage.h>
#import <WebKit/WebKit.h>
#include <os/lock.h>
#include <signal.h>
#include <limits.h>
#include <locale.h>
#include <math.h>

/* The shared C code's BOOL is compat.h's int; Objective-C has its own. Inside these headers it is SP_BOOL (int, as
 * the C files were built with), and BOOL is Objective-C's again below them. */
#define BOOL SP_BOOL
#include "../linux/desktop.h"
#include "../common/log.h"
#include "../common/paths.h"
#include "../common/util.h"
#include "../common/json.h"
#include "../renderer/html/wallpaper_api.h"
#undef BOOL

static Config g_cfg;
static int    g_appHeld, g_micHeld, g_locked, g_asleep;

/* wallpaper_api.c is shared with the service, where depth.c provides this. */
SP_BOOL upscale_model_ready(void)
{
    wchar_t model[SP_MAX_PATH], runtime[SP_MAX_PATH];
    path_join(model, SP_COUNTOF(model), paths_root(), L"depth/models/upscale/model.onnx");
    path_join(runtime, SP_COUNTOF(runtime), paths_root(), L"depth/ort/libonnxruntime.dylib");
    return file_exists(model) && file_exists(runtime);
}

static NSString *ns_w(const wchar_t *w)
{
    char *u = utf8_from_wide(w, NULL);
    NSString *s = u ? [NSString stringWithUTF8String:u] : @"";
    free(u);
    return s ? s : @"";
}

static NSString *ns_file(NSString *path)
{
    return [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
}

static NSString *cache_file(const wchar_t *name)
{
    wchar_t p[SP_MAX_PATH];
    path_join(p, SP_COUNTOF(p), paths_cache(), name);
    return ns_w(p);
}

/* ------------------------------------------------------------------ look (Core Image) */

typedef struct Look { WallpaperAdjust a; int sharp; } Look;

static int look_neutral(const Look *l) { return !adjust_active(&l->a) && !l->sharp; }

static CIImage *grade(CIImage *img, const Look *l)
{
    const WallpaperAdjust *a = &l->a;
    if (a->brightness || a->contrast || a->saturation)
        img = [img imageByApplyingFilter:@"CIColorControls" withInputParameters:@{
            kCIInputBrightnessKey: @(a->brightness / 200.0), kCIInputContrastKey: @(1.0 + a->contrast / 100.0),
            kCIInputSaturationKey: @(1.0 + a->saturation / 100.0) }];
    if (a->hue) img = [img imageByApplyingFilter:@"CIHueAdjust" withInputParameters:@{ kCIInputAngleKey: @(a->hue * M_PI / 180.0) }];
    if (a->gamma) img = [img imageByApplyingFilter:@"CIGammaAdjust" withInputParameters:@{ @"inputPower": @(pow(2.0, -a->gamma / 100.0)) }];
    if (a->tintLift > 0) {                              /* lift the blacks toward the tint colour */
        double k = a->tintLift / 100.0 * a->tintBrightness / 100.0 * 0.5;
        img = [img imageByApplyingFilter:@"CIColorMatrix" withInputParameters:@{
            @"inputBiasVector": [CIVector vectorWithX:((a->tint >> 16) & 255) / 255.0 * k Y:((a->tint >> 8) & 255) / 255.0 * k
                                                    Z:(a->tint & 255) / 255.0 * k W:0] }];
    }
    if (l->sharp) img = [img imageByApplyingFilter:@"CISharpenLuminance" withInputParameters:@{ kCIInputSharpnessKey: @0.6 }];
    return img;
}

static NSArray<CIFilter *> *look_filters(const Look *l)
{
    NSMutableArray *f = [NSMutableArray array];
    const WallpaperAdjust *a = &l->a;
    CIFilter *c;
    if (a->brightness || a->contrast || a->saturation) {
        c = [CIFilter filterWithName:@"CIColorControls"];
        [c setValue:@(a->brightness / 200.0) forKey:kCIInputBrightnessKey];
        [c setValue:@(1.0 + a->contrast / 100.0) forKey:kCIInputContrastKey];
        [c setValue:@(1.0 + a->saturation / 100.0) forKey:kCIInputSaturationKey];
        [f addObject:c];
    }
    if (a->hue) { c = [CIFilter filterWithName:@"CIHueAdjust"]; [c setValue:@(a->hue * M_PI / 180.0) forKey:kCIInputAngleKey]; [f addObject:c]; }
    if (a->gamma) { c = [CIFilter filterWithName:@"CIGammaAdjust"]; [c setValue:@(pow(2.0, -a->gamma / 100.0)) forKey:@"inputPower"]; [f addObject:c]; }
    if (l->sharp) { c = [CIFilter filterWithName:@"CISharpenLuminance"]; [c setValue:@0.6 forKey:kCIInputSharpnessKey]; [f addObject:c]; }
    return f;
}

/* ------------------------------------------------------------------ slots */

@interface SPSlot : NSObject <WKNavigationDelegate> {
@public
    CGDirectDisplayID display;
    wchar_t        monitor[SP_MONID_LEN];   /* the service's id for it */
    CGRect         geo;                     /* CGDisplayBounds: points, top-left origin */
    wchar_t        wallpaper[SP_ID_LEN];
    int            span, paused, occluded, appPid;
    JVal          *sent;
    Rec            rec[REC_SECTIONS];
    WallpaperAdjust recBase;
    JVal          *recProps;
    Look           look;
    os_unfair_lock lookLock;
    CGSize         media;                   /* the picture's size once known */
    FILE          *appIn;                   /* app wallpaper: its stdin */
}
@property (strong) NSWindow *win;
@property (strong) AVPlayer *player;
@property (strong) id loopToken;
@property (strong) AVPlayerLayer *playerLayer;
@property (strong) AVPlayerItem *item;
@property (strong) NSImageView *picture;
@property (strong) WKWebView *web;
@property (strong) NSTask *app;
@property (assign) BOOL webVideo;           /* a video played by WebKit */
@end

static SPSlot *g_slots[SP_MAX_MONITORS];

void slot_web_video(SPSlot *s);

static void slot_layout(SPSlot *s, const WallpaperEntry *e);
static void slot_set_sound(SPSlot *s, int on);
static void web_send_properties(SPSlot *s, const WallpaperEntry *e);

@implementation SPSlot
- (instancetype)init
{
    if ((self = [super init])) lookLock = OS_UNFAIR_LOCK_INIT;
    return self;
}

- (void)webView:(WKWebView *)v didFinishNavigation:(WKNavigation *)nav
{
    const WallpaperEntry *e = config_find(&g_cfg, wallpaper);
    if (e && self.web && !self.webVideo) web_send_properties(self, e);
    if (e) slot_set_sound(self, g_cfg.s.audio && !g_micHeld);
}

- (void)webView:(WKWebView *)v didFailProvisionalNavigation:(WKNavigation *)nav withError:(NSError *)err
{
    NSURL *failed = err.userInfo[NSURLErrorFailingURLErrorKey];
    LOGW(L"engine: page did not load (%s): %s", failed.absoluteString.UTF8String ? failed.absoluteString.UTF8String : "",
         err.localizedDescription.UTF8String);
    const WallpaperEntry *e = config_find(&g_cfg, wallpaper);
    if (e && [failed.host hasSuffix:@".localhost"]) {   /* the web host's name didn't resolve: the file, as before 2.0 */
        char *uri = sp_file_uri(e->path);
        NSURL *u = uri ? [NSURL URLWithString:[NSString stringWithUTF8String:uri]] : nil;
        free(uri);
        if (u.isFileURL) [v loadFileURL:u allowingReadAccessToURL:u.URLByDeletingLastPathComponent];
    }
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)v
{
    LOGW(L"engine: a page's process ended; loading it again");
    [v reload];
}

- (void)observeValueForKeyPath:(NSString *)key ofObject:(id)obj change:(NSDictionary *)change context:(void *)ctx
{
    AVPlayerItem *it = (AVPlayerItem *)obj;
    dispatch_async(dispatch_get_main_queue(), ^{
        if (it != self.item) return;
        if (it.status == AVPlayerItemStatusReadyToPlay && self->media.width <= 0) {
            self->media = it.presentationSize;
            const WallpaperEntry *e = config_find(&g_cfg, self->wallpaper);
            if (e) slot_layout(self, e);
        } else if (it.status == AVPlayerItemStatusFailed) {
            LOGW(L"engine: AVFoundation can't play this video (%s); playing it in WebKit", it.error.localizedDescription.UTF8String);
            slot_web_video(self);
        }
    });
}
@end

/* ------------------------------------------------------------------ video */

static void video_look(SPSlot *s, const WallpaperEntry *e)
{
    Look l = { adjust_defaults(), 0 };
    wpapi_load_adjust(e->id, &l.a);
    l.sharp = wpapi_load_upscale(e->id) == UPSCALE_SHARP;
    os_unfair_lock_lock(&s->lookLock);
    int was = !look_neutral(&s->look), now = !look_neutral(&l);
    s->look = l;
    os_unfair_lock_unlock(&s->lookLock);
    if (s.webVideo && s.web) {                          /* CSS's filters, near enough */
        NSString *css = [NSString stringWithFormat:@"document.querySelector('video').style.filter='brightness(%.3f) contrast(%.3f) saturate(%.3f) hue-rotate(%ddeg)'",
                         1.0 + l.a.brightness / 100.0, 1.0 + l.a.contrast / 100.0, 1.0 + l.a.saturation / 100.0, l.a.hue];
        [s.web evaluateJavaScript:css completionHandler:nil];
        return;
    }
    if (s.picture) {
        s.picture.layer.filters = look_filters(&l);
        return;
    }
    if (!s.item) return;
    s.item.videoComposition = nil;
    if (now) {                                          /* the composition only exists while it does something */
        __weak SPSlot *ws = s;
        s.item.videoComposition = [AVVideoComposition videoCompositionWithAsset:s.item.asset
            applyingCIFiltersWithHandler:^(AVAsynchronousCIImageFilteringRequest *req) {
                SPSlot *slot = ws;
                Look cur = { adjust_defaults(), 0 };
                if (slot) { os_unfair_lock_lock(&slot->lookLock); cur = slot->look; os_unfair_lock_unlock(&slot->lookLock); }
                CIImage *src = req.sourceImage;
                [req finishWithImage:[grade(src.imageByClampingToExtent, &cur) imageByCroppingToRect:src.extent] context:nil];
            }];
    }
    if ((was || now) && s->paused) [s.item seekToTime:s.item.currentTime completionHandler:nil];   /* show it on the still frame */
}

static void slot_layout(SPSlot *s, const WallpaperEntry *e)
{
    WallpaperLayout l;
    wpapi_load_layout(e->id, &l);
    NSView *root = s.win.contentView;
    CGSize box = root.bounds.size;
    CGColorRef bg = l.fill == LFILL_COLOR && !s->span
        ? CGColorCreateSRGB(((l.color >> 16) & 255) / 255.0, ((l.color >> 8) & 255) / 255.0, (l.color & 255) / 255.0, 1)
        : CGColorCreateSRGB(0, 0, 0, 1);
    root.layer.backgroundColor = bg;
    CGColorRelease(bg);
    CGRect frame;                                       /* top left first, like the layout's rect */
    if (s->span) {                                      /* this display's share of the box around all of them */
        CGRect all = CGRectNull;
        for (int i = 0; i < SP_MAX_MONITORS; i++) if (g_slots[i] && g_slots[i].win) all = CGRectUnion(all, g_slots[i]->geo);
        frame = CGRectMake(all.origin.x - s->geo.origin.x, all.origin.y - s->geo.origin.y, all.size.width, all.size.height);
    } else {
        float r[4];
        double px = s.win.backingScaleFactor > 0 ? s.win.backingScaleFactor : 1;   /* media in pixels, the box in points */
        layout_rect(&l, s->media.width / px, s->media.height / px, box.width, box.height, r);
        frame = CGRectMake(r[0] * box.width, r[1] * box.height, r[2] * box.width, r[3] * box.height);
    }
    frame.origin.y = box.height - frame.origin.y - frame.size.height;   /* AppKit's y goes up */
    if (s.playerLayer) {
        s.playerLayer.videoGravity = s->span || l.mode == LAYOUT_FILL ? AVLayerVideoGravityResizeAspectFill : AVLayerVideoGravityResize;
        s.playerLayer.frame = !s->span && l.mode == LAYOUT_FILL && s->media.width <= 0 ? root.bounds : frame;
    }
    if (s.picture) {
        s.picture.frame = frame;
        s.picture.layer.magnificationFilter = wpapi_load_upscale(e->id) == UPSCALE_OFF ? kCAFilterLinear : kCAFilterTrilinear;
    }
}

static void slot_video(SPSlot *s, const WallpaperEntry *e)
{
    char *uri = sp_file_uri(e->path);
    NSURL *url = uri ? [NSURL URLWithString:[NSString stringWithUTF8String:uri]] : nil;
    free(uri);
    if (!url) return;
    s.item = [AVPlayerItem playerItemWithURL:url];
    s.player = [AVPlayer playerWithPlayerItem:s.item];
    s.player.preventsDisplaySleepDuringVideoPlayback = NO;
    s.player.actionAtItemEnd = AVPlayerActionAtItemEndNone;
    __weak AVPlayer *wp = s.player;                     /* one item looped by seeking back, so its composition is the one that plays */
    s.loopToken = [NSNotificationCenter.defaultCenter addObserverForName:AVPlayerItemDidPlayToEndTimeNotification object:s.item
                                                                   queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
        [wp seekToTime:kCMTimeZero toleranceBefore:kCMTimeZero toleranceAfter:kCMTimeZero completionHandler:^(BOOL done) { (void)done; }];
    }];
    [s.item addObserver:s forKeyPath:@"status" options:0 context:NULL];
    s.playerLayer = [AVPlayerLayer playerLayerWithPlayer:s.player];
    [s.win.contentView.layer addSublayer:s.playerLayer];
    video_look(s, e);
    slot_layout(s, e);
    [s.player play];
}

static void slot_picture(SPSlot *s, const WallpaperEntry *e)
{
    NSImage *img = [[NSImage alloc] initWithContentsOfFile:ns_w(e->path)];
    if (!img) return;
    NSImageRep *rep = img.representations.firstObject;
    s->media = rep && rep.pixelsWide > 0 ? CGSizeMake(rep.pixelsWide, rep.pixelsHigh) : img.size;
    s.picture = [NSImageView imageViewWithImage:img];
    s.picture.imageScaling = NSImageScaleAxesIndependently;
    s.picture.animates = YES;                           /* GIF, APNG */
    s.picture.wantsLayer = YES;
    [s.win.contentView addSubview:s.picture];
    video_look(s, e);
    slot_layout(s, e);
}

/* ------------------------------------------------------------------ web */

static void web_send_properties(SPSlot *s, const WallpaperEntry *e)
{
    char *js = wpapi_properties_js(e, &s->sent);
    if (js) [s.web evaluateJavaScript:[NSString stringWithUTF8String:js] completionHandler:nil];
    free(js);
}

/* The service's local web host for this page (webhost.c), else the file. */
static NSURL *page_url(const WallpaperEntry *e)
{
    NSString *t = ns_file(cache_file(L"webhost.txt"));
    char token[32];
    int port = 0;
    if (t && !wallpaper_is_url(e->path) && sscanf(t.UTF8String, "%d %31s", &port, token) == 2) {
        NSString *name = [ns_w(path_filename(e->path)) stringByAddingPercentEncodingWithAllowedCharacters:NSCharacterSet.URLPathAllowedCharacterSet];
        NSURL *u = [NSURL URLWithString:[NSString stringWithFormat:@"http://%s-%@.localhost:%d/%@", token, ns_w(e->id), port, name]];
        if (u) return u;
    }
    char *uri = sp_file_uri(e->path);
    NSURL *u = uri ? [NSURL URLWithString:[NSString stringWithUTF8String:uri]] : nil;
    free(uri);
    return u;
}

static void add_script(WKUserContentController *uc, NSString *src, WKUserScriptInjectionTime when)
{
    if (src.length) [uc addUserScript:[[WKUserScript alloc] initWithSource:src injectionTime:when forMainFrameOnly:YES]];
}

static WKWebView *make_web(SPSlot *s, int page)
{
    WKWebViewConfiguration *c = [WKWebViewConfiguration new];
    c.mediaTypesRequiringUserActionForPlayback = WKAudiovisualMediaTypeNone;
    [c.preferences setValue:@YES forKey:@"allowFileAccessFromFileURLs"];
    if (page) {
        WKUserContentController *uc = c.userContentController;
        char *shim = utf8_from_wide(wpapi_shim_js(), NULL);
        if (shim) add_script(uc, [NSString stringWithUTF8String:shim], WKUserScriptInjectionTimeAtDocumentStart);
        free(shim);
        char limit[640];
        sp_fps_js(g_cfg.s.webFps, limit, sizeof limit);
        add_script(uc, [NSString stringWithUTF8String:limit], WKUserScriptInjectionTimeAtDocumentStart);
        NSString *res = [NSBundle.mainBundle.resourcePath stringByAppendingPathComponent:@"pointer.js"];
        add_script(uc, ns_file(res), WKUserScriptInjectionTimeAtDocumentStart);
        add_script(uc, ns_file(cache_file(L"feed.js")), WKUserScriptInjectionTimeAtDocumentEnd);
    }
    WKWebView *w = [[WKWebView alloc] initWithFrame:s.win.contentView.bounds configuration:c];
    w.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [w setValue:@NO forKey:@"drawsBackground"];
    w.navigationDelegate = s;
    [s.win.contentView addSubview:w];
    return w;
}

static void slot_page(SPSlot *s, const WallpaperEntry *e)
{
    s.web = make_web(s, 1);
    NSURL *u = page_url(e);
    if (u.isFileURL) [s.web loadFileURL:u allowingReadAccessToURL:u.URLByDeletingLastPathComponent];
    else if (u) [s.web loadRequest:[NSURLRequest requestWithURL:u]];
}

/* A video AVFoundation can't open (WebM, MKV...): WebKit plays it in a page of its own. */
void slot_web_video(SPSlot *s)
{
    const WallpaperEntry *e = config_find(&g_cfg, s->wallpaper);
    if (!e || s.webVideo) return;
    [s.item removeObserver:s forKeyPath:@"status"];
    if (s.loopToken) [NSNotificationCenter.defaultCenter removeObserver:s.loopToken];
    [s.player pause];
    [s.playerLayer removeFromSuperlayer];
    s.player = nil; s.loopToken = nil; s.item = nil; s.playerLayer = nil;
    char *uri = sp_file_uri(e->path);
    NSString *page = [NSString stringWithFormat:@"<!doctype html><style>html,body{margin:0;height:100%%;background:#000;overflow:hidden}"
                      "video{width:100%%;height:100%%;object-fit:cover}</style><video src=\"%s\" autoplay loop muted playsinline></video>", uri ? uri : ""];
    free(uri);
    NSString *file = cache_file(L"video.html");
    file = [file stringByReplacingOccurrencesOfString:@"video.html" withString:[NSString stringWithFormat:@"video-%@.html", ns_w(e->id)]];
    [page writeToFile:file atomically:YES encoding:NSUTF8StringEncoding error:nil];
    s.webVideo = YES;
    s.web = make_web(s, 0);
    [s.web loadFileURL:[NSURL fileURLWithPath:file] allowingReadAccessToURL:[NSURL fileURLWithPath:@"/"]];
    video_look(s, e);
}

/* ------------------------------------------------------------------ app wallpapers */

/* The program draws itself: it gets the display's rectangle (points, top left) and puts its window at the desktop
 * level there (Bare's `.parent` on macOS); properties, pause and volume reach it as JSON lines on stdin. */
static void slot_app(SPSlot *s, const WallpaperEntry *e)
{
    NSTask *t = [NSTask new];
    t.executableURL = [NSURL fileURLWithPath:ns_w(e->path)];
    t.currentDirectoryURL = t.executableURL.URLByDeletingLastPathComponent;
    t.arguments = @[ @"--screenplus-desktop",
                     [NSString stringWithFormat:@"%d,%d,%d,%d", (int)s->geo.origin.x, (int)s->geo.origin.y, (int)s->geo.size.width, (int)s->geo.size.height],
                     @"--screenplus-size", [NSString stringWithFormat:@"%dx%d", (int)s->geo.size.width, (int)s->geo.size.height] ];
    NSPipe *in = [NSPipe pipe];
    t.standardInput = in;
    NSError *err = nil;
    if (![t launchAndReturnError:&err]) {
        LOGE(L"engine: could not start '%ls': %s", e->name, err.localizedDescription.UTF8String);
        return;
    }
    s.app = t;
    s->appIn = fdopen(dup(in.fileHandleForWriting.fileDescriptor), "w");
    [in.fileHandleForWriting closeFile];
    char *lines = wpapi_properties_lines(e, &s->sent);
    if (lines && s->appIn) { fputs(lines, s->appIn); fflush(s->appIn); }
    free(lines);
}

static void app_line(SPSlot *s, const char *line)
{
    if (s->appIn && fputs(line, s->appIn) >= 0) fflush(s->appIn);
}

/* ------------------------------------------------------------------ recordings (played from memory) */

static NSTimer *g_recTimer;

static void rec_unload(SPSlot *s)
{
    for (int k = 0; k < REC_SECTIONS; k++) wprec_free(&s->rec[k]);
    json_free(s->recProps);
    s->recProps = NULL;
    if (s->wallpaper[0]) wpapi_live_set(s->wallpaper, NULL, NULL);
}

static void rec_load(SPSlot *s)
{
    rec_unload(s);
    if (!s->wallpaper[0]) return;
    wpapi_load_adjust(s->wallpaper, &s->recBase);
    for (int k = 0; k < REC_SECTIONS; k++)
        if (wprec_load(s->wallpaper, k, &s->rec[k]) && s->rec[k].paused > 0) wprec_free(&s->rec[k]);
}

static int rec_playing(SPSlot *s) { return s && s.win && !s->paused && (s->rec[REC_LOOK].n || s->rec[REC_PROPS].n || s->rec[REC_VOLUME].n); }

static void rec_tick(void)
{
    static unsigned ticks;
    double now = wprec_now();
    int props = ++ticks % 3 == 0, any = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        SPSlot *s = g_slots[i];
        if (!rec_playing(s)) continue;
        any = 1;
        Rec *look = &s->rec[REC_LOOK], *pr = &s->rec[REC_PROPS];
        WallpaperAdjust a = s->recBase;
        if (s->rec[REC_VOLUME].n) slot_set_sound(s, g_cfg.s.audio && !g_micHeld);
        if (look->n) wprec_look(look, wprec_pos(look, now), &a);
        int sendProps = pr->n && (props || !s->recProps);
        if (sendProps) {
            StrBuf b = { 0 };
            double pos = wprec_pos(pr, now);
            sb_puts(&b, "{");
            for (int k = 0; k < pr->count; k++) {
                JVal *v = wprec_value(&pr->t[k], pr->n, pos);
                if (!v) continue;
                if (b.len > 1) sb_puts(&b, ",");
                json_write_string(&b, pr->t[k].key);
                sb_puts(&b, ":");
                json_write(&b, v);
                json_free(v);
            }
            sb_puts(&b, "}");
            json_free(s->recProps);
            s->recProps = b.failed ? NULL : json_parse(b.p, b.len);
            sb_free(&b);
        }
        if (!look->n && !sendProps) continue;
        wpapi_live_set(s->wallpaper, look->n ? &a : NULL, s->recProps);
        WallpaperEntry *e = config_find(&g_cfg, s->wallpaper);
        if (!e) continue;
        if (look->n && !s.web) video_look(s, e);
        if (s.web && sendProps && !s.webVideo) web_send_properties(s, e);
    }
    if (!any) { [g_recTimer invalidate]; g_recTimer = nil; }
}

static void rec_schedule(void)
{
    int any = 0;
    for (int i = 0; i < SP_MAX_MONITORS; i++) any |= rec_playing(g_slots[i]);
    if (any && !g_recTimer) g_recTimer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 30 repeats:YES block:^(NSTimer *t) { rec_tick(); }];
}

/* ------------------------------------------------------------------ slot state */

static void slot_clear(SPSlot *s)
{
    if (!s) return;
    rec_unload(s);
    if (s.item) [s.item removeObserver:s forKeyPath:@"status"];
    if (s.loopToken) [NSNotificationCenter.defaultCenter removeObserver:s.loopToken];
    s.loopToken = nil;
    [s.player pause];
    s.player = nil;
    s.item = nil;
    [s.playerLayer removeFromSuperlayer];
    s.playerLayer = nil;
    [s.picture removeFromSuperview];
    s.picture = nil;
    if (s.web) { [s.web stopLoading]; [s.web removeFromSuperview]; s.web = nil; }
    if (s.app) {
        if (s->appIn) fclose(s->appIn);
        s->appIn = NULL;
        [s.app terminate];
        s.app = nil;
    }
    [s.win orderOut:nil];
    s.win = nil;
    json_free(s->sent);
    s->sent = NULL;
    s->wallpaper[0] = 0;
    s->paused = 0;
    s->media = CGSizeZero;
    s.webVideo = NO;
}

static NSScreen *screen_for(CGDirectDisplayID d)
{
    for (NSScreen *sc in NSScreen.screens)
        if ([sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == d) return sc;
    return nil;
}

static void slot_open(SPSlot *s, NSScreen *screen, const WallpaperEntry *e, int span)
{
    s->span = span;
    str_copy(s->wallpaper, SP_COUNTOF(s->wallpaper), e->id);
    if (e->type == WP_APP) {                            /* the program's own window */
        slot_app(s, e);
        if (s.app) LOGI(L"engine: app '%ls' on %ls", e->name, s->monitor);
        return;
    }
    NSWindow *w = [[NSWindow alloc] initWithContentRect:screen.frame styleMask:NSWindowStyleMaskBorderless
                                                backing:NSBackingStoreBuffered defer:NO];
    w.level = CGWindowLevelForKey(kCGDesktopWindowLevelKey);
    w.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary |
                           NSWindowCollectionBehaviorIgnoresCycle | NSWindowCollectionBehaviorFullScreenNone;
    w.ignoresMouseEvents = YES;
    w.hasShadow = NO;
    w.opaque = YES;
    w.backgroundColor = NSColor.blackColor;
    w.releasedWhenClosed = NO;
    w.animationBehavior = NSWindowAnimationBehaviorNone;
    w.contentView.wantsLayer = YES;
    w.contentView.layerUsesCoreImageFilters = YES;
    [w setFrame:screen.frame display:NO];
    s.win = w;
    if (e->type == WP_HTML) slot_page(s, e);
    else if (e->type == WP_IMAGE) slot_picture(s, e);
    else slot_video(s, e);
    [w orderFront:nil];
    LOGI(L"engine: '%ls' on %ls", e->name, s->monitor);
}

static void slot_set_paused(SPSlot *s, int paused)
{
    if (s->paused == paused) return;
    s->paused = paused;
    if (s.player) { if (paused) [s.player pause]; else [s.player play]; }
    if (s.web) {
        s.web.hidden = paused;                          /* a hidden page stops drawing */
        if (@available(macOS 12.0, *)) [s.web setAllMediaPlaybackSuspended:paused completionHandler:nil];
    }
    if (s.picture) s.picture.animates = !paused;
    if (s.app) app_line(s, paused ? "{\"type\":\"pause\",\"value\":true}\n" : "{\"type\":\"pause\",\"value\":false}\n");
}

/* on: Sound allowed. The volume is the global one times the wallpaper's own. */
static void slot_set_sound(SPSlot *s, int on)
{
    const WallpaperEntry *e = config_find(&g_cfg, s->wallpaper);
    int own = e ? entry_volume(e) : 100;
    Rec *vr = &s->rec[REC_VOLUME];
    if (vr->n && vr->count) own = SP_CLAMP((int)lround(wprec_num(&vr->t[0], vr->n, wprec_pos(vr, wprec_now()))), 0, 100);
    int vol = SP_CLAMP(g_cfg.s.volume, 0, 100) * own / 100;
    if (!vol) on = 0;
    if (s.player) {
        s.player.volume = vol / 100.0f;
        s.player.muted = !on;
    }
    if (s.web) {
        NSString *js = [NSString stringWithFormat:@"(function(v,m){document.querySelectorAll('audio,video').forEach(function(e){"
                        "if(e.__spBase===undefined)e.__spBase=e.volume;e.volume=e.__spBase*v;e.muted=m;});})(%.4f,%s)",
                        vol * vol / 10000.0, on ? "false" : "true"];
        [s.web evaluateJavaScript:js completionHandler:nil];
        SEL mute = NSSelectorFromString(@"_setPageMuted:");             /* Web Audio too; WebKit has no public switch */
        if ([s.web respondsToSelector:mute]) {
            NSUInteger state = on ? 0 : 1;
            NSInvocation *inv = [NSInvocation invocationWithMethodSignature:[s.web methodSignatureForSelector:mute]];
            inv.selector = mute;
            [inv setArgument:&state atIndex:2];
            [inv invokeWithTarget:s.web];
        }
    }
    if (s.app) {
        char line[64];
        snprintf(line, sizeof line, "{\"type\":\"volume\",\"value\":%d}\n", on ? vol : 0);
        app_line(s, line);
    }
}

static int should_pause(SPSlot *s)
{
    static int battery;
    static double checked;
    double now = time_ms();
    if (now - checked > 2000) {
        battery = sp_on_battery();
        checked = now;
    }
    return g_cfg.s.paused || g_appHeld || g_asleep || (g_cfg.s.pauseOnFullscreen && s->occluded) || (g_cfg.s.pauseOnBattery && battery)
        || (g_cfg.s.suspendOnLock && g_locked)
        || (g_cfg.s.pauseOnFullscreen && mac_display_covered(s->display, g_cfg.s.pauseWhenCovered));
}

static void tick(void)
{
    int held = g_cfg.s.pauseApps[0] && sp_pid_matches(mac_front_pid(), g_cfg.s.pauseApps);
    if (held != g_appHeld) LOGI(L"app rules: %ls", held ? L"a pause app is in front; pausing" : L"pause app left; resuming");
    g_appHeld = held;
    for (int i = 0; i < SP_MAX_MONITORS; i++) {
        SPSlot *s = g_slots[i];
        if (!s || (!s.win && !s.app)) continue;
        if (s.app && !s.app.running) {
            LOGW(L"engine: the app wallpaper on %ls ended (status %d)", s->monitor, s.app.terminationStatus);
            if (s->appIn) fclose(s->appIn);
            s->appIn = NULL;
            s.app = nil;
            continue;
        }
        slot_set_paused(s, should_pause(s));
    }
    rec_schedule();
}

/* Makes the windows match config.ini and the connected displays. */
static void sync_slots(void)
{
    CGDirectDisplayID ids[SP_MAX_MONITORS];
    uint32_t n = 0;
    CGGetActiveDisplayList(SP_MAX_MONITORS, ids, &n);
    int span = g_cfg.s.displayMode == DM_SPAN && n > 1;
    CGDirectDisplayID seen[SP_MAX_MONITORS];
    int seenCount = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (CGDisplayIsInMirrorSet(ids[i]) && CGDisplayMirrorsDisplay(ids[i]) != kCGNullDirectDisplay) continue;
        NSScreen *screen = screen_for(ids[i]);
        if (!screen) continue;
        SPSlot *s = nil;
        int freeAt = -1;
        for (int k = 0; k < SP_MAX_MONITORS && !s; k++) {
            if (g_slots[k] && g_slots[k]->display == ids[i]) s = g_slots[k];
            else if (!g_slots[k] && freeAt < 0) freeAt = k;
        }
        if (!s && freeAt >= 0) {
            s = g_slots[freeAt] = [SPSlot new];
            s->display = ids[i];
        }
        if (!s) continue;
        seen[seenCount++] = ids[i];
        mac_display_id(ids[i], s->monitor, SP_COUNTOF(s->monitor));
        const wchar_t *wp = config_assignment(&g_cfg, s->monitor);
        if (!wp[0]) {                                   /* saved before ids were EDID ones */
            wchar_t old[SP_MONID_LEN];
            str_printf(old, SP_COUNTOF(old), L"CG:%u", ids[i]);
            if (config_saved_for(&g_cfg, old)) wp = config_assignment(&g_cfg, old);
        }
        WallpaperEntry *e = wp[0] ? config_find(&g_cfg, wp) : NULL;
        int useSpan = span && e && e->type == WP_VIDEO;  /* pages, pictures and programs repeat on each display */
        CGRect g = CGDisplayBounds(ids[i]);
        int moved = !CGRectEqualToRect(g, s->geo);
        if (!e || !str_ieq(s->wallpaper, e->id) || s->span != useSpan || moved) {
            slot_clear(s);
            s->geo = g;
            if (e) slot_open(s, screen, e, useSpan);
        } else if (s.web && !s.webVideo) {
            web_send_properties(s, e);
        } else if (s.app) {
            char *lines = wpapi_properties_lines(e, &s->sent);
            if (lines) app_line(s, lines);
            free(lines);
        } else {
            video_look(s, e);
            slot_layout(s, e);
        }
        if (s.win || s.app) {
            slot_set_sound(s, g_cfg.s.audio && !g_micHeld);
            rec_load(s);                                /* a recording made, edited or deleted */
        }
    }
    for (int i = 0; i < SP_MAX_MONITORS; i++) {         /* unplugged displays */
        SPSlot *s = g_slots[i];
        int found = !s;
        for (int k = 0; k < seenCount && !found; k++) found = s->display == seen[k];
        if (!found) { slot_clear(s); g_slots[i] = nil; }
    }
    if (span) for (int i = 0; i < SP_MAX_MONITORS; i++) {    /* the box is known only now */
        SPSlot *s = g_slots[i];
        const WallpaperEntry *e = s && s->span ? config_find(&g_cfg, s->wallpaper) : NULL;
        if (e) slot_layout(s, e);
    }
    tick();
}

/* ------------------------------------------------------------------ microphone (mute_apps) */

static dispatch_source_t g_micSource;

static void mic_check(void)
{
    int busy = sp_mic_busy(g_cfg.s.muteApps);
    if (busy == g_micHeld) return;
    LOGI(L"app rules: %ls", busy ? L"an app is using the microphone; sound muted" : L"microphone free; sound back");
    g_micHeld = busy;
    for (int i = 0; i < SP_MAX_MONITORS; i++)
        if (g_slots[i] && (g_slots[i].win || g_slots[i].app)) slot_set_sound(g_slots[i], g_cfg.s.audio && !g_micHeld);
}

static void mic_watch(void)
{
    int want = g_cfg.s.muteApps[0] != 0, fd;
    if (want && !g_micSource && (fd = sp_mic_watch_start()) >= 0) {
        g_micSource = dispatch_source_create(DISPATCH_SOURCE_TYPE_READ, (uintptr_t)fd, 0, dispatch_get_main_queue());
        dispatch_source_set_event_handler(g_micSource, ^{ if (sp_mic_watch_read() > 0) mic_check(); });
        dispatch_resume(g_micSource);
    } else if (!want && g_micSource) {
        dispatch_source_cancel(g_micSource);
        g_micSource = nil;
        sp_mic_watch_stop();
    }
}

/* ------------------------------------------------------------------ the pointer (html_mouse) */

static id g_mouseMonitor;

/* The window under the pointer is the desktop itself (icons or nothing), not an app's. */
static int over_desktop(NSPoint p)
{
    NSInteger num = [NSWindow windowNumberAtPoint:p belowWindowWithWindowNumber:0];
    CFArrayRef ids = CFArrayCreate(NULL, (const void **)&num, 1, NULL);
    CFArrayRef info = ids ? CGWindowListCreateDescriptionFromArray(ids) : NULL;
    int layer = INT_MIN;                                /* nothing there: the desktop */
    if (info && CFArrayGetCount(info)) {
        CFNumberRef l = CFDictionaryGetValue(CFArrayGetValueAtIndex(info, 0), kCGWindowLayer);
        if (l) CFNumberGetValue(l, kCFNumberIntType, &layer);
    }
    if (info) CFRelease(info);
    if (ids) CFRelease(ids);
    return layer <= CGWindowLevelForKey(kCGDesktopIconWindowLevelKey);
}

static void pointer_event(NSEvent *ev)
{
    static SPSlot *last;
    NSPoint p = NSEvent.mouseLocation;
    const char *type = ev.type == NSEventTypeLeftMouseDown ? "down" : ev.type == NSEventTypeLeftMouseUp ? "up" : "move";
    SPSlot *hit = nil;
    for (int i = 0; i < SP_MAX_MONITORS && !hit; i++) {
        SPSlot *s = g_slots[i];
        if (s.web && !s.webVideo && !s->paused && NSPointInRect(p, s.win.frame)) hit = s;
    }
    if (hit && !over_desktop(p)) hit = nil;
    if (last && last != hit) [last.web evaluateJavaScript:@"window.__spPointer&&__spPointer('leave',0,0,0,0)" completionHandler:nil];
    last = hit;
    if (!hit) return;
    NSRect f = hit.win.frame;
    int buttons = (int)NSEvent.pressedMouseButtons & 1;
    NSString *js = [NSString stringWithFormat:@"window.__spPointer&&__spPointer('%s',%.1f,%.1f,0,%d)", type, p.x - f.origin.x, NSMaxY(f) - p.y, buttons];
    [hit.web evaluateJavaScript:js completionHandler:nil];
}

static void mouse_watch(void)
{
    if (g_cfg.s.htmlMouse && !g_mouseMonitor)
        g_mouseMonitor = [NSEvent addGlobalMonitorForEventsMatchingMask:NSEventMaskMouseMoved | NSEventMaskLeftMouseDown |
                                                                        NSEventMaskLeftMouseUp | NSEventMaskLeftMouseDragged
                                                                 handler:^(NSEvent *ev) { pointer_event(ev); }];
    else if (!g_cfg.s.htmlMouse && g_mouseMonitor) {
        [NSEvent removeMonitor:g_mouseMonitor];
        g_mouseMonitor = nil;
    }
}

/* ------------------------------------------------------------------ main */

static void reload(void)
{
    config_free(&g_cfg);
    memset(&g_cfg, 0, sizeof g_cfg);
    config_load(&g_cfg, paths_config());
    mic_watch();
    g_micHeld = sp_mic_busy(g_cfg.s.muteApps);
    mouse_watch();
    sync_slots();
}

static void quit(void)
{
    for (int i = 0; i < SP_MAX_MONITORS; i++) slot_clear(g_slots[i]);   /* app wallpapers' programs too */
    log_shutdown();
    exit(0);
}

static dispatch_source_t on_signal(int sig, void (^fn)(void))
{
    signal(sig, SIG_IGN);
    dispatch_source_t d = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, (uintptr_t)sig, 0, dispatch_get_main_queue());
    dispatch_source_set_event_handler(d, fn);
    dispatch_resume(d);
    return d;
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    @autoreleasepool {
        setlocale(LC_CTYPE, "");
        paths_init();
        log_init(L"engine");
        NSApplication *app = NSApplication.sharedApplication;
        app.activationPolicy = NSApplicationActivationPolicyAccessory;      /* no Dock icon or menu bar */

        static dispatch_source_t sigs[3];
        sigs[0] = on_signal(SIGUSR1, ^{ reload(); });
        sigs[1] = on_signal(SIGTERM, ^{ quit(); });
        sigs[2] = on_signal(SIGINT, ^{ quit(); });

        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter, *ws = NSWorkspace.sharedWorkspace.notificationCenter;
        NSDistributedNotificationCenter *dc = NSDistributedNotificationCenter.defaultCenter;
        [nc addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil
                    usingBlock:^(NSNotification *n) { sync_slots(); }];
        [nc addObserverForName:NSWindowDidChangeOcclusionStateNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            for (int i = 0; i < SP_MAX_MONITORS; i++) {
                SPSlot *s = g_slots[i];
                if (!s || s.win != n.object) continue;
                int hidden = !(s.win.occlusionState & NSWindowOcclusionStateVisible);
                if (hidden != s->occluded) LOGI(L"engine: %ls is %ls", s->monitor, hidden ? L"hidden; pausing" : L"visible again");
                s->occluded = hidden;
            }
            tick();
        }];
        [ws addObserverForName:NSWorkspaceDidActivateApplicationNotification object:nil queue:nil usingBlock:^(NSNotification *n) { tick(); }];
        [ws addObserverForName:NSWorkspaceScreensDidSleepNotification object:nil queue:nil usingBlock:^(NSNotification *n) { g_asleep = 1; tick(); }];
        [ws addObserverForName:NSWorkspaceScreensDidWakeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { g_asleep = 0; tick(); }];
        [ws addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { tick(); }];
        [dc addObserverForName:@"com.apple.screenIsLocked" object:nil queue:nil usingBlock:^(NSNotification *n) { g_locked = 1; tick(); }];
        [dc addObserverForName:@"com.apple.screenIsUnlocked" object:nil queue:nil usingBlock:^(NSNotification *n) { g_locked = 0; tick(); }];
        g_locked = mac_session_locked();
        [NSTimer scheduledTimerWithTimeInterval:1.0 repeats:YES block:^(NSTimer *t) { tick(); }];

        reload();
        [app run];
        quit();
    }
    return 0;
}
