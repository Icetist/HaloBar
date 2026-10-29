#include "prefs.h"

#include <algorithm>
#include <string>

namespace prefs {

namespace {
constexpr wchar_t kIslandKey[] = L"Software\\Astraeus"; // astraeus island's settings root, read only
constexpr wchar_t kBarKey[] = L"Software\\HaloBar"; // the bar's own settings, per user
constexpr wchar_t kOldBarKey[] = L"Software\\AstraeusBar"; // the name before halobar, moved over once
constexpr int kIslandStarfall = 5; // island's own starfall look: worn here only by following it, never offered as the bar's pick
HKEY watched = nullptr; // island's key, held open while its change notification is armed

int Read(const wchar_t* key, const wchar_t* name, int fallback) {
    DWORD value = 0, size = sizeof(value);
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_DWORD, nullptr, &value, &size) != ERROR_SUCCESS) return fallback; // missing or wrong type
    return (int)value;
}
}

void MoveOldKey() {
    HKEY old = nullptr, neu = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kOldBarKey, 0, KEY_READ, &old) != ERROR_SUCCESS) return; // nothing from before
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBarKey, 0, KEY_READ, &neu) == ERROR_SUCCESS) { // already moved: the old one is leftover
        RegCloseKey(neu);
        RegCloseKey(old);
        RegDeleteTreeW(HKEY_CURRENT_USER, kOldBarKey); // never copy over newer settings
        return;
    }
    DWORD disp = 0;
    bool ok = RegCreateKeyExW(HKEY_CURRENT_USER, kBarKey, 0, nullptr, 0, KEY_ALL_ACCESS, nullptr, &neu, &disp) == ERROR_SUCCESS &&
              RegCopyTreeW(old, nullptr, neu) == ERROR_SUCCESS; // settings, pins state and the taskbar-taken marker, all of it
    if (neu) RegCloseKey(neu);
    RegCloseKey(old);
    if (ok) RegDeleteTreeW(HKEY_CURRENT_USER, kOldBarKey); // only once the copy is whole, so a failed move is retried next start
}

bool IslandInstalled() {
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH); // the installer's default place first, then its autostart entry
    if (n > 0 && n < MAX_PATH && GetFileAttributesW((std::wstring(base) + L"\\Programs\\Astraeus\\Astraeus.exe").c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    wchar_t run[1024];
    DWORD size = sizeof(run); // in bytes, not characters
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", L"Astraeus", RRF_RT_REG_SZ, nullptr, run, &size) == ERROR_SUCCESS) {
        std::wstring cmd = run, exe; // "C:\path\Astraeus.exe" --args, or unquoted
        if (!cmd.empty() && cmd[0] == L'"') exe = cmd.substr(1, cmd.find(L'"', 1) - 1);
        else exe = cmd.substr(0, cmd.find(L' '));
        if (!exe.empty() && GetFileAttributesW(exe.c_str()) != INVALID_FILE_ATTRIBUTES) return true;
    }
    return FindWindowW(L"AstraeusHost", nullptr) != nullptr; // a portable copy, running right now
}

Look IslandLook() {
    Look l;
    l.look = Read(kIslandKey, L"Appearance", 0);
    l.followsBar = l.look == 4;
    if (!l.followsBar) l.bgOpacity = (std::clamp)(Read(kIslandKey, L"BgOpacity", 100), Settings::kMinOpacity, 100); // clamped as island clamps it; 4 follows the bar back: the default ends the loop
    if (l.look < 0 || (l.look > 3 && l.look != kIslandStarfall)) l.look = 0; // a look this build doesn't know, or 4 (following us): the default one
    l.accentFixed = Read(kIslandKey, L"AccentMode", 0) == 1;
    l.accentHex = Read(kIslandKey, L"AccentHex", l.accentHex) & 0xFFFFFF; // rgb only, any stray alpha byte dropped
    l.customBg = Read(kIslandKey, L"CustomBg", l.customBg) & 0xFFFFFF;
    l.glassBlur = Read(kIslandKey, L"GlassBlur", l.glassBlur);
    if (l.glassBlur < 0 || l.glassBlur > 100) l.glassBlur = 100;
    int rm = Read(kIslandKey, L"ReduceMotion", -1);
    if (rm < 0) { // island not set up: follow windows
        BOOL anim = TRUE;
        SystemParametersInfoW(SPI_GETCLIENTAREAANIMATION, 0, &anim, 0);
        rm = anim ? 0 : 1; // "show animations in windows" off means reduce motion
    }
    l.reduceMotion = rm != 0;
    return l;
}

Settings Load() {
    Settings s;
    auto num = [](const wchar_t* name, int fallback, int lo, int hi) { // anything out of range is a stale or hand-edited value: the default
        int v = Read(kBarKey, name, fallback);
        return v < lo || v > hi ? fallback : v;
    };
    s.look = num(L"Look", s.look, -1, 3); // -1 follows island
    s.blur = num(L"GlassBlur", s.blur, 0, 100);
    s.opacity = num(L"Opacity", s.opacity, Settings::kMinOpacity, 100);
    s.accent = num(L"Accent", s.accent, -1, 1);
    s.accentHex = Read(kBarKey, L"AccentHex", s.accentHex) & 0xFFFFFF;
    s.customBg = Read(kBarKey, L"CustomBg", s.customBg) & 0xFFFFFF;
    s.size = num(L"Size", s.size, 0, 2);
    s.room = num(L"Room", s.room, 0, 1);
    s.widgets = Read(kBarKey, L"Widgets", s.widgets) & 0x3FF;
    s.mods = Read(kBarKey, L"Mods", s.mods) & 0xFFF;
    if (Read(kBarKey, L"ModsV", 1) < 2) { // show desktop came after the rest: settings saved before it get it switched on, once
        s.mods |= 1 << 11;
        SaveNum(L"Mods", s.mods);
        SaveNum(L"ModsV", 2);
    }
    s.launcher = num(L"Launcher", s.launcher, 0, 1);
    s.pills = num(L"Pills", s.pills, 0, 1);
    s.song = num(L"Song", s.song, 0, 1);
    s.screens = num(L"Screens", s.screens, 0, 1);
    s.align = num(L"Align", s.align, 0, 2);
    s.edge = num(L"Edge", s.edge, 0, 2);
    s.winKeys = num(L"WinKeys", s.winKeys, 0, 1);
    s.adaptive = num(L"Adaptive", s.adaptive, 0, 1);
    s.trayApps = num(L"TrayApps", s.trayApps, 0, 1);
    s.startIcon = num(L"StartIcon", s.startIcon, 0, 1); // the old sparkle face reads as halobar's
    s.hoverPlay = num(L"HoverPlay", s.hoverPlay, 0, 1);
    s.date = num(L"Date", s.date, 0, 1);
    s.clock24 = num(L"Clock24", s.clock24, -1, 1); // -1 follows the windows region format
    return s;
}

HANDLE WatchIsland() {
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kIslandKey, 0, KEY_NOTIFY, &watched) != ERROR_SUCCESS) {
        watched = nullptr; // no island: nothing to follow
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto reset: one wake per change
    if (ev) Rearm(ev);
    return ev;
}

void Rearm(HANDLE ev) {
    if (watched && ev) RegNotifyChangeKeyValue(watched, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE); // one shot: armed again after each change
}

void Unwatch(HANDLE ev) {
    if (watched) RegCloseKey(watched); // closing the key ends the pending notification
    watched = nullptr;
    if (ev) CloseHandle(ev);
}

int ReadNum(const wchar_t* name, int fallback) { return Read(kBarKey, name, fallback); }

int ReadShared(const wchar_t* name, int fallback) { return Read(kIslandKey, name, fallback); }

void SaveShared(const wchar_t* name, int value) { // the one place halobar writes astraeus' key: a switch both apps show
    DWORD v = (DWORD)value;
    RegSetKeyValueW(HKEY_CURRENT_USER, kIslandKey, name, REG_DWORD, &v, sizeof(v));
}

void SaveNum(const wchar_t* name, int v) {
    DWORD value = (DWORD)v;
    RegSetKeyValueW(HKEY_CURRENT_USER, kBarKey, name, REG_DWORD, &value, sizeof(value)); // creates the key if missing
}

void Delete(const wchar_t* name) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kBarKey, name); }

}

namespace sys { // text settings for the code shared with island: the bar's key first, island's as a fallback, written only to the bar's

std::wstring ReadText(const wchar_t* name) {
    for (const wchar_t* key : {prefs::kBarKeyPath, prefs::kIslandKeyPath}) {
        wchar_t buf[512] = {}; // the widget layout is the longest
        DWORD size = sizeof(buf);
        if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS) return buf;
    }
    return {};
}

void SaveText(const wchar_t* name, const std::wstring& value) {
    RegSetKeyValueW(HKEY_CURRENT_USER, prefs::kBarKeyPath, name, REG_SZ, value.c_str(), (DWORD)((value.size() + 1) * sizeof(wchar_t)));
}

}
