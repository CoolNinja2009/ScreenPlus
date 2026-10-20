#pragma once
#include "../common/common.h"

/* "Now playing" from the Windows media session (System Media Transport Controls), as the
 * JSON text Lively passes to livelyCurrentTrack: {"Title","Artist","AlbumTitle",...,
 * "Thumbnail": base64 or null}, or "null" when nothing is playing. A background thread
 * polls once a second while started; now_playing_get never blocks the engine. */
BOOL now_playing_start(void);
void now_playing_stop(void);
/* Returns a malloc'ed copy of the current JSON when it changed since *serial (and updates
 * *serial), otherwise NULL. Start *serial at 0 to receive the current state. */
char *now_playing_get(unsigned *serial);
