/* disc_native.cpp — see disc_native.h. */
#include "disc_native.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <array>
#include <vector>

#include <toml.hpp>

namespace fs = std::filesystem;

namespace {

constexpr uint32_t RAW = 2352u;
constexpr uint32_t FORM1 = 2048u;
constexpr uint32_t FORM2_BYTES = 2336u;
constexpr uint32_t LEAD_IN = 150u;

struct FileRegion {
    std::string name;
    uint32_t    lba = 0;
    uint32_t    size = 0;
    uint32_t    sectors = 0;
    fs::path    path;
};

struct StreamRegion {
    std::string name;
    uint32_t    first_lba = 0;
    uint32_t    sectors = 0;
    uint32_t    interleave = 0;
    std::vector<std::array<uint8_t, 4>> subheaders;
};

struct State {
    bool                      active = false;
    std::vector<FileRegion>   files;
    std::vector<StreamRegion> streams;
    uint64_t file_sectors = 0, stream_sectors = 0, declined = 0;
    bool reported_file = false, reported_stream = false;
    uint64_t next_report = 20000;
};

State g;

/* One-shot lines say "it started"; a running tally says how much of the disc
 * the pack is actually carrying, which is the number that matters. */
void maybe_report() {
    const uint64_t total = g.file_sectors + g.stream_sectors;
    if (total < g.next_report) return;
    g.next_report = total + 20000;
    std::fprintf(stdout,
                 "psxrecomp: native disc: %llu file + %llu stream sectors "
                 "served, %llu declined to the disc image\n",
                 (unsigned long long)g.file_sectors,
                 (unsigned long long)g.stream_sectors,
                 (unsigned long long)g.declined);
}

inline uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Sync + MSF header + mode byte, common to both forms. The emulation reads
 * byte 15 (mode) and the subheader; EDC/ECC are not checked, so they stay
 * zero rather than being faked. */
void write_header(uint8_t* out, uint32_t lba, uint8_t mode) {
    std::memset(out, 0, RAW);
    out[0] = 0x00;
    std::memset(out + 1, 0xFF, 10);
    out[11] = 0x00;
    const uint32_t abs = lba + LEAD_IN;
    out[12] = bcd(abs / (75u * 60u));
    out[13] = bcd((abs / 75u) % 60u);
    out[14] = bcd(abs % 75u);
    out[15] = mode;
}

void write_subheader(uint8_t* out, uint8_t file, uint8_t chan,
                     uint8_t submode, uint8_t coding) {
    out[16] = file; out[17] = chan; out[18] = submode; out[19] = coding;
    out[20] = file; out[21] = chan; out[22] = submode; out[23] = coding;
}

}  // namespace

extern "C" int disc_native_load(const char* dir) {
    disc_native_shutdown();
    if (!dir || !*dir) return 0;
    const fs::path root(dir);
    const fs::path manifest = root / "disc.toml";
    std::error_code ec;
    if (!fs::exists(manifest, ec)) return 0;

    try {
        const auto cfg = toml::parse(manifest.string());
        if (cfg.contains("file")) {
            for (const auto& t : toml::find<toml::array>(cfg, "file")) {
                if (!toml::find_or<bool>(t, "stored", false)) continue;
                FileRegion r;
                r.name = toml::find<std::string>(t, "name");
                r.lba = (uint32_t)toml::find<int64_t>(t, "lba");
                r.size = (uint32_t)toml::find<int64_t>(t, "size");
                r.sectors = (r.size + FORM1 - 1) / FORM1;
                r.path = root / r.name;
                if (!fs::exists(r.path, ec)) continue;
                g.files.push_back(std::move(r));
            }
        }
        if (cfg.contains("stream")) {
            for (const auto& t : toml::find<toml::array>(cfg, "stream")) {
                StreamRegion r;
                r.name = toml::find<std::string>(t, "name");
                r.first_lba = (uint32_t)toml::find<int64_t>(t, "first_lba");
                r.sectors = (uint32_t)toml::find<int64_t>(t, "sectors");
                r.interleave = (uint32_t)toml::find<int64_t>(t, "interleave");
                for (const auto& row : toml::find<toml::array>(t, "subheaders")) {
                    const auto v = toml::get<std::vector<int64_t>>(row);
                    if (v.size() != 4) continue;
                    r.subheaders.push_back({(uint8_t)v[0], (uint8_t)v[1],
                                            (uint8_t)v[2], (uint8_t)v[3]});
                }
                if (r.interleave && r.subheaders.size() == r.interleave)
                    g.streams.push_back(std::move(r));
            }
        }
    } catch (const std::exception& e) {
        std::fprintf(stdout, "psxrecomp: disc.toml unreadable (%s)\n", e.what());
        g = State{};
        return 0;
    }
    if (g.files.empty() && g.streams.empty()) { g = State{}; return 0; }
    g.active = true;
    std::fprintf(stdout,
                 "psxrecomp: native disc provider: %zu stored file(s), "
                 "%zu synthesised stream(s)\n",
                 g.files.size(), g.streams.size());
    return 1;
}

extern "C" int disc_native_active(void) { return g.active ? 1 : 0; }

extern "C" int disc_native_raw_sector(uint32_t lba, uint8_t* out, uint32_t size) {
    if (!g.active || !out || size < RAW) return 0;

    for (const FileRegion& r : g.files) {
        if (lba < r.lba || lba >= r.lba + r.sectors) continue;
        const uint64_t off = (uint64_t)(lba - r.lba) * FORM1;
        std::ifstream f(r.path, std::ios::binary);
        if (!f) break;
        write_header(out, lba, 2);
        /* Form 1 data: submode bit 3 (data), no audio/video, no Form 2 bit. */
        write_subheader(out, 1, 0, 0x08, 0x00);
        f.seekg((std::streamoff)off);
        f.read((char*)(out + 24), FORM1);
        g.file_sectors++;
        maybe_report();
        if (!g.reported_file) {
            g.reported_file = true;
            std::fprintf(stdout,
                         "psxrecomp: native disc: serving %s from the pack "
                         "(first lba %u)\n", r.name.c_str(), lba);
        }
        return 1;
    }

    for (const StreamRegion& r : g.streams) {
        if (lba < r.first_lba || lba >= r.first_lba + r.sectors) continue;
        const uint32_t i = (lba - r.first_lba) % r.interleave;
        const auto& sh = r.subheaders[i];
        write_header(out, lba, 2);
        write_subheader(out, sh[0], sh[1], sh[2], sh[3]);
        /* Payload deliberately left zero: an XA audio sector's sound comes
         * from the native music pack, so the ADPCM never has to exist. */
        g.stream_sectors++;
        maybe_report();
        if (!g.reported_stream) {
            g.reported_stream = true;
            std::fprintf(stdout,
                         "psxrecomp: native disc: synthesising %s stream "
                         "sectors (first lba %u, %u-way)\n",
                         r.name.c_str(), lba, r.interleave);
        }
        return 1;
    }

    g.declined++;
    return 0;
}

extern "C" void disc_native_stats(uint64_t* fsec, uint64_t* ssec, uint64_t* dec) {
    if (fsec) *fsec = g.file_sectors;
    if (ssec) *ssec = g.stream_sectors;
    if (dec)  *dec  = g.declined;
}

extern "C" void disc_native_shutdown(void) { g = State{}; }
