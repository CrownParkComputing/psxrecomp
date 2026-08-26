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

/* Handles stay open for the life of the pack. Re-opening per sector put an
 * open()/close() pair on the emulation thread for every sector the guest
 * streamed — a few hundred a second during a movie, which is exactly the kind
 * of hitching that surfaces as audio artifacts rather than dropped frames. */
struct FileRegion {
    std::string name;
    uint32_t    lba = 0;
    uint32_t    size = 0;
    uint32_t    sectors = 0;
    fs::path    path;
    mutable std::ifstream file;
};

/* Sectors kept verbatim: whatever a stream rule cannot generate. For a movie
 * region the bytes genuinely matter — the guest's MDEC decodes them even when
 * a native re-encode is what the player sees — so they are stored as raw 2352
 * byte sectors rather than synthesised. */
struct RawBlob {
    std::string name;
    uint32_t    first_lba = 0;
    uint32_t    sectors = 0;
    fs::path    path;
    mutable std::ifstream file;
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
    std::vector<RawBlob>      blobs;
    uint64_t file_sectors = 0, stream_sectors = 0, raw_sectors = 0, declined = 0;
    bool reported_file = false, reported_stream = false, reported_raw = false;
    uint64_t next_report = 20000;
};

State g;

/* One-shot lines say "it started"; a running tally says how much of the disc
 * the pack is actually carrying, which is the number that matters. */
void maybe_report() {
    const uint64_t total = g.file_sectors + g.stream_sectors + g.raw_sectors;
    if (total < g.next_report) return;
    g.next_report = total + 20000;
    std::fprintf(stdout,
                 "psxrecomp: native disc: %llu file + %llu stream + %llu raw "
                 "sectors served, %llu declined to the disc image\n",
                 (unsigned long long)g.file_sectors,
                 (unsigned long long)g.stream_sectors,
                 (unsigned long long)g.raw_sectors,
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
        if (cfg.contains("rawblob")) {
            for (const auto& t : toml::find<toml::array>(cfg, "rawblob")) {
                RawBlob b;
                b.name = toml::find<std::string>(t, "name");
                b.first_lba = (uint32_t)toml::find<int64_t>(t, "first_lba");
                b.sectors = (uint32_t)toml::find<int64_t>(t, "sectors");
                b.path = root / toml::find<std::string>(t, "file");
                if (b.sectors && fs::exists(b.path, ec)) g.blobs.push_back(std::move(b));
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
    if (g.files.empty() && g.streams.empty() && g.blobs.empty()) {
        g = State{};
        return 0;
    }
    g.active = true;
    std::fprintf(stdout,
                 "psxrecomp: native disc provider: %zu stored file(s), "
                 "%zu synthesised stream(s), %zu raw region(s)\n",
                 g.files.size(), g.streams.size(), g.blobs.size());
    return 1;
}

extern "C" int disc_native_active(void) { return g.active ? 1 : 0; }

extern "C" uint32_t disc_native_lead_out(void) {
    uint32_t end = 0;
    for (const FileRegion& r : g.files)
        if (r.lba + r.sectors > end) end = r.lba + r.sectors;
    for (const StreamRegion& r : g.streams)
        if (r.first_lba + r.sectors > end) end = r.first_lba + r.sectors;
    for (const RawBlob& b : g.blobs)
        if (b.first_lba + b.sectors > end) end = b.first_lba + b.sectors;
    return end;
}

extern "C" int disc_native_raw_sector(uint32_t lba, uint8_t* out, uint32_t size) {
    if (!g.active || !out || size < RAW) return 0;

    for (const FileRegion& r : g.files) {
        if (lba < r.lba || lba >= r.lba + r.sectors) continue;
        const uint64_t off = (uint64_t)(lba - r.lba) * FORM1;
        if (!r.file.is_open()) r.file.open(r.path, std::ios::binary);
        if (!r.file) break;
        std::ifstream& f = r.file;
        f.clear();
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

    for (const RawBlob& b : g.blobs) {
        if (lba < b.first_lba || lba >= b.first_lba + b.sectors) continue;
        if (!b.file.is_open()) b.file.open(b.path, std::ios::binary);
        if (!b.file) break;
        std::ifstream& f = b.file;
        f.clear();
        f.seekg((std::streamoff)(lba - b.first_lba) * RAW);
        std::memset(out, 0, RAW);
        f.read((char*)out, RAW);
        g.raw_sectors++;
        maybe_report();
        if (!g.reported_raw) {
            g.reported_raw = true;
            std::fprintf(stdout,
                         "psxrecomp: native disc: serving %s raw sectors "
                         "(first lba %u)\n", b.name.c_str(), lba);
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
