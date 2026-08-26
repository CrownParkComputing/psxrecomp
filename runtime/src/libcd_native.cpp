/* libcd_native.cpp — see libcd_native.h. */
#include "libcd_native.h"
#include "native_call.h"
#include "cpu_state.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <toml.hpp>

extern "C" uint8_t* memory_get_ram_ptr(void);

namespace fs = std::filesystem;

namespace {

constexpr uint32_t RAM_SIZE = 2u * 1024u * 1024u;
constexpr uint32_t FORM1 = 2048u;
constexpr uint32_t LEAD_IN = 150u;      /* CdIntToPos' 2-second lead-in */

struct Entry {
    std::string name;       /* "R4.BIN" */
    std::string iso_name;   /* "R4.BIN;1" */
    uint32_t    lba = 0;
    uint32_t    size = 0;
    bool        stored = false;
    fs::path    path;
};

struct State {
    bool               active = false;
    std::vector<Entry> files;
    uint32_t           setloc_lba = 0;   /* last CdControl(CdlSetloc) */
    bool               served_read = false;
    uint64_t           bytes_served = 0;
    uint32_t           reads = 0;
    bool               reported = false;
};

State g;

inline uint8_t bcd(uint32_t v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

/* Guest RAM helpers. Addresses are KUSEG/KSEG0/KSEG1 views of the same 2 MB. */
inline bool ram_range(uint32_t addr, uint32_t len, uint8_t** out) {
    const uint32_t phys = addr & 0x1FFFFFFu;
    if (phys >= RAM_SIZE || (uint64_t)phys + len > RAM_SIZE) return false;
    uint8_t* ram = memory_get_ram_ptr();
    if (!ram) return false;
    *out = ram + phys;
    return true;
}

std::string guest_string(uint32_t addr, size_t max = 64) {
    uint8_t* p = nullptr;
    if (!ram_range(addr, 1, &p)) return {};
    std::string s;
    for (size_t i = 0; i < max && p[i]; i++) s.push_back((char)p[i]);
    return s;
}

const Entry* find_by_name(std::string name) {
    /* Guest names look like "\R4.BIN;1"; match on the bare filename. */
    size_t slash = name.find_last_of("\\/");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    const size_t semi = name.find(';');
    if (semi != std::string::npos) name = name.substr(0, semi);
    for (const Entry& e : g.files)
        if (e.name == name) return &e;
    return nullptr;
}

const Entry* find_by_lba(uint32_t lba) {
    for (const Entry& e : g.files) {
        if (!e.stored) continue;
        const uint32_t sectors = (e.size + FORM1 - 1) / FORM1;
        if (lba >= e.lba && lba < e.lba + sectors) return &e;
    }
    return nullptr;
}

/* --- handlers ---------------------------------------------------------- */

int on_search_file(CPUState* cpu, uint32_t, void*) {
    const uint32_t out = cpu->gpr[4];          /* CdlFILE*  */
    const std::string name = guest_string(cpu->gpr[5]);
    const Entry* e = find_by_name(name);
    if (!e) return 0;                          /* decline: let libcd search */

    uint8_t* p = nullptr;
    if (!ram_range(out, 24, &p)) return 0;     /* CdlFILE = pos+size+name[16] */

    /* CdlLOC is BCD minute/second/sector of (lba + lead-in). */
    const uint32_t abs = e->lba + LEAD_IN;
    p[0] = bcd(abs / (75u * 60u));
    p[1] = bcd((abs / 75u) % 60u);
    p[2] = bcd(abs % 75u);
    p[3] = 0;
    p[4] = (uint8_t)(e->size & 0xFF);
    p[5] = (uint8_t)((e->size >> 8) & 0xFF);
    p[6] = (uint8_t)((e->size >> 16) & 0xFF);
    p[7] = (uint8_t)((e->size >> 24) & 0xFF);
    std::memset(p + 8, 0, 16);
    std::snprintf((char*)(p + 8), 16, "%s", e->iso_name.c_str());

    cpu->gpr[2] = out;                         /* v0 = the CdlFILE* */
    return 1;
}

/* Observed, never skipped: libcd's own state has to stay correct for every
 * command we do not serve. */
int on_control(CPUState* cpu, uint32_t, void*) {
    const uint32_t com = cpu->gpr[4] & 0xFFu;
    if (com == 0x02u) {                        /* CdlSetloc */
        uint8_t* p = nullptr;
        if (ram_range(cpu->gpr[5], 3, &p)) {
            auto un = [](uint8_t b) { return (uint32_t)((b >> 4) * 10 + (b & 0xF)); };
            const uint32_t abs = (un(p[0]) * 60u + un(p[1])) * 75u + un(p[2]);
            g.setloc_lba = abs >= LEAD_IN ? abs - LEAD_IN : 0;
        }
    }
    return 0;
}

int on_read(CPUState* cpu, uint32_t, void*) {
    const uint32_t sectors = cpu->gpr[4];
    const uint32_t dest = cpu->gpr[5];
    if (!sectors || sectors > 4096u) return 0;

    const Entry* e = find_by_lba(g.setloc_lba);
    if (!e) return 0;                          /* not ours: real disc path */

    const uint64_t offset = (uint64_t)(g.setloc_lba - e->lba) * FORM1;
    const uint64_t want = (uint64_t)sectors * FORM1;
    if (offset + want > e->size + (FORM1 - 1)) return 0;

    uint8_t* p = nullptr;
    if (!ram_range(dest, (uint32_t)want, &p)) return 0;

    std::ifstream f(e->path, std::ios::binary);
    if (!f) return 0;
    f.seekg((std::streamoff)offset);
    std::memset(p, 0, (size_t)want);
    f.read((char*)p, (std::streamsize)want);

    g.served_read = true;
    g.reads++;
    g.bytes_served += want;
    if (!g.reported) {
        g.reported = true;
        std::fprintf(stdout,
                     "psxrecomp: native CD engaged (%s, lba %u, %u sectors)\n",
                     e->name.c_str(), g.setloc_lba, sectors);
    }
    cpu->gpr[2] = 1;                           /* v0 = 1: accepted */
    return 1;
}

int on_read_sync(CPUState* cpu, uint32_t, void*) {
    if (!g.served_read) return 0;              /* not our transfer */
    g.served_read = false;
    cpu->gpr[2] = 0;                           /* v0 = 0: complete, no error */
    return 1;
}

}  // namespace

extern "C" int libcd_native_load(const char* dir, const LibcdNativeAddrs* a) {
    libcd_native_shutdown();
    if (!dir || !*dir || !a) return 0;

    const fs::path root(dir);
    const fs::path manifest = root / "disc.toml";
    std::error_code ec;
    if (!fs::exists(manifest, ec)) return 0;

    try {
        const auto cfg = toml::parse(manifest.string());
        for (const auto& t : toml::find<toml::array>(cfg, "file")) {
            Entry e;
            e.name = toml::find<std::string>(t, "name");
            e.iso_name = toml::find_or<std::string>(t, "iso_name", e.name);
            e.lba = (uint32_t)toml::find<int64_t>(t, "lba");
            e.size = (uint32_t)toml::find<int64_t>(t, "size");
            e.stored = toml::find_or<bool>(t, "stored", false);
            e.path = root / e.name;
            if (e.stored && !fs::exists(e.path, ec)) e.stored = false;
            g.files.push_back(std::move(e));
        }
    } catch (const std::exception& ex) {
        std::fprintf(stdout, "psxrecomp: disc.toml unreadable (%s); ignoring\n",
                     ex.what());
        g = State{};
        return 0;
    }
    if (g.files.empty()) { g = State{}; return 0; }

    int bound = 0;
    if (a->cd_search_file)
        bound += psx_native_call_register(a->cd_search_file, on_search_file, nullptr);
    if (a->cd_control)
        bound += psx_native_call_register(a->cd_control, on_control, nullptr);
    if (a->cd_read)
        bound += psx_native_call_register(a->cd_read, on_read, nullptr);
    if (a->cd_read_sync)
        bound += psx_native_call_register(a->cd_read_sync, on_read_sync, nullptr);
    if (!bound) { g = State{}; return 0; }

    g.active = true;
    size_t stored = 0;
    for (const Entry& e : g.files) stored += e.stored ? 1 : 0;
    std::fprintf(stdout,
                 "psxrecomp: native CD layer: %zu file(s), %zu stored, "
                 "%d call(s) bound\n",
                 g.files.size(), stored, bound);
    return 1;
}

extern "C" int libcd_native_active(void) { return g.active ? 1 : 0; }

extern "C" void libcd_native_shutdown(void) { g = State{}; }
