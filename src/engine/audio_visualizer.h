#pragma once
#include "../common/common.h"

/* Shared-mode WASAPI loopback analyser. It is deliberately started only when a
 * wallpaper registers an audio listener; callers read a compact, normalised
 * 64-band spectrum without ever blocking the engine message loop. */
BOOL audio_visualizer_start(void);
void audio_visualizer_stop(void);
BOOL audio_visualizer_spectrum(float out[64]);
