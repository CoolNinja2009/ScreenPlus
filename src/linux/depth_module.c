/* `screenplus --depth ...` loads this: the depth child's FFmpeg and ONNX Runtime stay out of the service. */
#include "../app/app.h"
#include "../common/paths.h"
#include "../common/util.h"

BOOL video_ffmpeg_available(wchar_t *err, size_t cap) { (void)err; (void)cap; return TRUE; }

int depth_child_main(int argc, char **argv)
{
    wchar_t *image = NULL, *dir = NULL, *model = NULL;
    int mode = DEPTH_AUTO, png = 0, video = 0;
    HWND notify = NULL;
    paths_init();
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--depth") && i + 2 < argc) { image = wide_from_utf8(argv[i + 1], -1); dir = wide_from_utf8(argv[i + 2], -1); i += 2; }
        else if (!strcmp(argv[i], "--model") && i + 1 < argc) model = wide_from_utf8(argv[++i], -1);
        else if (!strcmp(argv[i], "--notify") && i + 1 < argc) notify = (HWND)(uintptr_t)strtoull(argv[++i], NULL, 16);
        else if (!strcmp(argv[i], "--video")) video = 1;
        else if (!strcmp(argv[i], "--cpu")) mode = DEPTH_CPU;
        else if (!strcmp(argv[i], "--bench")) mode = DEPTH_BENCH;
        else if (!strcmp(argv[i], "--crashed")) mode = DEPTH_CRASHED;
        else if (!strcmp(argv[i], "--png")) png = 1;
    }
    if (!image || !dir) return 2;
    return depth_infer_main(image, dir, mode, png, model ? model : L"", video, notify);
}
