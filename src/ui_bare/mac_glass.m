/* mac_glass.m: the macOS desktop behind the Bare window. Bare copies the desktop only on Windows
 * (bare_window_backdrop); here the window is see-through and macOS's own material sits behind what Bare draws:
 * Liquid Glass (NSGlassEffectView, macOS 26) or else the behind-window blur (NSVisualEffectView).
 * SP_MAC_GLASS=blur picks the blur on macOS 26 too, SP_MAC_GLASS=off the plain background. */
#import <Cocoa/Cocoa.h>
#include <stdlib.h>
#include <string.h>

#define RADIUS 16.0

static const char *mode(void)
{
    const char *m = getenv("SP_MAC_GLASS");
    return m && *m ? m : "glass";
}

int sp_mac_glass_wanted(void)
{
    return strcmp(mode(), "off") != 0;
}

/* The window SDL made for Bare (the only one this program has), behind its content view. */
int sp_mac_glass_attach(void)
{
    if (!sp_mac_glass_wanted()) return 0;
    NSWindow *win = nil;
    for (NSWindow *w in NSApp.windows)
        if (w.contentView) { win = w; break; }
    if (!win) return 0;
    NSView *content = win.contentView, *frame = content.superview;
    if (!frame) return 0;

    NSView *glass = nil;
    Class liquid = NSClassFromString(@"NSGlassEffectView");
    if (liquid && strcmp(mode(), "blur") != 0) {
        glass = [[liquid alloc] initWithFrame:frame.bounds];
        @try { [glass setValue:@(RADIUS) forKey:@"cornerRadius"]; } @catch (NSException *e) { (void)e; }
    } else {
        NSVisualEffectView *v = [[NSVisualEffectView alloc] initWithFrame:frame.bounds];
        v.material = NSVisualEffectMaterialUnderWindowBackground;
        v.blendingMode = NSVisualEffectBlendingModeBehindWindow;
        v.state = NSVisualEffectStateActive;
        v.wantsLayer = YES;
        v.layer.cornerRadius = RADIUS;
        v.layer.masksToBounds = YES;
        glass = v;
    }
    glass.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [frame addSubview:glass positioned:NSWindowBelow relativeTo:content];

    win.opaque = NO;
    win.backgroundColor = NSColor.clearColor;
    win.hasShadow = YES;
    [win invalidateShadow];
    return 1;
}
