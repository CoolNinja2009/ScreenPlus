/* Segoe Fluent/MDL2 glyph codes (GL_* in ui_internal.h) to the matching Fluent System Icons glyph. Generated from the font's
 * own name table; add a line when a new GL_* appears. */
#pragma once
#include <stdint.h>

static inline uint32_t ui_icon_map(uint32_t c)
{
    switch (c) {
    case 0xE8F1: return 0xF4D3;   /* LIBRARY: library */
    case 0xE7F4: return 0xF35A;   /* DISPLAY: desktop */
    case 0xE713: return 0xF6AA;   /* SETTINGS: settings */
    case 0xE74D: return 0xF34D;   /* TRASH: delete */
    case 0xE710: return 0xF10A;   /* ADD: add */
    case 0xE8B7: return 0xF419;   /* FOLDER: folder */
    case 0xE71B: return 0xF4E5;   /* LINK: link */
    case 0xE768: return 0xF606;   /* PLAY: play */
    case 0xE769: return 0xF5A2;   /* PAUSE: pause */
    case 0xE767: return 0xEB43;   /* VOLUME: speaker_2 */
    case 0xE74F: return 0xEB4B;   /* MUTE: speaker_mute */
    case 0xE72C: return 0xF13E;   /* REFRESH: arrow_clockwise */
    case 0xE721: return 0xF690;   /* SEARCH: search */
    case 0xE711: return 0xF36A;   /* CLOSE: dismiss */
    case 0xE73E: return 0xF295;   /* CHECK: checkmark */
    case 0xEB9F: return 0xF489;   /* PHOTO: image */
    case 0xE714: return 0xF84D;   /* VIDEO: video */
    case 0xE943: return 0xF2F0;   /* CODE: code */
    case 0xE7BA: return 0xF86A;   /* WARNING: warning */
    case 0xE946: return 0xF4A4;   /* INFO: info */
    case 0xEA39: return 0xF3F2;   /* ERROR: error_circle */
    case 0xE8A7: return 0xF583;   /* OPEN: open */
    case 0xE8A5: return 0xF379;   /* DOC: document */
    case 0xEC4A: return 0xF831;   /* SPEED: top_speed */
    case 0xE2B1: return 0xF2F6;   /* COLOR: color */
    case 0xE719: return 0xE22F;   /* STORE: building_shop */
    case 0xE896: return 0xF151;   /* DOWNLOAD: arrow_download */
    case 0xE734: return 0xF710;   /* STAR: star */
    case 0xE735: return 0xF710;   /* STAR_ON: star (no filled font) */
    case 0xE8CB: return 0xF18B;   /* SORT: arrow_sort */
    case 0xE70D: return 0xF2A4;   /* chevron down, drawn as a literal in ui_pages.c */
    default: return c;
    }
}
