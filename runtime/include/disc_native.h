/* disc_native.h — serve CD sectors from an asset pack instead of a disc image.
 *
 * This is the seam beneath libcd: everything a PS1 game streams — file data,
 * XA audio, MDEC video — arrives as raw 2352-byte sectors, so a provider here
 * covers every path at once, including the ones no function replacement can
 * reach (CdlReadS and the sector ring never call a function we could hook).
 *
 * Two kinds of region are served:
 *
 *   stored files   Form 1 data, rebuilt around the 2048 bytes held on disk.
 *   stream rules   Form 2 regions whose subheaders are a function of the
 *                  sector index. R4.STR interleaves its CD-XA channels on a
 *                  fixed 8-way cycle, so 372 MB of ADPCM needs no storage at
 *                  all: synthesise the subheader, leave the payload zero, and
 *                  let the native music pack supply the sound (xa_native.h).
 *
 * Declining a sector returns 0 and the caller reads the disc image, so a pack
 * that covers only part of a disc still works.
 */
#ifndef PSXRECOMP_DISC_NATIVE_H
#define PSXRECOMP_DISC_NATIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  disc_native_load(const char* dir);
int  disc_native_active(void);

/* One past the highest LBA the pack covers — the lead-out the controller
 * reports when no image is mounted. 0 when inactive. */
uint32_t disc_native_lead_out(void);

/* Fill `out` (2352 bytes) for `lba`. Returns 1 when served, 0 to fall back. */
int  disc_native_raw_sector(uint32_t lba, uint8_t* out, uint32_t size);

void disc_native_stats(uint64_t* file_sectors, uint64_t* stream_sectors,
                       uint64_t* declined);
void disc_native_shutdown(void);

#ifdef __cplusplus
}
#endif
#endif
