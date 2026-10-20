/* Validation helper (not shipped): prints the livelySystemInformation payload
 * produced by src/engine/sysinfo.c over a few samples. Compile against the
 * app sources with the same defines: the engine builds include common.h
 * (unicode, COBJMACROS) sourcewide.
 *
 *      cl /nologo /std:c17 /MT /DUNICODE /D_UNICODE /DCOBJMACROS /DNOMINMAX
 *         /D_WIN32_WINNT=0x0A00 /I src sysinfo_test.c src\engine\sysinfo.c
 *         src\common\util.c src\common\log.c src\common\json.c
 *         /Fe:sysinfo_test.exe pdh.lib advapi32.lib dxgi.lib
 *      sysinfo_test.exe
 */
#include "../src/engine/sysinfo.h"
#include <stdio.h>
#include <windows.h>

int main(void)
{
    char json[2048];
    for (int i = 0; i < 6; i++) {
        sysinfo_sample(json, sizeof json);
        printf("sample %d:\n  %s\n", i, json);
        Sleep(500);   /* narrow sampling windows on purpose: last window is shorter */
    }
    return 0;
}
