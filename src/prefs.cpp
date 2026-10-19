#include "prefs.h"

#include <string>

namespace prefs {

namespace {
constexpr wchar_t kIslandKey[] = L"Software\\Astraeus"; // astraeus island's settings root, read only
constexpr wchar_t kBarKey[] = L"Software\\HaloBar"; // the bar's own settings, per user
constexpr wchar_t kOldBarKey[] = L"Software\\AstraeusBar"; // the name before halobar, moved over once
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
    if (l.look < 0 || l.look > 3) l.look = 0; // a look this build doesn't know: the default one
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
    s.accent = num(L"Accent", s.accent, -1, 1);
    s.accentHex = Read(kBarKey, L"AccentHex", s.accentHex) & 0xFFFFFF;
    s.customBg = Read(kBarKey, L"CustomBg", s.customBg) & 0xFFFFFF;
    s.size = num(L"Size", s.size, 0, 2);
    s.room = num(L"Room", s.room, 0, 1);
    s.launcher = num(L"Launcher", s.launcher, 0, 1);
    s.pills = num(L"Pills", s.pills, 0, 1);
    s.song = num(L"Song", s.song, 0, 1);
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

void SaveNum(const wchar_t* name, int v) {
    DWORD value = (DWORD)v;
    RegSetKeyValueW(HKEY_CURRENT_USER, kBarKey, name, REG_DWORD, &value, sizeof(value)); // creates the key if missing
}

void Delete(const wchar_t* name) { RegDeleteKeyValueW(HKEY_CURRENT_USER, kBarKey, name); }

}
