#include "taskbar.h"

#include <shellapi.h>

#include "prefs.h"

namespace taskbar {

namespace {
constexpr wchar_t kTaken[] = L"TaskbarTaken"; // 1 while we hold the taskbar: a crash leaves it set, the next start knows
constexpr wchar_t kState[] = L"TaskbarState"; // the user's ABS_* flags from before we took it
bool taken = false;

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

void Take() {
    if (!Tray()) return; // explorer isn't up yet (early at sign-in): TaskbarCreated brings us back here
    if (!prefs::ReadNum(kTaken, 0)) { // not already ours: what's there now is the user's choice
        APPBARDATA abd = {sizeof(abd)};
        prefs::SaveNum(kState, (int)SHAppBarMessage(ABM_GETSTATE, &abd));
        prefs::SaveNum(kTaken, 1);
    }
    taken = true;
    SetState(ABS_AUTOHIDE); // auto-hide gives up its reserved strip, the bar takes it
    Hide();
}

void Hide() {
    if (!taken) return;
    if (HWND tray = Tray(); tray && IsWindowVisible(tray)) ShowWindow(tray, SW_HIDE); // secondary screens keep theirs, auto-hidden
}

void Give() {
    if (!taken && !prefs::ReadNum(kTaken, 0)) return;
    taken = false;
    HWND tray = Tray();
    if (!tray) return; // explorer is gone (restarting, or logging off): keep the marker so the next start still knows the user's state
    UINT want = (UINT)prefs::ReadNum(kState, 0) & ABS_AUTOHIDE; // always-on-top has been implied since windows 7
    SetState(want);
    SetWindowPos(tray, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // the tray host pushed it to the bottom
    ShowWindow(tray, SW_SHOWNA);
    APPBARDATA abd = {sizeof(abd)};
    if (((UINT)SHAppBarMessage(ABM_GETSTATE, &abd) & ABS_AUTOHIDE) == want) prefs::Delete(kTaken); // only once it really took
}

}
