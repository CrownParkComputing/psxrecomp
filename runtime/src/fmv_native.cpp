/* fmv_native.cpp — see fmv_native.h. */
#include "fmv_native.h"

#ifndef PSX_HAVE_FFMPEG

extern "C" int  fmv_native_load(const char*) { return 0; }
extern "C" int  fmv_native_active(void) { return 0; }
extern "C" void fmv_native_note_sector(uint32_t, uint32_t) {}
extern "C" int  fmv_native_frame(int, int, int*, int*, const uint32_t**) { return 0; }
extern "C" int  fmv_native_frame_is_new(void) { return 0; }
extern "C" void fmv_native_shutdown(void) {}

#else

#include <cstdio>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
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

/* Decoding runs on its own thread. Inline it cost ~10% of emulation
 * throughput on R4's intro (199 s of guest progress became 220 s), and the
 * thread paying that is the one feeding audio — so the symptom is audio
 * artifacts, not dropped frames.
 *
 * Three composited buffers: the worker writes one, publishes it ready, the
 * presenter holds a third. The worker never touches what the presenter is
 * showing, so a frame cannot be overwritten mid-upload. State is no longer
 * assignable (it owns a mutex), so reset() clears it in place. */
struct State {
    bool               active = false;
    std::vector<Movie> movies;
    Decoder            dec;              /* worker thread only */
    int                dec_movie = -1;

    std::vector<uint32_t> buf[3];
    int                buf_w[3] = {0,0,0}, buf_h[3] = {0,0,0};
    int                buf_frame[3] = {-1,-1,-1};
    int                idx_ready = -1, idx_shown = -1;
    int                shown_frame = -1, last_new = -2;

    int                want_movie = -1, want_frame = 0, decoded_for = -1;
    int                guest_w = 0, guest_h = 0;

    std::mutex              m;
    std::condition_variable cv;
    std::thread             worker;
    std::atomic<bool>       stop{false};
    unsigned long      decodes = 0, presents = 0;
    bool               reported = false;

    void reset() {
        active = false; movies.clear(); dec_movie = -1;
        for (int i = 0; i < 3; i++) {
            buf[i].clear(); buf_w[i] = buf_h[i] = 0; buf_frame[i] = -1;
        }
        idx_ready = idx_shown = -1; shown_frame = -1; last_new = -2;
        want_movie = -1; want_frame = 0; decoded_for = -1;
        guest_w = guest_h = 0; decodes = presents = 0; reported = false;
        stop.store(false);
    }
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
    g.dec_movie = index;
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
    const Movie& m = g.movies[g.dec_movie];
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


/* Decode + composite whatever the CD path last asked for, off the emulation
 * thread entirely. */
void worker_main() {
    for (;;) {
        int movie, frame, gw, gh;
        {
            std::unique_lock<std::mutex> lk(g.m);
            g.cv.wait(lk, [] {
                return g.stop.load() ||
                       (g.want_movie >= 0 && g.guest_w > 0 &&
                        g.want_frame != g.decoded_for);
            });
            if (g.stop.load()) return;
            movie = g.want_movie; frame = g.want_frame;
            gw = g.guest_w;       gh = g.guest_h;
        }
        if (movie < 0 || movie >= (int)g.movies.size()) continue;
        const Movie& m = g.movies[movie];

        if (!g.dec.fmt || g.dec_movie != movie) {
            if (!open_movie(movie)) { g.decoded_for = frame; continue; }
        }
        if (!seek_to(frame)) { g.decoded_for = frame; continue; }
        g.decoded_for = frame;
        g.decodes++;

        int w = -1;
        {
            std::lock_guard<std::mutex> lk(g.m);
            for (int i = 0; i < 3; i++)
                if (i != g.idx_shown && i != g.idx_ready) { w = i; break; }
            if (w < 0) w = 0;
        }
        /* Rebuild the guest's framing at native scale: presenting the bare
         * movie would stretch it, because the bars are part of the picture. */
        int out_w = m.width, out_h = m.height;
        if (gw > 0 && gh > 0 && m.width % gw == 0) {
            const int scale = m.width / gw;
            const int h2 = gh * scale;
            if (h2 >= m.height) out_h = h2;
        }
        if (g.buf_w[w] != out_w || g.buf_h[w] != out_h) {
            g.buf[w].assign((size_t)out_w * out_h, 0xFF000000u);
            g.buf_w[w] = out_w; g.buf_h[w] = out_h;
        }
        const int y0 = (out_h - m.height) / 2;
        for (int y = 0; y < m.height; y++)
            std::memcpy(&g.buf[w][(size_t)(y0 + y) * out_w],
                        &g.dec.rgb[(size_t)y * m.width],
                        (size_t)m.width * sizeof(uint32_t));
        g.buf_frame[w] = frame;
        {
            std::lock_guard<std::mutex> lk(g.m);
            g.idx_ready = w;
        }
    }
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
        g.reset();
        return 0;
    }
    if (g.movies.empty()) { g.reset(); return 0; }
    g.active = true;
    g.stop.store(false);
    g.worker = std::thread(worker_main);
    std::fprintf(stdout, "psxrecomp: native FMV: %zu movie(s)\n", g.movies.size());
    for (const Movie& m : g.movies)
        std::fprintf(stdout, "psxrecomp:   %s %dx%d %d frames, lba %u..%u\n",
                     m.name.c_str(), m.width, m.height, m.frames,
                     m.first_lba, m.last_lba);
    return 1;
}

extern "C" int fmv_native_active(void) { return g.active ? 1 : 0; }

extern "C" int fmv_native_frame_is_new(void) {
    std::lock_guard<std::mutex> lk(g.m);
    if (g.last_new == g.shown_frame) return 0;
    g.last_new = g.shown_frame;
    return 1;
}

extern "C" void fmv_native_note_sector(uint32_t lba, uint32_t frame) {
    if (!g.active) return;
    for (size_t i = 0; i < g.movies.size(); i++) {
        const Movie& m = g.movies[i];
        if (lba < m.first_lba || lba > m.last_lba) continue;
        /* STR frame numbers are 1-based. Clamp: a movie's final frame can be
         * short in the re-encode when its last chunk is truncated on disc. */
        int idx = (int)frame - 1;
        if (idx < 0) idx = 0;
        if (idx >= m.frames) idx = m.frames - 1;
        {
            std::lock_guard<std::mutex> lk(g.m);
            g.want_movie = (int)i;
            g.want_frame = idx;
        }
        g.cv.notify_one();
        return;
    }
}

extern "C" int fmv_native_frame(int guest_w, int guest_h,
                                int* w, int* h, const uint32_t** pixels) {
    if (!g.active) return 0;
    std::lock_guard<std::mutex> lk(g.m);
    if (guest_w > 0 && guest_h > 0 &&
        (g.guest_w != guest_w || g.guest_h != guest_h)) {
        g.guest_w = guest_w; g.guest_h = guest_h;   /* framing for the worker */
        g.cv.notify_one();
    }
    if (g.idx_ready >= 0 && g.idx_ready != g.idx_shown) {
        g.idx_shown = g.idx_ready;
        g.shown_frame = g.buf_frame[g.idx_shown];
    }
    const int i = g.idx_shown;
    if (i < 0 || g.buf_w[i] <= 0) return 0;
    g.presents++;
    if (!g.reported) {
        g.reported = true;
        std::fprintf(stdout,
                     "psxrecomp: native FMV engaged (%dx%d, threaded decode)\n",
                     g.buf_w[i], g.buf_h[i]);
    }
    if (getenv("PSX_FMV_TRACE") && (g.presents % 300) == 0)
        std::fprintf(stdout, "fmv: presents=%lu decodes=%lu\n",
                     g.presents, g.decodes);
    if (w) *w = g.buf_w[i];
    if (h) *h = g.buf_h[i];
    if (pixels) *pixels = g.buf[i].data();
    return 1;
}

extern "C" void fmv_native_shutdown(void) {
    if (g.worker.joinable()) {
        g.stop.store(true);
        g.cv.notify_all();
        g.worker.join();
    }
    close_decoder();
    g.reset();
}

#endif /* PSX_HAVE_FFMPEG */
