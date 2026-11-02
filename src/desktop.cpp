#include "desktop.h"

#include <dwmapi.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <vector>

namespace desktop {

namespace {

constexpr ULONGLONG kSettleMs = 500; // our own minimizes and the desktop taking the keyboard echo back for a moment: not the user
constexpr ULONGLONG kRestoreWaitMs = 800; // each app comes back on its own thread: this long for all of them before the order is set
constexpr DWORD kPollMs = 15;

std::vector<HWND> saved; // what the last click put away, top of the z-order first; the ui thread's alone
HWND focus = nullptr; // what had the keyboard then
ULONGLONG hiddenAt = 0;
HWINEVENTHOOK hooks[2] = {};
std::atomic<unsigned> gen{0}; // a newer click wins over a restore still under way

bool Cloaked(HWND h) { // on another virtual desktop, or a suspended store app
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0;
}

bool Neutral(HWND h) { // never put away, and its turn in front never undoes show desktop: the shell, menus, overlays, the bar and the island
    if (!h) return true;
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return true;
    wchar_t cls[64] = {};
    GetClassNameW(h, cls, 64);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd", L"Windows.UI.Core.CoreWindow"})
        if (!wcscmp(cls, shell)) return true; // the desktop, the taskbars, start and search
    if (!wcsncmp(cls, L"Astraeus", 8)) return true;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOPMOST) return true; // pinned on top on purpose: overlays, players, recorders
    return !(ex & WS_EX_APPWINDOW) && (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)); // palettes and popups
}

bool Candidate(HWND h) { // an app window that's up: on this virtual desktop, with a taskbar button of its own
    if (!IsWindowVisible(h) || IsIconic(h) || Cloaked(h) || Neutral(h)) return false;
    if (!(GetWindowLongPtrW(h, GWL_EXSTYLE) & WS_EX_APPWINDOW) && GetWindow(h, GW_OWNER)) return false; // dialogs go down with their owner
    wchar_t t[2];
    return InternalGetWindowText(h, t, 2) > 0; // never sends a message: a hung app can't stall us
}

void Forget() {
    saved.clear();
    focus = nullptr;
    for (auto& k : hooks)
        if (k) UnhookWinEvent(k), k = nullptr;
}

void CALLBACK OnEvent(HWINEVENTHOOK, DWORD, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    if (obj != OBJID_WINDOW || child != CHILDID_SELF || saved.empty()) return;
    if (GetTickCount64() - hiddenAt < kSettleMs) return;
    HWND root = GetAncestor(h, GA_ROOTOWNER);
    if (Neutral(root ? root : h)) return; // start, the desktop, a menu: the next click still brings the windows back
    Forget(); // the user went back to an app: the next click puts away what's up then, as windows does
}

bool Hide() {
    Forget();
    ++gen; // a restore still setting the order stops
    std::vector<HWND> up;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (Candidate(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&up)); // top of the z-order first
    if (up.empty()) return true; // the desktop already shows
    HWND fore = GetForegroundWindow();
    std::vector<HWND> down;
    for (HWND h : up)
        if (ShowWindowAsync(h, SW_SHOWMINNOACTIVE)) down.push_back(h); // async: a hung app never stalls the bar; one elevated above us refuses
    if (down.empty()) return false;
    if (HWND shell = GetShellWindow()) SetForegroundWindow(shell); // the keyboard to the desktop, before the apps get round to going down: none is handed it
    saved = std::move(down);
    focus = fore;
    hiddenAt = GetTickCount64();
    constexpr DWORD kFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    hooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnEvent, 0, 0, kFlags);
    hooks[1] = SetWinEventHook(EVENT_SYSTEM_MINIMIZEEND, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnEvent, 0, 0, kFlags); // one of them brought back by hand
    return true;
}

void Restore() {
    std::vector<HWND> wins = std::move(saved);
    HWND fore = focus;
    Forget();
    if (Neutral(fore) || !IsWindow(fore)) fore = wins.front(); // the desktop or the bar had the keyboard: the app in front gets it
    unsigned me = ++gen;
    std::thread([wins = std::move(wins), fore, me] { // setting the order waits on each app: never on the ui thread
        for (auto it = wins.rbegin(); it != wins.rend(); ++it)
            if (IsWindow(*it) && IsIconic(*it)) ShowWindowAsync(*it, SW_SHOWNOACTIVATE); // restored as it was, maximized or not, without a flurry of focus
        auto pending = [&] { return std::any_of(wins.begin(), wins.end(), [](HWND h) { return IsWindow(h) && IsIconic(h); }); };
        for (ULONGLONG until = GetTickCount64() + kRestoreWaitMs; pending() && GetTickCount64() < until && gen == me;) Sleep(kPollMs);
        HWND above = HWND_TOP;
        for (HWND h : wins) { // the top one first, each next tucked under the one before: the order they had
            if (gen != me) return;
            if (!IsWindow(h) || IsIconic(h)) continue;
            UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | (IsHungAppWindow(h) ? SWP_ASYNCWINDOWPOS : 0); // a hung app gets it posted
            if (SetWindowPos(h, above, 0, 0, 0, 0, flags)) above = h;
        }
        if (gen != me || !IsWindow(fore) || !IsWindowVisible(fore) || IsIconic(fore)) return;
        if (!SetForegroundWindow(fore)) SwitchToThisWindow(fore, TRUE); // the click was ours, so windows normally lets us
    }).detach();
}

}

bool Toggle() {
    bool back = std::any_of(saved.begin(), saved.end(), [](HWND h) { return IsWindow(h) && IsIconic(h); });
    if (!back) return Hide(); // nothing of ours left to bring back: put away what's up now
    Restore();
    return true;
}

}
