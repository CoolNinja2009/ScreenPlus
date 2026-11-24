/* sp_kit: Screen+'s liquid-glass controls on Bare. Glass bends the real
 * desktop behind the window (bare_window_backdrop); selections are glass
 * drops that glide between choices on springs, knobs turn to clear glass
 * while held, as in Bare's glass_motion example. */
#pragma once
#include "bare.h"
#include <stdbool.h>

typedef struct SpTheme {
    bool dark;
    float motion;             /* 1 full, 0.5 reduced, 0 off */
    BareColor ink, dim, faint, hair;
    BareColor accent, accent_soft, on_accent;
    BareColor panel_tint, card_tint, control_tint, chrome;
} SpTheme;

extern SpTheme sp;

/* Icons: Segoe Fluent Icons on Windows 11 (MDL2 Assets on 10); nothing elsewhere. */
#define SPI_LIBRARY   "\xEE\xA3\xB1"   /* E8F1 */
#define SPI_DISPLAY   "\xEE\x9F\xB4"   /* E7F4 */
#define SPI_PALETTE   "\xEE\x9E\x90"   /* E790 */
#define SPI_STORE     "\xEE\x9C\x99"   /* E719 */
#define SPI_SETTINGS  "\xEE\x9C\x93"   /* E713 */
#define SPI_SEARCH    "\xEE\x9C\xA1"   /* E721 */
#define SPI_LINK      "\xEE\x9C\x9B"   /* E71B */
#define SPI_FOLDER    "\xEE\xA2\xB7"   /* E8B7 */
#define SPI_ADD       "\xEE\x9C\x90"   /* E710 */
#define SPI_PAUSE     "\xEE\x9D\xA9"   /* E769 */
#define SPI_PLAY      "\xEE\x9D\xA8"   /* E768 */
#define SPI_MUTE      "\xEE\x9D\x8F"   /* E74F */
#define SPI_VOLUME    "\xEE\x9D\xA7"   /* E767 */
#define SPI_REFRESH   "\xEE\x9C\xAC"   /* E72C */
#define SPI_OPEN      "\xEE\xA2\xA7"   /* E8A7 */
#define SPI_CHECK     "\xEE\x9C\xBE"   /* E73E */
#define SPI_STAR      "\xEE\x9C\xB5"   /* E735 */
#define SPI_STAR_LINE "\xEE\x9C\xB4"   /* E734 */
#define SPI_SORT      "\xEE\xA3\x8B"   /* E8CB */
#define SPI_MIN       "\xEE\xA4\xA1"   /* E921 */
#define SPI_MAX       "\xEE\xA4\xA2"   /* E922 */
#define SPI_RESTORE   "\xEE\xA4\xA3"   /* E923 */
#define SPI_CLOSE     "\xEE\xA2\xBB"   /* E8BB */
#define SPI_DOWNLOAD  "\xEE\xA2\x96"   /* E896 */
#define SPI_SYNC      "\xEE\xA2\x95"   /* E895 */
#define SPI_LAYERS    "\xEE\xA0\x9E"   /* E81E */
#define SPI_BRUSH     "\xEE\x9D\xB1"   /* E771 */
#define SPI_GLOBE     "\xEE\x9D\xB4"   /* E774 */
#define SPI_FILM      "\xEE\x9C\x94"   /* E714 */
#define SPI_DROPPER   "\xEE\xBC\xBC"   /* EF3C */
#define SPI_CHEVRON   "\xEE\x9C\x8D"   /* E70D */
#define SPI_TILES     "\xEE\xB2\xA5"   /* ECA5 */
#define SPI_PICTURE   "\xEE\xA2\xB9"   /* E8B9 */
#define SPI_DELETE    "\xEE\x9D\x8D"   /* E74D */
#define SPI_BATTERY   "\xEE\xA0\xBF"   /* E83F */
#define SPI_LOCK      "\xEE\x9C\xAE"   /* E72E */
#define SPI_FULL      "\xEE\x9D\x80"   /* E740 */
#define SPI_EYE       "\xEE\x9E\xB3"   /* E7B3 */
#define SPI_MOUSE     "\xEE\xA5\xA2"   /* E962 */
#define SPI_AUDIO     "\xEE\xA3\x96"   /* E8D6 */
#define SPI_HEAL      "\xEE\x9D\xB7"   /* E777 */
#define SPI_SLIDES    "\xEE\x9E\x86"   /* E786 */
#define SPI_SHUFFLE   "\xEE\xA2\xB1"   /* E8B1 */
#define SPI_CLOCK     "\xEE\xA0\xA3"   /* E823 */
#define SPI_SPEED     "\xEE\xB1\x8A"   /* EC4A */
#define SPI_DOT       "dot"          /* sp_chip: a small glowing dot */
#define SPI_INFO      "\xEE\xA5\x86"   /* E946 */

BareFont sp_icon_font(void);
void     sp_icon(const char *glyph, float size, BareColor color);
void     sp_label(const char *text, float size, BareColor color, int weight);

/* Looks */
BareGlass sp_glass_panel(void);
BareGlass sp_glass_card(void);
BareGlass sp_glass_control(void);
BareGlass sp_glass_accent(void);
BareGlass sp_glass_lens(void);       /* clear, magnifying: a knob being held */
BareAnim  sp_spring(void);           /* layout and looks */
BareAnim  sp_liquid(void);           /* selections gliding: a little overshoot */
BareAnim  sp_fade(void);

enum { SP_BTN_GLASS, SP_BTN_PRIMARY, SP_BTN_ICON, SP_BTN_PLAIN, SP_BTN_CHROME, SP_BTN_DANGER };

/* A glass capsule with an icon and/or label. Returns its state (clicked ...). */
BareState sp_button(BareId id, const char *icon, const char *label, int kind);
/* A chip (small capsule): outlined, or filled when `on`. */
BareState sp_chip(BareId id, const char *icon, const char *label, bool on, BareColor tint);
/* iOS-style switch; the knob turns to glass while held. True when clicked. */
bool sp_toggle(BareId id, bool on);
/* Choices with a glass drop behind the chosen one that glides on a spring and
 * lifts into a clear lens while it moves. `track` draws the capsule behind.
 * Returns the index clicked, else -1. selected < 0: none. */
int  sp_choices(BareId id, const char *const *labels, int count, int selected, bool track, float height);
/* A slider whose knob becomes a lens while dragged. Returns 1 while moving, 2 on release. */
int  sp_slider(BareId id, float *value, float lo, float hi, float width);

/* Pages slide and fade in when `since` (seconds) is small. */
float sp_enter(double since);

void sp_theme_set(bool dark, BareColor accent, int motion_level);
BareColor sp_mix(BareColor a, BareColor b, float t);
BareColor sp_alpha(BareColor c, int a);
