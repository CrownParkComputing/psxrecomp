/* fmv_native.cpp — see fmv_native.h. */
#include "fmv_native.h"

#ifndef PSX_HAVE_FFMPEG

extern "C" int  fmv_native_load(const char*) { return 0; }
extern "C" int  fmv_native_active(void) { return 0; }
extern "C" void fmv_native_note_sector(uint32_t, uint32_t) {}
extern "C" int  fmv_native_frame(int, int, int*, int*, const uint32_t**) { return 0; }
extern "C" void fmv_native_shutdown(void) {}

#else

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include <toml.hpp>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

namespace {

struct Movie {
    std::string name, file;
    uint32_t    first_lba = 0, last_lba = 0;
    int         frames = 0, width = 0, height = 0;
    fs::path    path;
};

struct Decoder {
    AVFormatContext* fmt = nullptr;
    AVCodecContext*  dec = nullptr;
    SwsContext*      sws = nullptr;
    AVPacket*        pkt = nullptr;
    AVFrame*         frm = nullptr;
    int              stream = -1;
    int              pos = -1;        /* index of the frame in `rgb` */
    std::vector<uint32_t> rgb;
};

struct State {
    bool               active = false;
    std::vector<Movie> movies;
    int                current = -1;  /* index into movies */
    uint32_t           want_frame = 0;
    bool               have_request = false;
    Decoder            dec;
    std::vector<uint32_t> composed;   /* movie centred in the guest's framing */
    int                composed_w = 0, composed_h = 0;
    int                composed_for = -1;   /* frame currently composited */
    int                decoded_for = -1;
    bool               reported = false;
};

State g;

void close_decoder() {
    if (g.dec.sws) { sws_freeContext(g.dec.sws); g.dec.sws = nullptr; }
    if (g.dec.frm) av_frame_free(&g.dec.frm);
    if (g.dec.pkt) av_packet_free(&g.dec.pkt);
    if (g.dec.dec) avcodec_free_context(&g.dec.dec);
    if (g.dec.fmt) avformat_close_input(&g.dec.fmt);
    g.dec = Decoder{};
}

bool open_movie(int index) {
    close_decoder();
    const Movie& m = g.movies[index];
    if (avformat_open_input(&g.dec.fmt, m.path.string().c_str(), nullptr, nullptr) < 0)
        return false;
    if (avformat_find_stream_info(g.dec.fmt, nullptr) < 0) return false;
    const AVCodec* codec = nullptr;
    g.dec.stream = av_find_best_stream(g.dec.fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (g.dec.stream < 0 || !codec) return false;
    g.dec.dec = avcodec_alloc_context3(codec);
    if (!g.dec.dec) return false;
    avcodec_parameters_to_context(g.dec.dec, g.dec.fmt->streams[g.dec.stream]->codecpar);
    if (avcodec_open2(g.dec.dec, codec, nullptr) < 0) return false;
    g.dec.pkt = av_packet_alloc();
    g.dec.frm = av_frame_alloc();
    if (!g.dec.pkt || !g.dec.frm) return false;
    g.dec.rgb.assign((size_t)m.width * m.height, 0);
    g.dec.pos = -1;
    return true;
}

/* Pull exactly one more frame out of the stream into `rgb`. */
bool decode_next() {
    for (;;) {
        int r = avcodec_receive_frame(g.dec.dec, g.dec.frm);
        if (r == 0) break;
        if (r != AVERROR(EAGAIN)) return false;
        if (av_read_frame(g.dec.fmt, g.dec.pkt) < 0) return false;
        if (g.dec.pkt->stream_index != g.dec.stream) { av_packet_unref(g.dec.pkt); continue; }
        r = avcodec_send_packet(g.dec.dec, g.dec.pkt);
        av_packet_unref(g.dec.pkt);
        if (r < 0) return false;
    }
    const Movie& m = g.movies[g.current];
    g.dec.sws = sws_getCachedContext(
        g.dec.sws, g.dec.frm->width, g.dec.frm->height,
        (AVPixelFormat)g.dec.frm->format, m.width, m.height,
        AV_PIX_FMT_BGRA, SWS_BILINEAR, nullptr, nullptr, nullptr);
    if (!g.dec.sws) return false;
    uint8_t* dst[4] = { (uint8_t*)g.dec.rgb.data(), nullptr, nullptr, nullptr };
    int stride[4] = { m.width * 4, 0, 0, 0 };
    sws_scale(g.dec.sws, g.dec.frm->data, g.dec.frm->linesize, 0,
              g.dec.frm->height, dst, stride);
    g.dec.pos++;
    return true;
}

bool seek_to(int frame) {
    if (frame == g.dec.pos) return true;
    /* Forward within a short reach is cheaper decoded than seeked; anything
     * else goes back to the nearest keyframe and rolls forward. */
    if (frame < g.dec.pos || frame - g.dec.pos > 48) {
        const AVStream* st = g.dec.fmt->streams[g.dec.stream];
        const int64_t ts = av_rescale_q(frame, av_inv_q(st->avg_frame_rate), st->time_base);
        if (av_seek_frame(g.dec.fmt, g.dec.stream, ts, AVSEEK_FLAG_BACKWARD) < 0)
            return false;
        avcodec_flush_buffers(g.dec.dec);
        /* Decode one to learn where the keyframe actually landed. */
        if (!decode_next()) return false;
        const AVStream* s2 = g.dec.fmt->streams[g.dec.stream];
        const int64_t pts = g.dec.frm->best_effort_timestamp;
        g.dec.pos = (pts == AV_NOPTS_VALUE)
            ? frame
            : (int)av_rescale_q(pts, s2->time_base, av_inv_q(s2->avg_frame_rate));
    }
    while (g.dec.pos < frame)
        if (!decode_next()) return false;
    return true;
}

}  // namespace

extern "C" int fmv_native_load(const char* dir) {
    fmv_native_shutdown();
    if (!dir || !*dir) return 0;
    /* Runtime kill-switch. Substitution quality is a judgement only a listener
     * can make, so make it flippable without a rebuild or a config edit. */
    {
        const char* e = getenv("PSX_FMV_NATIVE");
        if (e && (*e == '0' || *e == 'n' || *e == 'N')) {
            std::fprintf(stdout, "psxrecomp: native FMV disabled (PSX_FMV_NATIVE=0)\n");
            return 0;
        }
    }
    const fs::path root(dir);
    const fs::path manifest = root / "fmv.toml";
    std::error_code ec;
    if (!fs::exists(manifest, ec)) return 0;
    try {
        const auto cfg = toml::parse(manifest.string());
        for (const auto& t : toml::find<toml::array>(cfg, "movie")) {
            Movie m;
            m.name = toml::find<std::string>(t, "name");
            m.file = toml::find<std::string>(t, "file");
            m.frames = (int)toml::find<int64_t>(t, "frames");
            m.width = (int)toml::find<int64_t>(t, "width");
            m.height = (int)toml::find<int64_t>(t, "height");
            m.first_lba = (uint32_t)toml::find_or<int64_t>(t, "first_lba", 0);
            m.last_lba = (uint32_t)toml::find_or<int64_t>(t, "last_lba", 0);
            m.path = root / m.file;
            if (!m.first_lba || !fs::exists(m.path, ec)) continue;
            g.movies.push_back(std::move(m));
        }
    } catch (const std::exception& e) {
        std::fprintf(stdout, "psxrecomp: fmv.toml unreadable (%s)\n", e.what());
        g = State{};
        return 0;
    }
    if (g.movies.empty()) { g = State{}; return 0; }
    g.active = true;
    std::fprintf(stdout, "psxrecomp: native FMV: %zu movie(s)\n", g.movies.size());
    for (const Movie& m : g.movies)
        std::fprintf(stdout, "psxrecomp:   %s %dx%d %d frames, lba %u..%u\n",
                     m.name.c_str(), m.width, m.height, m.frames,
                     m.first_lba, m.last_lba);
    return 1;
}

extern "C" int fmv_native_active(void) { return g.active ? 1 : 0; }

extern "C" void fmv_native_note_sector(uint32_t lba, uint32_t frame) {
    if (!g.active) return;
    for (size_t i = 0; i < g.movies.size(); i++) {
        const Movie& m = g.movies[i];
        if (lba < m.first_lba || lba > m.last_lba) continue;
        if ((int)i != g.current) {
            g.current = (int)i;
            g.decoded_for = -1;
            close_decoder();
        }
        /* STR frame numbers are 1-based. Clamp: a movie's final frame can be
         * short in the re-encode when the last chunk is truncated on disc. */
        int idx = (int)frame - 1;
        if (idx < 0) idx = 0;
        if (idx >= m.frames) idx = m.frames - 1;
        g.want_frame = (uint32_t)idx;
        g.have_request = true;
        return;
    }
}

extern "C" int fmv_native_frame(int guest_w, int guest_h,
                                int* w, int* h, const uint32_t** pixels) {
    if (!g.active || !g.have_request || g.current < 0) return 0;
    const Movie& m = g.movies[g.current];
    if (!g.dec.fmt && !open_movie(g.current)) { g.have_request = false; return 0; }
    if (g.decoded_for != (int)g.want_frame) {
        if (!seek_to((int)g.want_frame)) return 0;
        g.decoded_for = (int)g.want_frame;
    }

    /* Rebuild the guest's framing at the native scale. Without this a 2:1
     * movie presented as a whole 4:3 frame is stretched, because the bars it
     * sat inside were part of the original picture. */
    if (guest_w > 0 && guest_h > 0 && m.width % guest_w == 0) {
        const int scale = m.width / guest_w;
        const int out_w = m.width;
        const int out_h = guest_h * scale;
        if (out_h >= m.height) {
            if (g.composed_w != out_w || g.composed_h != out_h) {
                /* The surround is static black and every rebuild overwrites
                 * the movie rect in full, so this clear belongs to allocation
                 * — not to each frame. */
                g.composed.assign((size_t)out_w * out_h, 0xFF000000u);
                g.composed_w = out_w;
                g.composed_h = out_h;
                g.composed_for = -1;
            }
            /* Recomposite only when the frame actually changed. A 15 fps movie
             * presented at 60 Hz repeats each frame four times, and rebuilding
             * a 1280x960 buffer every present put ~490 MB/s of needless copy
             * traffic on the thread that also feeds audio. */
            if (g.composed_for != (int)g.want_frame) {
                const int y0 = (out_h - m.height) / 2;
                for (int y = 0; y < m.height; y++)
                    std::memcpy(&g.composed[(size_t)(y0 + y) * out_w],
                                &g.dec.rgb[(size_t)y * m.width],
                                (size_t)m.width * sizeof(uint32_t));
                g.composed_for = (int)g.want_frame;
            }
            if (!g.reported) {
                g.reported = true;
                std::fprintf(stdout,
                             "psxrecomp: native FMV engaged (%s, %dx%d in a "
                             "%dx%d frame, %dx)\n", m.name.c_str(),
                             m.width, m.height, out_w, out_h, scale);
            }
            if (w) *w = out_w;
            if (h) *h = out_h;
            if (pixels) *pixels = g.composed.data();
            return 1;
        }
    }
    if (!g.reported) {
        g.reported = true;
        std::fprintf(stdout,
                     "psxrecomp: native FMV engaged (%s, %dx%d, frame %u)\n",
                     m.name.c_str(), m.width, m.height, g.want_frame);
    }
    if (w) *w = m.width;
    if (h) *h = m.height;
    if (pixels) *pixels = g.dec.rgb.data();
    return 1;
}

extern "C" void fmv_native_shutdown(void) {
    close_decoder();
    g = State{};
}

#endif /* PSX_HAVE_FFMPEG */
