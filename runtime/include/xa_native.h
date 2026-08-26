/* xa_native.h — native music substitution for CD-XA audio.
 *
 * CD-XA is decoded by the runtime, not the guest: the game only ever asks the
 * drive to stream file F / channel C from an LBA, and cdrom.c turns the ADPCM
 * into PCM. That makes the audio trivially replaceable — swap in a
 * pre-decoded, re-encoded track and the guest cannot tell.
 *
 * A pack is generated from the player's own disc (tools/audio/build_xa_pack.py)
 * and described by xa.toml: the interleave, which channels carry audio, and
 * how many PCM frames one sector is worth. From those, the sector at `lba` on
 * `channel` maps to an exact sample offset, so playback never drifts:
 *
 *     k      = (lba - first_lba) / interleave
 *     offset = k * out_frames_per_sector
 *
 * Tracks are Vorbis at the SPU's own 44100, so substituted audio skips both
 * the ADPCM decode and the 37800->44100 resample.
 *
 * Absent or unreadable pack => every call reports "not covered" and cdrom.c
 * keeps decoding the disc, so this is purely additive.
 */
#ifndef PSXRECOMP_XA_NATIVE_H
#define PSXRECOMP_XA_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Load <dir>/xa.toml and its tracks. Returns 1 when a usable pack is live.
 * Safe to call with a NULL/missing directory (returns 0, stays inert). */
int  xa_native_load(const char *dir);

/* 1 when a pack is loaded and substitution should be attempted. */
int  xa_native_active(void);

/* PCM for one XA sector. Writes interleaved stereo frames at 44100 into `out`
 * and returns the frame count, or 0 when this sector is not covered by the
 * pack (wrong file/channel, outside the audio region, decode failure) — in
 * which case the caller must fall back to decoding the disc sector. */
int  xa_native_sector(int lba, int file, int channel,
                      int16_t *out, int max_frames);

void xa_native_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
