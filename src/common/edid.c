#include "edid.h"
#include "util.h"
#include "../monitor/monitor.h"
#include <stdio.h>
#include <string.h>
#ifndef _WIN32
#include <glob.h>
#endif

static int serial_char(unsigned char ch)
{
    return (ch >= '0' && ch <= '9') || (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || ch == '-' || ch == '.';
}

int edid_id(const unsigned char *e, size_t n, wchar_t *out, size_t cap)
{
    static const unsigned char head[8] = { 0, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0 };
    if (!e || n < 128 || memcmp(e, head, 8) != 0) return 0;
    unsigned mfg = ((unsigned)e[8] << 8) | e[9], product = e[10] | ((unsigned)e[11] << 8);
    wchar_t model[8] = { (wchar_t)('@' + ((mfg >> 10) & 31)), (wchar_t)('@' + ((mfg >> 5) & 31)), (wchar_t)('@' + (mfg & 31)), 0 };
    wchar_t serial[16] = { 0 };
    for (int d = 54; d <= 108 && !serial[0]; d += 18) {
        if (e[d] || e[d + 1] || e[d + 3] != 0xFF) continue;            /* display descriptor: serial number string */
        int k = 0;
        for (int i = 5; i < 18 && e[d + i] != 0x0A && k < 13; i++)
            if (serial_char(e[d + i])) serial[k++] = (wchar_t)e[d + i];
        serial[k] = 0;
    }
    unsigned long num = e[12] | ((unsigned long)e[13] << 8) | ((unsigned long)e[14] << 16) | ((unsigned long)e[15] << 24);
    if (serial[0]) str_printf(out, cap, L"EDID:%ls%04X:%ls", model, product, serial);
    else if (num && num != 0x01010101UL) str_printf(out, cap, L"EDID:%ls%04X:%lu", model, product, num);
    else return 0;
    return 1;
}

#ifndef _WIN32
int edid_id_for_connector(const char *connector, wchar_t *out, size_t cap)
{
    char pat[160];
    glob_t g;
    int ok = 0;
    if (!connector || !connector[0] || strchr(connector, '/')) return 0;
    snprintf(pat, sizeof pat, "/sys/class/drm/card*-%s/edid", connector);
    if (glob(pat, 0, NULL, &g) != 0) return 0;
    for (size_t i = 0; i < g.gl_pathc && !ok; i++) {
        unsigned char buf[512];
        FILE *f = fopen(g.gl_pathv[i], "rb");
        if (!f) continue;
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        ok = edid_id(buf, n, out, cap);
    }
    globfree(&g);
    return ok;
}
#endif

void edid_dedupe(MonitorInfo *mons, int n)
{
    for (int i = 0; i < n; i++) {
        if (!str_istarts(mons[i].id, L"EDID:")) continue;
        int dup = 0;
        for (int k = i + 1; k < n; k++)
            if (str_ieq(mons[i].id, mons[k].id)) {
                str_copy(mons[k].id, SP_COUNTOF(mons[k].id), mons[k].oldId);
                dup = 1;
            }
        if (dup) str_copy(mons[i].id, SP_COUNTOF(mons[i].id), mons[i].oldId);
    }
}
