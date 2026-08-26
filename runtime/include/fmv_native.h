/* fmv_native.h — present a re-encoded movie in place of the guest's MDEC.
 *
 * The guest keeps streaming and decoding its own MDEC: its frame pacing, its
 * XA audio and its skip/abort logic all stay authoritative. Only the picture
 * is replaced, at present time.
 *
 * Sync needs no timestamps and no counting. Every STR video sector carries the
 * frame number it belongs to, so cdrom.c reports those as they pass and the
 * presenter simply shows native frame N for guest frame N. That survives
 * pauses, skips and loops for free, because it is the guest's own numbering.
 *
 * Needs FFmpeg; without it PSX_HAVE_FFMPEG is undefined, every entry point is
 * a stub, and the guest's MDEC output is presented as before.
 */
#ifndef PSXRECOMP_FMV_NATIVE_H
#define PSXRECOMP_FMV_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  fmv_native_load(const char* dir);
int  fmv_native_active(void);

/* Called for each STR video sector delivered, with the frame number from its
 * header. Selects the movie by LBA and tracks the frame being decoded. */
void fmv_native_note_sector(uint32_t lba, uint32_t frame);

/* The native picture for the frame the guest is currently showing. Returns 1
 * and fills a BGRA8 buffer valid until the next call; 0 when there is nothing
 * to substitute (no pack, no movie playing, decode failed). */
/* guest_w/guest_h are the dimensions of the frame being replaced. MDEC movies
 * are usually letterboxed inside a taller display, and presenting the bare
 * movie would stretch it to that display's aspect; the returned buffer keeps
 * the guest's proportions with the movie centred, so only resolution changes. */
int  fmv_native_frame(int guest_w, int guest_h,
                      int* w, int* h, const uint32_t** pixels);

/* 1 when the last fmv_native_frame() produced a picture the presenter has not
 * shown yet. A 15 fps movie repeats each frame across several presents, and
 * re-uploading an unchanged one is pure bandwidth. */
int  fmv_native_frame_is_new(void);

void fmv_native_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
