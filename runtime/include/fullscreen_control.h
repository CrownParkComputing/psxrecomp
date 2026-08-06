#ifndef PSXRECOMP_FULLSCREEN_CONTROL_H
#define PSXRECOMP_FULLSCREEN_CONTROL_H

#include <stddef.h>

#include "psx_sdl.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Tri-state display control shared by every PSXRecomp title:
 *   0 = decorated/resizable window
 *   1 = true borderless window (monitor-sized, not SDL fullscreen)
 *   2 = exclusive fullscreen with an explicit display mode
 */
int psx_fullscreen_init(SDL_Window *window, int configured_mode);
int psx_fullscreen_toggle(int configured_mode);
void psx_fullscreen_handle_event(const SDL_Event *event);
void psx_fullscreen_shutdown(void);
int psx_fullscreen_applied_mode(void);

/*
 * Structured, bounded diagnostic report for the TCP debug surface.
 * Returns the number of bytes written, excluding the trailing NUL.
 */
int psx_fullscreen_debug_json(char *out, size_t capacity, int request_id);

#ifdef __cplusplus
}
#endif

#endif
