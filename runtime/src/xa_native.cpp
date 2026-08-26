/* xa_native.cpp — see xa_native.h.
 *
 * One stb_vorbis handle per channel, kept open. Sectors for a channel arrive
 * in order during normal streaming, so the common case is a straight sequential
 * read; a seek only happens when the guest jumps (new track, loop, menu
 * change), tracked per channel by the next expected sample offset.
 */
#include "xa_native.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <toml.hpp>

extern "C" {
#include "stb_vorbis_decl.h"
}

namespace fs = std::filesystem;

namespace {

struct Track {
    stb_vorbis *vorb = nullptr;
    long long   next_offset = -1;   /* expected next read, in frames */
    int         sectors = 0;
};

struct Pack {
    bool  active = false;
    int   first_lba = 0;
    int   audio_sectors = 0;
    int   interleave = 0;
    int   out_frames_per_sector = 0;
    std::map<int, Track> tracks;    /* keyed by CD-XA channel */
};

Pack g_pack;
bool g_reported = false;   /* one-shot 'substitution is live' notice */
/* A decline is not benign here. When the sector came from a synthesised
 * stream region its ADPCM payload is ZERO, so falling back to decoding it
 * yields silence/garbage — an audible dropout rather than a graceful
 * fallback. Count them: they should be zero inside a covered region. */
unsigned long g_served = 0, g_declined = 0, g_seek_fail = 0, g_short = 0;
void xa_tally(const char* why) {
    g_declined++;
    if (getenv("PSX_XA_TRACE"))
        std::fprintf(stdout, "xa: decline (%s) served=%lu declined=%lu\n",
                     why, g_served, g_declined);
}

void close_tracks() {
    for (auto &kv : g_pack.tracks)
        if (kv.second.vorb) stb_vorbis_close(kv.second.vorb);
    g_pack.tracks.clear();
}

}  // namespace

extern "C" int xa_native_load(const char *dir) {
    xa_native_shutdown();
    if (!dir || !*dir) return 0;
    /* Runtime kill-switch — same reasoning as PSX_FMV_NATIVE. */
    {
        const char* e = getenv("PSX_XA_NATIVE");
        if (e && (*e == '0' || *e == 'n' || *e == 'N')) {
            std::fprintf(stdout, "psxrecomp: native XA music disabled (PSX_XA_NATIVE=0)\n");
            return 0;
        }
    }

    const fs::path root(dir);
    const fs::path manifest = root / "xa.toml";
    std::error_code ec;
    if (!fs::exists(manifest, ec)) return 0;

    try {
        const auto cfg = toml::parse(manifest.string());
        const auto &xa = toml::find(cfg, "xa");
        g_pack.first_lba = toml::find<int>(xa, "first_lba");
        g_pack.audio_sectors = toml::find<int>(xa, "audio_sectors");
        g_pack.interleave = toml::find<int>(xa, "interleave");
        g_pack.out_frames_per_sector = toml::find<int>(xa, "out_frames_per_sector");
        if (g_pack.interleave <= 0 || g_pack.out_frames_per_sector <= 0) {
            std::fprintf(stdout, "psxrecomp: xa pack has a bad layout; ignoring\n");
            g_pack = Pack{};
            return 0;
        }

        for (const auto &t : toml::find<toml::array>(cfg, "track")) {
            const int ch = toml::find<int>(t, "channel");
            const std::string file = toml::find<std::string>(t, "file");
            const fs::path p = root / file;
            int err = 0;
            stb_vorbis *v = stb_vorbis_open_filename(p.string().c_str(), &err, nullptr);
            if (!v) {
                std::fprintf(stdout, "psxrecomp: xa pack: cannot open %s (%d)\n",
                             p.string().c_str(), err);
                continue;
            }
            const stb_vorbis_info info = stb_vorbis_get_info(v);
            if (info.channels != 2) {
                std::fprintf(stdout,
                             "psxrecomp: xa pack: %s is not stereo; skipping\n",
                             file.c_str());
                stb_vorbis_close(v);
                continue;
            }
            Track tr;
            tr.vorb = v;
            tr.sectors = toml::find_or<int>(t, "sectors", 0);
            g_pack.tracks[ch] = tr;
        }
    } catch (const std::exception &e) {
        std::fprintf(stdout, "psxrecomp: xa pack unreadable (%s); ignoring\n", e.what());
        close_tracks();
        g_pack = Pack{};
        return 0;
    }

    if (g_pack.tracks.empty()) {
        g_pack = Pack{};
        return 0;
    }
    g_pack.active = true;
    std::fprintf(stdout,
                 "psxrecomp: native XA music: %zu tracks, interleave %d, "
                 "lba %d..%d\n",
                 g_pack.tracks.size(), g_pack.interleave, g_pack.first_lba,
                 g_pack.first_lba + g_pack.audio_sectors - 1);
    return 1;
}

extern "C" int xa_native_active(void) { return g_pack.active ? 1 : 0; }

extern "C" void xa_native_shutdown(void) {
    close_tracks();
    g_pack = Pack{};
    g_reported = false;
}

extern "C" int xa_native_sector(int lba, int file, int channel,
                                int16_t *out, int max_frames) {
    (void)file;   /* single-file packs; the manifest pins the region by LBA */
    if (!g_pack.active || !out) return 0;

    const int index = lba - g_pack.first_lba;
    if (index < 0 || index >= g_pack.audio_sectors) return 0;  /* outside: fine */
    if (index % g_pack.interleave != channel % g_pack.interleave) { xa_tally("interleave"); return 0; }

    auto it = g_pack.tracks.find(channel);
    if (it == g_pack.tracks.end() || !it->second.vorb) { xa_tally("no track"); return 0; }

    Track &tr = it->second;
    const int frames = g_pack.out_frames_per_sector;
    if (frames > max_frames) { xa_tally("buffer"); return 0; }

    const long long offset = (long long)(index / g_pack.interleave) * frames;
    if (offset != tr.next_offset) {
        if (!stb_vorbis_seek(tr.vorb, (unsigned int)offset)) {
            tr.next_offset = -1;
            g_seek_fail++;
            xa_tally("seek");
            return 0;
        }
    }

    const int got = stb_vorbis_get_samples_short_interleaved(tr.vorb, 2, out,
                                                             frames * 2);
    if (got <= 0) {
        tr.next_offset = -1;
        xa_tally("decode");
        return 0;
    }
    /* Past the end of the track the decoder short-reads; pad with silence so
     * the caller still gets a full sector and timing does not shift. */
    if (got < frames) g_short++;
    if (got < frames)
        std::memset(out + (size_t)got * 2, 0,
                    (size_t)(frames - got) * 2 * sizeof(int16_t));
    tr.next_offset = offset + frames;
    g_served++;
    /* One XA sector is 2016 frames at 37800 Hz = 53.33 ms of audio. If the
     * served rate exceeds ~18.75/s of guest time the same audio is being
     * pushed more than once, which is what a doubled/echoing stream sounds
     * like. Report the rate so that is measurable rather than guessed. */
    if (getenv("PSX_XA_TRACE") && (g_served % 200) == 0)
        std::fprintf(stdout,
                     "xa: served=%lu declined=%lu seekfail=%lu short=%lu "
                     "audio=%.2fs\n",
                     g_served, g_declined, g_seek_fail, g_short,
                     (double)g_served * 2352.0 / 44100.0);
    if (!g_reported) {
        g_reported = true;
        std::fprintf(stdout,
                     "psxrecomp: native XA music engaged (first at lba %d, "
                     "channel %d)\n", lba, channel);
    }
    return frames;
}
