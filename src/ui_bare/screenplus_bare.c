#include "screenplus_bare.h"
#include <ctype.h>
#include <stdio.h>
#include <string.h>

static BareColor ink(void) { return BARE_RGB(244, 250, 247); }
static BareColor dim(void) { return BARE_RGBA(214, 228, 225, 180); }
static BareColor green(void) { return BARE_RGB(0, 215, 80); }
static BareAnim ui_motion;
static float ui_lift;

static void send(SpBareDispatch dispatch, void *user, SpBareActionKind kind, int index, int value) {
    if (dispatch) dispatch(user, (SpBareAction){ kind, index, value, NULL });
}

static bool contains(const char *haystack, const char *needle) {
    if (!needle || !*needle) return true;
    if (!haystack) return false;
    for (; *haystack; haystack++) {
        const char *a = haystack, *b = needle;
        while (*a && *b && tolower((unsigned char)*a) == tolower((unsigned char)*b)) a++, b++;
        if (!*b) return true;
    }
    return false;
}

bool sp_bare_wallpaper_matches(const SpBareWallpaper *w, const char *search, int filter) {
    static const char *kinds[] = { "", "image", "video", "html" };
    if (!w || filter < 0 || filter >= (int)(sizeof kinds / sizeof kinds[0])) return false;
    if (filter && !contains(w->kind, kinds[filter])) return false;
    return contains(w->title, search) || contains(w->id, search);
}

static void label(const char *s, float size, BareColor color) {
    bare_text(s ? s : "", .size = size, .color = color, .nowrap = true, .ellipsis = true);
}

static bool pill(const char *s, bool active) {
    BareState state = bare_button(s, .height = 38, .padding = 11, .radius = 19,
        .color = active ? BARE_RGBA(0, 170, 60, 130) : BARE_RGBA(40, 62, 59, 115),
        .glass = { .refraction = 0.55f, .blur = 9, .merge = 10, .tint = { 185, 240, 226, 25 } },
        .border = 1, .border_color = active ? green() : BARE_RGBA(212, 245, 244, 70),
        .shadow = active ? (BareShadow){ 0, 0, 18, 0, { 0, 215, 80, 100 } } : (BareShadow){ 0 },
        .hover = { .scale = ui_lift }, .press = { .scale = 0.96f }, .animate = ui_motion);
    return state.clicked;
}

static void sidebar(SpBareView *view, const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    static const char *pages[] = { "Library", "Displays", "Customise", "Store", "Settings" };
    bare_glass_layer(BARE_GLASS_FROST, .width = 248, .height = BARE_GROW, .radius = 32,
                     .padding = 18, .gap = 12, .glass = { .refraction = 0.8f, .blur = 22,
                     .tint = { 20, 50, 47, 85 } }, .border = 1,
                     .border_color = BARE_RGBA(220, 255, 251, 75)) {
        bare_row(.height = 68, .align = BARE_CENTER, .gap = 12) {
            bare_box(.width = 42, .height = 42, .radius = 12, .color = green());
            label("Screen+", 23, ink());
        }
        for (int i = 0; i < 5; i++) {
            bare_row(.id = BARE_IDI("nav", i), .height = 54, .padding = 13,
                     .align = BARE_CENTER, .radius = 20, .cursor = BARE_CURSOR_HAND,
                     .color = (int)view->page == i ? BARE_RGBA(0, 175, 67, 116) : BARE_CLEAR,
                     .border = (int)view->page == i ? 1 : BARE_ZERO,
                     .border_color = BARE_RGBA(104, 245, 159, 100),
                     .hover = { .color = BARE_RGBA(83, 153, 124, 70), .scale = ui_lift },
                     .animate = ui_motion) {
                BareState state = bare_this();
                label(pages[i], 17, ink());
                if (state.clicked) { view->page = (SpBarePage)i; send(dispatch, user, SP_BARE_NAVIGATE, i, 0); }
            }
        }
        bare_box(.height = BARE_GROW);
        bare_glass_layer(BARE_GLASS_CLEAR, .height = 85, .padding = 14, .gap = 4,
                         .radius = 22, .border = 1, .border_color = BARE_RGBA(217, 255, 249, 85)) {
            label(m->status ? m->status : "Ready", 15, ink());
            label(m->display_name ? m->display_name : "Desktop", 13, dim());
        }
    }
}

static void wallpaper_card(const SpBareWallpaper *w, int i, int display, SpBareDispatch dispatch, void *user) {
    bare_glass_layer(BARE_GLASS_CLEAR, .id = BARE_IDI("wallpaper", i), .width = BARE_GROW,
                     .height = 240, .radius = 24, .padding = 10, .gap = 8,
                     .border = 1, .border_color = w->selected ? green() : BARE_RGBA(232, 255, 249, 55),
                     .shadow = w->selected ? (BareShadow){ 0, 0, 18, 0, { 0, 215, 80, 115 } }
                                           : BARE_SHADOW_SMALL,
                     .hover = { .scale = ui_lift }, .animate = ui_motion,
                     .cursor = BARE_CURSOR_HAND) {
        BareState state = bare_this();
        bare_box(.width = BARE_GROW, .height = 174, .radius = 18,
                 .image = w->thumbnail, .color = BARE_RGBA(25, 40, 52, 180), .passthrough = true);
        bare_row(.gap = 8, .align = BARE_CENTER) {
            label(w->title, 17, ink());
            if (w->favourite) label("★", 17, green());
        }
        if (w->missing) label("Source missing", 13, dim());
        if (state.clicked && !w->missing) send(dispatch, user, SP_BARE_SELECT_WALLPAPER, i, display);
    }
}

static void library(SpBareView *view, const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    bare_row(.height = 75, .align = BARE_CENTER, .gap = 12) {
        bare_column(.width = BARE_GROW, .gap = 5) {
            label("Library", 34, ink());
            char count[48]; snprintf(count, sizeof count, "%d wallpapers", m->wallpaper_count);
            label(count, 14, dim());
        }
        bare_input(view->search, sizeof view->search, .placeholder = "Search wallpapers", .width = 280,
                   .style = &bare_theme()->input);
        if (pill("Refresh", false)) send(dispatch, user, SP_BARE_REFRESH, 0, 0);
        if (pill("+ Import", true)) bare_file_dialog(BARE_ID("sp-import"), BARE_FILE_OPEN, NULL, NULL);
    }
    if (view->target_display) {
        bare_row(.height = 45, .align = BARE_CENTER, .gap = 10) {
            char target[64];
            snprintf(target, sizeof target, "Choosing for display %d", view->target_display);
            label(target, 15, dim());
            if (pill("Use all displays", false)) view->target_display = 0;
        }
    }
    const char *import_path = bare_file_chosen(BARE_ID("sp-import"));
    if (import_path && *import_path && dispatch)
        dispatch(user, (SpBareAction){ SP_BARE_IMPORT, 0, 0, import_path });
    if (m->active_wallpaper >= 0 && m->active_wallpaper < m->wallpaper_count) {
        const SpBareWallpaper *w = &m->wallpapers[m->active_wallpaper];
        bare_glass_layer(BARE_GLASS_FROST, .height = 252, .padding = 18, .radius = 32,
                         .border = 1, .border_color = BARE_RGBA(221, 255, 249, 90)) {
            bare_row(.gap = 24) {
                bare_box(.width = 390, .height = 216, .radius = 22, .image = w->thumbnail,
                         .color = BARE_RGBA(25, 40, 52, 190));
                bare_column(.gap = 11) {
                    label("NOW SHOWING  ·  DESKTOP", 13, dim());
                    label(w->title, 28, ink());
                    label(w->kind, 14, green());
                    bare_row(.gap = 8) {
                        if (pill(m->paused ? "Resume" : "Pause", false)) send(dispatch, user, SP_BARE_PAUSE, 0, !m->paused);
                        if (pill(m->muted ? "Unmute" : "Mute", false)) send(dispatch, user, SP_BARE_MUTE, 0, !m->muted);
                        if (pill("Restart", false)) send(dispatch, user, SP_BARE_RESTART, 0, 0);
                    }
                }
            }
        }
    }
    bare_row(.height = 56, .gap = 8, .align = BARE_CENTER) {
        static const char *filters[] = { "All", "Images", "Videos", "HTML" };
        for (int i = 0; i < 4; i++) if (pill(filters[i], view->filter == i)) view->filter = i;
    }
    int shown = 0, available = 0;
    for (int i = 0; m->wallpapers && i < m->wallpaper_count; i++)
        available += sp_bare_wallpaper_matches(&m->wallpapers[i], view->search, view->filter);
    int limit = view->visible_limit > 0 ? view->visible_limit : 60;
    BareVec2 win = bare_window_size();
    int columns = win.x > 1250 ? 3 : win.x > 900 ? 2 : 1;
    bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 16) {
        if (m->wallpapers) for (int i = 0; i < m->wallpaper_count && shown < limit;) {
            while (i < m->wallpaper_count &&
                   !sp_bare_wallpaper_matches(&m->wallpapers[i], view->search, view->filter)) i++;
            if (i == m->wallpaper_count) break;
            bare_row(.gap = 16) {
                for (int col = 0; col < columns && i < m->wallpaper_count && shown < limit;) {
                    int index = i++;
                    const SpBareWallpaper *w = &m->wallpapers[index];
                    if (!sp_bare_wallpaper_matches(w, view->search, view->filter)) continue;
                    wallpaper_card(w, index, view->target_display, dispatch, user);
                    shown++;
                    col++;
                }
            }
        }
        if (!shown) label(m->wallpaper_count ? "No wallpapers match your search" : "Your library is empty", 17, dim());
        if (available > shown && pill("Load more", false)) {
            view->visible_limit = limit + 60;
            send(dispatch, user, SP_BARE_LOAD_MORE, view->visible_limit, 0);
        }
    }
}

static void displays(SpBareView *view, const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    label("Displays", 34, ink());
    bare_row(.height = 54, .gap = 8, .align = BARE_CENTER) {
        static const char *modes[] = { "Independent", "Duplicate", "Span" };
        for (int i = 0; i < 3; i++)
            if (pill(modes[i], m->display_mode == i)) send(dispatch, user, SP_BARE_DISPLAY_MODE, i, 0);
    }
    bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 14) {
        if (!m->display_count) label("No displays reported by Screen+", 17, dim());
        for (int i = 0; m->displays && i < m->display_count; i++) {
            const SpBareDisplay *d = &m->displays[i];
            bare_glass_layer(BARE_GLASS_FROST, .height = 146, .radius = 26, .padding = 18,
                             .gap = 8, .border = 1, .border_color = BARE_RGBA(215, 250, 243, 75)) {
                bare_row(.align = BARE_CENTER, .gap = 12) {
                    label(d->name, 21, ink());
                    if (d->primary) label("Primary", 13, green());
                    if (!d->connected) label("Disconnected", 13, dim());
                    bare_box(.width = BARE_GROW);
                    if (d->connected && pill("Choose wallpaper", false)) {
                        view->target_display = d->index;
                        view->page = SP_BARE_LIBRARY;
                        send(dispatch, user, SP_BARE_NAVIGATE, SP_BARE_LIBRARY, 0);
                    }
                }
                char specs[96];
                snprintf(specs, sizeof specs, "%d × %d  ·  %d Hz", d->width, d->height, d->hz);
                label(specs, 14, dim());
                label(d->wallpaper_title && *d->wallpaper_title ? d->wallpaper_title : "No wallpaper", 16, ink());
            }
        }
    }
}

static void store_page(SpBareView *view, const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    bare_row(.height = 70, .gap = 10, .align = BARE_CENTER) {
        label("Store", 34, ink());
        bare_box(.width = BARE_GROW);
        bare_input(view->store_search, sizeof view->store_search, .placeholder = "Search the Store", .width = 270);
        if (pill("Search", true) && dispatch)
            dispatch(user, (SpBareAction){ SP_BARE_STORE_SEARCH, 0, 0, view->store_search });
        if (pill("Refresh", false)) send(dispatch, user, SP_BARE_NAVIGATE, SP_BARE_STORE, 0);
    }
    if (m->store_error && *m->store_error) label(m->store_error, 16, dim());
    else if (m->store_loading) label("Loading Store items…", 16, dim());
    bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 10) {
        for (int i = 0; m->store_items && i < m->store_count; i++) {
            const SpBareStoreItem *item = &m->store_items[i];
            bare_glass_layer(BARE_GLASS_FROST, .height = 72, .radius = 22, .padding = 14,
                             .border = 1, .border_color = BARE_RGBA(210, 250, 241, 70)) {
                bare_row(.align = BARE_CENTER, .gap = 12) {
                    bare_column(.width = BARE_GROW, .gap = 4) {
                        label(item->title, 17, ink());
                        label(item->source, 13, dim());
                    }
                    label(item->kind, 13, dim());
                    if (pill("Get", false)) send(dispatch, user, SP_BARE_STORE_GET, item->number, 0);
                }
            }
        }
        if (m->store_count == 0 && !m->store_loading && !(m->store_error && *m->store_error))
            label("No Store items yet. Use Refresh to check again.", 16, dim());
        if (pill("More", false)) send(dispatch, user, SP_BARE_STORE_MORE, 0, 0);
    }
}

static void customise(const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    static const char *names[] = { "Brightness", "Contrast", "Saturation", "Hue", "Gamma" };
    static const int limits[] = { 100, 100, 100, 180, 100 };
    bare_row(.height = 68, .align = BARE_CENTER) {
        label("Customise", 34, ink());
        bare_box(.width = BARE_GROW);
        if (m->look_available && pill("Reset look", false)) send(dispatch, user, SP_BARE_LOOK_RESET, 0, 0);
    }
    if (!m->look_available) {
        label("Apply a wallpaper to adjust its look.", 17, dim());
        return;
    }
    label("COLOUR GRADING", 13, dim());
    bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 12) {
        for (int i = 0; i < 5; i++) {
            bare_glass_layer(BARE_GLASS_FROST, .height = 68, .radius = 22, .padding = 12,
                             .border = 1, .border_color = BARE_RGBA(211, 250, 240, 70)) {
                bare_row(.align = BARE_CENTER, .gap = 12) {
                    label(names[i], 17, ink());
                    bare_box(.width = BARE_GROW);
                    if (pill("−", false)) {
                        int next = m->look[i] - 5;
                        send(dispatch, user, SP_BARE_LOOK_SET, i, next < -limits[i] ? -limits[i] : next);
                    }
                    char value[24]; snprintf(value, sizeof value, "%+d", m->look[i]);
                    label(value, 16, ink());
                    if (pill("+", false)) {
                        int next = m->look[i] + 5;
                        send(dispatch, user, SP_BARE_LOOK_SET, i, next > limits[i] ? limits[i] : next);
                    }
                }
            }
        }
    }
}

static void setting(const char *title, bool value, int index, SpBareDispatch dispatch, void *user) {
    bare_glass_layer(BARE_GLASS_FROST, .height = 58, .radius = 23, .padding = 12,
                     .border = 1, .border_color = BARE_RGBA(214, 255, 249, 75)) {
        bare_row(.align = BARE_CENTER) {
            
            label(title, 17, ink());
            bare_box(.width = BARE_GROW);
            if (pill(value ? "On" : "Off", value)) send(dispatch, user, SP_BARE_SETTING, index, !value);
        }
    }
}

static void settings(const SpBareModel *m, SpBareDispatch dispatch, void *user) {
    static const char *names[] = {
#ifdef _WIN32
        "Start with Windows", "Start minimised to the tray", "Show a still while Screen+ starts",
#else
        "Start with Linux", "Keep the window closed at sign-in", "Show a still while Screen+ starts",
#endif
        "Auto update", "Sample wallpaper updates" };
    const bool values[] = { m->start_with_system, m->start_minimised, m->still_at_start,
                            m->auto_update, m->sample_updates };
    label("Settings", 34, ink());
    label("GENERAL", 13, dim());
    bare_column(.height = BARE_GROW, .scroll = BARE_SCROLL_Y, .gap = 12) {
        for (int i = 0; i < 5; i++) {
#ifndef _WIN32
            if (i == 2) continue;
#endif
            setting(names[i], values[i], i, dispatch, user);
        }
        bare_glass_layer(BARE_GLASS_FROST, .height = 70, .radius = 23, .padding = 13) {
            bare_row(.align = BARE_CENTER, .gap = 12) {
                label("Appearance", 17, ink()); bare_box(.width = BARE_GROW);
                if (pill("System", m->appearance == 0)) send(dispatch, user, SP_BARE_SETTING, 5, 0);
                if (pill("Light", m->appearance == 1)) send(dispatch, user, SP_BARE_SETTING, 5, 1);
                if (pill("Dark", m->appearance == 2)) send(dispatch, user, SP_BARE_SETTING, 5, 2);
            }
        }
        bare_glass_layer(BARE_GLASS_FROST, .height = 70, .radius = 23, .padding = 13) {
            bare_row(.align = BARE_CENTER, .gap = 12) {
                label("Animations", 17, ink()); bare_box(.width = BARE_GROW);
                if (pill("Full", m->animation_level == 0)) send(dispatch, user, SP_BARE_SETTING, 6, 0);
                if (pill("Reduced", m->animation_level == 1)) send(dispatch, user, SP_BARE_SETTING, 6, 1);
                if (pill("Off", m->animation_level == 2)) send(dispatch, user, SP_BARE_SETTING, 6, 2);
            }
        }
    }
}

void sp_bare_draw(SpBareView *view, const SpBareModel *model, SpBareDispatch dispatch, void *user) {
    if (!view || !model) return;
    ui_motion = model->animation_level == 2 ? BARE_INSTANT :
                model->animation_level == 1 ? BARE_EASE(0.12f, BARE_EASE_OUT) : BARE_SPRING_SNAPPY;
    ui_lift = model->animation_level == 2 ? 1.0f : model->animation_level == 1 ? 1.01f : 1.035f;
    BareVec2 win = bare_window_size();
    bare_glass_stack(.width = win.x, .height = win.y) {
        if (!model->transparent_window && model->wallpapers && model->active_wallpaper >= 0 && model->active_wallpaper < model->wallpaper_count)
            bare_box(.width = win.x, .height = win.y, .image = model->wallpapers[model->active_wallpaper].thumbnail,
                     .color = BARE_RGB(7, 20, 24), .passthrough = true);
        if (!model->transparent_window)
            bare_box(.width = win.x, .height = win.y, .color = BARE_RGBA(4, 17, 19, 155), .passthrough = true);
        bare_row(.width = win.x, .height = win.y, .padding = 18, .gap = 18) {
            sidebar(view, model, dispatch, user);
            bare_glass_layer(BARE_GLASS_FROST, .width = BARE_GROW, .height = BARE_GROW,
                             .padding = 24, .gap = 18, .radius = 32,
                             .glass = { .refraction = 0.65f, .blur = 25,
                                        .tint = { 8, 29, 31, 105 } },
                             .border = 1, .border_color = BARE_RGBA(232, 255, 251, 80)) {
                if (view->page == SP_BARE_LIBRARY) library(view, model, dispatch, user);
                else if (view->page == SP_BARE_DISPLAYS) displays(view, model, dispatch, user);
                else if (view->page == SP_BARE_STORE) store_page(view, model, dispatch, user);
                else if (view->page == SP_BARE_SETTINGS) settings(model, dispatch, user);
                else customise(model, dispatch, user);
            }
        }
    }
}

