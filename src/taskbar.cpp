#include "taskbar.h"

#include <shellapi.h>

#include "prefs.h"

namespace taskbar {

namespace {
constexpr wchar_t kTaken[] = L"TaskbarTaken"; // 1 while we hold the taskbar: a crash leaves it set, the next start knows
constexpr wchar_t kState[] = L"TaskbarState"; // the user's ABS_* flags from before we took it
constexpr wchar_t kMulti[] = L"TaskbarMulti"; // the user's "taskbar on all displays", from before we took it: -1 never set
constexpr wchar_t kAdvanced[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced";
constexpr int kUnsaved = -2;
bool taken = false;

int ReadMulti() {
    DWORD v = 0, n = sizeof(v);
    return RegGetValueW(HKEY_CURRENT_USER, kAdvanced, L"MMTaskbarEnabled", RRF_RT_REG_DWORD, nullptr, &v, &n) == ERROR_SUCCESS ? (int)v : -1;
}

void SetMulti(int v) { // explorer's own "show my taskbar on all displays": off, it closes the other screens' taskbars instead of fighting a hide
    if (ReadMulti() == v) return;
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAdvanced, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (v < 0) RegDeleteValueW(k, L"MMTaskbarEnabled");
    else RegSetValueExW(k, L"MMTaskbarEnabled", 0, REG_DWORD, reinterpret_cast<const BYTE*>(&v), sizeof(DWORD));
    RegCloseKey(k);
    SendMessageTimeoutW(HWND_BROADCAST, WM_SETTINGCHANGE, 0, reinterpret_cast<LPARAM>(L"TraySettings"), SMTO_ABORTIFHUNG, 1000, nullptr); // as the settings app does: explorer applies it live
}

void Secondaries(int show) { // explorer's taskbars on the other screens: found by class, not by asking every window on the desktop its name
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, L"Shell_SecondaryTrayWnd", nullptr));)
        if ((show == SW_HIDE) == (IsWindowVisible(h) != FALSE)) show == SW_HIDE ? ShowWindowAsync(h, show) : ShowWindow(h, show); // hiding, often: never waits on explorer
}

HWND Tray() { return ExplorerTray(); }

void SetState(UINT state) {
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = Tray();
    abd.lParam = state;
    SHAppBarMessage(ABM_SETSTATE, &abd);
}
}

HWND ExplorerTray() {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"Shell_TrayWnd", nullptr))) {
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid != GetCurrentProcessId()) return h; // the bar keeps a window of this class for tray icons
    }
    return nullptr;
}

bool IsTray(HWND h) {
    wchar_t cls[32] = {};
    return h && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"Shell_TrayWnd");
}

bool IsSecondary(HWND h) {
    wchar_t cls[32] = {};
    return h && GetClassNameW(h, cls, 32) && !wcscmp(cls, L"Shell_SecondaryTrayWnd");
}

void Take() {
    if (!Tray()) return; // explorer isn't up yet (early at sign-in): TaskbarCreated brings us back here
    if (!prefs::ReadNum(kTaken, 0)) { // not already ours: what's there now is the user's choice
        APPBARDATA abd = {sizeof(abd)};
        prefs::SaveNum(kState, (int)SHAppBarMessage(ABM_GETSTATE, &abd));
        prefs::SaveNum(kTaken, 1);
    }
    if (prefs::ReadNum(kMulti, kUnsaved) == kUnsaved) prefs::SaveNum(kMulti, ReadMulti()); // kept apart: a hold from before this setting existed still saves it once
    taken = true;
    SetMulti(0);
    SetState(ABS_AUTOHIDE); // auto-hide gives up its reserved strip, the bar takes it
    Hide();
}

void Hide() {
    if (!taken) return;
    if (HWND tray = Tray(); tray && IsWindowVisible(tray)) ShowWindowAsync(tray, SW_HIDE); // async: a busy explorer never stalls the dock
    Secondaries(SW_HIDE); // the other screens' too, docks there or not: auto-hidden they still pop up at a touch of their edge, a second taskbar beside the bar
}

void Give() {
    if (!taken && !prefs::ReadNum(kTaken, 0)) return;
    taken = false;
    HWND tray = Tray();
    if (!tray) return; // explorer is gone (restarting, or logging off): keep the marker so the next start still knows the user's state
    UINT want = (UINT)prefs::ReadNum(kState, 0) & ABS_AUTOHIDE; // always-on-top has been implied since windows 7
    SetState(want);
    if (int multi = prefs::ReadNum(kMulti, kUnsaved); multi != kUnsaved) { // their other screens' taskbars back as they had them
        SetMulti(multi);
        if (ReadMulti() == multi) prefs::Delete(kMulti); // only once it's really back: a failed write keeps it for the next try
    }
    SetWindowPos(tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // the tray host pushed it to the bottom
    ShowWindow(tray, SW_SHOWNA);
    Secondaries(SW_SHOWNA); // and theirs on the other screens
    APPBARDATA abd = {sizeof(abd)};
    if (((UINT)SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) == want) prefs::Delete(kTaken); // only once it really took
}

}
