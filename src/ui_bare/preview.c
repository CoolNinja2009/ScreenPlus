/* preview.c: the Screen+ Bare window on sample data, no service needed.
 *
 *   screenplus-bare-preview [--page library|displays|customise|store|settings] [--light]
 *
 * SP_BACKDROP=gen draws a generated picture behind the glass instead of the
 * live desktop (SP_BACKDROP=<file> one of yours), for repeatable captures. */
#include "screenplus_bare.h"
#include "sp_kit.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct Wall { const char *id, *name, *type, *format, *file, *tags; bool fav; } Wall;

static Wall walls[] = {
    { "singularity", "Singularity", "html", "lively", "index.html", "test 3", true },
    { "aurora", "Aurora Drift", "video", "file", "aurora.mp4", "", false },
    { "koi", "Koi Pond", "image", "file", "koi.jpg", "Test2", true },
    { "neon-rain", "Neon Rain", "html", "lively", "index.html", "", false },
    { "dunes", "Dunes at Dusk", "video", "file", "dunes.webm", "test 3", false },
    { "glacier", "Glacier", "image", "file", "glacier.png", "", false },
    { "fluid", "Fluid Simulation", "html", "lively", "index.html", "Test2", false },
    { "city", "City Lights", "video", "file", "city.mp4", "", true },
    { "forest", "Misty Forest", "image", "file", "forest.jpg", "", false },
    { "particles", "Particles", "html", "lively", "index.html", "", false },
    { "ocean", "Ocean Waves", "video", "file", "ocean.mp4", "test 3", false },
    { "nebula", "Nebula", "image", "file", "nebula.png", "", false },
};
#define WALLS (int)(sizeof walls / sizeof walls[0])

typedef struct Setting { const char *key; char value[32]; bool is_bool, is_num; } Setting;
static Setting settings[] = {
    { "start_with_windows", "on", true }, { "launch_to_tray", "off", true }, { "still_wallpaper", "on", true },
    { "pause_on_fullscreen", "on", true }, { "pause_when_covered", "on", true }, { "suspend_on_lock", "on", true },
    { "pause_on_battery", "off", true }, { "bare_ui", "on", true }, { "auto_recover", "on", true }, { "audio", "off", true },
    { "volume", "60", false, true }, { "html_mouse", "on", true }, { "html_audio", "on", true }, { "web_fps", "60" },
    { "slideshow", "off", true }, { "slideshow_minutes", "15", false, true }, { "shuffle", "off", true },
    { "schedule", "off", true }, { "lively_auto_import", "on", true }, { "url_import", "on", true },
    { "interactive_images", "off", true }, { "screensaver_audio", "off", true }, { "screensaver_displays", "all" },
    { "screensaver_exit_on_mouse", "on", true }, { "auto_update", "on", true }, { "auto_update_samples", "on", true },
    { "theme", "dark" }, { "ui_motion", "full" }, { "accent_color", "#00D705" }, { "library_sort", "custom" },
    { "display_mode", "independent" }, { "language", "en" },
};
#define SETTINGS (int)(sizeof settings / sizeof settings[0])

typedef struct Preview {
    SpView view;
    SpModel model;
    JVal *status, *settings, *list, *look, *store, *layout, *upscale, *props, *record, *cool;
    int on_screen;
    bool paused, audio;
    int look_values[5];
    BareImage thumbs[64];
    BareImage backdrop;
    char message[320];
} Preview;

static Setting *setting(const char *key) {
    for (int i = 0; i < SETTINGS; i++) if (!strcmp(settings[i].key, key)) return &settings[i];
    return NULL;
}

static void wall_json(StrBuf *b, int i) {
    const Wall *w = &walls[i];
    sb_printf(b, "{\"id\":\"%s\",\"name\":\"%s\",\"type\":\"%s\",\"format\":\"%s\",\"path\":\"C:\\\\Wallpapers\\\\%s\\\\%s\","
                 "\"thumbnail\":\"gen:%d\",\"missing\":false,\"favourite\":%s,\"volume\":100,\"collections\":\"%s\"}",
              w->id, w->name, w->type, w->format, w->id, w->file, i, w->fav ? "true" : "false", w->tags);
}

static JVal *parse(StrBuf *b) {
    JVal *v = b->failed ? NULL : json_parse(b->p, b->len);
    sb_free(b);
    return v;
}

static void rebuild(Preview *p) {
    json_free(p->status); json_free(p->settings); json_free(p->list); json_free(p->look); json_free(p->store);
    json_free(p->layout); json_free(p->upscale); json_free(p->props); json_free(p->record); json_free(p->cool);
    StrBuf b = { 0 };
    sb_printf(&b, "{\"version\":\"2.1.0\",\"state\":\"%s\",\"severity\":0,\"paused\":%s,\"audio\":%s,"
                  "\"renderer\":{\"pid\":0},\"memoryMB\":{\"app\":38,\"renderer\":74},\"displays\":["
                  "{\"index\":1,\"name\":\"XV272U X1\",\"id\":\"d1\",\"width\":2560,\"height\":1440,\"hz\":170,"
                  "\"primary\":true,\"state\":\"Playing\",\"covered\":true,"
                  "\"renderer\":\"WebView2 \\u00b7 Lively \\u00b7 audio-reactive \\u00b7 60 fps\",\"wallpaper\":",
              p->paused ? "Paused" : "Idle (desktop covered)", p->paused ? "true" : "false", p->audio ? "true" : "false");
    wall_json(&b, p->on_screen);
    sb_puts(&b, "}]}");
    p->status = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{");
    for (int i = 0; i < SETTINGS; i++) {
        const Setting *s = &settings[i];
        if (i) sb_puts(&b, ",");
        if (s->is_bool) sb_printf(&b, "\"%s\":%s", s->key, strcmp(s->value, "on") ? "false" : "true");
        else if (s->is_num) sb_printf(&b, "\"%s\":%s", s->key, s->value);
        else sb_printf(&b, "\"%s\":\"%s\"", s->key, s->value);
    }
    sb_puts(&b, "}");
    p->settings = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "[");
    int n = 0;
    for (int rep = 0; rep < 5; rep++)
        for (int i = 0; i < WALLS && n < 59; i++, n++) {
            if (n) sb_puts(&b, ",");
            sb_printf(&b, "{\"number\":%d,\"onScreen\":%s,\"wallpaper\":", n + 1, n == p->on_screen ? "true" : "false");
            wall_json(&b, i);
            sb_puts(&b, "}");
        }
    sb_puts(&b, "]");
    p->list = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"wallpaper\":");
    wall_json(&b, p->on_screen % WALLS);
    sb_printf(&b, ",\"tint\":\"#FF9F0A\",\"tintStrength\":20,\"tintBrightness\":50,\"brightness\":%d,\"contrast\":%d,\"saturation\":%d,\"hue\":%d,\"gamma\":%d}", p->look_values[0],
              p->look_values[1], p->look_values[2], p->look_values[3], p->look_values[4]);
    p->look = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"layout\":\"custom\",\"x\":30,\"y\":15,\"w\":85,\"h\":85,\"display\":1.778,\"picture\":1.778,\"rect\":[0.3,0.15,0.85,0.85],\"displayW\":2560,\"displayH\":1440,\"border\":\"color\",\"color\":\"#000000\",\"page\":false}");
    p->layout = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"upscale\":\"smooth\",\"smooth\":true}");
    p->upscale = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"properties\":[{\"name\":\"rain\",\"label\":\"Rain\",\"type\":\"toggle\",\"value\":true,\"display\":\"on\"},"
                "{\"name\":\"speed\",\"label\":\"Speed\",\"type\":\"slider\",\"value\":60,\"display\":\"60\",\"min\":0,\"max\":100,\"step\":1},"
                "{\"name\":\"quality\",\"label\":\"Quality\",\"type\":\"choice\",\"value\":1,\"display\":\"High\",\"choices\":[\"Low\",\"High\",\"Ultra\"]},"
                "{\"name\":\"glow\",\"label\":\"Glow colour\",\"type\":\"colour\",\"value\":\"#FF375F\",\"display\":\"#FF375F\"},"
                "{\"name\":\"caption\",\"label\":\"Caption\",\"type\":\"text\",\"value\":\"Hello\",\"display\":\"Hello\"}]}");
    p->props = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, getenv("SP_PREVIEW_RECORDING") ? "{\"recording\":{\"section\":\"look\",\"seconds\":7}," : "{\"recording\":null,");
    sb_puts(&b, "\"look\":{\"samples\":300,\"seconds\":10,\"speed\":1,\"smooth\":20,\"paused\":false,\"position\":0.3,\"rate\":30,\"tracks\":[{\"key\":\"brightness\",\"label\":\"brightness\",\"kind\":\"number\",\"lo\":-100,\"hi\":100,\"values\":[0.500,0.547,0.594,0.639,0.683,0.725,0.764,0.801,0.834,0.864,0.890,0.911,0.928,0.940,0.948,0.950,0.948,0.940,0.928,0.911,0.890,0.864,0.834,0.801,0.765,0.725,0.683,0.639,0.594,0.547,0.500,0.453,0.406,0.361,0.317,0.275,0.236,0.199,0.166,0.136,0.110,0.089,0.072,0.060,0.052,0.050,0.052,0.060,0.072,0.089,0.110,0.136,0.166,0.199,0.235,0.275,0.317,0.361,0.406,0.453,0.500,0.547,0.593,0.639,0.683,0.725,0.764,0.801,0.834,0.864,0.890,0.911,0.928,0.940,0.948,0.950,0.948,0.940,0.928,0.911,0.890,0.864,0.834,0.801,0.765,0.725,0.683,0.639,0.594,0.547,0.500,0.453,0.407,0.361,0.317,0.275,0.236,0.199,0.166,0.136,0.110,0.089,0.072,0.060,0.052,0.050,0.052,0.060,0.072,0.089,0.110,0.136,0.165,0.199,0.235,0.275,0.317,0.361,0.406,0.453]},{\"key\":\"tint\",\"label\":\"tint\",\"kind\":\"colour\",\"lo\":0,\"hi\":0,\"values\":[\"#7F50FF\",\"#8C50FE\",\"#9A50FC\",\"#A650F8\",\"#B350F3\",\"#BF50ED\",\"#CA50E6\",\"#D450DE\",\"#DE50D4\",\"#E650CA\",\"#ED50BF\",\"#F350B3\",\"#F850A6\",\"#FC509A\",\"#FE508C\",\"#FE507F\",\"#FE5072\",\"#FC5064\",\"#F85058\",\"#F3504B\",\"#ED503F\",\"#E65034\",\"#DE502A\",\"#D45020\",\"#CA5018\",\"#BF5011\",\"#B3500B\",\"#A65006\",\"#9A5002\",\"#8C5000\",\"#7F5000\",\"#725000\",\"#655002\",\"#585006\",\"#4B500B\",\"#3F5011\",\"#345018\",\"#2A5020\",\"#20502A\",\"#185034\",\"#11503F\",\"#0B504B\",\"#065058\",\"#025064\",\"#005072\",\"#00507F\",\"#00508C\",\"#025099\",\"#0650A6\",\"#0B50B3\",\"#1150BF\",\"#1850CA\",\"#2050D4\",\"#2A50DE\",\"#3450E6\",\"#3F50ED\",\"#4B50F3\",\"#5850F8\",\"#6450FC\",\"#7250FE\"]}]},\"props\":null,\"volume\":{\"samples\":150,\"seconds\":5,\"speed\":1,\"smooth\":0,\"paused\":true,\"position\":0.6,\"rate\":30,\"tracks\":[{\"key\":\"volume\",\"label\":\"volume\",\"kind\":\"number\",\"lo\":0,\"hi\":100,\"values\":[0.000,0.008,0.017,0.025,0.033,0.042,0.050,0.058,0.067,0.075,0.083,0.092,0.100,0.108,0.117,0.125,0.133,0.142,0.150,0.158,0.167,0.175,0.183,0.192,0.200,0.208,0.217,0.225,0.233,0.242,0.250,0.258,0.267,0.275,0.283,0.292,0.300,0.308,0.317,0.325,0.333,0.342,0.350,0.358,0.367,0.375,0.383,0.392,0.400,0.408,0.417,0.425,0.433,0.442,0.450,0.458,0.467,0.475,0.483,0.492,0.500,0.508,0.517,0.525,0.533,0.542,0.550,0.558,0.567,0.575,0.583,0.592,0.600,0.608,0.617,0.625,0.633,0.642,0.650,0.658,0.667,0.675,0.683,0.692,0.700,0.708,0.717,0.725,0.733,0.742,0.750,0.758,0.767,0.775,0.783,0.792,0.800,0.808,0.817,0.825,0.833,0.842,0.850,0.858,0.867,0.875,0.883,0.892,0.900,0.908,0.917,0.925,0.933,0.942,0.950,0.958,0.967,0.975,0.983,0.992]}]},\"presets\":[{\"name\":\"Sine\",\"times\":1,\"shape\":[0.500,0.567,0.632,0.695,0.755,0.810,0.859,0.903,0.938,0.967,0.986,0.997,1.000,0.993,0.978,0.954,0.921,0.882,0.835,0.783,0.726,0.664,0.600,0.533,0.467,0.400,0.336,0.275,0.217,0.165,0.118,0.079,0.046,0.022,0.007,0.000,0.003,0.014,0.033,0.061,0.097,0.141,0.190,0.245,0.305,0.368,0.433,0.500]},{\"name\":\"Triangle\",\"times\":1,\"shape\":[0.921,0.977,1.000,0.987,0.939,0.860,0.756,0.633,0.501,0.369,0.246,0.142,0.062,0.014,0.000,0.022,0.078,0.164,0.273,0.399,0.532,0.663,0.782,0.881,0.953,0.993,0.998,0.967,0.903,0.811,0.697,0.568,0.435,0.306,0.191,0.098,0.034,0.003,0.007,0.046,0.117,0.216,0.335,0.465,0.598,0.724,0.834,0.921]},{\"name\":\"Ramp up\",\"times\":1,\"shape\":[0.955,0.837,0.667,0.469,0.277,0.120,0.023,0.002,0.060,0.188,0.365,0.564,0.752,0.901,0.986,0.993,0.923,0.785,0.602,0.403,0.219,0.080,0.007,0.013,0.096,0.243,0.431,0.629,0.808,0.937,0.997,0.978,0.884,0.728,0.536,0.339,0.167,0.048,0.000,0.032,0.139,0.302,0.497,0.693,0.857,0.966,1.000,0.955]},{\"name\":\"Ramp down\",\"times\":1,\"shape\":[0.571,0.504,0.437,0.372,0.308,0.249,0.193,0.143,0.100,0.063,0.035,0.015,0.003,0.000,0.006,0.021,0.045,0.076,0.116,0.162,0.214,0.271,0.332,0.396,0.463,0.529,0.596,0.660,0.722,0.780,0.832,0.879,0.919,0.952,0.976,0.992,1.000,0.998,0.987,0.968,0.940,0.905,0.862,0.813,0.758,0.699,0.636,0.571]},{\"name\":\"Square\",\"times\":1,\"shape\":[0.122,0.049,0.008,0.002,0.032,0.094,0.186,0.300,0.428,0.561,0.690,0.806,0.899,0.965,0.997,0.994,0.956,0.885,0.787,0.669,0.539,0.406,0.279,0.169,0.081,0.024,0.000,0.012,0.059,0.137,0.241,0.363,0.495,0.627,0.750,0.856,0.936,0.985,1.000,0.979,0.924,0.839,0.730,0.605,0.472,0.341,0.222,0.122]},{\"name\":\"Pulse\",\"times\":1,\"shape\":[0.021,0.114,0.269,0.460,0.658,0.830,0.951,0.999,0.969,0.864,0.701,0.507,0.311,0.145,0.036,0.000,0.044,0.160,0.330,0.527,0.719,0.877,0.976,0.998,0.942,0.815,0.639,0.440,0.251,0.102,0.015,0.006,0.075,0.211,0.394,0.593,0.777,0.918,0.992,0.988,0.907,0.761,0.573,0.375,0.196,0.065,0.003,0.020]},{\"name\":\"Steps\",\"times\":1,\"shape\":[0.360,0.426,0.492,0.559,0.624,0.688,0.748,0.804,0.854,0.898,0.935,0.964,0.984,0.997,1.000,0.994,0.980,0.957,0.926,0.887,0.841,0.789,0.733,0.672,0.607,0.541,0.475,0.408,0.344,0.282,0.224,0.171,0.123,0.083,0.050,0.025,0.008,0.001,0.002,0.012,0.031,0.058,0.093,0.135,0.184,0.238,0.297,0.360]},{\"name\":\"Bounce\",\"times\":1,\"shape\":[0.828,0.916,0.975,0.999,0.988,0.943,0.866,0.763,0.641,0.509,0.377,0.253,0.147,0.066,0.016,0.000,0.020,0.074,0.158,0.266,0.391,0.524,0.655,0.775,0.876,0.950,0.991,0.998,0.970,0.908,0.817,0.704,0.576,0.443,0.313,0.198,0.103,0.037,0.004,0.005,0.043,0.112,0.209,0.327,0.457,0.590,0.717,0.828]},{\"name\":\"Breathe\",\"times\":1,\"shape\":[0.995,0.927,0.792,0.610,0.411,0.226,0.084,0.009,0.011,0.091,0.236,0.423,0.622,0.801,0.933,0.996,0.981,0.889,0.735,0.544,0.346,0.173,0.051,0.001,0.030,0.133,0.295,0.489,0.685,0.852,0.963,1.000,0.958,0.843,0.674,0.477,0.284,0.125,0.026,0.002,0.056,0.182,0.358,0.556,0.745,0.896,0.984,0.995]},{\"name\":\"Heartbeat\",\"times\":1,\"shape\":[0.706,0.644,0.578,0.512,0.445,0.379,0.316,0.255,0.200,0.149,0.105,0.067,0.038,0.017,0.004,0.000,0.005,0.019,0.042,0.072,0.111,0.156,0.207,0.264,0.325,0.389,0.455,0.521,0.588,0.653,0.715,0.773,0.826,0.874,0.915,0.948,0.974,0.991,0.999,0.999,0.989,0.971,0.944,0.910,0.868,0.819,0.765,0.706]},{\"name\":\"Ease in-out\",\"times\":1,\"shape\":[0.228,0.127,0.052,0.009,0.001,0.029,0.090,0.180,0.293,0.420,0.554,0.683,0.799,0.895,0.962,0.996,0.995,0.959,0.890,0.794,0.676,0.547,0.413,0.286,0.175,0.086,0.027,0.001,0.011,0.055,0.131,0.234,0.355,0.487,0.619,0.743,0.850,0.932,0.983,1.000,0.981,0.928,0.845,0.737,0.613,0.480,0.349,0.228]},{\"name\":\"Spike\",\"times\":1,\"shape\":[0.000,0.041,0.154,0.322,0.519,0.712,0.872,0.973,0.999,0.945,0.821,0.646,0.448,0.258,0.106,0.017,0.005,0.071,0.205,0.386,0.585,0.771,0.913,0.990,0.990,0.911,0.767,0.581,0.382,0.202,0.069,0.004,0.018,0.109,0.261,0.452,0.650,0.824,0.947,0.999,0.972,0.870,0.709,0.515,0.318,0.151,0.039,0.000]},{\"name\":\"Wander\",\"times\":1,\"shape\":[0.232,0.290,0.353,0.418,0.484,0.551,0.617,0.681,0.741,0.797,0.848,0.893,0.931,0.961,0.982,0.996,1.000,0.995,0.982,0.960,0.930,0.892,0.847,0.796,0.740,0.679,0.615,0.549,0.482,0.416,0.351,0.289,0.230,0.177,0.129,0.087,0.053,0.027,0.010,0.001,0.001,0.010,0.028,0.054,0.088,0.130,0.178,0.232]},{\"name\":\"Random\",\"times\":1,\"shape\":[0.710,0.822,0.912,0.972,0.999,0.990,0.946,0.871,0.769,0.649,0.517,0.385,0.260,0.153,0.070,0.018,0.000,0.018,0.070,0.152,0.259,0.384,0.516,0.648,0.769,0.870,0.946,0.990,0.999,0.973,0.913,0.823,0.711,0.584,0.451,0.321,0.204,0.108,0.040,0.005,0.004,0.039,0.107,0.203,0.320,0.449,0.583,0.710]}]}");
    p->record = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"can\":true,\"cool\":false,\"busy\":false,\"otherBusy\":false,\"failed\":false,\"percent\":-1,\"mouse\":true,\"ready\":true,\"video\":false,\"text\":\"\",\"model\":\"da2-small\",\"models\":[{\"key\":\"da2-small\",\"name\":\"Depth Anything 2 Small\",\"desc\":\"Fast, good edges\",\"ready\":true,\"used\":false,\"user\":false,\"off\":false,\"level\":5,\"seconds\":0.4,\"size\":99000000,\"rating\":\"Very good\"},{\"key\":\"da3-large\",\"name\":\"Depth Anything 3 Large\",\"desc\":\"Most detail\",\"ready\":false,\"used\":false,\"user\":false,\"off\":false,\"level\":2,\"seconds\":6,\"size\":1300000000,\"rating\":\"Slow\"},{\"key\":\"sharp\",\"name\":\"SHARP\",\"desc\":\"3D Gaussians from one picture\",\"ready\":false,\"used\":false,\"user\":true,\"off\":false,\"level\":3,\"seconds\":3,\"size\":0,\"rating\":\"OK\"}]}");
    p->cool = parse(&b);
    b = (StrBuf){ 0 };
    sb_puts(&b, "{\"loading\":false,\"items\":[");
    static const char *sources[] = { "MotionBGS", "Lively", "MoeWalls", "Wallhaven" };
    for (int i = 0; i < 12; i++) {
        if (i) sb_puts(&b, ",");
        sb_printf(&b, "{\"number\":%d,\"title\":\"%s\",\"source\":\"%s\",\"kind\":\"%s\",\"preview\":\"gen:%d\","
                      "\"badge\":\"%s\",\"download\":%d,\"percent\":%d}",
                  i + 1, walls[(i + 3) % WALLS].name, sources[i % 4], i % 3 ? "live" : "still", (i + 3) % WALLS,
                  i % 3 ? "4K" : "", i == 1 ? 2 : i == 4 ? 3 : 0, i == 1 ? 42 : -1);
    }
    sb_puts(&b, "]}");
    p->store = parse(&b);
    p->model.status = p->status;
    p->model.settings = p->settings;
    p->model.list = p->list;
    p->model.look = p->look;
    p->model.store = p->store;
    p->model.layout = p->layout;
    p->model.upscale = p->upscale;
    p->model.props = p->props;
    p->model.record = p->record;
    p->model.cool = p->cool;
    p->model.record_time = bare_time();
}

static void say(Preview *p, const char *text, bool failed) {
    snprintf(p->message, sizeof p->message, "%s", text);
    p->model.message = p->message;
    p->model.message_time = bare_time();
    p->model.message_failed = failed;
}

static void command(void *user, const char *const *a) {
    Preview *p = user;
    char text[256];
    if (!strcmp(a[0], "apply") && a[1]) {
        for (int i = 0; i < WALLS; i++) if (!strcmp(walls[i].id, a[1])) p->on_screen = i;
        snprintf(text, sizeof text, "Showing %s", walls[p->on_screen].name);
        say(p, text, false);
    } else if (!strcmp(a[0], "pause") || !strcmp(a[0], "resume")) {
        p->paused = !strcmp(a[0], "pause");
        say(p, p->paused ? "Paused" : "Playing", false);
    } else if (!strcmp(a[0], "audio") && a[1]) {
        p->audio = !strcmp(a[1], "on");
        say(p, p->audio ? "Sound on" : "Muted", false);
    } else if (!strcmp(a[0], "settings") && a[1] && a[2] && a[3]) {
        Setting *s = setting(a[2]);
        if (s) snprintf(s->value, sizeof s->value, "%s", a[3]);
        say(p, "Saved", false);
    } else if (!strcmp(a[0], "favourite") && a[1] && a[2]) {
        for (int i = 0; i < WALLS; i++) if (!strcmp(walls[i].id, a[1])) walls[i].fav = !strcmp(a[2], "on");
        say(p, !strcmp(a[2], "on") ? "Added to Favourites" : "Removed from Favourites", false);
    } else if (!strcmp(a[0], "look") && a[1] && !strcmp(a[1], "reset")) {
        memset(p->look_values, 0, sizeof p->look_values);
        say(p, "Look reset", false);
    } else if (!strcmp(a[0], "look") && a[1] && a[2] && a[3]) {
        static const char *keys[] = { "brightness", "contrast", "saturation", "hue", "gamma" };
        for (int i = 0; i < 5; i++) if (!strcmp(keys[i], a[2])) p->look_values[i] = atoi(a[3]);
    } else if (!strcmp(a[0], "mode") && a[1]) {
        Setting *s = setting("display_mode");
        snprintf(s->value, sizeof s->value, "%s", a[1]);
    } else {
        snprintf(text, sizeof text, "Preview: \"%s\" needs Screen+ running", a[0]);
        say(p, text, true);
    }
    rebuild(p);
}

/* Pictures for the cards: soft gradients with a glow, a different hue each. */
static BareImage generated(int seed, int w, int h, bool blobs) {
    unsigned char *px = malloc((size_t)w * h * 4);
    if (!px) return 0;
    float hue = (float)seed * 0.61803f;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float u = (float)x / w, v = (float)y / h, t = hue * 6.2831f;
            float r = 0.5f + 0.5f * sinf(t + u * 3.0f + v * 1.5f);
            float g = 0.5f + 0.5f * sinf(t + 2.1f + u * 2.0f - v * 2.5f);
            float bl = 0.5f + 0.5f * sinf(t + 4.2f - u * 1.5f + v * 3.0f);
            float glow = expf(-((u - 0.62f) * (u - 0.62f) + (v - 0.4f) * (v - 0.4f)) * 9.0f);
            if (blobs) {
                glow += 0.8f * expf(-((u - 0.2f) * (u - 0.2f) + (v - 0.75f) * (v - 0.75f)) * 14.0f);
                glow += 0.7f * expf(-((u - 0.85f) * (u - 0.85f) + (v - 0.85f) * (v - 0.85f)) * 20.0f);
            }
            float dark = 0.35f + 0.65f * v * 0.3f;
            unsigned char *o = px + ((size_t)y * w + x) * 4;
            o[0] = (unsigned char)fminf(255, 255 * (r * dark + glow * 0.8f));
            o[1] = (unsigned char)fminf(255, 255 * (g * dark + glow * 0.6f));
            o[2] = (unsigned char)fminf(255, 255 * (bl * dark + glow * 0.9f));
            o[3] = 255;
        }
    BareImage img = bare_image_from_pixels(w, h, px);
    free(px);
    return img;
}

static BareImage thumbnail(void *user, const char *path, int width, int height) {
    Preview *p = user;
    (void)width, (void)height;
    if (strncmp(path, "gen:", 4)) return 0;
    int i = atoi(path + 4) & 63;
    if (!p->thumbs[i]) p->thumbs[i] = generated(i + 2, 480, 270, false);
    return p->thumbs[i];
}

static void open_path(void *user, const char *path) {
    char text[300];
    snprintf(text, sizeof text, "Would show %s", path);
    say(user, text, false);
}

static void frame(void *user) {
    Preview *p = user;
    const char *accent = setting("accent_color")->value;
    unsigned rgb = (unsigned)strtoul(accent + 1, NULL, 16);
    const char *motion = setting("ui_motion")->value;
    sp_theme_set(strcmp(setting("theme")->value, "light") != 0, BARE_RGB((rgb >> 16) & 255, (rgb >> 8) & 255, rgb & 255),
                 !strcmp(motion, "off") ? 2 : !strcmp(motion, "reduced") ? 1 : 0);
    p->model.backdrop = p->backdrop ? p->backdrop : bare_window_backdrop();
    sp_draw(&p->view, &p->model);
}

int main(int argc, char **argv) {
    static Preview p;
    static const char *pages[] = { "library", "displays", "customise", "store", "settings" };
    bool light = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--page") && i + 1 < argc)
            for (int k = 0; k < SP_PAGE_COUNT; k++) if (!strcmp(argv[i + 1], pages[k])) p.view.page = (SpPage)k;
        if (!strcmp(argv[i], "--light")) light = true;
        if (!strcmp(argv[i], "--graphs")) p.view.graphs[0] = p.view.graphs[2] = true, p.view.presets_for = 1;
    }
    if (light) snprintf(setting("theme")->value, 32, "light");
    p.audio = false;
    p.model.user = &p;
    p.model.cpu = 4.0f;
    p.model.connected = true;
    p.model.picture = thumbnail;
    p.model.run = command;
    p.model.open_path = open_path;
    p.view.page_time = -10;
    const char *tall = getenv("SP_PREVIEW_HEIGHT");   /* captures of a whole page */
    if (!bare_window_with(.width = 1475, .height = tall ? atoi(tall) : 950, .zoom = 0.8f, .title = "Screen+ (preview)", .frameless = true,
                          .min_width = 1020, .min_height = 660))
        return 1;
    rebuild(&p);
    const char *bg = getenv("SP_BACKDROP");
    if (bg && !strcmp(bg, "gen")) p.backdrop = generated(7, 1560, 1000, true);
    else if (bg && *bg) p.backdrop = bare_image_load(bg);
    bare_run(frame, &p);
    bare_quit();
    return 0;
}
