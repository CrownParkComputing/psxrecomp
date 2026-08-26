/* libcd_native.h — serve PSY-Q libcd from an extracted asset pack.
 *
 * Every loader in a PSY-Q title funnels through the same handful of libcd
 * calls, and they all work in absolute LBAs the game got from CdSearchFile.
 * So the way to stop needing a drive is not to fake sectors underneath the CD
 * emulation — it is to answer those calls directly, with the SAME positions
 * the disc would have reported, from files on disk.
 *
 *   CdSearchFile  -> fill CdlFILE from disc.toml (real LBA, real size)
 *   CdControl     -> observed only, to track the Setloc position
 *   CdRead        -> memcpy Form 1 user data from the extracted file
 *   CdReadSync    -> complete immediately (loads stop costing anything)
 *
 * Handlers decline anything they do not cover, which runs the original guest
 * code, so the layer can be brought up one call at a time and a gap degrades
 * to "reads the disc" rather than "breaks".
 *
 * Addresses are per-game and come from [libcd] in game.toml; they must also be
 * listed in [recompiler] native_funcs or no hook is emitted.
 */
#ifndef PSXRECOMP_LIBCD_NATIVE_H
#define PSXRECOMP_LIBCD_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t cd_search_file;
    uint32_t cd_control;
    uint32_t cd_read;
    uint32_t cd_read_sync;
} LibcdNativeAddrs;

/* Load <dir>/disc.toml and bind handlers. Returns 1 when the layer is live. */
int  libcd_native_load(const char* dir, const LibcdNativeAddrs* addrs);
int  libcd_native_active(void);

/* The resolved asset directory ([libcd] asset_dir, made absolute against
 * game.toml). Empty string when no pack is loaded. Game-specific hooks should
 * use this rather than guessing a path relative to the process cwd. */
const char* libcd_native_dir(void);
void libcd_native_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
