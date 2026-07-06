// launcher.cpp — see launcher.h. RmlUi (HTML/CSS) front-end over SDL2 + GL3.
//
// Uses RmlUi's official SDL platform + GL3 renderer backends (lib/RmlUi/Backends).
// The base RenderInterface_GL3 is used directly (no SDL_image dependency) — the
// minimal launcher draws with CSS, not external <img> bitmaps; image-rich polish
// is a later phase.

#include "launcher.h"

#include "config_loader.h"
#include "disc_identity.h"

extern "C" {
#include "memcard.h"
}

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/ElementDocument.h>

#include "RmlUi_Platform_SDL.h"
#include "RmlUi_Renderer_GL3.h"

#include "third_party/stb_image.h"

#include <SDL.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#  include <windows.h>
#  include <commdlg.h>
#endif

namespace fs = std::filesystem;

namespace {

// ---- Tweaks tab: apply a ROM-hack variant by driving tools/tweaks_resolver.py ----
//
// The heavy work (drive acediez's patcher engine -> patched BIN -> extract SLUS
// -> variant id -> stage variants/<id>/ -> emit game.<id>.toml) runs in a
// background thread so the UI stays responsive; the main loop polls this shared
// state and pushes it into the data model. Producing the variant + toml is the
// tab's job; the (multi-minute) regen + rebuild of that variant is a separate,
// explicit step surfaced in the status line.
struct TweaksApplyState {
    std::mutex        mtx;
    std::atomic<bool> dirty{false};
    std::atomic<bool> busy{false};
    std::atomic<bool> tree_ready{false};
    std::string       status;   // guarded by mtx
    std::string       variant;  // guarded by mtx
    std::string       tree;     // guarded by mtx: catalog markup (data-rml)
};
TweaksApplyState g_tweaks;

// Walk up from a path looking for tools/tweaks_resolver.py; return the
// containing project root (empty if not found from this anchor).
fs::path walk_up_for_resolver(fs::path start) {
    std::error_code ec;
    if (start.empty()) return {};
    if (fs::is_regular_file(start, ec)) start = start.parent_path();
    for (fs::path d = start; !d.empty(); d = d.parent_path()) {
        if (fs::is_regular_file(d / "tools" / "tweaks_resolver.py", ec))
            return d;
        if (d == d.root_path()) break;
    }
    return {};
}

// Locate the project root holding the resolver. Try walking up from the disc
// (may resolve into a junctioned input tree without tools/), then from the
// current working directory (the launcher is typically run from its build dir,
// a child of the worktree root that has tools/).
fs::path find_resolver_root(const std::string& disc) {
    if (fs::path r = walk_up_for_resolver(fs::path(disc)); !r.empty()) return r;
    std::error_code ec;
    if (fs::path r = walk_up_for_resolver(fs::current_path(ec)); !r.empty()) return r;
    return {};
}

// Find a Python interpreter the launcher can invoke via cmd.exe. Prefer a
// NATIVE Windows python (mingw) — a cygwin/msys python cannot exec the Windows
// AutoHotkey path the resolver drives when cygpath isn't on cmd's PATH. Returns
// a command token (quoted abspath, or a bare name found on PATH).
std::string find_python() {
    const char* abs[] = {
        "C:\\msys64\\mingw64\\bin\\python.exe",   // native mingw python (preferred)
        "C:\\Python312\\python.exe",
        "C:\\Python311\\python.exe",
        "C:\\Python310\\python.exe",
    };
    std::error_code ec;
    for (const char* c : abs)
        if (fs::is_regular_file(fs::path(c), ec))
            return std::string("\"") + c + "\"";
#if defined(_WIN32)
    // py launcher, then bare names on PATH.
    if (std::system("where py >nul 2>nul") == 0)      return "py -3";
    if (std::system("where python >nul 2>nul") == 0)  return "python";
    if (std::system("where python3 >nul 2>nul") == 0) return "python3";
#endif
    return "python";  // last resort; may fail loudly in status
}

void tweaks_set_status(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_tweaks.mtx);
    g_tweaks.status = s;
    g_tweaks.dirty.store(true);
}

// Run `python tools/tweaks_resolver.py <args>` in `root`; capture combined output.
std::string run_resolver(const std::string& root, const std::string& args) {
#if defined(_WIN32)
    const std::string py = find_python();
    std::string cmd = "cmd /c \"cd /d \"" + root + "\" && " + py +
                      " tools\\tweaks_resolver.py " + args + " 2>&1\"";
    FILE* p = _popen(cmd.c_str(), "r");
#else
    std::string cmd = "cd '" + root + "' && python3 tools/tweaks_resolver.py " + args + " 2>&1";
    FILE* p = popen(cmd.c_str(), "r");
#endif
    std::string out;
    if (p) {
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), p)) > 0) out.append(buf, n);
#if defined(_WIN32)
        _pclose(p);
#else
        pclose(p);
#endif
    }
    return out;
}

// Background worker: fetch the option catalog as an RML fragment.
void tweaks_load_tree_worker(std::string root) {
    std::string rml = run_resolver(root, "catalog --rml");
    std::lock_guard<std::mutex> lk(g_tweaks.mtx);
    g_tweaks.tree = (rml.find("tw_toggle") != std::string::npos)
        ? rml
        : "<div class=\"tw-sec-hd\">Could not load options — need Python and the "
          "acediez patcher archive under mmx6-tweaks/_patcher/.</div>";
    g_tweaks.tree_ready.store(true);
    g_tweaks.dirty.store(true);
}

// Background worker: apply a UI selection (JSON) -> patched variant + toml.
void tweaks_apply_worker(std::string root, std::string selection_json) {
    tweaks_set_status("Applying selection — driving the patcher engine (~15s)…");
    // Stage the selection JSON where the resolver's work dir lives.
    fs::path wd = fs::path(root) / "mmx6-tweaks" / "_patcher" / "_worktmp";
    std::error_code ec; fs::create_directories(wd, ec);
    fs::path selpath = wd / "_ui_selection.json";
    { std::ofstream f(selpath.string()); f << selection_json; }

    std::string out = run_resolver(root, "apply --selection \"" + selpath.generic_string() + "\"");
    std::string variant, last;
    std::istringstream ss(out);
    for (std::string line; std::getline(ss, line); ) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        last = line;
        if (line.rfind("[apply]", 0) == 0) tweaks_set_status(line.substr(0, 90));
        auto v = line.find("variant=");
        if (v != std::string::npos) {
            variant = line.substr(v + 8);
            while (!variant.empty() && variant.back() == ' ') variant.pop_back();
        }
    }
    {
        std::lock_guard<std::mutex> lk(g_tweaks.mtx);
        if (!variant.empty()) {
            g_tweaks.variant = variant;
            g_tweaks.status  = "Variant " + variant + " staged. Build it: regen with game." +
                               variant + ".toml, then rebuild the runtime.";
        } else {
            g_tweaks.status = std::string("Apply failed. ") +
                (last.empty() ? "Is Python + AutoHotkey installed?" : last);
        }
        g_tweaks.dirty.store(true);
    }
    g_tweaks.busy.store(false);
}

// RenderInterface_GL3 only decodes uncompressed TGA. Override LoadTexture to
// decode PNG via stb_image (falling back to the base TGA path), so the launcher
// can use <img> with PNG art. RmlUi textures are premultiplied-alpha RGBA.
class LauncherRenderInterface : public RenderInterface_GL3 {
public:
    Rml::TextureHandle LoadTexture(Rml::Vector2i& dims, const Rml::String& source) override {
        Rml::FileInterface* fi = Rml::GetFileInterface();
        Rml::FileHandle fh = fi ? fi->Open(source) : Rml::FileHandle(0);
        if (!fh) return RenderInterface_GL3::LoadTexture(dims, source);
        fi->Seek(fh, 0, SEEK_END);
        const size_t sz = (size_t)fi->Tell(fh);
        fi->Seek(fh, 0, SEEK_SET);
        std::vector<unsigned char> buf(sz);
        fi->Read(buf.data(), sz, fh);
        fi->Close(fh);

        int w = 0, h = 0, comp = 0;
        unsigned char* px = stbi_load_from_memory(buf.data(), (int)sz, &w, &h, &comp, 4);
        if (!px) return RenderInterface_GL3::LoadTexture(dims, source);  // maybe a TGA

        const size_t n = (size_t)w * (size_t)h;
        for (size_t i = 0; i < n; i++) {  // straight -> premultiplied alpha
            const unsigned a = px[i * 4 + 3];
            px[i * 4 + 0] = (unsigned char)(px[i * 4 + 0] * a / 255);
            px[i * 4 + 1] = (unsigned char)(px[i * 4 + 1] * a / 255);
            px[i * 4 + 2] = (unsigned char)(px[i * 4 + 2] * a / 255);
        }
        dims.x = w; dims.y = h;
        Rml::TextureHandle th = GenerateTexture({px, n * 4}, dims);
        stbi_image_free(px);
        return th;
    }
};

// Route RmlUi's own diagnostics to stdout. The base SystemInterface logs via
// OutputDebugString on Windows (invisible to a normal console/redirect), which
// hides data-binding errors; surfacing them here keeps RML issues debuggable.
class LauncherSystemInterface : public SystemInterface_SDL {
public:
    bool LogMessage(Rml::Log::Type type, const Rml::String& message) override {
        // Surface problems (warnings/errors/asserts); skip routine info spam.
        if (type == Rml::Log::LT_INFO || type == Rml::Log::LT_DEBUG) return true;
        const char* tag = type == Rml::Log::LT_ERROR  ? "error"
                        : type == Rml::Log::LT_ASSERT ? "assert" : "warning";
        std::fprintf(stdout, "launcher/rml %s: %s\n", tag, message.c_str());
        std::fflush(stdout);
        return true;
    }
};

// Mirror of the user-tunable settings, in the value shapes the RML binds to.
struct LauncherModel {
    int  renderer        = 0;  // 0=software, 1=opengl
    int  supersampling   = 1;  // 1..4
    bool antialiasing    = true;
    int  texture_filter  = 0;  // 0=nearest, 1=bilinear
    int  crt             = 0;  // 0=raw,1=crt,2=composite,3=trinitron
    bool auto_skip_fmv   = false; // skip FMVs via the game's own skip
    bool turbo_loads     = true;  // fast-forward the machine through load screens (audio plays through); default on
    // (the old "Skip PSX BIOS" toggle is gone: the HLE boot shell-skip ships
    // on by default via [runtime] bios_hle in the player game.toml)
    bool spu_hq          = false;
    int  aspect_index    = 0;  // index into kAspects (0 = 4:3 native)
    int  window_width    = 1280; // window size (height = width*den/num per aspect)
    bool widescreen      = false; // EXPERIMENTAL 16:9 native-wide (aspect_index==1)
    bool ws_eligible     = true;  // toggle shown only when renderer==software (native-wide is SW-only)
    bool fullscreen      = false; // launch the game window in desktop fullscreen
    // Skip-launcher: boot straight into the game on subsequent launches. Turning
    // it ON shows a confirmation modal (show_skip_modal) so the user is told how
    // to get the launcher back (run with --launcher). Mirrors SMW's feature.
    bool skip_launcher   = false;
    bool show_skip_modal = false;

    Rml::String bios_path;
    Rml::String disc_path;

    // Display labels (kept in sync with the enum/int values above).
    Rml::String renderer_label;
    Rml::String crt_label;
    Rml::String texfilter_label;
    Rml::String aspect_label;
    Rml::String winsize_label;

    // Disc verification (recomputed whenever disc_path changes).
    Rml::String disc_file;      // file name only, e.g. "tomba.cue"
    Rml::String disc_region;    // "NTSC-U (USA)" | "PAL" | "NTSC-J" | "—"
    Rml::String disc_serial;    // "SCUS-94236" | "—"
    bool        v_header   = false;  // ISO9660 header present
    bool        v_crc      = false;  // CRC/hash (or serial identity) check passed
    bool        v_verified = false;  // overall verdict good
    Rml::String verdict_title;  // big line, e.g. "Tomba! disc verified"
    Rml::String verdict_detail; // sub line
    Rml::String verdict_state;  // "ok" | "warn" | "bad" | "none" — drives colour

    // View toggle: "dashboard" (default) | "settings" | "tweaks".
    Rml::String view = "dashboard";

    // Tweaks tab (MMX6 Tweaks ROM-hack variant builder). The full option tree is
    // fetched from tools/tweaks_resolver.py (catalog --rml) and injected via
    // data-rml; ticking options and Apply drives the resolver's selection apply.
    bool        tweaks_busy         = false;
    bool        tweaks_tree_loaded  = false;
    Rml::String tweaks_status  = "Tick the options you want, then Apply to build the variant.";
    Rml::String tweaks_variant;
    Rml::String tweaks_tree    = "<div class=\"tw-sec-hd\">Loading options…</div>";

    // Player cards — real device routing. Each port picks a device (None /
    // Keyboard / a plugged-in SDL controller) and a pad type (DualShock=analog).
    int  p1_dev_index = 1;     // index into the shared device option list
    int  p2_dev_index = 0;
    // Pad input mode (PSXRecompV4::PadMode): 0=hybrid (default), 1=analog,
    // 2=digital. Bound to the segmented 3-way selector in each player card.
    int  p1_mode      = 0;
    int  p2_mode      = 0;
    bool allow_hybrid = true;  // game.allow_hybrid: when false the Hybrid segment is hidden
    bool mode_selectable = true; // game.lock_mode == false: when false the whole pad-mode selector is hidden
    int  deadzone_pct = 37;    // analog-stick deadzone 0-100% (raw = pct*32767/100)
    Rml::String p1_dev_label = "Keyboard";
    Rml::String p2_dev_label = "None";
    Rml::String p1_status, p2_status;        // resolved status line
    Rml::String p1_dot, p2_dot;              // "" (on) | "off"
    Rml::String p1_options, p2_options;      // data-rml option-list markup
    Rml::String dd_open;                     // "" | "p1" | "p2" (which list is open)

    // Memory cards — real introspection of the on-disk .mcd images. Each slot
    // has a resolved file path, an enable toggle, and parsed directory stats.
    bool mc1_enabled = true;
    bool mc2_enabled = true;
    Rml::String mc1_path,  mc2_path;   // resolved absolute .mcd path
    Rml::String mc1_name,  mc2_name;   // file name only
    Rml::String mc1_size,  mc2_size;   // "128 KB (15 blocks)" | "—"
    Rml::String mc1_used,  mc2_used;   // "7 / 15" | "—"
    Rml::String mc1_foot,  mc2_foot;   // "Last modified — …" | status line
    // 15-cell block grids, built as RML markup and injected via data-rml. (A
    // data-for over a bound array is the natural fit, but the structural
    // data-for view does not capture inner-xml in this build; data-rml is the
    // robust path and the markup is fully launcher-controlled.)
    Rml::String mc1_grid, mc2_grid;

    bool launch_requested = false;
    bool quit_requested   = false;
};

// Format a unix timestamp as e.g. "Jun 12, 2026". Empty for 0/unknown.
std::string fmt_mtime(long long secs) {
    if (secs <= 0) return std::string();
    const std::time_t t = (std::time_t)secs;
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    char buf[32];
    if (std::strftime(buf, sizeof(buf), "%b %d, %Y", &tmv) == 0) return std::string();
    return std::string(buf);
}

// Resolve a slot's effective .mcd path: explicit override, else <dir>/card<N>.mcd.
std::string memcard_slot_path(const PSXRecompV4::UserSettings& io, int slot /*0|1*/) {
    const bool has = slot == 0 ? io.has_memcard1_path : io.has_memcard2_path;
    const std::filesystem::path& p = slot == 0 ? io.memcard1_path : io.memcard2_path;
    if (has && !p.empty()) return p.generic_string();
    fs::path dir = io.has_memcard_dir ? io.memcard_dir : fs::path();
    if (dir.empty()) return std::string();
    return (dir / (std::string("card") + (slot == 0 ? "1" : "2") + ".mcd")).generic_string();
}

// Parse the slot's .mcd file and fill the model's display fields for it.
void refresh_memcard(LauncherModel& m, int slot /*0|1*/) {
    Rml::String& path  = slot == 0 ? m.mc1_path   : m.mc2_path;
    Rml::String& name  = slot == 0 ? m.mc1_name   : m.mc2_name;
    Rml::String& size  = slot == 0 ? m.mc1_size   : m.mc2_size;
    Rml::String& used  = slot == 0 ? m.mc1_used   : m.mc2_used;
    Rml::String& foot  = slot == 0 ? m.mc1_foot   : m.mc2_foot;
    Rml::String& grid  = slot == 0 ? m.mc1_grid   : m.mc2_grid;

    auto build_grid = [](const uint8_t used[15]) {
        Rml::String html;
        for (int i = 0; i < 15; i++)
            html += used[i] ? "<span class=\"blk b\"></span>" : "<span class=\"blk\"></span>";
        return html;
    };
    const uint8_t empty15[15] = {0};
    grid = build_grid(empty15);
    name = path.empty() ? Rml::String("(no card)")
                        : fs::path(std::string(path)).filename().generic_string();

    if (path.empty()) {
        size = used = "—";
        foot = "No card configured.";
        return;
    }

    MemcardSummary s;
    memcard_summary_path(std::string(path).c_str(), &s);
    grid = build_grid(s.block_used);

    if (!s.exists) {
        size = "128 KB (15 blocks)";
        used = "0 / 15";
        foot = "New blank card — created on launch.";
        return;
    }
    if (!s.valid) {
        size = used = "—";
        foot = "Not a valid memory-card image.";
        return;
    }
    size = "128 KB (15 blocks)";
    used = std::to_string(s.used_blocks) + " / 15";
    const std::string when = fmt_mtime(s.mtime);
    foot = when.empty() ? Rml::String("On-disk memory card.")
                        : Rml::String("Last modified — " + when);
}

// ---- input-device enumeration (None / Keyboard / plugged-in controllers) ----
struct DeviceOption {
    int         kind;   // 0=none, 1=keyboard, 2=controller
    std::string guid;   // SDL joystick GUID string when kind==controller
    std::string label;  // display name
};

std::vector<DeviceOption> enumerate_devices() {
    std::vector<DeviceOption> opts;
    opts.push_back({0, "", "None"});
    opts.push_back({1, "", "Keyboard"});
    const int n = SDL_NumJoysticks();
    for (int i = 0; i < n; i++) {
        if (!SDL_IsGameController(i)) continue;
        SDL_JoystickGUID g = SDL_JoystickGetDeviceGUID(i);
        char buf[40] = {0};
        SDL_JoystickGetGUIDString(g, buf, sizeof(buf));
        const char* nm = SDL_GameControllerNameForIndex(i);
        opts.push_back({2, std::string(buf), nm ? std::string(nm) : std::string("Controller")});
    }
    return opts;
}

// The settings device string ("none"/"keyboard"/<guid>) for an option.
std::string device_string(const DeviceOption& o) {
    if (o.kind == 0) return "none";
    if (o.kind == 1) return "keyboard";
    return o.guid;
}

// Minimal RML/attribute text escape for injected option labels.
std::string rml_escape(const std::string& s) {
    std::string o;
    for (char c : s) {
        switch (c) {
            case '&': o += "&amp;";  break;
            case '<': o += "&lt;";   break;
            case '>': o += "&gt;";   break;
            case '"': o += "&quot;"; break;
            case '\'':o += "&#39;";  break;
            default:  o += c;        break;
        }
    }
    return o;
}

// Resolve a saved device string to an index in opts. A saved controller GUID
// that is not currently plugged in is appended as an "(offline)" option so the
// user's selection survives across unplug/replug.
int find_or_add_device_index(std::vector<DeviceOption>& opts, const std::string& dev) {
    if (dev.empty() || dev == "none") return 0;
    if (dev == "keyboard") return 1;
    for (size_t i = 0; i < opts.size(); i++)
        if (opts[i].kind == 2 && opts[i].guid == dev) return (int)i;
    opts.push_back({2, dev, "Saved controller (offline)"});
    return (int)opts.size() - 1;
}

// Build the data-rml option-list markup for a player's dropdown. Each row is a
// clickable element whose data-event-click selects that option (and closes).
std::string build_options_rml(int player, const std::vector<DeviceOption>& opts) {
    std::string s;
    for (size_t i = 0; i < opts.size(); i++) {
        s += "<p class=\"dd-opt\" data-event-click=\"pick_device(";
        s += std::to_string(player);
        s += ",";
        s += std::to_string(i);
        s += ")\">";
        s += rml_escape(opts[i].label);
        s += "</p>";
    }
    return s;
}

// Recompute a player's derived display fields from its selected option index.
void refresh_player(LauncherModel& m, int player, const std::vector<DeviceOption>& opts) {
    int&         idx    = player == 0 ? m.p1_dev_index : m.p2_dev_index;
    Rml::String& label  = player == 0 ? m.p1_dev_label : m.p2_dev_label;
    Rml::String& status = player == 0 ? m.p1_status    : m.p2_status;
    Rml::String& dot    = player == 0 ? m.p1_dot       : m.p2_dot;
    Rml::String& options= player == 0 ? m.p1_options   : m.p2_options;
    const int    mode   = player == 0 ? m.p1_mode      : m.p2_mode;

    if (idx < 0 || idx >= (int)opts.size()) idx = 0;
    const DeviceOption& o = opts[idx];
    label   = o.label;
    options = build_options_rml(player, opts);

    const char* type = mode == 1 ? "DualShock (analog)"
                     : mode == 2 ? "digital pad"
                                 : "hybrid (auto analog/d-pad)";
    if (o.kind == 0)      { status = "No device — port empty"; dot = "off"; }
    else if (o.kind == 1) { status = Rml::String("Keyboard \xE2\x80\x94 ") + type; dot = ""; }
    else                  { status = o.label + Rml::String(" \xE2\x80\x94 ") + type; dot = ""; }
}

const char* renderer_name(int v)  { return v == 1 ? "OpenGL" : "Software"; }
const char* texfilter_name(int v) { return v == 1 ? "Bilinear" : "Nearest"; }
const char* crt_name(int v) {
    switch (v) {
        case 1:  return "CRT";
        case 2:  return "Composite";
        case 3:  return "Trinitron";
        default: return "Raw (off)";
    }
}

// Offered display aspects. 4:3 is the native presentation every game ships
// with; wider aspects enable the runtime widescreen hack (GTE X-squash +
// stretched present — see [video] aspect_ratio in config_loader.h).
const int kAspects[][2] = { {4, 3}, {16, 9}, {21, 9} };
const int kNumAspects = (int)(sizeof(kAspects) / sizeof(kAspects[0]));
const char* aspect_name(int i) {
    switch (i) {
        case 1:  return "16:9 (Widescreen)";
        case 2:  return "21:9 (Ultrawide)";
        default: return "4:3 (Native)";
    }
}
int aspect_index_for(int num, int den) {
    for (int i = 0; i < kNumAspects; i++)
        if (kAspects[i][0] == num && kAspects[i][1] == den) return i;
    return 0;
}

// Offered window widths (height follows the chosen aspect). The toggle cycles
// through these.
const int kWinWidths[] = { 960, 1280, 1600, 1920 };
const int kNumWinWidths = (int)(sizeof(kWinWidths) / sizeof(kWinWidths[0]));

// Snap an arbitrary width to the nearest offered option index.
int winsize_index(int width) {
    int best = 1, bestd = 1 << 30;  // default to 1280
    for (int i = 0; i < kNumWinWidths; i++) {
        int d = width > kWinWidths[i] ? width - kWinWidths[i] : kWinWidths[i] - width;
        if (d < bestd) { bestd = d; best = i; }
    }
    return best;
}

std::string winsize_label_for(int width, int aspect_index) {
    const int num = kAspects[aspect_index][0], den = kAspects[aspect_index][1];
    return std::to_string(width) + " \xC3\x97 " + std::to_string(width * den / num);  // "1280 × 960"
}

void refresh_labels(LauncherModel& m) {
    m.renderer_label  = renderer_name(m.renderer);
    m.crt_label       = crt_name(m.crt);
    m.texfilter_label = texfilter_name(m.texture_filter);
    m.aspect_label    = aspect_name(m.aspect_index);
    m.winsize_label   = winsize_label_for(m.window_width, m.aspect_index);
    m.widescreen      = (m.aspect_index == 1);   // 16:9 == experimental native-wide
    m.ws_eligible     = true;                     // native-wide works on BOTH backends now (SW + GL compositor)
}

std::string region_long(const std::string& r) {
    if (r == "NTSC-U") return "NTSC-U (USA)";
    if (r == "NTSC-J") return "NTSC-J (Japan)";
    if (r == "PAL")    return "PAL (Europe)";
    return r;
}

// Re-run disc verification against m.disc_path and update the panel fields.
// `expected_serial`/`expected_crc` come from game.toml (via GameInfo);
// `game_name` is used for the verdict headline.
void refresh_disc_status(LauncherModel& m, const std::string& game_name,
                         const std::string& expected_serial,
                         uint32_t expected_crc, bool has_expected_crc) {
    m.v_header = m.v_crc = m.v_verified = false;
    m.disc_region = m.disc_serial = "—";
    m.disc_file = "—";

    if (m.disc_path.empty()) {
        m.verdict_title  = "No disc selected";
        m.verdict_detail = "Choose a disc image to verify it against this build.";
        m.verdict_state  = "none";
        return;
    }
    m.disc_file = fs::path(std::string(m.disc_path)).filename().generic_string();

    // Only spend time hashing when there is an expected CRC to compare against.
    const PSXRecompV4::DiscIdentity id = PSXRecompV4::identify_disc(
        fs::path(std::string(m.disc_path)), expected_serial,
        expected_crc, has_expected_crc, /*compute_crc=*/has_expected_crc);

    if (!id.opened) {
        m.verdict_title  = "Disc not found";
        m.verdict_detail = "Could not open the image or its CUE-referenced BIN.";
        m.verdict_state  = "bad";
        return;
    }
    if (!id.has_header) {
        m.verdict_title  = "Not a PlayStation disc";
        m.verdict_detail = "No ISO9660 header at the expected sectors.";
        m.verdict_state  = "bad";
        return;
    }
    m.v_header = true;
    if (!id.detected_serial.empty()) m.disc_serial = id.detected_serial;
    else if (!expected_serial.empty()) m.disc_serial = expected_serial;
    if (!id.region.empty()) m.disc_region = region_long(id.region);

    const bool serial_ok = !id.expected_serial_given || id.serial_matches;

    // Middle check: exact CRC match if we have one to compare, otherwise the
    // serial-based identity match stands in for the hash.
    if (id.expected_crc_given && id.crc_computed)
        m.v_crc = id.crc_matches;
    else
        m.v_crc = serial_ok && id.expected_serial_given;

    m.v_verified = m.v_header && serial_ok &&
                   (!id.expected_crc_given || id.crc_matches);

    const std::string nm = game_name.empty() ? std::string("Disc") : game_name;
    if (m.v_verified) {
        m.verdict_title  = nm + " disc verified";
        m.verdict_detail = "Correct disc image loaded. Ready to launch.";
        m.verdict_state  = "ok";
    } else if (!serial_ok) {
        m.verdict_title  = "Wrong disc?";
        m.verdict_detail = "Serial does not match this build (expected " + expected_serial + ").";
        m.verdict_state  = "bad";
    } else if (id.expected_crc_given && id.crc_computed && !id.crc_matches) {
        m.verdict_title  = "Disc image differs";
        m.verdict_detail = "Right game, but the image hash does not match the expected dump.";
        m.verdict_state  = "warn";
    } else {
        // Header ok, nothing authoritative to compare against.
        m.verdict_title  = "PlayStation disc";
        m.verdict_detail = "Recognised PlayStation disc. No reference hash configured.";
        m.verdict_state  = "ok";
        m.v_verified     = true;
    }
}

#if defined(_WIN32)
// Native open-file dialog. Returns "" if cancelled.
std::string win_pick_file(SDL_Window* parent, const char* title, const char* filter) {
    char buf[MAX_PATH] = {0};
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;  // e.g. "BIOS (*.bin)\0*.bin\0All files\0*.*\0\0"
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = title;
    ofn.Flags       = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    (void)parent;
    if (GetOpenFileNameA(&ofn)) return std::string(buf);
    return std::string();
}

// Native save-file dialog (overwrite-prompts). Returns "" if cancelled.
std::string win_pick_save_file(SDL_Window* parent, const char* title,
                               const char* filter, const char* default_ext,
                               const std::string& initial) {
    char buf[MAX_PATH] = {0};
    if (!initial.empty()) {
        std::snprintf(buf, sizeof(buf), "%s", initial.c_str());
        for (char* p = buf; *p; ++p) if (*p == '/') *p = '\\';  // native sep
    }
    OPENFILENAMEA ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.lpstrFilter = filter;
    ofn.lpstrFile   = buf;
    ofn.nMaxFile    = sizeof(buf);
    ofn.lpstrTitle  = title;
    ofn.lpstrDefExt = default_ext;  // appended if the user types no extension
    ofn.Flags       = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    (void)parent;
    if (GetSaveFileNameA(&ofn)) return std::string(buf);
    return std::string();
}
#else
// POSIX native dialogs via popen-based choosers (zenity / kdialog / qarma /
// osascript), each gated on `command -v` so an absent tool falls through.
// Returns "" on cancel or when no chooser is installed. The Win32 double-null
// `filter` string isn't portable, so the POSIX path uses a generic chooser
// (the title is honored); BIOS / disc / memory-card browsing works on Linux.
#include <cstdio>
namespace {
std::string sh_squote(const std::string& s) {
    std::string q = "'";
    for (char c : s) { if (c == '\'') q += "'\\''"; else q += c; }
    return q + "'";
}
std::string run_chooser(const std::string& cmd) {
    std::string out;
    FILE* p = popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[2048];
    if (fgets(buf, sizeof(buf), p)) out = buf;
    int rc = pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    if (rc != 0) out.clear();
    return out;
}
} // namespace

std::string win_pick_file(SDL_Window*, const char* title, const char*) {
    std::string t = sh_squote(title ? title : "Select file");
    std::string r;
    if (!(r = run_chooser("command -v zenity >/dev/null 2>&1 && "
            "zenity --file-selection --title=" + t + " 2>/dev/null")).empty()) return r;
    if (!(r = run_chooser("command -v kdialog >/dev/null 2>&1 && "
            "kdialog --getopenfilename \"${HOME:-/}\" 2>/dev/null")).empty()) return r;
    if (!(r = run_chooser("command -v qarma >/dev/null 2>&1 && "
            "qarma --file-selection --title=" + t + " 2>/dev/null")).empty()) return r;
    return run_chooser("command -v osascript >/dev/null 2>&1 && "
            "osascript -e 'POSIX path of (choose file)' 2>/dev/null");
}

std::string win_pick_save_file(SDL_Window*, const char* title, const char*,
                               const char*, const std::string& initial) {
    std::string t = sh_squote(title ? title : "Save file");
    std::string r;
    std::string z = "command -v zenity >/dev/null 2>&1 && "
                    "zenity --file-selection --save --confirm-overwrite --title=" + t;
    if (!initial.empty()) z += " --filename=" + sh_squote(initial);
    if (!(r = run_chooser(z + " 2>/dev/null")).empty()) return r;
    std::string k = "command -v kdialog >/dev/null 2>&1 && kdialog --getsavefilename ";
    k += initial.empty() ? std::string("\"${HOME:-/}\"") : sh_squote(initial);
    return run_chooser(k + " 2>/dev/null");
}
#endif

// Load at least one font face so RmlUi can render text. Tries bundled fonts in
// assets_dir, then a couple of platform fallbacks. Returns true if any loaded.
bool load_fonts(const fs::path& assets_dir) {
    const char* bundled[] = {
        "fonts/LatoLatin-Regular.ttf",
        "fonts/LatoLatin-Bold.ttf",
        "LatoLatin-Regular.ttf",
    };
    bool any = false;
    for (const char* rel : bundled) {
        const fs::path p = assets_dir / rel;
        std::error_code ec;
        if (fs::exists(p, ec) && Rml::LoadFontFace(p.generic_string())) any = true;
    }
    if (any) return true;
#if defined(_WIN32)
    const char* sys[] = { "C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/arial.ttf" };
    for (const char* p : sys)
        if (Rml::LoadFontFace(p)) any = true;
#endif
    return any;
}

} // namespace

namespace psx_launcher {

Result run(SDL_Window* window, void* gl_context,
           PSXRecompV4::UserSettings& io,
           const GameInfo& game, const char* assets_dir)
{
    (void)gl_context;  // already created + current; we only need the window.

    const std::string expected_serial = game.expected_serial ? game.expected_serial : "";
    const uint32_t    expected_crc    = game.expected_crc;
    const bool        has_expected_crc = game.has_expected_crc;

    if (!RmlGL3::Initialize()) {
        std::fprintf(stderr, "launcher: RmlGL3::Initialize failed\n");
        return Result::Unavailable;
    }

    LauncherSystemInterface system_interface;
    system_interface.SetWindow(window);
    LauncherRenderInterface render_interface;
    if (!render_interface) {
        std::fprintf(stderr, "launcher: GL3 render interface init failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    Rml::SetSystemInterface(&system_interface);
    Rml::SetRenderInterface(&render_interface);
    if (!Rml::Initialise()) {
        std::fprintf(stderr, "launcher: Rml::Initialise failed\n");
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    const fs::path assets = assets_dir ? fs::path(assets_dir) : fs::current_path();
    if (!load_fonts(assets))
        std::fprintf(stderr, "launcher: warning — no font face loaded; text will not render\n");

    int win_w = 0, win_h = 0;
    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
    if (win_w <= 0 || win_h <= 0) { win_w = 1280; win_h = 800; }
    render_interface.SetViewport(win_w, win_h);

    Rml::Context* context = Rml::CreateContext("launcher", Rml::Vector2i(win_w, win_h));
    if (!context) {
        std::fprintf(stderr, "launcher: CreateContext failed\n");
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }

    // ---- Seed the model from the effective settings ----
    LauncherModel m;
    m.renderer       = io.renderer;
    m.supersampling  = io.supersampling;
    m.antialiasing   = io.antialiasing;
    m.texture_filter = io.texture_filter;
    m.crt            = io.screen_kind;
    m.auto_skip_fmv  = io.auto_skip_fmv;
    m.turbo_loads    = io.turbo_loads;
    m.fullscreen     = io.fullscreen;
    m.skip_launcher  = io.skip_launcher;
    m.spu_hq         = io.spu_hq;
    m.aspect_index   = io.has_aspect_ratio ? aspect_index_for(io.aspect_num, io.aspect_den) : 0;
    m.window_width   = kWinWidths[winsize_index(io.has_window_width ? io.window_width : 1280)];
    m.bios_path      = io.has_bios_path ? io.bios_path.generic_string() : Rml::String();
    m.disc_path      = io.has_disc_path ? io.disc_path.generic_string() : Rml::String();
    refresh_labels(m);
    const std::string game_name_s = game.name ? game.name : "";
    refresh_disc_status(m, game_name_s, expected_serial, expected_crc, has_expected_crc);

    // ---- Seed the memory-card slots from the effective settings ----
    if (io.has_memcard1_enabled) m.mc1_enabled = io.memcard1_enabled;
    if (io.has_memcard2_enabled) m.mc2_enabled = io.memcard2_enabled;
    m.mc1_path = memcard_slot_path(io, 0);
    m.mc2_path = memcard_slot_path(io, 1);
    refresh_memcard(m, 0);
    refresh_memcard(m, 1);

    // ---- Seed the controller slots: enumerate devices, resolve selections ----
    std::vector<DeviceOption> dev_opts = enumerate_devices();
    m.p1_mode = io.has_p1_mode ? io.p1_mode : PSXRecompV4::PAD_MODE_HYBRID;
    m.p2_mode = io.has_p2_mode ? io.p2_mode : PSXRecompV4::PAD_MODE_HYBRID;
    // When the game hides Hybrid, never leave a port selected on it (a stale
    // settings.toml or the Hybrid default would otherwise highlight nothing).
    m.allow_hybrid = game.allow_hybrid;
    if (!m.allow_hybrid) {
        if (m.p1_mode == PSXRecompV4::PAD_MODE_HYBRID) m.p1_mode = PSXRecompV4::PAD_MODE_ANALOG;
        if (m.p2_mode == PSXRecompV4::PAD_MODE_HYBRID) m.p2_mode = PSXRecompV4::PAD_MODE_ANALOG;
    }
    // lock_mode: a single-pad-type game (e.g. Tomba 2, digital-only). Hide the
    // whole pad-mode selector and force both ports to the game's locked mode,
    // overriding any stale settings.toml so a broken mode can't be selected.
    m.mode_selectable = !game.lock_mode;
    if (game.lock_mode) {
        m.p1_mode = game.locked_mode;
        m.p2_mode = game.locked_mode;
    }
    m.deadzone_pct = io.has_deadzone ? (io.deadzone * 100 / 32767) : 37;
    if (io.has_p1_device) {
        m.p1_dev_index = find_or_add_device_index(dev_opts, io.p1_device);
    } else {
        // Zero-config default: first plugged-in controller, else keyboard.
        m.p1_dev_index = (dev_opts.size() > 2) ? 2 : 1;
    }
    m.p2_dev_index = io.has_p2_device ? find_or_add_device_index(dev_opts, io.p2_device) : 0;
    refresh_player(m, 0, dev_opts);
    refresh_player(m, 1, dev_opts);

    // ---- Data model: bind fields + action callbacks ----
    Rml::DataModelConstructor c = context->CreateDataModel("settings");
    if (!c) {
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    Rml::String title = game.name ? Rml::String(game.name) : Rml::String("PSX");
    c.BindFunc("game_name", [title](Rml::Variant& out) { out = title; });
    c.Bind("supersampling",  &m.supersampling);
    c.Bind("antialiasing",   &m.antialiasing);
    c.Bind("auto_skip_fmv",  &m.auto_skip_fmv);
    c.Bind("turbo_loads",    &m.turbo_loads);
    c.Bind("fullscreen",     &m.fullscreen);
    c.Bind("skip_launcher",  &m.skip_launcher);
    c.Bind("show_skip_modal",&m.show_skip_modal);
    c.Bind("spu_hq",         &m.spu_hq);
    c.Bind("renderer_label", &m.renderer_label);
    c.Bind("crt_label",      &m.crt_label);
    c.Bind("aspect_label",   &m.aspect_label);
    c.Bind("widescreen",     &m.widescreen);
    c.Bind("ws_eligible",    &m.ws_eligible);
    c.Bind("winsize_label",  &m.winsize_label);
    c.Bind("texfilter_label",&m.texfilter_label);
    c.Bind("bios_path",      &m.bios_path);
    c.Bind("disc_path",      &m.disc_path);
    c.Bind("disc_file",      &m.disc_file);
    c.Bind("disc_region",    &m.disc_region);
    c.Bind("disc_serial",    &m.disc_serial);
    c.Bind("v_header",       &m.v_header);
    c.Bind("v_crc",          &m.v_crc);
    c.Bind("v_verified",     &m.v_verified);
    c.Bind("verdict_title",  &m.verdict_title);
    c.Bind("verdict_detail", &m.verdict_detail);
    c.Bind("verdict_state",  &m.verdict_state);
    c.Bind("view",           &m.view);
    c.Bind("tweaks_busy",    &m.tweaks_busy);
    c.Bind("tweaks_status",  &m.tweaks_status);
    c.Bind("tweaks_variant", &m.tweaks_variant);
    c.Bind("tweaks_tree",    &m.tweaks_tree);
    c.Bind("p1_mode",        &m.p1_mode);
    c.Bind("p2_mode",        &m.p2_mode);
    c.Bind("allow_hybrid",   &m.allow_hybrid);
    c.Bind("mode_selectable",&m.mode_selectable);
    c.Bind("deadzone_pct",   &m.deadzone_pct);
    c.Bind("p1_dev_label",   &m.p1_dev_label);
    c.Bind("p2_dev_label",   &m.p2_dev_label);
    c.Bind("p1_status",      &m.p1_status);
    c.Bind("p2_status",      &m.p2_status);
    c.Bind("p1_dot",         &m.p1_dot);
    c.Bind("p2_dot",         &m.p2_dot);
    c.Bind("p1_options",     &m.p1_options);
    c.Bind("p2_options",     &m.p2_options);
    c.Bind("dd_open",        &m.dd_open);
    c.Bind("mc1_enabled",    &m.mc1_enabled);
    c.Bind("mc2_enabled",    &m.mc2_enabled);
    c.Bind("mc1_name",       &m.mc1_name);
    c.Bind("mc2_name",       &m.mc2_name);
    c.Bind("mc1_size",       &m.mc1_size);
    c.Bind("mc2_size",       &m.mc2_size);
    c.Bind("mc1_used",       &m.mc1_used);
    c.Bind("mc2_used",       &m.mc2_used);
    c.Bind("mc1_foot",       &m.mc1_foot);
    c.Bind("mc2_foot",       &m.mc2_foot);
    c.Bind("mc1_grid",       &m.mc1_grid);
    c.Bind("mc2_grid",       &m.mc2_grid);

    Rml::DataModelHandle handle = c.GetModelHandle();

    c.BindEventCallback("cycle_renderer",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.renderer ^= 1;
            refresh_labels(m);
            handle.DirtyVariable("renderer_label");
        });
    c.BindEventCallback("cycle_ss",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.supersampling = (m.supersampling % 4) + 1;
            handle.DirtyVariable("supersampling");
        });
    c.BindEventCallback("toggle_aa",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.antialiasing = !m.antialiasing;
            handle.DirtyVariable("antialiasing");
        });
    c.BindEventCallback("cycle_texfilter",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.texture_filter ^= 1; refresh_labels(m);
            handle.DirtyVariable("texfilter_label");
        });
    c.BindEventCallback("cycle_crt",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.crt = (m.crt + 1) % 4; refresh_labels(m);
            handle.DirtyVariable("crt_label");
        });
    c.BindEventCallback("cycle_aspect",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.aspect_index = (m.aspect_index + 1) % kNumAspects; refresh_labels(m);
            handle.DirtyVariable("aspect_label");
            handle.DirtyVariable("winsize_label");  /* height follows aspect */
        });
    // EXPERIMENTAL widescreen On/Off. On => 16:9 native-wide (aspect_index 1),
    // Off => 4:3 (aspect_index 0). Works on BOTH renderers (SW + the GL wide
    // compositor), so it is no longer gated on the software renderer.
    //
    // 21:9 (kAspects[2]) is STUBBED but intentionally hidden: the engine handles
    // it (offset / cull / compositor are all aspect-derived), but the parallax +
    // far-backdrop pipeline only generates ~16:9 of coverage, so 21:9 voids the
    // far background. When that pipeline is widened, promote this 2-state toggle
    // to a 3-way Off / 16:9 / 21:9 — the existing cycle_aspect callback already
    // cycles aspect_index 0/1/2 and is the scaffold for it.
    c.BindEventCallback("toggle_widescreen",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.aspect_index = (m.aspect_index == 1) ? 0 : 1;    // 16:9 <-> 4:3
            refresh_labels(m);
            handle.DirtyVariable("widescreen");
            handle.DirtyVariable("aspect_label");
            handle.DirtyVariable("winsize_label");  /* height follows aspect */
        });
    c.BindEventCallback("cycle_winsize",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            int i = (winsize_index(m.window_width) + 1) % kNumWinWidths;
            m.window_width = kWinWidths[i]; refresh_labels(m);
            handle.DirtyVariable("winsize_label");
        });
    c.BindEventCallback("toggle_spu",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.spu_hq = !m.spu_hq;
            handle.DirtyVariable("spu_hq");
        });
    c.BindEventCallback("toggle_skip_fmv",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.auto_skip_fmv = !m.auto_skip_fmv;
            handle.DirtyVariable("auto_skip_fmv");
        });
    c.BindEventCallback("toggle_turbo_loads",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.turbo_loads = !m.turbo_loads;
            handle.DirtyVariable("turbo_loads");
        });
    c.BindEventCallback("toggle_fullscreen",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.fullscreen = !m.fullscreen;
            handle.DirtyVariable("fullscreen");
        });
    // Skip launcher: turning OFF is immediate; turning ON opens a confirm modal
    // first, so the user learns the --launcher escape hatch before committing.
    c.BindEventCallback("toggle_skip_launcher",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            if (m.skip_launcher) { m.skip_launcher = false; handle.DirtyVariable("skip_launcher"); }
            else { m.show_skip_modal = true; handle.DirtyVariable("show_skip_modal"); }
        });
    c.BindEventCallback("skip_modal_confirm",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.skip_launcher = true; m.show_skip_modal = false;
            handle.DirtyVariable("skip_launcher"); handle.DirtyVariable("show_skip_modal");
        });
    c.BindEventCallback("skip_modal_cancel",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.show_skip_modal = false; handle.DirtyVariable("show_skip_modal");
        });
    c.BindEventCallback("browse_bios",
        [&m, window, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            std::string p = win_pick_file(window, "Select PlayStation BIOS",
                "BIOS image (*.bin;*.rom)\0*.bin;*.rom\0All files (*.*)\0*.*\0\0");
            if (!p.empty()) {
                m.bios_path = fs::path(p).generic_string();
                handle.DirtyVariable("bios_path");
            }
        });
    auto do_browse_disc =
        [&m, window, handle, game_name_s, expected_serial, expected_crc, has_expected_crc]
        (Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            std::string p = win_pick_file(window, "Select disc image",
                "Disc image (*.cue;*.bin;*.iso)\0*.cue;*.bin;*.iso\0All files (*.*)\0*.*\0\0");
            if (!p.empty()) {
                m.disc_path = fs::path(p).generic_string();
                refresh_disc_status(m, game_name_s, expected_serial, expected_crc, has_expected_crc);
                for (const char* v : {"disc_path", "disc_file", "disc_region", "disc_serial",
                                      "v_header", "v_crc", "v_verified",
                                      "verdict_title", "verdict_detail", "verdict_state"})
                    handle.DirtyVariable(v);
            }
        };
    c.BindEventCallback("browse_disc", do_browse_disc);
    c.BindEventCallback("change_iso",  do_browse_disc);

    c.BindEventCallback("show_settings",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.view = "settings"; handle.DirtyVariable("view");
        });
    c.BindEventCallback("show_dashboard",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.view = "dashboard"; handle.DirtyVariable("view");
        });
    // ---- Tweaks tab ----
    c.BindEventCallback("show_tweaks",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.view = "tweaks"; handle.DirtyVariable("view");
            if (m.tweaks_tree_loaded) return;              // fetch the option tree once
            m.tweaks_tree_loaded = true;
            fs::path root = find_resolver_root(std::string(m.disc_path));
            if (root.empty()) {
                m.tweaks_tree = "<div class=\"tw-sec-hd\">Could not locate "
                                "tools/tweaks_resolver.py near the disc.</div>";
                handle.DirtyVariable("tweaks_tree");
                return;
            }
            std::thread(tweaks_load_tree_worker, root.generic_string()).detach();
        });
    // Toggle an option in the injected tree. Checkbox flips its own state; a
    // radio (data-group set) becomes the sole selection in its group.
    c.BindEventCallback("tw_toggle",
        [](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) {
            Rml::Element* el = ev.GetCurrentElement();
            if (!el) return;
            Rml::String group = el->GetAttribute<Rml::String>("data-group", "");
            if (group.empty()) {
                el->SetClass("on", !el->IsClassSet("on"));
            } else if (Rml::Element* parent = el->GetParentNode()) {
                for (int i = 0; i < parent->GetNumChildren(); ++i) {
                    Rml::Element* c = parent->GetChild(i);
                    if (c->GetAttribute<Rml::String>("data-group", "") == group)
                        c->SetClass("on", c == el);
                }
            }
        });
    c.BindEventCallback("apply_tweaks",
        [&m, handle](Rml::DataModelHandle, Rml::Event& ev, const Rml::VariantList&) mutable {
            if (g_tweaks.busy.load()) return;               // one apply at a time
            Rml::Element* btn = ev.GetCurrentElement();
            Rml::ElementDocument* doc = btn ? btn->GetOwnerDocument() : nullptr;
            Rml::Element* tree = doc ? doc->GetElementById("tw_tree") : nullptr;
            if (!tree) {
                m.tweaks_status = "Options aren't loaded yet.";
                handle.DirtyVariable("tweaks_status"); return;
            }
            // Collect the tree's current state into a selection JSON: every
            // checkbox (true/false) + each group's selected radio (true).
            std::string json = "{";
            bool first = true;
            std::function<void(Rml::Element*)> walk = [&](Rml::Element* e) {
                if (e->IsClassSet("tw-opt")) {
                    Rml::String var  = e->GetAttribute<Rml::String>("data-var", "");
                    Rml::String type = e->GetAttribute<Rml::String>("data-type", "");
                    if (!var.empty()) {
                        bool on = e->IsClassSet("on");
                        bool include = (type == "checkbox") || (type == "radio" && on);
                        if (include) {
                            if (!first) json += ",";
                            json += "\"" + std::string(var.c_str()) + "\":" +
                                    ((type == "checkbox") ? (on ? "true" : "false") : "true");
                            first = false;
                        }
                    }
                }
                for (int i = 0; i < e->GetNumChildren(); ++i) walk(e->GetChild(i));
            };
            walk(tree);
            json += "}";
            fs::path root = find_resolver_root(std::string(m.disc_path));
            if (root.empty()) {
                m.tweaks_status = "Could not locate tools/tweaks_resolver.py near the disc.";
                handle.DirtyVariable("tweaks_status"); return;
            }
            g_tweaks.busy.store(true);
            m.tweaks_busy = true; handle.DirtyVariable("tweaks_busy");
            std::thread(tweaks_apply_worker, root.generic_string(), json).detach();
        });
    // ---- controller: device dropdown + pad-mode segmented selector ----
    auto dirty_player = [handle](int player) mutable {
        const char* v0[] = {"p1_dev_label","p1_status","p1_dot","p1_options","p1_mode"};
        const char* v1[] = {"p2_dev_label","p2_status","p2_dot","p2_options","p2_mode"};
        for (const char* v : (player == 0 ? v0 : v1)) handle.DirtyVariable(v);
    };
    // dev_opts is captured by value: the device list is fixed for the launcher
    // session (a hot-plug here would require a re-enumerate, deferred).
    c.BindEventCallback("open_dd",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) mutable {
            const int player = args.empty() ? 0 : (int)args[0].Get<int>();
            const char* key = player == 0 ? "p1" : "p2";
            m.dd_open = (m.dd_open == key) ? Rml::String() : Rml::String(key);
            handle.DirtyVariable("dd_open");
        });
    c.BindEventCallback("close_dd",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.dd_open = Rml::String(); handle.DirtyVariable("dd_open");
        });
    c.BindEventCallback("pick_device",
        [&m, handle, dev_opts, dirty_player](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) mutable {
            if (args.size() < 2) return;
            const int player = (int)args[0].Get<int>();
            const int idx    = (int)args[1].Get<int>();
            (player == 0 ? m.p1_dev_index : m.p2_dev_index) = idx;
            refresh_player(m, player, dev_opts);
            m.dd_open = Rml::String();
            dirty_player(player);
            handle.DirtyVariable("dd_open");
        });
    // Pad-mode segmented selector: each segment passes its mode (0=hybrid,
    // 1=analog, 2=digital) so any mode is one click away.
    c.BindEventCallback("set_mode_p1",
        [&m, handle, dev_opts, dirty_player](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) mutable {
            if (args.empty()) return;
            m.p1_mode = (int)args[0].Get<int>(); refresh_player(m, 0, dev_opts); dirty_player(0);
        });
    c.BindEventCallback("set_mode_p2",
        [&m, handle, dev_opts, dirty_player](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList& args) mutable {
            if (args.empty()) return;
            m.p2_mode = (int)args[0].Get<int>(); refresh_player(m, 1, dev_opts); dirty_player(1);
        });
    /* Analog-stick deadzone, stepped 0..50% (wraps). Applies to both the
     * stick->d-pad threshold and the analog centre dead-band. */
    c.BindEventCallback("cycle_deadzone",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.deadzone_pct += 5;
            if (m.deadzone_pct > 50) m.deadzone_pct = 0;
            handle.DirtyVariable("deadzone_pct");
        });
    c.BindEventCallback("toggle_mc1",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.mc1_enabled = !m.mc1_enabled; handle.DirtyVariable("mc1_enabled");
        });
    c.BindEventCallback("toggle_mc2",
        [&m, handle](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable {
            m.mc2_enabled = !m.mc2_enabled; handle.DirtyVariable("mc2_enabled");
        });

    auto dirty_mc = [handle](int slot) mutable {
        const char* v0[] = {"mc1_name","mc1_size","mc1_used","mc1_foot","mc1_grid"};
        const char* v1[] = {"mc2_name","mc2_size","mc2_used","mc2_foot","mc2_grid"};
        for (const char* v : (slot == 0 ? v0 : v1)) handle.DirtyVariable(v);
    };
    auto browse_mc = [&m, window, dirty_mc](int slot) mutable {
        std::string p = win_pick_file(window, "Select memory-card image",
            "Memory card (*.mcd;*.mc;*.mcr)\0*.mcd;*.mc;*.mcr\0All files (*.*)\0*.*\0\0");
        if (p.empty()) return;
        (slot == 0 ? m.mc1_path : m.mc2_path) = fs::path(p).generic_string();
        refresh_memcard(m, slot);
        dirty_mc(slot);
    };
    auto new_mc = [&m, window, dirty_mc](int slot) mutable {
        Rml::String& cur = (slot == 0 ? m.mc1_path : m.mc2_path);
        std::string p = win_pick_save_file(window, "Create new memory card",
            "Memory card (*.mcd)\0*.mcd\0All files (*.*)\0*.*\0\0", "mcd",
            std::string(cur));
        if (p.empty()) return;
        if (memcard_format_file(p.c_str()) != 0) return;  // I/O failure: leave as-is
        cur = fs::path(p).generic_string();
        refresh_memcard(m, slot);
        dirty_mc(slot);
    };
    c.BindEventCallback("browse_mc1",
        [browse_mc](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable { browse_mc(0); });
    c.BindEventCallback("browse_mc2",
        [browse_mc](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable { browse_mc(1); });
    c.BindEventCallback("new_mc1",
        [new_mc](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable { new_mc(0); });
    c.BindEventCallback("new_mc2",
        [new_mc](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) mutable { new_mc(1); });
    c.BindEventCallback("launch",
        [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m.launch_requested = true; });
    c.BindEventCallback("quit",
        [&m](Rml::DataModelHandle, Rml::Event&, const Rml::VariantList&) { m.quit_requested = true; });

    // ---- Load the document ----
    const fs::path rml = assets / "launcher.rml";
    Rml::ElementDocument* doc = context->LoadDocument(rml.generic_string());
    if (!doc) {
        std::fprintf(stderr, "launcher: failed to load %s — booting without launcher\n",
                     rml.generic_string().c_str());
        Rml::Shutdown();
        RmlGL3::Shutdown();
        return Result::Unavailable;
    }
    doc->Show();

    // ---- Main loop ----
    Result result = Result::Quit;
    bool running = true;
    while (running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                m.quit_requested = true;
                break;
            case SDL_WINDOWEVENT:
                if (ev.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    SDL_GL_GetDrawableSize(window, &win_w, &win_h);
                    render_interface.SetViewport(win_w, win_h);
                    context->SetDimensions(Rml::Vector2i(win_w, win_h));
                }
                RmlSDL::InputEventHandler(context, ev);
                break;
            default:
                RmlSDL::InputEventHandler(context, ev);
                break;
            }
        }

        if (m.launch_requested) { result = Result::Launch; running = false; }
        if (m.quit_requested)   { result = Result::Quit;   running = false; }

        // Pull any background tweaks progress into the model (main thread only).
        if (g_tweaks.dirty.exchange(false)) {
            bool tree_ready = g_tweaks.tree_ready.exchange(false);
            {
                std::lock_guard<std::mutex> lk(g_tweaks.mtx);
                m.tweaks_status = Rml::String(g_tweaks.status);
                if (!g_tweaks.variant.empty())
                    m.tweaks_variant = Rml::String(g_tweaks.variant);
                if (tree_ready)
                    m.tweaks_tree = Rml::String(g_tweaks.tree);
            }
            m.tweaks_busy = g_tweaks.busy.load();
            handle.DirtyVariable("tweaks_status");
            handle.DirtyVariable("tweaks_variant");
            handle.DirtyVariable("tweaks_busy");
            if (tree_ready) handle.DirtyVariable("tweaks_tree");
        }

        context->Update();

        render_interface.Clear();
        render_interface.BeginFrame();
        context->Render();
        render_interface.EndFrame();
        SDL_GL_SwapWindow(window);
    }

    // ---- Commit choices on launch ----
    if (result == Result::Launch) {
        io.renderer = m.renderer;             io.has_renderer = true;
        io.supersampling = m.supersampling;   io.has_supersampling = true;
        io.antialiasing = m.antialiasing;     io.has_antialiasing = true;
        io.texture_filter = m.texture_filter; io.has_texture_filter = true;
        io.screen_kind = m.crt;               io.has_screen_kind = true;
        io.auto_skip_fmv = m.auto_skip_fmv;   io.has_auto_skip_fmv = true;
        io.turbo_loads = m.turbo_loads;       io.has_turbo_loads = true;
        io.fullscreen = m.fullscreen;         io.has_fullscreen = true;
        io.skip_launcher = m.skip_launcher;   io.has_skip_launcher = true;
        io.spu_hq = m.spu_hq;                 io.has_spu_hq = true;
        io.aspect_num = kAspects[m.aspect_index][0];
        io.aspect_den = kAspects[m.aspect_index][1];
        io.has_aspect_ratio = true;
        io.window_width = m.window_width;     io.has_window_width = true;
        if (!m.bios_path.empty()) { io.bios_path = fs::path(std::string(m.bios_path)); io.has_bios_path = true; }
        if (!m.disc_path.empty()) { io.disc_path = fs::path(std::string(m.disc_path)); io.has_disc_path = true; }

        io.memcard1_enabled = m.mc1_enabled; io.has_memcard1_enabled = true;
        io.memcard2_enabled = m.mc2_enabled; io.has_memcard2_enabled = true;
        if (!m.mc1_path.empty()) { io.memcard1_path = fs::path(std::string(m.mc1_path)); io.has_memcard1_path = true; }
        if (!m.mc2_path.empty()) { io.memcard2_path = fs::path(std::string(m.mc2_path)); io.has_memcard2_path = true; }

        const int i1 = (m.p1_dev_index >= 0 && m.p1_dev_index < (int)dev_opts.size()) ? m.p1_dev_index : 0;
        const int i2 = (m.p2_dev_index >= 0 && m.p2_dev_index < (int)dev_opts.size()) ? m.p2_dev_index : 0;
        io.p1_device = device_string(dev_opts[i1]); io.has_p1_device = true;
        io.p2_device = device_string(dev_opts[i2]); io.has_p2_device = true;
        io.p1_mode = m.p1_mode; io.has_p1_mode = true;
        io.p2_mode = m.p2_mode; io.has_p2_mode = true;
        io.deadzone = m.deadzone_pct * 32767 / 100; io.has_deadzone = true;
    }

    Rml::Shutdown();
    RmlGL3::Shutdown();
    return result;
}

} // namespace psx_launcher
