#include "fullscreen_control.h"

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

constexpr int kEventCapacity = 64;

#if defined(PSX_SDL3)
using PsxDisplayID = SDL_DisplayID;
#else
using PsxDisplayID = int;
#endif

struct DisplaySnapshot {
    Uint64 ticks_ms = 0;
    char reason[40] = {};
    int configured_mode = 0;
    int applied_mode = 0;
    Uint64 window_flags = 0;
    int window_x = 0;
    int window_y = 0;
    int window_w = 0;
    int window_h = 0;
    int pixel_w = 0;
    int pixel_h = 0;
    int display_x = 0;
    int display_y = 0;
    int display_w = 0;
    int display_h = 0;
    int desktop_w = 0;
    int desktop_h = 0;
    float desktop_hz = 0.0f;
    int current_w = 0;
    int current_h = 0;
    float current_hz = 0.0f;
    int fullscreen_mode_set = 0;
    int cursor_visible = 0;
    float display_scale = 1.0f;
#ifdef _WIN32
    uintptr_t hwnd = 0;
    uint64_t win_style = 0;
    uint64_t win_ex_style = 0;
    int win_rect_x = 0;
    int win_rect_y = 0;
    int win_rect_w = 0;
    int win_rect_h = 0;
    int win_mode_w = 0;
    int win_mode_h = 0;
    int win_mode_hz = 0;
    int win_mode_bpp = 0;
    unsigned win_dpi = 0;
#endif
};

SDL_Window *s_window = nullptr;
int s_configured_mode = 0;
int s_applied_mode = 0;
int s_windowed_x = SDL_WINDOWPOS_CENTERED;
int s_windowed_y = SDL_WINDOWPOS_CENTERED;
int s_windowed_w = 1280;
int s_windowed_h = 960;
bool s_have_windowed_rect = false;
bool s_has_focus = true;

DisplaySnapshot s_events[kEventCapacity];
unsigned s_event_head = 0;
unsigned s_event_count = 0;
uint64_t s_event_total = 0;

static int clamp_mode(int mode)
{
    return std::max(0, std::min(2, mode));
}

static const char *mode_name(int mode)
{
    switch (mode) {
    case 1: return "borderless_window";
    case 2: return "exclusive";
    default: return "windowed";
    }
}

static void set_cursor_for_state()
{
    const bool hide = s_window && s_applied_mode != 0 && s_has_focus;
#if defined(PSX_SDL3)
    if (hide) {
        (void)SDL_HideCursor();
    } else {
        (void)SDL_ShowCursor();
    }
#else
    (void)SDL_ShowCursor(hide ? SDL_DISABLE : SDL_ENABLE);
#endif
}

static PsxDisplayID window_display()
{
#if defined(PSX_SDL3)
    SDL_DisplayID display = s_window ? SDL_GetDisplayForWindow(s_window) : 0;
    return display ? display : SDL_GetPrimaryDisplay();
#else
    int display = s_window ? SDL_GetWindowDisplayIndex(s_window) : 0;
    return display >= 0 ? display : 0;
#endif
}

static void capture_snapshot(const char *reason)
{
    DisplaySnapshot snap = {};
    snap.ticks_ms = SDL_GetTicks();
    std::snprintf(snap.reason, sizeof(snap.reason), "%s",
                  reason ? reason : "unspecified");
    snap.configured_mode = s_configured_mode;
    snap.applied_mode = s_applied_mode;

    if (s_window) {
        snap.window_flags = static_cast<Uint64>(SDL_GetWindowFlags(s_window));
        (void)SDL_GetWindowPosition(s_window, &snap.window_x, &snap.window_y);
        (void)SDL_GetWindowSize(s_window, &snap.window_w, &snap.window_h);
#if defined(PSX_SDL3)
        (void)SDL_GetWindowSizeInPixels(s_window, &snap.pixel_w, &snap.pixel_h);
        snap.fullscreen_mode_set =
            SDL_GetWindowFullscreenMode(s_window) != nullptr ? 1 : 0;
        snap.cursor_visible = SDL_CursorVisible() ? 1 : 0;
        snap.display_scale = SDL_GetWindowDisplayScale(s_window);
#else
        SDL_GL_GetDrawableSize(s_window, &snap.pixel_w, &snap.pixel_h);
        snap.fullscreen_mode_set =
            (snap.window_flags & SDL_WINDOW_FULLSCREEN_DESKTOP) ==
                    SDL_WINDOW_FULLSCREEN
                ? 1
                : 0;
        snap.cursor_visible = SDL_ShowCursor(SDL_QUERY) == SDL_ENABLE ? 1 : 0;
#endif
    }

    const PsxDisplayID display = window_display();
    SDL_Rect bounds = {};
#if defined(PSX_SDL3)
    if (display) {
        (void)SDL_GetDisplayBounds(display, &bounds);
        if (const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display)) {
            snap.desktop_w = desktop->w;
            snap.desktop_h = desktop->h;
            snap.desktop_hz = desktop->refresh_rate;
        }
        SDL_DisplayMode current = {};
        if (SDL_GetCurrentDisplayMode(display, &current) == 0) {
            snap.current_w = current.w;
            snap.current_h = current.h;
            snap.current_hz = current.refresh_rate;
        }
    }
#else
    (void)SDL_GetDisplayBounds(display, &bounds);
    SDL_DisplayMode desktop = {};
    SDL_DisplayMode current = {};
    if (SDL_GetDesktopDisplayMode(display, &desktop) == 0) {
        snap.desktop_w = desktop.w;
        snap.desktop_h = desktop.h;
        snap.desktop_hz = static_cast<float>(desktop.refresh_rate);
    }
    if (SDL_GetCurrentDisplayMode(display, &current) == 0) {
        snap.current_w = current.w;
        snap.current_h = current.h;
        snap.current_hz = static_cast<float>(current.refresh_rate);
    }
#endif
    snap.display_x = bounds.x;
    snap.display_y = bounds.y;
    snap.display_w = bounds.w;
    snap.display_h = bounds.h;

#if defined(_WIN32) && defined(PSX_SDL3)
    const SDL_PropertiesID props = SDL_GetWindowProperties(s_window);
    HWND hwnd = reinterpret_cast<HWND>(SDL_GetPointerProperty(
        props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr));
    if (hwnd) {
        snap.hwnd = reinterpret_cast<uintptr_t>(hwnd);
        snap.win_style = static_cast<uint64_t>(
            static_cast<uintptr_t>(GetWindowLongPtrW(hwnd, GWL_STYLE)));
        snap.win_ex_style = static_cast<uint64_t>(
            static_cast<uintptr_t>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE)));
        RECT rect = {};
        if (GetWindowRect(hwnd, &rect)) {
            snap.win_rect_x = rect.left;
            snap.win_rect_y = rect.top;
            snap.win_rect_w = rect.right - rect.left;
            snap.win_rect_h = rect.bottom - rect.top;
        }
        HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFOEXW monitor_info = {};
        monitor_info.cbSize = sizeof(monitor_info);
        if (GetMonitorInfoW(monitor, &monitor_info)) {
            DEVMODEW mode = {};
            mode.dmSize = sizeof(mode);
            if (EnumDisplaySettingsW(monitor_info.szDevice,
                                     ENUM_CURRENT_SETTINGS, &mode)) {
                snap.win_mode_w = static_cast<int>(mode.dmPelsWidth);
                snap.win_mode_h = static_cast<int>(mode.dmPelsHeight);
                snap.win_mode_hz = static_cast<int>(mode.dmDisplayFrequency);
                snap.win_mode_bpp = static_cast<int>(mode.dmBitsPerPel);
            }
        }
        using GetDpiForWindowFn = UINT(WINAPI *)(HWND);
        const auto get_dpi_for_window = reinterpret_cast<GetDpiForWindowFn>(
            GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow"));
        if (get_dpi_for_window) {
            snap.win_dpi = get_dpi_for_window(hwnd);
        }
    }
#endif

    s_events[s_event_head] = snap;
    s_event_head = (s_event_head + 1u) % kEventCapacity;
    if (s_event_count < kEventCapacity) {
        s_event_count++;
    }
    s_event_total++;
}

static bool leave_fullscreen_state()
{
    if (!s_window) return false;
#if defined(PSX_SDL3)
    bool ok = SDL_SetWindowFullscreen(s_window, false);
    ok = SDL_SetWindowFullscreenMode(s_window, nullptr) && ok;
    return ok;
#else
    return SDL_SetWindowFullscreen(s_window, 0) == 0;
#endif
}

static bool restore_windowed()
{
    if (!s_window) return false;
    bool ok = leave_fullscreen_state();
#if defined(PSX_SDL3)
    ok = SDL_SetWindowBordered(s_window, true) && ok;
    ok = SDL_SetWindowResizable(s_window, true) && ok;
    if (s_have_windowed_rect) {
        ok = SDL_SetWindowSize(s_window, s_windowed_w, s_windowed_h) && ok;
        ok = SDL_SetWindowPosition(s_window, s_windowed_x, s_windowed_y) && ok;
    }
    (void)SDL_SyncWindow(s_window);
#else
    SDL_SetWindowBordered(s_window, SDL_TRUE);
    SDL_SetWindowResizable(s_window, SDL_TRUE);
    if (s_have_windowed_rect) {
        SDL_SetWindowSize(s_window, s_windowed_w, s_windowed_h);
        SDL_SetWindowPosition(s_window, s_windowed_x, s_windowed_y);
    }
#endif
    return ok;
}

static bool apply_borderless_window()
{
    if (!s_window) return false;
    bool ok = leave_fullscreen_state();
    const PsxDisplayID display = window_display();
    SDL_Rect bounds = {};
#if defined(PSX_SDL3)
    ok = SDL_GetDisplayBounds(display, &bounds) && ok;
    ok = SDL_SetWindowResizable(s_window, false) && ok;
    ok = SDL_SetWindowBordered(s_window, false) && ok;
    ok = SDL_SetWindowPosition(s_window, bounds.x, bounds.y) && ok;
    ok = SDL_SetWindowSize(s_window, bounds.w, bounds.h) && ok;
    (void)SDL_SyncWindow(s_window);
#else
    ok = SDL_GetDisplayBounds(display, &bounds) == 0 && ok;
    SDL_SetWindowResizable(s_window, SDL_FALSE);
    SDL_SetWindowBordered(s_window, SDL_FALSE);
    SDL_SetWindowPosition(s_window, bounds.x, bounds.y);
    SDL_SetWindowSize(s_window, bounds.w, bounds.h);
#endif
    return ok;
}

static bool apply_exclusive()
{
    if (!s_window) return false;
#if defined(PSX_SDL3)
    const PsxDisplayID display = window_display();
    const SDL_DisplayMode *desktop = SDL_GetDesktopDisplayMode(display);
    if (!desktop) return false;

    SDL_DisplayMode closest = {};
    if (!SDL_GetClosestFullscreenDisplayMode(
            display, desktop->w, desktop->h, desktop->refresh_rate,
            true, &closest)) {
        return false;
    }
    bool ok = SDL_SetWindowBordered(s_window, true);
    ok = SDL_SetWindowResizable(s_window, true) && ok;
    ok = SDL_SetWindowFullscreenMode(s_window, &closest) && ok;
    ok = SDL_SetWindowFullscreen(s_window, true) && ok;
    (void)SDL_SyncWindow(s_window);
    return ok;
#else
    SDL_DisplayMode desktop = {};
    const int display = window_display();
    if (SDL_GetDesktopDisplayMode(display, &desktop) != 0) return false;
    if (SDL_SetWindowDisplayMode(s_window, &desktop) != 0) return false;
    return SDL_SetWindowFullscreen(s_window, SDL_WINDOW_FULLSCREEN) == 0;
#endif
}

static bool apply_mode(int mode, const char *reason)
{
    if (!s_window) return false;
    mode = clamp_mode(mode);
    capture_snapshot(reason ? reason : "mode_change_begin");

    bool ok = false;
    switch (mode) {
    case 1: ok = apply_borderless_window(); break;
    case 2: ok = apply_exclusive(); break;
    default: ok = restore_windowed(); break;
    }

    if (ok) {
        s_applied_mode = mode;
    }
    set_cursor_for_state();
    capture_snapshot(ok ? "mode_change_applied" : "mode_change_failed");
    return ok;
}

struct JsonWriter {
    char *out;
    size_t capacity;
    size_t used;
    bool truncated;

    void append(const char *format, ...)
    {
        if (truncated || used >= capacity) {
            truncated = true;
            return;
        }
        va_list args;
        va_start(args, format);
        const int written = std::vsnprintf(
            out + used, capacity - used, format, args);
        va_end(args);
        if (written < 0 || static_cast<size_t>(written) >= capacity - used) {
            used = capacity ? capacity - 1 : 0;
            truncated = true;
            return;
        }
        used += static_cast<size_t>(written);
    }
};

static void append_snapshot(JsonWriter &writer, const DisplaySnapshot &snap)
{
    writer.append(
        "{\"ticks_ms\":%llu,\"reason\":\"%s\","
        "\"configured_mode\":%d,\"configured_name\":\"%s\","
        "\"applied_mode\":%d,\"applied_name\":\"%s\","
        "\"window_flags\":\"0x%016llX\","
        "\"window\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"pixel_w\":%d,\"pixel_h\":%d},"
        "\"display\":{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d,"
        "\"scale\":%.3f,\"desktop_w\":%d,\"desktop_h\":%d,"
        "\"desktop_hz\":%.3f,\"current_w\":%d,\"current_h\":%d,"
        "\"current_hz\":%.3f},"
        "\"fullscreen_mode_set\":%s,\"cursor_visible\":%s",
        static_cast<unsigned long long>(snap.ticks_ms), snap.reason,
        snap.configured_mode, mode_name(snap.configured_mode),
        snap.applied_mode, mode_name(snap.applied_mode),
        static_cast<unsigned long long>(snap.window_flags),
        snap.window_x, snap.window_y, snap.window_w, snap.window_h,
        snap.pixel_w, snap.pixel_h,
        snap.display_x, snap.display_y, snap.display_w, snap.display_h,
        snap.display_scale, snap.desktop_w, snap.desktop_h, snap.desktop_hz,
        snap.current_w, snap.current_h, snap.current_hz,
        snap.fullscreen_mode_set ? "true" : "false",
        snap.cursor_visible ? "true" : "false");
#ifdef _WIN32
    writer.append(
        ",\"win32\":{\"hwnd\":\"0x%llX\",\"style\":\"0x%llX\","
        "\"ex_style\":\"0x%llX\",\"rect_x\":%d,\"rect_y\":%d,"
        "\"rect_w\":%d,\"rect_h\":%d,\"mode_w\":%d,\"mode_h\":%d,"
        "\"mode_hz\":%d,\"mode_bpp\":%d,\"dpi\":%u}",
        static_cast<unsigned long long>(snap.hwnd),
        static_cast<unsigned long long>(snap.win_style),
        static_cast<unsigned long long>(snap.win_ex_style),
        snap.win_rect_x, snap.win_rect_y, snap.win_rect_w, snap.win_rect_h,
        snap.win_mode_w, snap.win_mode_h, snap.win_mode_hz,
        snap.win_mode_bpp, snap.win_dpi);
#endif
    writer.append("}");
}

} // namespace

extern "C" int psx_fullscreen_init(SDL_Window *window, int configured_mode)
{
    s_window = window;
    s_configured_mode = clamp_mode(configured_mode);
    s_applied_mode = 0;
    s_has_focus = true;
    s_event_head = 0;
    s_event_count = 0;
    s_event_total = 0;

    if (!s_window) return 0;
    (void)SDL_GetWindowPosition(s_window, &s_windowed_x, &s_windowed_y);
    (void)SDL_GetWindowSize(s_window, &s_windowed_w, &s_windowed_h);
    s_have_windowed_rect = true;
    capture_snapshot("window_created");
    if (apply_mode(s_configured_mode, "startup_mode_begin")) {
        return 1;
    }

    (void)restore_windowed();
    s_applied_mode = 0;
    set_cursor_for_state();
    capture_snapshot("startup_fallback_windowed");
    return 0;
}

extern "C" int psx_fullscreen_toggle(int configured_mode)
{
    s_configured_mode = clamp_mode(configured_mode);
    if (!s_window) return 0;
    if (s_applied_mode != 0) {
        return apply_mode(0, "hotkey_leave_begin") ? 1 : 0;
    }
    const int target = s_configured_mode != 0 ? s_configured_mode : 1;
    return apply_mode(target, "hotkey_enter_begin") ? 1 : 0;
}

extern "C" void psx_fullscreen_handle_event(const SDL_Event *event)
{
    if (!event || !s_window) return;
#if defined(PSX_SDL3)
    const char *reason = nullptr;
    switch (event->type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        s_has_focus = true;
        set_cursor_for_state();
        reason = "focus_gained";
        break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        s_has_focus = false;
        set_cursor_for_state();
        reason = "focus_lost";
        break;
    case SDL_EVENT_WINDOW_MINIMIZED: reason = "minimized"; break;
    case SDL_EVENT_WINDOW_RESTORED: reason = "restored"; break;
    case SDL_EVENT_WINDOW_ENTER_FULLSCREEN: reason = "enter_fullscreen"; break;
    case SDL_EVENT_WINDOW_LEAVE_FULLSCREEN: reason = "leave_fullscreen"; break;
    case SDL_EVENT_WINDOW_DISPLAY_CHANGED: reason = "display_changed"; break;
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED: reason = "pixel_size_changed"; break;
    default: break;
    }
    if (reason) capture_snapshot(reason);
#else
    if (event->type != SDL_WINDOWEVENT) return;
    const char *reason = nullptr;
    switch (event->window.event) {
    case SDL_WINDOWEVENT_FOCUS_GAINED:
        s_has_focus = true;
        set_cursor_for_state();
        reason = "focus_gained";
        break;
    case SDL_WINDOWEVENT_FOCUS_LOST:
        s_has_focus = false;
        set_cursor_for_state();
        reason = "focus_lost";
        break;
    case SDL_WINDOWEVENT_MINIMIZED: reason = "minimized"; break;
    case SDL_WINDOWEVENT_RESTORED: reason = "restored"; break;
    case SDL_WINDOWEVENT_DISPLAY_CHANGED: reason = "display_changed"; break;
    case SDL_WINDOWEVENT_SIZE_CHANGED: reason = "size_changed"; break;
    default: break;
    }
    if (reason) capture_snapshot(reason);
#endif
}

extern "C" void psx_fullscreen_shutdown(void)
{
    if (s_window) {
        capture_snapshot("shutdown");
#if defined(PSX_SDL3)
        (void)SDL_ShowCursor();
#else
        (void)SDL_ShowCursor(SDL_ENABLE);
#endif
    }
    s_window = nullptr;
    s_applied_mode = 0;
}

extern "C" int psx_fullscreen_applied_mode(void)
{
    return s_applied_mode;
}

extern "C" int psx_fullscreen_debug_json(char *out, size_t capacity,
                                           int request_id)
{
    if (!out || capacity < 2) return 0;
    out[0] = '\0';
    JsonWriter writer{out, capacity, 0, false};

#if defined(PSX_SDL3)
    const int version = SDL_GetVersion();
    const int version_major = SDL_VERSIONNUM_MAJOR(version);
    const int version_minor = SDL_VERSIONNUM_MINOR(version);
    const int version_micro = SDL_VERSIONNUM_MICRO(version);
#else
    SDL_version version = {};
    SDL_GetVersion(&version);
    const int version_major = version.major;
    const int version_minor = version.minor;
    const int version_micro = version.patch;
#endif
    const char *driver = SDL_GetCurrentVideoDriver();
    writer.append(
        "{\"id\":%d,\"ok\":true,\"schema\":1,"
        "\"sdl_version\":\"%d.%d.%d\","
        "\"video_driver\":\"%s\",\"event_total\":%llu,"
        "\"event_retained\":%u,\"event_capacity\":%d,\"truncated\":%s,"
        "\"events\":[",
        request_id, version_major, version_minor, version_micro,
        driver ? driver : "unknown",
        static_cast<unsigned long long>(s_event_total),
        s_event_count, kEventCapacity,
        s_event_total > s_event_count ? "true" : "false");

    const unsigned oldest =
        (s_event_head + kEventCapacity - s_event_count) % kEventCapacity;
    for (unsigned i = 0; i < s_event_count; i++) {
        if (i) writer.append(",");
        append_snapshot(writer, s_events[(oldest + i) % kEventCapacity]);
    }
    writer.append("]}");

    if (writer.truncated) {
        const char fallback[] =
            "{\"ok\":false,\"error\":\"fullscreen diagnostic response "
            "buffer was too small\"}";
        std::snprintf(out, capacity, "%s", fallback);
        return static_cast<int>(std::strlen(out));
    }
    return static_cast<int>(writer.used);
}
