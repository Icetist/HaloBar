#include "bar.h"

#include <dbt.h>
#include <dwmapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <ctime>
#include <cwctype>

#include "glyphs.h"
#include "overflow.h"
#include "popmenu.h"
#include "rest.h"
#include "setui.h"
#include "taskbar.h"
#include "version.h"

#pragma comment(lib, "shcore.lib")

void Census(UINT what, WPARAM sub, int add = 1);

namespace {

constexpr UINT_PTR kClockTimer = 1, kRescanTimer = 2, kHoverTimer = 3, kCloseTimer = 4, kIdleTimer = 5, kDevTimer = 6, kStatusTimer = 7, kReserveTimer = 8, kClipTimer = 31;
constexpr float kLogoGrow = 1.6f; // the icon has room around the mark: drawn larger, the mark matches windows' logo
constexpr UINT kClipSettleMs = 150; // a copy is often several clipboard calls: opening it in between makes the copying app's last one fail
std::vector<Bar*> coverBars; // every dock, main one first: the shared hooks and reads reach each of them
HWINEVENTHOOK coverHooks[3] = {}, moveHook = nullptr; // what moves windows: focus, drags, minimizes; and the app in front's own moves. one set for every screen
Bar* keyBar = nullptr; // the dock the keyboard hook speaks for: the main one
constexpr UINT kReserveSettleMs = 700; // explorer's taskbar takes a moment to really give up its strip once it auto-hides
constexpr UINT kStatusMs = 10000; // network, battery and volume in the row: windows doesn't tell us, so a slow look
constexpr UINT kRescanMs = 40; // a burst of window events lands in one walk of the windows
constexpr UINT kHoverMs = 260, kCloseMs = 420; // song: rest on it to open the player; leave the island this long to close what's open
constexpr UINT kSetCloseMs = 1400; // settings forgive a longer trip outside
constexpr UINT_PTR kHideTimer = 90; // explorer's taskbar came back in a burst: hide it once more after a pause
constexpr int kHideBurst = 6; // shows in one second before we stop answering each one
constexpr UINT kHideCalmMs = 1500, kHideCalmMaxMs = 30000; // the pause doubles while explorer keeps at it, up to this
constexpr int kHideStrikesMax = 5;
constexpr UINT_PTR kScreensTimer = 91; // a screen came or went: the docks are made again once the screens hold still
constexpr UINT kScreensSettleMs = 1500; // a flapping cable or a waking dock station sends a burst of changes
constexpr int kScreensGapSec = 10; // restarts for screens at most this often, whatever the screens do
constexpr UINT kStatsMs = 2000; // the load and network widgets' tick while in sight
constexpr UINT kNotifQuickMs = 2000; // the store looked at while a pill could rise
constexpr UINT kFullMs = 10000; // behind a fullscreen app: a slow look that it's still there, in case the shell's notice goes missing
constexpr WORD kMaskKey = 0xE8; // unassigned: sent so a win + number never leaves the start menu to open
constexpr WPARAM kKeyNext = 100, kKeyBack = 101, kKeyTray = 102; // WM_APP_WINKEY past the numbers: win + t, shift + win + t, win + b
constexpr WPARAM kTrayShowDesktop = 407; // the command explorer's taskbar runs for its show desktop corner
constexpr UINT kIdleMs = 2500; // still this long: efficiency mode
constexpr UINT_PTR kTrimTimer = 92; // still for longer: memory handed back to windows
constexpr UINT kTrimMs = 30000; // a wake after a short pause doesn't fault every page back in; a long one gives them up
constexpr double kBusyFps = 60; // springs and fades: smooth at 60, and a 240 hz screen doesn't quadruple the cost
constexpr int kDevTries = 30; // ~3 seconds of retries, 100 ms apart
constexpr UINT kDevRetryMs = 2000; // after that, a calm retry until the gpu answers again
constexpr float kThemeSec = 0.8f; // palette crossfade, as island's
constexpr double kSelfCallSec = 3; // our own TaskbarCreated echoes back for a moment
constexpr UINT kMenuRestart = 1, kMenuExit = 2, kMenuSettings = 3, kMenuStart = 6, kMenuUpdate = 7, kMenuEdit = 8;

void Log(const char* what) { // DebugLog set: frame trouble goes to %TEMP%\HaloBar-log.txt, nothing otherwise
    static int on = prefs::ReadNum(L"DebugLog", 0);
    if (!on) return;
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    HANDLE f = CreateFileW((std::wstring(tmp) + L"HaloBar-log.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    std::string line = std::to_string(GetTickCount64()) + " " + what + "\r\n"; // any length: a census line runs long
    DWORD w = 0;
    WriteFile(f, line.data(), (DWORD)line.size(), &w, nullptr);
    CloseHandle(f);
}

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

}

void Census(UINT what, WPARAM sub, int add) { // DebugLog set: what wakes the ui thread, tallied and written every 10 s; nothing otherwise
    constexpr ULONGLONG kCensusMs = 10000;
    constexpr size_t kCensusMax = 64; // kinds kept apart; the rest share one line
    static const bool on = prefs::ReadNum(L"DebugLog", 0) != 0;
    if (!on) return;
    static std::map<uint64_t, int> seen;
    static ULONGLONG since = GetTickCount64();
    uint64_t key = (uint64_t)what << 32 | (what == WM_TIMER || what == WM_APP_TASKS || what >= 0xF0000 ? (uint32_t)sub : 0); // timers and task pings by their id
    if (seen.size() < kCensusMax || seen.count(key)) seen[key] += add;
    else seen[~0ull] += add;
    ULONGLONG now = GetTickCount64();
    if (now - since < kCensusMs) return;
    std::string line = "census " + std::to_string((now - since) / 1000) + "s:";
    for (auto& [k, n] : seen) {
        char part[48];
        sprintf_s(part, " %x/%x=%d", (unsigned)(k >> 32), (unsigned)(k & 0xFFFFFFFF), n);
        line += part;
    }
    seen.clear();
    since = now;
    Log(line.c_str());
}

Bar::Bar() {
    look = prefs::IslandLook();
    islandHere = prefs::IslandInstalled();
    cfg = prefs::Load();
    shellSeed = mediaSeed = Seed{}; // SyncLook fills it in at create, from the chosen accent or the wallpaper
    shellFrom = shellTo = shellNow = mediaFrom = mediaTo = mediaNow = Make(shellSeed);
}

Bar::~Bar() {
    if (hwnd) {
        for (UINT_PTR t : {kClockTimer, kRescanTimer, kHoverTimer, kCloseTimer, kIdleTimer, kDevTimer, kStatusTimer, kReserveTimer, kPreviewTimer, kPreviewHideTimer, kCoverTimer, kPeekTimer}) KillTimer(hwnd, t);
        Unreserve();
    }
    if (shelf) DestroyWindow(shelf), shelf = nullptr;
    if (HMODULE u = GetModuleHandleW(L"user32.dll"); u && taskmanWas && IsWindow(taskmanWas)) // explorer answers for minimizing windows again
        if (auto set = reinterpret_cast<BOOL(WINAPI*)(HWND)>(GetProcAddress(u, "SetTaskmanWindow"))) set(taskmanWas);
    if (clipListening && hwnd) RemoveClipboardFormatListener(hwnd);
    if (clipKeyOn && hwnd) UnregisterHotKey(hwnd, 0x4842);
    if (!lead && clip::History().dirty) clip::History().Save(); // the last copies' index, written before leaving
    if (hwnd && !moon) HaloIcon(false);
    if (keyHook) UnhookWindowsHookEx(keyHook), keyHook = nullptr;
    if (keyBar == this) keyBar = nullptr;
    if (dropper && hwnd) RevokeDragDrop(hwnd), dropper.Reset(); // ole holds the target until told
    tray.reset(); // apps fall back to explorer's tray, which has every icon
    HookCover(false);
    std::erase(coverBars, this);
    if (notifWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(notifWatch);
    if (!moon) Banners(false);
    card.Hide();
    icons.Stop(); // its thread posts to the window: done before the window goes
    if (tasks && lead) tasks->Unlisten(hwnd); // a moon only lets go: the main dock's list lives on
    tasks.reset(); // unhook before the window goes
    spectrum.reset();
    FreeDev();
    StopPoller(); // it closes the status handles on its own thread
    if (prefsEv) prefs::Unwatch(prefsEv); // the watches are the main dock's: a moon has none to close
    if (pinsEv) pins::Unwatch(pinsEv);
    if (frameWait) CloseHandle(frameWait);
    if (hwnd) DestroyWindow(hwnd);
}

bool Bar::Create(HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = L"HaloBar";
    RegisterClassExW(&wc);
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_NOREDIRECTIONBITMAP, // no focus steal, no gdi surface (dcomp only)
                           wc.lpszClassName, L"HaloBar", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, this);
    if (!hwnd) return false;
    BOOL off = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_TRANSITIONS_FORCEDISABLED, &off, sizeof(off)); // no dwm fade, we animate ourselves
    frameWait = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_MANUAL_RESET | CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!frameWait) return false;
    wc.lpfnWndProc = DefWindowProcW; // the dock's strip holder: never shown, never painted, it only owns an appbar
    wc.lpszClassName = L"HaloBarShelf";
    RegisterClassExW(&wc);
    shelf = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, instance, nullptr);
    tbMsg = RegisterWindowMessageW(L"TaskbarCreated");
    icons.Start(hwnd, WM_APP_ICON); // the shell's icons are read off the ui thread: the first frame doesn't wait seconds for them
    if (lead) tasks = lead->tasks, tasks->Listen(hwnd); // one window list for every screen: the main dock walks it, the moons hear each change
    else tasks = std::make_shared<Tasks>(hwnd, WM_APP_TASKS);
    if (!tasks) return false;
    Place();
    if (!lead) tasks->Rescan();
    coverBars.push_back(this);
    HookCover(true); // always: auto-hide and the glass's tint both follow the windows
    notifStamp = sys::NotifStamp();
    dnd = Quiet(); // the bell shows do not disturb as it already is, from either app, before any panel opens
    LoadNotifs(); // the counts start from what's there now
    wchar_t dir[MAX_PATH];
    if (!lead && ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Notifications", dir, MAX_PATH)) // one watch: the main dock tells the moons
        notifWatch = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE); // the kernel tells us when a toast lands
    if (Hush()) Banners(true);
    screensAt = GetSystemMetrics(SM_CMONITORS);
    if (!lead) SetTimer(hwnd, kAppsTimer, 8000, nullptr); // a moon borrows the main dock's list as its launcher opens
    if (!lead) notifPolling = true, SetCoalescableTimer(hwnd, kNotifPollTimer, kNotifQuickMs, nullptr, 500); // windows writes its store lazily and the folder watch can lag: two attribute reads every 2 s catch a new toast fast
    cals.Sync(hwnd, WM_APP_CAL, 0); // and again whenever the calendar opens: nothing fetches while it's closed
    if (!moon) SetTimer(hwnd, kUpdateTimer, kUpdateFirstMs, nullptr); // one updater: the main dock's
    if (!lead) media.Start(hwnd, WM_APP_MEDIA); // one session watcher: a moon shows the main dock's song
    if (!lead) prefsEv = prefs::WatchIsland(), pinsEv = pins::Watch(); // watched once: the main dock passes changes on
    SyncLook();
    SyncUser(); // the toolbar's user module
    SyncDesktops();
    shellNow = shellTo; // start on the final colors, no fade at boot
    mediaNow = mediaTo;
    shellT = mediaT = 1;
    ArmClock();
    radios.Start(hwnd, WM_APP_RADIOS);
    SyncAudio();
    if (!lead) StartPoller(); // one poller: its reads reach every dock
    else status = lead->status, binFull = lead->binFull, wx = lead->wx, OnMedia(); // what the main dock knows so far
    LoadIconPaths(); // the icons picked for apps
    LoadSlots(); // the dock's widgets, in their order and sizes
    SyncWidgets(); // the clipboard listener, the stats timer, the forecast: only for the widgets in the dock
    PollStatus();
    SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    SyncDrop(); // files dragged onto the dock's apps
    BelowIsland(); // astraeus came up first: its strip stays over the top bar
    Redraw();
    return true;
}

void Bar::ClaimTray() {
    if (!tray) {
        tray = std::make_unique<TrayHost>(hwnd, WM_APP_TRAY);
        tray->iconAt = [this](uint64_t key, RECT& at) { // in the dock or its tray, else at the tray's chevron
            auto it = trayAt.find(key);
            return (it != trayAt.end() && ScreenRect(it->second, at)) || ScreenRect(trayR, at) || ScreenRect(wegR, at);
        };
        tray->barAt = [this](RECT& at, UINT& edge) { // the dock's strip along its whole edge, as windows' own taskbar's
            MONITORINFO mi = {sizeof(mi)};
            if (!ScreenRect(wegR, at) || !GetMonitorInfoW(mon, &mi)) return false;
            edge = !Vert() ? ABE_BOTTOM : cfg.edge == 1 ? ABE_LEFT : ABE_RIGHT;
            if (edge == ABE_BOTTOM) at.left = mi.rcMonitor.left, at.right = mi.rcMonitor.right, at.bottom = mi.rcMonitor.bottom;
            else at.top = mi.rcMonitor.top, at.bottom = mi.rcMonitor.bottom, (edge == ABE_LEFT ? at.left : at.right) = edge == ABE_LEFT ? mi.rcMonitor.left : mi.rcMonitor.right;
            return true;
        };
    }
    tbSelfAt = Seconds(); // the broadcast comes back to us too
    tray->Claim();
}

HMONITOR Bar::MainScreen() {
    std::wstring want = sys::ReadText(L"DockScreen");
    for (HMONITOR m : Screens()) {
        MONITORINFOEXW mi = {};
        mi.cbSize = sizeof(mi);
        if (!want.empty() && GetMonitorInfoW(m, &mi) && want == mi.szDevice) return m;
    }
    return MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY); // none picked, or it's unplugged: windows' main screen
}

std::vector<HMONITOR> Bar::Screens() {
    std::vector<std::pair<LONG, HMONITOR>> found;
    EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT r, LPARAM lp) {
        reinterpret_cast<std::vector<std::pair<LONG, HMONITOR>>*>(lp)->push_back({r->left, m});
        return TRUE;
    }, (LPARAM)&found);
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<HMONITOR> out;
    for (auto& f : found) out.push_back(f.second);
    return out;
}

void Bar::Place() {
    MONITORINFO probe = {sizeof(probe)};
    mon = moonScreen && GetMonitorInfoW(moonScreen, &probe) ? moonScreen : MainScreen();
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    monitor = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    dpi = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    float wasScale = scale;
    scale = dpi * Zoom(); // the size setting is just a bigger dip: everything, hit tests included, follows
    if (wasScale != scale) anchorX *= wasScale / scale, cardX *= wasScale / scale; // an open panel stays over the same spot on screen
    DWM_TIMING_INFO ti = {sizeof(ti)};
    if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator > 0) {
        constexpr double kAmbientFps = 15; // the song's little bars and other slow motion: lively at that size, half the cost of 30
        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
        ambientSync = (UINT)(std::clamp)((int)std::lround(hz / kAmbientFps), 1, 4);
    }
    winPxW = monitor.right - monitor.left;
    winPxH = monitor.bottom - monitor.top - 1; // all but the top row: a window the size of the screen reads as a fullscreen app to the shell and to astraeus, which then hides; the top edge is astraeus' anyway
    barPx = TopOn() ? (int)std::lround(kBarH * scale) : 0; // the top bar is retired: nothing reserved at the top
    W = winPxW / scale;
    winH = winPxH / scale;
    SetWindowPos(hwnd, HWND_TOPMOST, monitor.left, monitor.bottom - winPxH, winPxW, winPxH, SWP_NOACTIVATE);
    BelowIsland();
    pill = {};
    drawnTop = drawnBottom = {};
    rowW = 0; // re-fit at the new size
    regionNow = {RECT{-1, -1, -1, -1}}; // never what SyncRegion wants: its first call always sets the region, even an empty one, so the screen-sized window never takes clicks before the first frame
    SyncRegion(); // right away: the window must never swallow clicks, even before the first frame
    if (AutoHide()) Unreserve(); // its strip is gone; a kept one moves in place below, so the work area goes straight to its new edge or size
    Reserve();
    int needW = winPxW, needH = winPxH;
    if (ctx && (needW != surfPxW || needH != surfPxH)) FreeDev(); // a new screen: the surface is sized for the old one
    else if (ctx) TrimSurface(nullptr, 0), fullNext = allNext = true; // only a new size: the same surface, its old pixels dropped and drawn again
    surfPxW = needW, surfPxH = needH;
    Redraw();
}

void Bar::Reserve() {
    if (tray) tray->StepBack(); // appbar requests find explorer's own tray for these few calls: through ours their answers are lost
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = hwnd;
    if (!appbar) {
        abd.uCallbackMessage = WM_APP_APPBAR;
        appbar = SHAppBarMessage(ABM_NEW, &abd) != FALSE;
    }
    Log("reserve");
    if (appbar && barPx > 0) { // no strip with auto-hide: still registered, so fullscreen apps are still noticed
        reserving = true;
        abd.uEdge = ABE_TOP;
        abd.rc = monitor;
        abd.rc.bottom = monitor.top + barPx;
        SHAppBarMessage(ABM_QUERYPOS, &abd); // another top bar may push us down
        abd.rc.bottom = abd.rc.top + barPx;
        SHAppBarMessage(ABM_SETPOS, &abd); // explorer keeps the work area from here on, as it does for its own taskbar
        reserving = false;
    }
    if (shelf && !AutoHide()) { // the dock stays: its strip at the bottom is kept free too, as windows' own taskbar's
        APPBARDATA sb = {sizeof(sb)};
        sb.hWnd = shelf;
        if (!shelfBar) shelfBar = SHAppBarMessage(ABM_NEW, &sb) != FALSE;
        LONG px = (LONG)std::lround(kShelfH * scale);
        sb.uEdge = cfg.edge == 1 ? ABE_LEFT : cfg.edge == 2 ? ABE_RIGHT : ABE_BOTTOM; // the strip is kept free on the dock's own edge
        sb.rc = monitor;
        if (sb.uEdge == ABE_LEFT) sb.rc.right = monitor.left + px;
        else if (sb.uEdge == ABE_RIGHT) sb.rc.left = monitor.right - px;
        else sb.rc.top = monitor.bottom - px;
        SHAppBarMessage(ABM_QUERYPOS, &sb);
        if (sb.uEdge == ABE_LEFT) sb.rc.right = sb.rc.left + px;
        else if (sb.uEdge == ABE_RIGHT) sb.rc.left = sb.rc.right - px;
        else sb.rc.top = sb.rc.bottom - px;
        if (shelfBar) SHAppBarMessage(ABM_SETPOS, &sb);
    }
    if (tray) tray->Reassert();
}


void Bar::Unreserve() {
    if (shelfBar) { // the dock's strip, if it held one
        APPBARDATA sb = {sizeof(sb)};
        sb.hWnd = shelf;
        SHAppBarMessage(ABM_REMOVE, &sb);
        shelfBar = false;
    }
    if (!appbar) return;
    if (tray) tray->StepBack();
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd); // or the shell keeps the strip reserved
    if (tray) tray->Reassert();
    appbar = false;
}


void Bar::BelowIsland() { // astraeus hangs over the top bar's middle, its hover strip on the very edge; both are topmost, so the bar steps just under it
    HWND isl = FindWindowW(L"AstraeusDrawer", nullptr);
    if (!isl || !IsWindowVisible(isl) || !(GetWindowLongW(isl, GWL_EXSTYLE) & WS_EX_TOPMOST)) return; // under a window that isn't topmost, the bar would fall out of the topmost band
    for (HWND w = GetWindow(isl, GW_HWNDNEXT); w; w = GetWindow(w, GW_HWNDNEXT)) // already somewhere under it: nothing to do, no z-order churn
        if (w == hwnd) return;
    lowering = true; // our own move: WM_WINDOWPOSCHANGED doesn't answer it
    SetWindowPos(hwnd, isl, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOOWNERZORDER);
    lowering = false;
}

void Bar::SyncKeyHook() {
    bool want = !moon && took && cfg.winKeys;
    if (want == (keyHook != nullptr)) return;
    if (want) keyHook = SetWindowsHookExW(WH_KEYBOARD_LL, OnKey, GetModuleHandleW(nullptr), 0), keyBar = this;
    else UnhookWindowsHookEx(keyHook), keyHook = nullptr, keyBar = nullptr;
}

LRESULT CALLBACK Bar::OnKey(int code, WPARAM wp, LPARAM lp) { // win + 1..9: taken before explorer's own hotkey, the app's spot in our dock instead
    static bool win = false;
    if (code == HC_ACTION && keyBar) {
        const auto* k = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        bool down = wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN;
        bool plain = !(k->flags & LLKHF_INJECTED) && GetAsyncKeyState(VK_CONTROL) >= 0 && GetAsyncKeyState(VK_MENU) >= 0;
        bool shift = GetAsyncKeyState(VK_SHIFT) < 0;
        WPARAM what = k->vkCode >= '1' && k->vkCode <= '9' && !shift ? k->vkCode - '1' : k->vkCode == 'T' ? (shift ? kKeyBack : kKeyNext) : k->vkCode == 'B' && !shift ? kKeyTray : (WPARAM)-1;
        if (k->vkCode == VK_LWIN || k->vkCode == VK_RWIN) win = down;
        else if (win && plain && what != (WPARAM)-1) { // win + t and win + b too: windows' would put the keyboard on its hidden taskbar
            if (down) {
                PostMessageW(keyBar->hwnd, WM_APP_WINKEY, what, 0); // acted on outside the hook: hooks must answer fast
                INPUT mask[2] = {}; // an unassigned key while win is still down: its release then opens no start menu
                mask[0].type = mask[1].type = INPUT_KEYBOARD;
                mask[0].ki.wVk = mask[1].ki.wVk = kMaskKey;
                mask[1].ki.dwFlags = KEYEVENTF_KEYUP;
                SendInput(2, mask, sizeof(INPUT)); // our own input last: windows lets us take the keyboard for win + t
            }
            return 1; // swallowed: explorer's hidden taskbar never hears it
        }
    }
    return CallNextHookEx(nullptr, code, wp, lp);
}

void Bar::HaloIcon(bool show) {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = hwnd;
    nid.uID = 1;
    if (!show) {
        Shell_NotifyIconW(NIM_DELETE, &nid);
        return;
    }
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    nid.uCallbackMessage = WM_APP_HALOICON;
    int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem());
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
    wcscpy_s(nid.szTip, L"HaloBar");
    if (!Shell_NotifyIconW(NIM_ADD, &nid)) Shell_NotifyIconW(NIM_MODIFY, &nid); // add first: our own tray host takes a modify as an add, so explorer's flyout would only hear a modify it rejects
    if (nid.hIcon) DestroyIcon(nid.hIcon);
}

void Bar::KeepOnTop() {
    if (full || !IsWindowVisible(hwnd)) return; // behind a game on purpose
    for (HWND w = GetWindow(hwnd, GW_HWNDPREV); w; w = GetWindow(w, GW_HWNDPREV)) {
        if (!IsWindowVisible(w) || (GetWindowLongW(w, GWL_EXSTYLE) & WS_EX_TOPMOST)) continue;
        BOOL cloaked = FALSE;
        DwmGetWindowAttribute(w, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
        if (cloaked) continue; // a suspended store app's frame: there, but never drawn or hit
        lowering = true; // our own move
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // a plain window over us: the edge strip under it would never see the mouse
        lowering = false;
        BelowIsland();
        return;
    }
}

void Bar::SetFull(bool on) {
    if (on == full) return;
    full = on;
    ShowWindow(hwnd, on ? SW_HIDE : SW_SHOWNOACTIVATE); // games and videos get the whole screen
    bool away = AllAway();
    SetThreadPriority(GetCurrentThread(), away ? THREAD_PRIORITY_BELOW_NORMAL : THREAD_PRIORITY_ABOVE_NORMAL); // one thread for every screen: a game's threads come first only once no dock is in use
    if (tasks) tasks->Pause(away); // the list is shared: a dock on a screen without the game still needs every window event
    if (on && moveHook) UnhookWinEvent(moveHook), moveHook = nullptr; // the game's own cursor and moves: nothing to follow
    if (on) needsFrame = false, CancelWaitableTimer(frameWait), KillTimer(hwnd, kIdleTimer), SetTimer(hwnd, kTrimTimer, kIdleMs, nullptr); // nothing drawn while away: memory back soon
    if (on) SetCoalescableTimer(hwnd, kFullTimer, kFullMs, nullptr, 1000); // the shell's "it's over" can go missing: never stay hidden on its word alone; a window event looks sooner
    else KillTimer(hwnd, kFullTimer);
    if (!on) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        BelowIsland();
        PollStatus(); // the status skipped its ticks while away
        OnCoverEvent(nullptr, EVENT_SYSTEM_FOREGROUND, GetForegroundWindow(), OBJID_WINDOW, CHILDID_SELF, 0, 0); // follows the app in front again
        Redraw();
    }
}

bool Bar::AllAway() {
    for (Bar* b : Docks())
        if (!b->full) return false;
    return true;
}

bool Bar::FullInFront() const {
    HWND f = GetForegroundWindow();
    if (!f || f == hwnd || IsIconic(f) || !IsWindowVisible(f)) return false;
    wchar_t cls[32] = {};
    GetClassNameW(f, cls, 32);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, shell)) return false; // the desktop itself is screen-sized too
    if (!wcsncmp(cls, L"Astraeus", 8) || !wcsncmp(cls, L"HaloBar", 7)) return false; // the island, or one of our own
    LONG style = GetWindowLongW(f, GWL_STYLE), ex = GetWindowLongW(f, GWL_EXSTYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (IsZoomed(f) && (style & WS_THICKFRAME))) return false; // a maximized app, however it draws its title bar: fullscreen drops the sizing frame
    if ((ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) || ((ex & WS_EX_LAYERED) && (ex & WS_EX_TRANSPARENT))) return false; // a game overlay, a recorder's frame, a wallpaper: screen sized, never a fullscreen app
    BOOL cloaked = FALSE;
    if (SUCCEEDED(DwmGetWindowAttribute(f, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
    RECT r{};
    GetWindowRect(f, &r);
    return r.left <= monitor.left && r.top <= monitor.top && r.right >= monitor.right && r.bottom >= monitor.bottom;
}

void Bar::SyncRegion() { // the window takes clicks only on the toolbar, the dock and an open dropdown: everywhere else falls through to the desktop
    auto px = [&](const D2D1_RECT_F& r) {
        return r.right > r.left ? RECT{(LONG)std::floor(r.left * scale) - 1, (LONG)std::floor(r.top * scale) - 1, (LONG)std::ceil(r.right * scale) + 1, (LONG)std::ceil(r.bottom * scale) + 1} : RECT{};
    };
    std::vector<RECT> want;
    if (TopOn() && topSink > 0.5f && !topPeek) want.push_back({0, 0, winPxW - (LONG)std::lround(140 * scale), 2}); // slipped up: the top edge calls it back, all but the corner where windows keep their close button
    else if (TopOn()) {
        if (capL.right > capL.left) want.push_back(px(capL));
        if (capR.right > capR.left) want.push_back(px(capR));
    }
    bool tucked = sink > 0.5f && !peek;
    if (tucked) want.push_back(cfg.edge == 1 ? RECT{0, 0, 2, winPxH} : cfg.edge == 2 ? RECT{winPxW - 2, 0, winPxW, winPxH} : RECT{0, winPxH - 2, winPxW, winPxH}); // dock tucked away: only its screen edge, to call it back
    else if (wegR.right > wegR.left) want.push_back(px(AutoHide() ? Union(wegR, wegHit) : wegR)); // auto-hide: down to the screen's edge too, so a mouse resting there keeps it up
    if (D2D1_RECT_F c = CardOnScreen(); c.right > c.left) want.push_back(px(c));
    if (!tucked && tipNow.right > tipNow.left) want.push_back(px(tipNow)); // a hovered item's name pill, shown only while the mouse is on the dock
    bool same = want.size() == regionNow.size();
    for (size_t i = 0; same && i < want.size(); ++i) same = EqualRect(&want[i], &regionNow[i]) != 0;
    if (same) return; // resting: set once, not every frame
    if (tucked) GetCursorPos(&parkedAt); // windows answers the new strip with a move at the cursor's resting place: not the user calling the dock
    HRGN rg = CreateRectRgn(0, 0, 0, 0);
    for (const RECT& r : want)
        if (HRGN part = CreateRectRgnIndirect(&r)) CombineRgn(rg, rg, part, RGN_OR), DeleteObject(part);
    if (rg && SetWindowRgn(hwnd, rg, FALSE)) regionNow = want; // the system owns rg now
    else if (rg) DeleteObject(rg);
}

D2D1_POINT_2F Bar::ToDip(LPARAM lp) const { return {GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale}; }

POINT Bar::ToScreen(D2D1_POINT_2F p) const {
    return {monitor.left + (LONG)std::lround(p.x * scale), monitor.bottom - winPxH + (LONG)std::lround(p.y * scale)};
}

void Bar::TrackLeave() {
    if (tracking) return;
    TRACKMOUSEEVENT t = {sizeof(t), TME_LEAVE, hwnd, 0};
    tracking = TrackMouseEvent(&t) != FALSE;
}

void Bar::ArmClock() {
    SYSTEMTIME st;
    GetLocalTime(&st);
    UINT ms = (60 - st.wSecond) * 1000 - st.wMilliseconds + 20; // just past the next minute
    SetCoalescableTimer(hwnd, kClockTimer, ms, nullptr, 100); // windows may batch it with other wakeups
}

void Bar::StartMenu() { // the windows start menu, until the bar has its own launcher
    INPUT in[2] = {};
    in[0].type = in[1].type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[1].ki.wVk = VK_LWIN;
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, in, sizeof(INPUT));
}

void Bar::ShowPreview() {
    const TaskApp* a = FindApp(hoverApp);
    if (!a || a->wins.empty()) {
        card.Hide();
        return;
    }
    std::vector<Preview::Win> wins;
    for (const auto& w : a->wins) wins.push_back({w.hwnd, w.title});
    int med = 0; // is this the app playing music: its card gets the transport
    if (hasTrack && !track.app.empty()) {
        std::wstring ta = track.app, an = a->name;
        for (auto& ch : ta) ch = (wchar_t)towlower(ch);
        for (auto& ch : an) ch = (wchar_t)towlower(ch);
        if (!an.empty() && (ta.find(an) != std::wstring::npos || an.find(ta) != std::wstring::npos))
            med = Preview::kHasMedia | (track.playing ? Preview::kPlaying : 0) | (track.canPrev ? Preview::kCanPrev : 0) | (track.canNext ? Preview::kCanNext : 0);
    }
    if (Vert()) card.Show(hwnd, a->key, a->name, wins, ToScreen({cfg.edge == 1 ? wegR.right : wegR.left, (hoverSlot.top + hoverSlot.bottom) / 2}), scale, shellNow, med, cfg.edge); // beside the column
    else card.Show(hwnd, a->key, a->name, wins, ToScreen({(hoverSlot.left + hoverSlot.right) / 2, wegR.top}), scale, shellNow, med); // over the dock, centered on the app
}

void Bar::HookCover(bool on) { // one set of hooks for every dock: a second screen doesn't double what each window event costs
    covered = false;
    if (on && !coverHooks[0]) {
        constexpr DWORD kFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
        coverHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnCoverEvent, 0, 0, kFlags);
        coverHooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnCoverEvent, 0, 0, kFlags);
        coverHooks[2] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnCoverEvent, 0, 0, kFlags);
    }
    if (on) {
        OnCoverEvent(nullptr, EVENT_SYSTEM_FOREGROUND, GetForegroundWindow(), OBJID_WINDOW, CHILDID_SELF, 0, 0);
        return;
    }
    if (std::any_of(coverBars.begin(), coverBars.end(), [this](Bar* b) { return b != this; })) return; // another dock still listens
    for (auto& h : coverHooks)
        if (h) UnhookWinEvent(h), h = nullptr;
    if (moveHook) UnhookWinEvent(moveHook), moveHook = nullptr;
}

void CALLBACK Bar::OnCoverEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    Census(0xF0004, ev);
    static DWORD movePid = 0; // the process the location hook follows
    if (coverBars.empty() || obj != OBJID_WINDOW || child != CHILDID_SELF) return; // carets, the cursor, scrollbars and the like move all the time: not windows
    if (ev == EVENT_SYSTEM_FOREGROUND && h) { // only the app in front can be dragged or maximized over us: follow its moves alone, not the whole desktop's
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (pid == GetCurrentProcessId()) pid = 0;
        bool away = std::any_of(coverBars.begin(), coverBars.end(), [](Bar* b) { return b->full; });
        if (away) pid = 0; // a fullscreen game in front: its moves are never followed
        if (pid != movePid || (!moveHook && pid)) { // another window of the same app: the hook already follows it
            if (moveHook) UnhookWinEvent(moveHook), moveHook = nullptr;
            movePid = pid;
            if (pid) moveHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnCoverEvent, pid, 0, WINEVENT_OUTOFCONTEXT);
        }
    }
    if (ev == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(h, GA_ROOT) != h) return;
    for (Bar* b : coverBars)
        if (b->hwnd) SetTimer(b->hwnd, kCoverTimer, kCoverMs, nullptr); // a burst of moves, one check per dock
}

void Bar::CheckCovered() {
    KeepOnTop(); // the app that just came to the front may have been lifted over us
    bool was = covered, wasTop = topCovered;
    covered = topCovered = false;
    if (TopAuto() && tasks) {
        RECT strip = {monitor.left, monitor.top, monitor.right, monitor.top + (LONG)std::lround(kBarH * scale)}; // where the top bar rests
        for (const auto& a : tasks->Apps())
            for (const auto& w : a.wins) {
                if (topCovered || !IsWindowVisible(w.hwnd) || IsIconic(w.hwnd)) continue;
                RECT wr{}, x{};
                if (FAILED(DwmGetWindowAttribute(w.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(w.hwnd, &wr);
                topCovered = IntersectRect(&x, &wr, &strip) != FALSE;
            }
    }
    if (topCovered != wasTop) Redraw();
    if (AutoHide() && tasks) {
        RECT spot = {monitor.left + (LONG)std::floor(wegR.left * scale), monitor.bottom - (LONG)std::lround((kWegH + kWegMargin) * scale), monitor.left + (LONG)std::ceil(wegR.right * scale),
                     monitor.bottom}; // where the dock rests
        if (Vert()) { // at a side: the column's band along that edge
            LONG across = (LONG)std::lround((kWegH + kWegMargin) * scale);
            spot = {cfg.edge == 1 ? monitor.left : monitor.right - across, ToScreen({0, wegR.top}).y, cfg.edge == 1 ? monitor.left + across : monitor.right, ToScreen({0, wegR.bottom}).y};
        }
        for (const auto& a : tasks->Apps())
            for (const auto& w : a.wins) {
                if (covered || !IsWindowVisible(w.hwnd) || IsIconic(w.hwnd)) continue;
                RECT wr{}, x{};
                if (FAILED(DwmGetWindowAttribute(w.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(w.hwnd, &wr); // without the invisible resize border
                covered = IntersectRect(&x, &wr, &spot) != FALSE;
            }
    }
    bool wasMax = maxHere;
    maxHere = false;
    if (cfg.adaptive && tasks) // the centered dock runs the screen's width while a maximized window is up on its screen
        for (const auto& a : tasks->Apps())
            for (const auto& w : a.wins)
                if (!maxHere && IsWindowVisible(w.hwnd) && IsZoomed(w.hwnd) && MonitorFromWindow(w.hwnd, MONITOR_DEFAULTTONULL) == mon) maxHere = true;
    if (covered != was || maxHere != wasMax) Redraw();
}

void Bar::OnNotifWrite() {
    FindNextChangeNotification(notifWatch); // keep watching
    SetTimer(hwnd, kNotifReadTimer, 150, nullptr); // a toast writes in a burst: read once it settles
}

namespace {
constexpr wchar_t kPushKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
constexpr wchar_t kAppsKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Notifications\\Settings"; // each app's own notification switches
constexpr wchar_t kBannersWas[] = L"Software\\HaloBar\\BannersWas"; // each app's banner switch before we hushed it, kept across a crash
constexpr DWORD kNoValue = 0xFFFFFFFF; // the app had no banner switch of its own

void ToastsOn() { // older builds hushed with windows' master switch, which stops windows keeping notifications at all: never again
    if (prefs::ReadNum(L"ToastWas", -1) < 0) return;
    DWORD on = 1;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", REG_DWORD, &on, sizeof(on));
    prefs::Delete(L"ToastWas");
}
}

void RestoreBanners() {
    ToastsOn();
    HKEY saved = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kBannersWas, 0, KEY_READ, &saved) != ERROR_SUCCESS) return; // we never touched them
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = 512, type = 0, v = 0, size = sizeof(v);
        if (RegEnumValueW(saved, i, name, &len, nullptr, &type, reinterpret_cast<BYTE*>(&v), &size) != ERROR_SUCCESS) break;
        std::wstring app = std::wstring(kAppsKey) + L"\\" + name;
        if (v == kNoValue) RegDeleteKeyValueW(HKEY_CURRENT_USER, app.c_str(), L"ShowBanner");
        else RegSetKeyValueW(HKEY_CURRENT_USER, app.c_str(), L"ShowBanner", REG_DWORD, &v, sizeof(v));
    }
    RegCloseKey(saved);
    RegDeleteKeyW(HKEY_CURRENT_USER, kBannersWas);
}

void Bar::Banners(bool hush) { // windows still keeps every notification, for its list and ours; only the banner each app pops is turned off
    if (!hush) {
        RestoreBanners();
        return;
    }
    ToastsOn();
    HKEY apps = nullptr, saved = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kAppsKey, 0, KEY_READ, &apps) != ERROR_SUCCESS) return;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kBannersWas, 0, nullptr, 0, KEY_READ | KEY_WRITE, nullptr, &saved, nullptr) != ERROR_SUCCESS) {
        RegCloseKey(apps);
        return;
    }
    wchar_t name[512];
    for (DWORD i = 0;; ++i) {
        DWORD len = 512;
        if (RegEnumKeyExW(apps, i, name, &len, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
        DWORD was = kNoValue, size = sizeof(was), off = 0;
        if (RegGetValueW(saved, nullptr, name, RRF_RT_REG_DWORD, nullptr, &was, &size) != ERROR_SUCCESS) { // not yet ours: remember the app's own choice first
            was = kNoValue, size = sizeof(was);
            RegGetValueW(apps, name, L"ShowBanner", RRF_RT_REG_DWORD, nullptr, &was, &size);
            RegSetValueExW(saved, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&was), sizeof(was));
        }
        RegSetKeyValueW(apps, name, L"ShowBanner", REG_DWORD, &off, sizeof(off));
    }
    RegCloseKey(saved);
    RegCloseKey(apps);
}

bool Bar::Hush() const { return !moon && (cfg.pills || Quiet()); } // our pills show them, or quiet with windows' own banners

bool Bar::Quiet() const { return prefs::ReadShared(L"Quiet", prefs::ReadNum(L"Quiet", 0)) != 0; } // shared with astraeus: quiet in one is quiet in both; the old setting until either app first writes it

void Bar::SetQuiet(bool on) {
    prefs::SaveShared(L"Quiet", on ? 1 : 0); // astraeus hears it and goes quiet too
    if (!moon) Banners(cfg.pills || on); // windows' own banners show: quiet them as well
    dnd = on;
    if (on) pillUp = false;
}

void Bar::ShowDesktop() { // explorer's own show desktop, as its corner button: toggles
    if (HWND explorer = taskbar::ExplorerTray()) { // our tray host shares its class: win+d and the shell's lookups can land on ours
        PostMessageW(explorer, WM_COMMAND, kTrayShowDesktop, 0);
        return;
    }
    INPUT in[4] = {}; // no explorer taskbar at all: win+d
    for (auto& i : in) i.type = INPUT_KEYBOARD;
    in[0].ki.wVk = in[3].ki.wVk = VK_LWIN;
    in[1].ki.wVk = in[2].ki.wVk = 'D';
    in[2].ki.dwFlags = in[3].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(4, in, sizeof(INPUT));
}

void Bar::ShowMenu() { // windows' power user menu, as right clicking its start button opens, with halobar's own items under it
    struct Entry {
        const wchar_t* label;
        const wchar_t* file; // what it opens: a settings page, a console, a program
        const wchar_t* args;
        bool admin;
        int glyph;
        const wchar_t* hint; // windows' own shortcut for it, where there is one
    };
    static const Entry kEntries[] = {
        {L"Installed apps", L"ms-settings:appsfeatures", nullptr, false, glyph::Apps, nullptr},
        {L"Mobility Center", L"mblctr.exe", nullptr, false, glyph::Battery, nullptr}, // laptops only: skipped below without a battery
        {L"Power Options", L"ms-settings:powersleep", nullptr, false, glyph::Bolt, nullptr},
        {L"Event Viewer", L"eventvwr.msc", nullptr, false, glyph::Doc, nullptr},
        {L"System", L"ms-settings:about", nullptr, false, glyph::Pc, nullptr},
        {L"Device Manager", L"devmgmt.msc", nullptr, false, glyph::Devices, nullptr},
        {L"Network Connections", L"ms-settings:network", nullptr, false, glyph::Lan, nullptr},
        {L"Disk Management", L"diskmgmt.msc", nullptr, true, glyph::SysDrive, nullptr},
        {L"Computer Management", L"compmgmt.msc", nullptr, false, glyph::Tune, nullptr},
        {L"Terminal", L"wt.exe", nullptr, false, glyph::Terminal, nullptr},
        {L"Terminal (Admin)", L"wt.exe", nullptr, true, glyph::Admin, nullptr},
        {nullptr, nullptr, nullptr, false, 0, nullptr},
        {L"Task Manager", L"taskmgr.exe", nullptr, false, glyph::SysChart, L"Ctrl+Shift+Esc"},
        {L"Settings", L"ms-settings:", nullptr, false, glyph::Settings, L"Win+I"},
        {L"File Explorer", L"explorer.exe", nullptr, false, glyph::Folder, L"Win+E"},
        {L"Search", nullptr, nullptr, false, glyph::Search, L"Win+S"}, // keys, below
        {L"Run", nullptr, nullptr, false, glyph::OpenNew, L"Win+R"},
    };
    constexpr UINT kBase = 100, kOut = 200, kSleep = 201, kHibernate = 202, kOff = 203, kReboot = 204, kDesk = 205;
    std::vector<popmenu::Item> items;
    for (UINT i = 0; i < ARRAYSIZE(kEntries); ++i) {
        const Entry& e = kEntries[i];
        if (!e.label) {
            items.push_back(popmenu::Sep());
            continue;
        }
        if (i == 1 && !status.battery) continue; // mobility center exists only on laptops
        items.push_back({.id = kBase + i, .glyph = e.glyph, .label = e.label, .hint = e.hint ? e.hint : L""});
    }
    popmenu::Item power{.glyph = glyph::Power, .label = L"Shut down or sign out"};
    power.sub.push_back({.id = kOut, .glyph = glyph::SignOut, .label = L"Sign out"});
    power.sub.push_back({.id = kSleep, .glyph = glyph::Sleep, .label = L"Sleep"});
    SYSTEM_POWER_CAPABILITIES caps{};
    if (CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps, sizeof(caps)) == 0 && caps.HiberFilePresent) power.sub.push_back({.id = kHibernate, .glyph = glyph::Snow, .label = L"Hibernate"});
    power.sub.push_back({.id = kOff, .glyph = glyph::Power, .label = L"Shut down", .danger = true});
    power.sub.push_back({.id = kReboot, .glyph = glyph::Restart, .label = L"Restart", .danger = true});
    items.push_back(std::move(power));
    items.push_back({.id = kDesk, .glyph = glyph::Desktop, .label = L"Desktop", .hint = L"Win+D"});
    items.push_back(popmenu::Sep());
    std::wstring head = L"HaloBar " APP_VER_W + (updateNote.empty() ? L"" : L"  ·  " + updateNote);
    popmenu::Item own{.glyph = glyph::Sparkle, .label = L"HaloBar"};
    own.sub.push_back(popmenu::Section(head)); // what was the native menu's greyed first line
    own.sub.push_back({.id = kMenuStart, .glyph = glyph::Home, .label = L"Windows Start"});
    own.sub.push_back({.id = kMenuSettings, .glyph = glyph::Settings, .label = L"HaloBar settings"});
    own.sub.push_back({.id = kMenuEdit, .glyph = glyph::Edit, .label = L"Customize dock"});
    own.sub.push_back({.id = kMenuUpdate, .glyph = glyph::Refresh, .label = L"Check for updates"});
    own.sub.push_back(popmenu::Sep());
    own.sub.push_back({.id = kMenuRestart, .glyph = glyph::Restart, .label = L"Restart HaloBar"});
    own.sub.push_back({.id = kMenuExit, .glyph = glyph::Close, .label = L"Exit HaloBar", .danger = true});
    items.push_back(std::move(own));
    POINT p;
    GetCursorPos(&p);
    popmenu::Head top{.title = L"HaloBar", .sub = L"v" APP_VER_W + (updateNote.empty() ? L"" : L"  ·  " + updateNote), .logo = true};
    UINT pick = popmenu::Pick(hwnd, items, {.at = p, .h = popmenu::Place::Left, .v = popmenu::Place::Auto}, shellNow, scale, &top); // above the cursor in the screen's lower half, as before
    HWND back = tasks ? tasks->Fore() : nullptr; // the bar never keeps the keyboard: give it back to the app that had it
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick >= kBase && pick < kBase + ARRAYSIZE(kEntries)) {
        const Entry& e = kEntries[pick - kBase];
        if (!wcscmp(e.label, L"Search")) Keys({VK_LWIN, 'S'});
        else if (!wcscmp(e.label, L"Run")) Keys({VK_LWIN, 'R'});
        else if (!wcscmp(e.label, L"Terminal") || !wcscmp(e.label, L"Terminal (Admin)")) { // windows terminal, or powershell where it isn't installed
            if ((INT_PTR)ShellExecuteW(nullptr, e.admin ? L"runas" : L"open", L"wt.exe", nullptr, nullptr, SW_SHOWNORMAL) <= 32)
                ShellExecuteW(nullptr, e.admin ? L"runas" : L"open", L"powershell.exe", nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            ShellExecuteW(nullptr, e.admin ? L"runas" : L"open", e.file, e.args, nullptr, SW_SHOWNORMAL);
        }
    } else if (pick == kOut) sys::power::SignOut();
    else if (pick == kSleep) sys::power::Sleep();
    else if (pick == kHibernate) SetSuspendState(TRUE, FALSE, FALSE);
    else if (pick == kOff) sys::power::Shutdown();
    else if (pick == kReboot) sys::power::Restart();
    else if (pick == kDesk) ShowDesktop();
    else if (pick == kMenuStart) StartMenu();
    else if (pick == kMenuSettings) Anchor(W / 2, TopOn()), SetSettingsOpen(true);
    else if (pick == kMenuEdit) StartEdit();
    else if (pick == kMenuUpdate) updateNote = L"checking…", updater.Check(hwnd, WM_APP_UPDATE, true);
    else if (pick == kMenuRestart) PostQuitMessage(2); // main relaunches on 2
    else if (pick == kMenuExit) PostQuitMessage(0);
}

void Bar::OnUpdate(WPARAM result) {
    static const char* const kNames[] = {"update: installed", "update: up to date", "update: check failed", "update: downloading"};
    Log(result < 4 ? kNames[result] : "update: ?");
    rest::Note(result, updater.Applied()); // the tray dashboard's update row, if it's open
    switch (result) {
    case sys::Updater::kInstalled:
        updateReady = true;
        updateNote = updater.Applied() + L" ready";
        if (!AnyOpen()) PostQuitMessage(2); // nothing in use: the new one starts in its place right away
        break;
    case sys::Updater::kDownloading: updateNote = L"downloading " + updater.Applied(); break;
    case sys::Updater::kCurrent: updateNote = L"up to date"; break;
    default: updateNote = L"couldn't check"; break;
    }
}

void Bar::SetPanel(Panel p) {
    bool quickWas = qsOpen, trayWas = trayOpen, calWas = calOpen;
    if (p != Panel::Quick) EndPass();
    if (p != Panel::None) card.Hide();
    if (p != Panel::None && !AnyOpen()) SampleBehind(); // what the island is about to grow over
    mediaOpen = p == Panel::Media;
    trayOpen = p == Panel::Tray;
    setOpen = p == Panel::Settings;
    sheetOpen = p == Panel::Sheet;
    qsOpen = p == Panel::Quick;
    calOpen = p == Panel::Calendar;
    bool launchWas = launchOpen, noteWas = wgOpen && TypingKind(wgShown);
    int shownWas = wgShown;
    launchOpen = p == Panel::Launch;
    wgOpen = p == Panel::Widget;
    wgShown = wgOpen ? wgKind : -1;
    if (!wgOpen || wgKind != kWgGallery) editing = false, dragSlot = -1; // edit mode lasts as long as its gallery
    if (shownWas == kWgNotes && wgShown != kWgNotes && noteLoaded) SaveNote(); // the note is written as it closes
    if (noteWas && !Typing()) draft.clear();
    if (shownWas == kWgClip && wgShown != kWgClip) ClipClosed();
    if (p == Panel::None && updateReady) PostQuitMessage(2); // an update waited for the panel to close
    if (trayOpen && !trayWas) SyncGhosts(); // the icons explorer kept, read fresh as the tray opens
    if (calOpen && !calWas) cals.Sync(hwnd, WM_APP_CAL, sys::cal::Calendars::kOnOpen), calPick = 0; // opening it shows changes made a minute ago; today picked
    if (p == Panel::Media || p == Panel::Settings) trimDue = true; // art and layers get cleared from d2d's cache once it's gone again
    ringDrag = false;
    if (qsOpen && !quickWas) radios.Recheck(); // bluetooth plugged in after the bar started shows up
    if (qsOpen && !quickWas) { // fresh numbers as it opens
        hotspot.Refresh(hwnd, WM_APP_HOTSPOT);
        dnd = Quiet();
        SyncAudio();
        PollStatus();
        bright.ReadAsync(hwnd, WM_APP_BRIGHT);
        powerRow = false;
        qsPage = 0;
    }
    if ((launchWas && !launchOpen) || (noteWas && !wgOpen)) { // the keyboard goes back to the app that had it
        HWND back = tasks ? tasks->Fore() : nullptr;
        if (GetForegroundWindow() == hwnd && back && IsWindow(back)) SetForegroundWindow(back);
    }
    if (!qsOpen && quickWas) OpenQuickPage(0); // the wi-fi and bluetooth lists let go of their scans, the mixer its sessions
    if (calOpen) { // the calendar opens on this month, with the notifications read fresh
        calYear = 0;
        notifUnread = false;
        notifScroll = 0;
        SeeNotifs(L"");
        dnd = Quiet();
        LoadNotifs();
    }
    KillTimer(hwnd, kCloseTimer);
    closeArmed = false;
    Redraw();
}

void Bar::SetSheet(const std::wstring& key) {
    if (key.empty()) {
        if (sheetOpen) SetPanel(Panel::None);
        return;
    }
    if (sheetOpen && key == sheetKey) return;
    sheetKey = key;
    if (const TaskApp* a = FindApp(key)) ReadRecent(*a); // its jump list's recent files
    SetPanel(Panel::Sheet);
}

void Bar::SetPref(int& field, const wchar_t* name, int value) {
    if (field == value) return;
    field = value;
    prefs::SaveNum(name, value);
    SyncLook(); // look and accent take effect with island's crossfade
    bool place = &field == &cfg.size || &field == &cfg.room || &field == &cfg.top;
    PostMessageW(hwnd, WM_APP_RESTYLE, place ? 1 : 0, 0); // never mid-frame: the canvas drawing this very button would be freed under it
    if (&field == &cfg.pills && !moon) Banners(Hush());
    if (&field == &cfg.song && !cfg.song && mediaOpen) SetMediaOpen(false);
    SampleShade(); // a look change: glass measures what it sits over, others drop the tint
    Redraw();
}

void Bar::SetEdge(int e) { // live in every dock, never a restart: a restart hands windows' taskbar back for a moment, and it shows
    if (e == cfg.edge || e < 0 || e > 2) return;
    prefs::SaveNum(L"Edge", e); // astraeus reads it and keeps off that side
    for (Bar* x : coverBars) x->ToEdge(e);
}

void Bar::ToEdge(int e) {
    cfg.edge = e;
    rowW = 0; // its new length at once, no stretch from the old edge's
    sink = 1; // starts tucked past its new edge and slides in from there
    if (!cardDown) anchorX = (Vert() ? winH : W) / 2, cardX = 0; // an open panel re-hangs centered over or beside it, no glide across axes
    PostMessageW(hwnd, WM_APP_RESTYLE, 1, 0); // the strip moves after this frame: never mid-frame
    Redraw();
}

bool Bar::EnsureDev() {
    if (ctx) return true;
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_SINGLETHREADED | // bgra for d2d interop
                 D3D11_CREATE_DEVICE_PREVENT_INTERNAL_THREADING_OPTIMIZATIONS; // fewer driver threads, less idle memory
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr);
    if (FAILED(hr)) hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, nullptr, 0, D3D11_SDK_VERSION, &d3d, nullptr, nullptr); // any pc, even without a gpu driver
    if (FAILED(hr)) return false;
    ComPtr<IDXGIDevice> dxgi;
    ComPtr<ID2D1Device> device;
    if (FAILED(d3d.As(&dxgi)) || FAILED(gfx::D2D()->CreateDevice(dxgi.Get(), &device)) ||
        (device->SetMaximumTextureMemory(16ull << 20), false) || // d2d's own cache stays small however long a session runs
        FAILED(device->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &ctx))) {
        FreeDev();
        return false;
    }
    if (LookNow() == 2 && !OpenGlass()) { // no backdrop blur here: dark glass from now on, on the plain path
        glassFailed = true;
        SyncLook();
    }
    if (!glass && (FAILED(DCompositionCreateDevice(dxgi.Get(), IID_PPV_ARGS(&dcomp))) || FAILED(dcomp->CreateTargetForHwnd(hwnd, TRUE, &target)) ||
        FAILED(dcomp->CreateVisual(&visual)) ||
        FAILED(dcomp->CreateVirtualSurface((UINT)surfPxW, (UINT)surfPxH, DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_ALPHA_MODE_PREMULTIPLIED, &surface)) ||
        FAILED(visual->SetContent(surface.Get())) || FAILED(target->SetRoot(visual.Get())) || FAILED(dcomp->Commit()))) {
        FreeDev();
        return false;
    }
    ctx->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE); // cleartype breaks on a transparent surface
    canvas = std::make_unique<Canvas>(ctx.Get());
    drawnTop = drawnBottom = {}; // a new surface holds nothing yet
    fullNext = true;
    return true;
}

void Bar::FreeDev() {
    FreeGlass();
    canvas.reset();
    icons.Drop();
    trayBmps.clear();
    binBmp[0].Reset(), binBmp[1].Reset();
    mixIcons.clear();
    appBmps.clear();
    for (Ghost& gh : ghosts) gh.bmp.Reset(); // bitmaps belong to the old device: remade from their pixels on the new one
    setui::FreeWheel();
    clip::History().DropPixels(); // the clipboard panel's previews and coloured text belong to the old device
    clipLays.clear(), clipInks.clear(), clipPeekLay.Reset(), clipPeekBmp.Reset(), clipPeekId = clipPeekImg = 0;
    customBmps.clear();
    haloLogo.Reset();
    for (FileRow& f : files) f.icon.Reset();
    photoBmp.Reset();
    cover.Reset();
    coverBlur.Reset();
    surface.Reset();
    visual.Reset();
    target.Reset();
    dcomp.Reset();
    if (ctx) ctx->SetTarget(nullptr);
    ctx.Reset();
    if (d3d) gfx::Settle(d3d.Get());
    d3d.Reset();
}

void Bar::Retry() {
    needsFrame = false; // no spinning: a timer brings the next attempt
    if (devTries++ < kDevTries) {
        SetTimer(hwnd, kDevTimer, 100, nullptr); // often an old composition target still being let go
        return;
    }
    if (took && !moon) taskbar::Give(), took = false; // can't draw right now (a driver update, a sleepy gpu): the user keeps a working taskbar meanwhile
    SetTimer(hwnd, kDevTimer, kDevRetryMs, nullptr); // and the dock comes back, taking it again, once a frame lands
}

void Bar::Arm(double sec) {
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(sec * 1e7); // relative, 100 ns units; zero is already due
    SetWaitableTimer(frameWait, &due, 0, nullptr, nullptr, FALSE);
}

bool Bar::Caret() {
    constexpr double kOnSec = 0.55; // of each second
    double f = std::fmod(ui.Now(), 1.0);
    bool on = f < kOnSec;
    UINT ms = (UINT)std::ceil(((on ? kOnSec : 1.0) - f) * 1000) + 1; // just past the flip
    SetTimer(hwnd, kCaretTimer, ms, nullptr); // one frame per flip, not frames all along
    return on;
}

void Bar::Redraw() {
    fullNext = allNext = true;
    if (full) return; // behind a fullscreen app nothing shows: the frame that brings the dock back draws it all
    if (eco) sys::SetEco(eco = false);
    KillTimer(hwnd, kIdleTimer);
    KillTimer(hwnd, kTrimTimer);
    needsFrame = true;
    Arm(0);
}

void Bar::Idle() {
    if ((needsFrame && !barsOnly) || AnyOpen() || Grown() > 0.5f) return;
    spectrum.reset(); // the player is gone, so is the capture
    launchLabels.clear(); // laid out again while the launcher closed: nothing shows them now
    if (trimDue && ctx) { // the player left blurred art and its layers in d2d's cache
        trimDue = false;
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        if (dev) dev->ClearResources(0);
        gfx::Settle(d3d.Get());
    }
    if (!eco && !barsOnly) sys::SetEco(eco = true); // the bars keep their beat at full priority
    if (!barsOnly) SetCoalescableTimer(hwnd, kTrimTimer, kTrimMs, nullptr, 5000); // still for a good while: then memory goes back
}

void Bar::Trim() { // long idle, or a game in front: caches and pages back to windows
    if (AnyOpen()) return;
    if (!launchOpen) appBmps.clear(), appIcons.clear(), appIcons.shrink_to_fit(), appIconsDone = false; // the launcher reads them again as it opens
    mixIcons.clear();
    if (ctx) {
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        if (dev) dev->ClearResources(0);
        gfx::Settle(d3d.Get()); // the driver's own scratch too
    }
    gfx::GiveBack();
}

int Bar::LookNow() const {
    int l = cfg.look == prefs::Settings::kFollow ? look.look : cfg.look;
    return l == 2 && glassFailed ? 0 : l; // glass without a blur to sit on is just dark
}

float Bar::Blur() const { return (cfg.look == prefs::Settings::kFollow ? look.glassBlur : cfg.blur) / 100.0f; }

Theme Bar::Make(Seed s) const { // island's materials: its own look, or the one picked here
    s.look = LookNow();
    s.bg = cfg.look == 3 ? cfg.customBg : look.customBg; // the bar's own custom look, or island's
    return Theme::From(s);
}

void Bar::SyncLook() {
    look = prefs::IslandLook();
    bool follow = cfg.accent == prefs::Settings::kFollow; // island's accent: its own color, the wallpaper's, or the bar's pick
    bool fixed = follow ? look.accentFixed : cfg.accent == 1;
    if (fixed) shellSeed = theme::FromCol(Color::Rgb((uint32_t)(follow ? look.accentHex : cfg.accentHex))), wallSeeded = false;
    else {
        if (!wallSeeded) wallKey.clear(); // back from a fixed accent: the wallpaper's seed again, even if the file didn't change
        SyncWall(); // the same file as last time is not decoded again: an island setting change costs nothing here
    }
    SetShell(Make(shellSeed));
    if (coverTint) SetMedia(Make(mediaSeed));
}

bool Bar::SyncWall() {
    if (cfg.accent == prefs::Settings::kFollow ? look.accentFixed : cfg.accent == 1) return false; // a fixed accent: the wallpaper doesn't decide
    std::wstring path = theme::WallPath();
    WIN32_FILE_ATTRIBUTE_DATA info;
    if (path.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info)) return false;
    std::wstring key = path + L"|" + std::to_wstring(info.ftLastWriteTime.dwLowDateTime); // path + mtime catches a same-name replace
    if (key == wallKey) return false;
    wallKey = key;
    ComPtr<IWICBitmapSource> src = gfx::DecodeFile(path.c_str(), 256); // a small decode is enough for a palette
    if (!src) return false;
    shellSeed = theme::FromBmp(src.Get());
    wallSeeded = true;
    return true;
}

namespace {
bool SameTheme(const Theme& a, const Theme& b) { return memcmp(&a, &b, offsetof(Theme, glass)) == 0 && a.glass == b.glass; } // colors are plain floats: no padding before glass
}

void Bar::SetShell(const Theme& t) {
    if (SameTheme(t, shellTo)) { // nothing really changed: no crossfade, no frames
        if (!coverTint) SetMedia(t);
        return;
    }
    shellFrom = shellNow;
    shellTo = t;
    shellT = 0;
    if (!coverTint) SetMedia(t); // media follows the shell unless the album tints it
}

void Bar::SetMedia(const Theme& t) {
    if (SameTheme(t, mediaTo)) return;
    mediaFrom = mediaNow;
    mediaTo = t;
    mediaT = 0;
}

bool Bar::StepTheme(float dt) {
    bool moving = false;
    if (shellT < 1) {
        shellT = (std::min)(1.0f, shellT + dt / kThemeSec);
        shellNow = shellFrom.Lerp(shellTo, anim::EaseOut(shellT));
        moving = true;
    }
    if (mediaT < 1) {
        mediaT = (std::min)(1.0f, mediaT + dt / kThemeSec);
        mediaNow = mediaFrom.Lerp(mediaTo, anim::EaseOut(mediaT));
        moving = true;
    }
    return moving;
}

const std::vector<Bar*>& Bar::Docks() { return coverBars; }

bool Bar::ScreensMoved() const { return GetSystemMetrics(SM_CMONITORS) != screensAt || (moon && !SameScreen()); }

void Bar::RefreshWx() {
    Bar* src = lead ? lead : this; // its answer reaches every dock
    src->weather.Refresh(src->hwnd, WM_APP_WX);
}

void Bar::FollowNotifs() {
    int64_t s = sys::NotifStamp();
    if (s != notifStamp && notifStamp && !calOpen) notifUnread = true;
    notifStamp = s;
    LoadNotifs();
}

bool Bar::PillsLive() const { return !lead && cfg.pills && !full && !Quiet() && !FindWindowW(L"AstraeusHost", nullptr); } // astraeus running: it shows the notification, not us

void Bar::OnPins() {
    pins::Rearm(pinsEv);
    if (!tasks) return;
    if (pins::Dirty()) pins::Assert(tasks->PinList()); // explorer wrote its older list over ours: ours stays until it reads it
    else tasks->ReloadPins(); // pinned or unpinned from somewhere else
}

void Bar::OnPrefs() {
    if (prefsEv) { // the main dock's watch: every dock follows
        prefs::Rearm(prefsEv);
        for (Bar* b : coverBars)
            if (b != this) b->OnPrefs();
    }
    islandHere = prefs::IslandInstalled(); // its settings just changed: it may have just been installed
    if (bool q = Quiet(); q != dnd) { // switched in astraeus
        dnd = q;
        if (q) pillUp = false;
        if (!moon) Banners(cfg.pills || q);
    }
    bool glassNow = glass != nullptr;
    SyncLook();
    if (ctx && (LookNow() == 2) != glassNow) FreeDev(); // island went into or out of glass, and the bar follows it
    Redraw();
}

LRESULT CALLBACK Bar::Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) { // stash this ptr from CreateWindowEx
        auto* self = (Bar*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)self);
        self->hwnd = hwnd; // messages arrive before Create returns
    }
    auto* self = (Bar*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    Census(msg, wp);
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Bar::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (tasks && tasks->ShellMsg() && msg == tasks->ShellMsg() && wp == HSHELL_GETMINRECT && lp) { // a window minimizes or comes back: it flies to its icon here, not to explorer's hidden one
        auto* info = reinterpret_cast<SHELLHOOKINFO*>(lp);
        int ai = tasks->AppOf(info->hwnd);
        auto it = ai >= 0 ? slots.find(tasks->Apps()[ai].key) : slots.end();
        if (it != slots.end()) { // tucked away: still its spot, at the screen's edge, so windows animates instead of fading
            float lift = sink * (kWegH + kWegMargin + 4);
            D2D1_POINT_2F d = !Vert() ? D2D1_POINT_2F{0, -lift} : D2D1_POINT_2F{cfg.edge == 1 ? lift : -lift, 0}; // back to where it rests
            POINT a = ToScreen({it->second.left + d.x, it->second.top + d.y}), b = ToScreen({it->second.right + d.x, it->second.bottom + d.y});
            info->rc = {a.x, a.y, b.x, b.y};
            return TRUE;
        }
    }
    if (tasks && moon && tasks->ShellMsg() && msg == tasks->ShellMsg()) return 0; // a moon hears the shell only for its minimize targets: the main dock keeps the list
    if (tasks && tasks->OnShell(msg, wp, lp)) return 0;
    if (tasks && msg >= WM_USER && msg < WM_APP && tasks->OnTaskband(msg, wp, lp)) return 0; // progress and badges, from apps' ITaskbarList3
    if (tbMsg && msg == tbMsg) { // explorer (re)started: its taskbar is back and our reserved strip is gone
        if (Seconds() - tbSelfAt < kSelfCallSec) return 0; // our own call to apps for their tray icons
        APPBARDATA abd = {sizeof(abd)};
        abd.hWnd = hwnd;
        SHAppBarMessage(ABM_REMOVE, &abd); // in case the old registration survived, so the new one takes
        appbar = false;
        Reserve();
        overflow::Forget(); // its chevron and flyout are new windows now
        if (took && !moon) {
            taskbar::Take();
            ClaimTray(); // the new explorer tray came up in front of ours
            HaloIcon(true);
            SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // its strip goes once it auto-hides: drop back to the bottom then
        }
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return passFor.empty() && !launchOpen && !Typing() ? MA_NOACTIVATE : MA_ACTIVATE; // clicks never steal focus from the user's app, except while a password is typed here
    case WM_CHAR:
        if (wgOpen && wgKind == kWgClip) { // the clipboard's search
            ClipChar((wchar_t)wp);
            return 0;
        }
        if (Typing()) { // into the note, a to-do or a habit
            WgChar((wchar_t)wp);
            return 0;
        }
        if (launchOpen) {
            if (wp != VK_RETURN && wp != VK_ESCAPE) LaunchChar((wchar_t)wp);
            return 0;
        }
        if (passFor.empty()) return 0;
        if (wp == VK_RETURN) SubmitPass();
        else if (wp == VK_ESCAPE) EndPass();
        else if (wp == VK_BACK) {
            if (!pass.empty()) pass.back() = 0, pass.pop_back();
        } else if (wp >= 32 && pass.size() < 63) pass.push_back((wchar_t)wp); // wpa keys are 8 to 63 characters
        Redraw();
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE && dockKey >= 0) dockKey = -1, keyBack = nullptr, Redraw(); // clicked away: the keyboard left the dock
        if (LOWORD(wp) == WA_INACTIVE && (launchOpen || Typing()) && !clipMenuUp) SetPanel(Panel::None); // clicked away: the launcher goes, as start does
        if (LOWORD(wp) == WA_INACTIVE && !passFor.empty()) { // clicked away mid-password: drop it rather than hold the keyboard
            if (!pass.empty()) SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t));
            pass.clear();
            passFor.clear();
            Redraw();
        }
        return 0;
    case WM_MOUSEMOVE: {
        D2D1_POINT_2F p = ToDip(lp);
        TrackLeave();
        input.mouse = p;
        POINT cur{};
        GetCursorPos(&cur);
        bool moved = cur.x != parkedAt.x || cur.y != parkedAt.y; // a cursor left resting on the edge would bounce the dock up and down forever
        if (sink > 0.5f && !peek && moved && (cfg.edge == 1 ? p.x < W / 2 : cfg.edge == 2 ? p.x > W / 2 : p.y > winH / 2)) { // tucked away and the mouse came to its edge: back out
            peek = true;
            Redraw();
        }
        if (topSink > 0.5f && !topPeek && p.y < winH / 2) { // and the top bar, from the top edge
            topPeek = true;
            Redraw();
        }
        KillTimer(hwnd, kPeekTimer);
        bool onSong = false; // the player opens on a click, never on its own
        if (onSong && !mediaOpen && !hoverArmed) {
            SetTimer(hwnd, kHoverTimer, kHoverMs, nullptr); // rest on the song and the player grows out of the island
            hoverArmed = true;
        } else if (!onSong && hoverArmed) {
            KillTimer(hwnd, kHoverTimer);
            hoverArmed = false;
        }
        if (AnyOpen() && closeArmed && OnChrome(p)) KillTimer(hwnd, kCloseTimer), closeArmed = false;
        if (ui.Captured() || ui.HitAt(p) != ui.Hovered()) Redraw(); // only when what's under the mouse changes
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        input.mouse = {-1e6f, -1e6f}; // park offscreen to clear hover
        parkedAt = {LONG_MIN, LONG_MIN}; // the cursor went: coming back to that same spot is a real call
        if (peek || topPeek) SetTimer(hwnd, kPeekTimer, kPeekMs, nullptr); // called up from the edge: tuck away again a moment after the mouse goes
        KillTimer(hwnd, kHoverTimer);
        hoverArmed = false;
        if (AnyOpen() && !closeArmed && GetCapture() != hwnd) SetTimer(hwnd, kCloseTimer, setOpen || wgOpen ? kSetCloseMs : kCloseMs, nullptr), closeArmed = true; // a widget's panel, like settings, forgives the trip up from the dock
        Redraw();
        return 0;
    case WM_LBUTTONDOWN:
        SetCapture(hwnd); // keep getting moves outside the window while held
        input.mouse = input.pressPos = ToDip(lp);
        input.pressed = input.down = true;
        Redraw();
        return 0;
    case WM_LBUTTONUP:
        BelowIsland(); // a click can lift the bar over astraeus
        input.mouse = ToDip(lp);
        input.released = true;
        input.down = false;
        ReleaseCapture();
        Redraw();
        return 0;
    case WM_CAPTURECHANGED:
        if (input.down) { // synthesize a release so nothing sticks
            input.down = false;
            input.released = true;
            Redraw();
        }
        return 0;
    case WM_MBUTTONUP:
        input.mouse = ToDip(lp);
        midClick = true;
        Redraw();
        return 0;
    case WM_RBUTTONUP:
        BelowIsland();
        input.mouse = ToDip(lp);
        input.rclick = true;
        Redraw();
        return 0;
    case WM_MOUSEWHEEL:
        input.wheel += GET_WHEEL_DELTA_WPARAM(wp) / (float)WHEEL_DELTA;
        Redraw();
        return 0;
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, hand ? IDC_HAND : IDC_ARROW));
        return TRUE;
    case WM_TIMER:
        KillTimer(hwnd, wp); // all timers are one-shot
        switch (wp) {
        case kPreviewTimer:
            if (!hoverApp.empty()) ShowPreview();
            break;
        case kPreviewHideTimer:
            if (hoverApp.empty() && !card.Hovered()) card.Hide();
            break;
        case kNotifPollTimer:
            if (int64_t s = sys::NotifStamp(); s != notifStamp) {
                if (notifStamp && !calOpen) notifUnread = true;
                notifStamp = s;
                LoadNotifs();
                for (Bar* b : coverBars)
                    if (b != this) b->FollowNotifs();
            }
            notifPolling = PillsLive(); // no pill could rise (astraeus shows them, quiet, a fullscreen app): the folder watch and the status look are soon enough
            if (notifPolling) SetCoalescableTimer(hwnd, kNotifPollTimer, kNotifQuickMs, nullptr, 500);
            break;
        case kAppsTimer:
            LoadApps(false); // a while after start, in the background: names for search; icons only once the launcher opens, so they don't sit in memory
            break;
        case kNotifReadTimer:
            LoadNotifs();
            for (Bar* b : coverBars)
                if (b != this) b->FollowNotifs(); // one watch: the moons read when it fires
            break;
        case kPillTimer:
            pillUp = false;
            Redraw();
            break;
        case kGhostTimer:
            SyncGhosts();
            break;
        case kMixTimer:
            if (mixer.IsOpen() && qsOpen && qsPage == 4) {
                mixer.Refresh(); // apps start and stop making sound
                SetTimer(hwnd, kMixTimer, 1000, nullptr);
                Redraw();
            }
            break;
        case kMixCloseTimer:
            if (!(qsOpen && qsPage == 4)) mixer.Close(), mixIcons.clear(); // a wheel over an app opened it: let go once the scrolling stops
            break;
        case kPosTimer: {
            KillTimer(hwnd, kPosTimer);
            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            Log("appbar moved");
            if ((barPx > 0 && wa.top < monitor.top + barPx) || (shelfBar && wa.bottom > monitor.bottom - (LONG)std::lround(kShelfH * scale))) Reserve(); // only if our strip really went: claiming it again tells every bar, and they answer
            break;
        }
        case kUpdateTimer:
            updater.Check(hwnd, WM_APP_UPDATE);
            SetCoalescableTimer(hwnd, kUpdateTimer, kUpdateEveryMs, nullptr, 60 * 1000);
            break;
        case kCalWaitTimer:
            StopCalWait(); // the link never came: stop watching the clipboard
            if (calOpen) Redraw();
            break;
        case kFullTimer: // one-shot like every timer here: looked at again for as long as the bar is away
            if (!FullInFront()) SetFull(false);
            else SetCoalescableTimer(hwnd, kFullTimer, kFullMs, nullptr, 1000);
            break;
        case kCoverTimer:
            if (full && !FullInFront()) SetFull(false); // the game or video left the front, or left fullscreen: back at once, not at the slow look
            CheckCovered();
            if (LookNow() == 2) SetTimer(hwnd, kShadeTimer, 500, nullptr); // what's behind the glass may have changed: one look once things settle, a screen read is costly
            break;
        case kTitleTimer: // only the top bar shows titles: the dock's pixels stay as they are
            KillTimer(hwnd, kTitleTimer);
            titleArmed = false;
            topAsk = fullNext = needsFrame = true;
            if (eco) sys::SetEco(eco = false);
            Arm(0);
            break;
        case kStatsTimer: // the load and the network, only while someone can see them
            if (!HasWg(kWgStats) && !HasWg(kWgNet)) break;
            if (StatsUnseen()) { // tucked away or behind a game: no ticks at all, the first frame back in sight starts them again
                statsParked = true;
                break;
            }
            SetCoalescableTimer(hwnd, kStatsTimer, kStatsMs, nullptr, 500); // one-shot like every timer here: armed again each time
            WidgetTick();
            break;
        case kCaretTimer: // a text field's caret flips
            if (AnyOpen()) Redraw();
            break;
        case kLangTimer: // the app switched layouts: the label follows
            KillTimer(hwnd, kLangTimer);
            SyncLang();
            Redraw();
            break;
        case kFocusTimer:
            StepFocus();
            break;
        case kClipTimer:
            OnClipboard(); // a calendar link being waited for
            if (OnClipboard2() < 0 && ++clipTries < 5) SetTimer(hwnd, kClipTimer, kClipSettleMs, nullptr); // the copying app still holds it: a little later
            else clipTries = 0;
            break;
        case kClipPasteTimer:
            ClipPaste();
            break;
        case kClipSaveTimer:
            if (clip::History().dirty) clip::History().Save();
            break;
        case kShadeTimer:
            KillTimer(hwnd, kShadeTimer);
            SampleShade();
            break;
        case kPeekTimer:
            peek = topPeek = false;
            Redraw();
            break;
        case kReserveTimer:
            Unreserve(); // registered while windows' taskbar still stood: explorer stacked us above it, so take the bottom again now it's hidden
            Reserve();
            BelowIsland();
            break;
        case kClockTimer:
            ArmClock();
            if (!Hidden()) Redraw(); // tucked away: the frame that brings the dock back draws the new minute
            break;
        case kRescanTimer: // the main dock's alone: every dock shows the one list
            tasks->Rescan();
            for (Bar* b : coverBars) {
                b->icons.Keep(tasks->Apps());
                for (const auto& a : tasks->Apps()) b->icons.Get(nullptr, a, b->IconPx()); // read new icons now, not in the middle of a frame
            }
            if (tray) tray->Prune(); // an app that quit without tidying up its icon
            break;
        case kHoverTimer: {
            hoverArmed = false;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt);
            break;
        }
        case kCloseTimer: {
            closeArmed = false;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt); // leave messages can lie, the cursor can't
            if (GetCapture() != hwnd && !editing && !Typing() && !OnChrome({pt.x / scale, pt.y / scale})) CloseAll(); // editing the dock or typing a note: only done or esc ends it, never a stray mouse
            break;
        }
        case kIdleTimer: Idle(); break;
        case kTrimTimer: Trim(); break;
        case kDevTimer: Redraw(); break;
        case kHideTimer:
            if (!moon) taskbar::Hide();
            if (tray) tray->Reassert();
            break;
        case kScreensTimer:
            if (!ScreensMoved()) { // back as it was: no restart after all
                if (!full && !IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOWNOACTIVATE);
                Place();
                break;
            }
            if (int now = (int)time(nullptr), last = prefs::ReadNum(L"ScreensAt", 0); now >= last && now - last < kScreensGapSec) {
                SetTimer(hwnd, kScreensTimer, (UINT)(kScreensGapSec - (now - last)) * 1000, nullptr); // made afresh a moment ago: never a restart storm, whatever the screens do
                break;
            }
            prefs::SaveNum(L"ScreensAt", (int)time(nullptr));
            PostQuitMessage(2); // every screen's docks made afresh, windows' taskbar kept hidden meanwhile
            break;
        case kStatusTimer: {
            if (AllAway()) { // every screen behind a game: no reads at all, the poller and its drives stay asleep; SetFull(false) catches up
                SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
                break;
            }
            PollStatus();
            radios.Recheck(); // throttled inside, and nothing once both radios are known
            if (HasWg(kWgWeather)) RefreshWx(); // cached for half an hour: fetches only when that's up
            if (HasWg(kWgPhoto) && shadeTick % 360 == 0) PickPhoto(); // another picture each hour
            if (HasWg(kWgFiles) && shadeTick % 6 == 0 && !(wgOpen && wgKind == kWgFiles) && !Hidden()) LoadFiles(L""); // the newest download, each minute
            if (++shadeTick % 3 == 0) SampleShade(); // a page can scroll from dark to white without any window moving; a screen read is costly, so every 30 s
            BelowIsland(); // astraeus may have started since: its hover strip goes back over the top bar
            if (auto was = promoted; SyncPromoted(), promoted != was) Redraw(); // an icon moved in or out of the taskbar in windows' settings
            if (!notifPolling && PillsLive()) notifPolling = true, SetCoalescableTimer(hwnd, kNotifPollTimer, kNotifQuickMs, nullptr, 500); // pills can rise again: the quick look comes back
            SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
            break;
        }
        }
        return 0;
    case WM_APP_TASKS:
        if (wp == Tasks::kRescan) SetTimer(hwnd, kRescanTimer, kRescanMs, nullptr); // restarting it coalesces a burst
        else if (wp == Tasks::kTitle) { // a terminal or player can retitle every second: nothing moved, nothing to cover or relayout
            if (card.Shown()) ShowPreview();
            if (!full && TopOn() && Shows(kModTitle) && !titleArmed) titleArmed = true, SetTimer(hwnd, kTitleTimer, kTitleMs, nullptr); // a spinner in a title: at most a few top bar frames a second
        } else if (wp == Tasks::kIsland) {
            BelowIsland();
        } else if (wp == Tasks::kOverflow) {
            bool up = overflow::Shown();
            if (up) overflow::Place(); // explorer put it over its own hidden chevron
            HWND f = GetForegroundWindow();
            if (!up && overflowUp && overflowFore && IsWindow(overflowFore) && (!f || taskbar::IsTray(f) || overflow::Is(f)))
                SetForegroundWindow(overflowFore); // closed with escape or a second click: focus stays on the hidden taskbar otherwise
            if (up != overflowUp) overflowUp = up, Redraw(); // the chevron turns
        } else if (wp == Tasks::kTrayShown) {
            if (moon) return 0;
            double now = Seconds();
            if (now - hideAt > 1) hideStrikes = hideCount > kHideBurst ? (std::min)(hideStrikes + 1, kHideStrikesMax) : 0, hideAt = now, hideCount = 0; // a new second: explorer still fighting, or calm again
            if (++hideCount <= kHideBurst) {
                taskbar::Hide();
                if (tray) tray->Reassert();
            } else if (hideCount == kHideBurst + 1) {
                SetTimer(hwnd, kHideTimer, (std::min)(kHideCalmMs << hideStrikes, kHideCalmMaxMs), nullptr); // explorer showing it back as fast as we hide it: one look after a pause that doubles while it keeps on, never a spin
            }
        } else if (wp == Tasks::kBadge) {
            if (!Hidden()) Redraw(); // a progress step: nothing moved, nothing to cover
        } else {
            if (card.Shown()) ShowPreview(); // titles and windows change under the card
            CheckCovered();
            SyncLang(); // a different app in front may type in a different layout
            SyncDesktops(); // a desktop switch brings another window to the front
            if (int ai = tasks->AppOf(tasks->Fore()); ai >= 0 && unread.count(tasks->Apps()[ai].key)) SeeNotifs(tasks->Apps()[ai].key); // its notifications were seen
            if (!Unseen()) Redraw(); // a title ticking every second behind a maximized window wakes nothing
        }
        return 0;
    case Preview::kLeft:
        SetTimer(hwnd, kPreviewHideTimer, kPreviewHideMs, nullptr);
        return 0;
    case Preview::kMediaCmd:
        if (wp == 0) Player().Previous();
        else if (wp == 1) Player().TogglePlay();
        else Player().Next();
        return 0;
    case Preview::kDone:
        card.Hide();
        previewMuted = hoverApp;
        Redraw();
        return 0;
    case WM_APP_TRAY: {
        if (tray) tray->Posted(); // the next change may post again
        if (tray) { // drop bitmaps of icons that are gone
            for (auto it = trayBmps.begin(); it != trayBmps.end();) {
                bool alive = std::any_of(tray->Items().begin(), tray->Items().end(), [&](const TrayItem& t) { return t.key == it->first; });
                it = alive ? std::next(it) : trayBmps.erase(it);
            }
            if (trayOpen && TrayCount() == 0) SetTrayOpen(false); // the last icon went
        }
        auto was = promoted;
        SyncPromoted();
        if (overflowUp) overflow::Place(); // explorer re-centers it when it grows or shrinks
        int shown = HiddenCount();
        if (trayOpen || (!Hidden() && (!promoted.empty() || promoted != was || (shown > 0) != (trayShown > 0)))) Redraw(); // tucked or behind a game: the next frame back draws them // an app animating its icon costs nothing unless it's in sight, or the button comes or goes
        trayShown = shown;
        return 0;
    }
    case WM_APP_CAL:
        if (wp == 1) cals.Sync(hwnd, WM_APP_CAL, 0); // a link came in while the last fetch ran: fetched now
        if (calOpen) Redraw();
        return 0;
    case WM_APP_UPDATE:
        OnUpdate(wp);
        return 0;
    case WM_APP_CALMENU:
        CalMenu();
        return 0;
    case WM_HOTKEY: // the clipboard's quick key, from any app
        if (wp == 0x4842) {
            if (full) break; // a fullscreen game keeps the screen
            if (wgOpen && wgKind == kWgClip) SetPanel(Panel::None);
            else OpenClip(clipAnchor > 0 ? clipAnchor : W / 2, false);
        }
        return 0;
    case WM_APP_CLIP:
        OnClipDone(wp, 0);
        return 0;
    case WM_APP_CLIPTITLE:
        OnClipDone(0, lp);
        return 0;
    case WM_SYSKEYDOWN:
    case WM_SYSKEYUP:
        if (ClipKey(wp, msg == WM_SYSKEYDOWN)) return 0;
        break;
    case WM_KEYUP:
        if (ClipKey(wp, false)) return 0;
        return 0;
    case WM_CLIPBOARDUPDATE:
        SetTimer(hwnd, kClipTimer, kClipSettleMs, nullptr); // read once the copying app is done with it; a burst of updates reads once
        return 0;
    case WM_APP_HOTSPOT:
        if (!hotspot.Busy()) hotspotSwitching = false;
        if (qsOpen) Redraw();
        return 0;
    case WM_APP_APPS:
        OnApps(wp, lp);
        return 0;
    case WM_KEYDOWN:
        if (ClipKey(wp, true)) return 0;
        if (dockKey >= 0) DockKey(wp);
        else if (launchOpen && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN || wp == VK_ESCAPE || wp == VK_RETURN)) LaunchKey(wp);
        return 0;
    case WM_APP_NOTIFS:
        if (auto* got = reinterpret_cast<std::vector<sys::Notif>*>(lp)) {
            notifs = std::move(*got);
            delete got;
            if (notifSeen.empty()) SeeNotifs(L""); // what was there before the bar started isn't news
            CountUnread();
            int64_t top = 0;
            const sys::Notif* fresh = nullptr;
            for (const auto& n : notifs)
                if (n.id > top) top = n.id, fresh = &n;
            if (pillSeen == 0) pillSeen = top; // the first read: nothing in it is new
            else if (fresh && top > pillSeen) { // something new: a pill, unless the user asked for quiet or is in a fullscreen app
                pillSeen = top;
                if (Hush()) Banners(true); // an app notifying for the first time has a switch of its own now: its next banner stays hidden too
                if (cfg.pills && !moon && !Quiet() && !full && !FindWindowW(L"AstraeusHost", nullptr)) { // astraeus running: it shows the notification, not us
                    pillNote = *fresh;
                    pillUp = true;
                    SetTimer(hwnd, kPillTimer, kPillMs, nullptr);
                }
            }
        }
        notifLoading = false;
        Redraw(); // the counts on the icons, and the panel if it's open
        return 0;
    case WM_APP_RADIOS:
        Redraw();
        return 0;
    case WM_APP_BRIGHT:
        brightPct = (int)(INT_PTR)wp;
        Redraw();
        return 0;
    case WM_APP_WIFI:
        wifi.Snap(nets);
        Redraw();
        return 0;
    case WM_APP_BT:
        bt.Snap(devs);
        Redraw();
        return 0;
    case WM_APP_MEDIA:
        OnMedia();
        return 0;
    case WM_APP_MENU: // wparam: 0 the power user menu, 1 power, 2 the recycle bin
        wp == 1 ? ShowPowerMenu() : wp == 2 ? BinMenu() : wp == 3 ? ShowUserMenu() : wp == 4 ? AppMenu() : wp == 5 ? ClipMenu() : ShowMenu();
        BelowIsland(); // a menu brings its owner to the front: astraeus goes back on top
        return 0;
    case WM_WINDOWPOSCHANGED: // something raised the bar (a menu, the shell, our own topmost call): astraeus' strip goes back on top
        if (!lowering && !(reinterpret_cast<WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) BelowIsland();
        break;
    case WM_APP_WX:
        wx = weather.Get();
        for (Bar* b : coverBars) { // one fetch for every dock
            b->wx = wx;
            if (b->HasWg(kWgWeather)) b->SyncFit(), b->Redraw(); // a new temperature may be a digit longer or shorter
        }
        return 0;
    case WM_APP_ICON:
        if (icons.Deliver(lp)) Redraw();
        return 0;
    case WM_APP_PHOTO: {
        struct Pic {
            std::wstring path;
            ComPtr<IWICBitmapSource> src;
        };
        std::unique_ptr<Pic> pic(reinterpret_cast<Pic*>(lp));
        photoBusy = false;
        if (pic && pic->src) photoSrc = pic->src, photoPath = pic->path, photoBmp.Reset(), Redraw();
        return 0;
    }
    case WM_APP_WINKEY:
        if (wp == kKeyNext || wp == kKeyBack) FocusDock(wp == kKeyNext ? 1 : -1);
        else if (wp == kKeyTray) OpenOverflow(); // windows' own tray flyout: it takes the keyboard itself
        else if (tasks && (int)wp < (int)tasks->Apps().size()) tasks->Click((int)wp); // start it, bring it up, or tuck it away, as windows' taskbar does
        return 0;
    case WM_DEVICECHANGE:
        if (HasWg(kWgDrives) && (wp == DBT_DEVICEARRIVAL || wp == DBT_DEVICEREMOVECOMPLETE)) LoadDrives(), Redraw(); // a stick came or went
        return TRUE;
    case WM_APP_HALOICON:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) rest::ShowMenu(hwnd);
        return 0;
    case WM_COMMAND: // the tray dashboard's picks
        switch (LOWORD(wp)) {
        case rest::kSettings: Anchor(W / 2, false), SetSettingsOpen(true); break;
        case rest::kCustomize: StartEdit(); break;
        case rest::kWindows: prefs::SaveNum(L"WindowsBar", 1), PostQuitMessage(3); break; // resting: windows has the taskbar, halobar the tray
        case rest::kUpdate: updateNote = L"checking\u2026", updater.Check(hwnd, WM_APP_UPDATE, true); break;
        case rest::kRestart: PostQuitMessage(2); break;
        case rest::kExit: PostQuitMessage(0); break;
        case kIslandBack: // astraeus' fullscreen player, opened from our cover, just closed: the player panel it came from opens again
            if (!moon && hasTrack) Anchor(mediaR.right > mediaR.left ? Mid(mediaR) : W / 2, false), SetMediaOpen(true); // under the song, or mid-dock with the song switched off
            break;
        }
        return 0;
    case WM_APP_STATSRC:
        statMaking = false;
        if (HasWg(kWgStats) && !statSrc) statSrc.reset(reinterpret_cast<sys::StatSrc*>(lp));
        else delete reinterpret_cast<sys::StatSrc*>(lp); // switched off meanwhile
        return 0;
    case WM_APP_PICK:
        OnPick(wp, lp);
        return 0;
    case WM_APP_POLLED:
        OnPolled(lp);
        return 0;
    case WM_APP_RESTYLE:
        if (ctx && (LookNow() == 2) != (glass != nullptr)) FreeDev(); // into or out of glass: a different composition tree
        if (wp) {
            Place(); // a new size or strip
            if (!TopAuto()) topPeek = topCovered = false;
            CheckCovered();
        }
        Redraw();
        return 0;
    case WM_APP_APPBAR:
        if (wp == ABN_FULLSCREENAPP) { // with auto-hide a maximized window fills the screen too: only a real fullscreen app (no frame) sends the bar away
            bool on = lp != 0 && FullInFront();
            Log(lp ? (on ? "fullscreen notice: on" : "fullscreen notice: not fullscreen, ignored") : "fullscreen notice: off");
            SetFull(on);
        }
        else if (wp == ABN_POSCHANGED && !reserving && barPx > 0) SetTimer(hwnd, kPosTimer, 250, nullptr); // a burst of moves, one look: answering each one starts a ping-pong with explorer
        return 0;
    case WM_SETTINGCHANGE:
        if (wp == SPI_SETDESKWALLPAPER && SyncWall()) {
            SetShell(Make(shellSeed));
            Redraw();
        }
        return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
        if (cfg.screens && ScreensMoved()) { // a screen came, went or moved: every screen's docks are made afresh once the screens hold still
            if (moon && !SameScreen()) ShowWindow(hwnd, SW_HIDE); // its screen is gone: never drawn over another dock meanwhile
            SetTimer(hwnd, kScreensTimer, kScreensSettleMs, nullptr); // a burst of changes restarts the wait: one restart after it
            return 0;
        }
        if (took && !moon) taskbar::Hide(); // a screen added: explorer gave it a taskbar of its own
        Place();
        return 0;
    case WM_CLOSE: // taskkill without /f, or any polite request to leave: hand the taskbar back on the way out
        PostQuitMessage(0);
        return 0;
    case WM_ENDSESSION:
        if (wp) {
            taskbar::Give(); // the process may not get to run its exit path
            PostQuitMessage(0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Bar::Pass(const D2D1_RECT_F& area, const std::function<void()>& draw) {
    RECT px = {(LONG)std::floor(area.left * scale), (LONG)std::floor(area.top * scale), (LONG)std::ceil(area.right * scale), (LONG)std::ceil(area.bottom * scale)};
    px.left = (std::max)(px.left, 0L), px.top = (std::max)(px.top, 0L);
    px.right = (std::min)(px.right, (LONG)winPxW), px.bottom = (std::min)(px.bottom, (LONG)winPxH);
    if (px.right <= px.left || px.bottom <= px.top) return true; // off screen: nothing to do, nothing failed
    ComPtr<IDXGISurface> dxgi;
    POINT off{};
    ComPtr<ID2D1Bitmap1> tgt;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(BeginSurface(px, &dxgi, &off))) return false; // device lost or display changed
    if (FAILED(ctx->CreateBitmapFromDxgiSurface(dxgi.Get(), &bp, &tgt))) {
        EndSurface();
        return false;
    }
    ctx->SetTarget(tgt.Get());
    ctx->SetDpi(96, 96); // draw in dips, scaled by the transform: the atlas offset stays a whole pixel count, so nothing jitters at sizes like 0.9
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation((float)(off.x - px.left), (float)(off.y - px.top))); // the update sits at off in the surface's atlas
    ctx->PushAxisAlignedClip({px.left / scale, px.top / scale, px.right / scale, px.bottom / scale}, D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent: the desktop shows through around the toolbar and the dock
    ui.Rebase(); // hit rects are kept in dips, whatever this pass's atlas offset
    draw();
    ctx->PopAxisAlignedClip();
    HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(nullptr);
    EndSurface();
    return hr != D2DERR_RECREATE_TARGET;
}

void Bar::DrawCard(const Theme& t, ID2D1Geometry* shape) { // the open dropdown, drawn where it was always laid out and moved by cardOff
    if (!shape) return;
    D2D1_MATRIX_3X2_F was;
    ctx->GetTransform(&was);
    ctx->SetTransform(D2D1::Matrix3x2F::Translation(cardOff.x, cardOff.y) * *D2D1::Matrix3x2F::ReinterpretBaseType(&was));
    D2D1_POINT_2F m = input.mouse, p = input.pressPos;
    input.mouse = {m.x - cardOff.x, m.y - cardOff.y}, input.pressPos = {p.x - cardOff.x, p.y - cardOff.y}; // the panels test the mouse in their own layout
    ui.Shift(cardOff);
    PaintShape(shape, t);
    DrawPlayer(shape);
    DrawTrayGrid(t, shape);
    DrawSettings(t, shape);
    DrawSheet(t, shape);
    DrawQuick(t, shape);
    DrawCalendar(t, shape);
    DrawLauncher(t, shape);
    DrawWidgetPanel(t, shape);
    DrawCalling(t, shape);
    DrawPeek(t, shape);
    ui.Shift({-cardOff.x, -cardOff.y});
    input.mouse = m, input.pressPos = p;
    ctx->SetTransform(was);
}

void Bar::Frame() {
    if (full) { // hidden behind a fullscreen app: no frames until it's gone
        needsFrame = false;
        return;
    }
    double now = Seconds();
    float dt = lastFrame > 0 ? (float)(std::min)(now - lastFrame, 0.05) : 1 / 60.0f; // clamp so a stall doesn't jump the springs
    lastFrame = now;
    if (!EnsureDev()) {
        Log("frame: no device");
        Retry();
        return;
    }
    SyncCover();
    bool themes = StepTheme(dt);
    bool springs = StepSprings(dt);
    bool easing = Layout(dt); // the toolbar's, the dock's and a dropdown's bounds for this frame, known before anything is drawn
    bool fullFrame = fullNext || springs || themes || easing || Inside(mediaR, input.mouse) || !mediaCalm || AnyOpen() || card.Shown(); // the fast frames repaint only a dock at rest: a fading hover, a panel's or a card's shadow over the dock would show as a lighter square

    const Theme& t = shellNow;
    if (!fullFrame) { // only the toolbar's little bars could have moved: step them first, and draw nothing at all if no bar moved by half a pixel
        barsMoving = StepBars(dt, now);
        bool visible = false;
        for (int i = 0; i < 4; ++i) visible = visible || std::fabs(bars[i] - barsDrawn[i]) * 11 > 0.5f;
        if (!visible) {
            needsFrame = barsMoving;
            if (needsFrame) Arm(ambientSync / hz);
            else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
            return;
        }
        canvas->NewFrame();
        D2D1_RECT_F area = Inset(barsRect, -2, -3), reach = Inset(area, -1 / scale, -1 / scale); // the pass rounds out to whole pixels
        bool ok = Pass(area, [&] { // only the bars' own square: the plate, the pill's tint, the bars; cover and text stay as drawn
            if (Vert()) canvas->Glow({cfg.edge == 1 ? wegR.left + 4 : wegR.right - 4, (wegR.top + wegR.bottom) / 2}, 20, Height(wegR) / 2 + 12, Color{0, 0, 0, 0.22f}); // the column's shadow, as DrawWegCol lays it
            else canvas->Glow({(wegR.left + wegR.right) / 2, wegR.bottom - 4}, Width(wegR) / 2 + 12, 20, Color{0, 0, 0, 0.22f}); // the dock's shadow first, as DrawWeg lays it: through glass, a square without it reads lighter
            PaintPlate(wegR, WegRadius(), t, shadeBotNow, &reach); // the square sits well inside the dock: no curves to work out, no rim
            float pr = (std::min)(Width(mediaR), Height(mediaR)) / 2;
            canvas->Fill(mediaR, Within(reach, mediaR, pr, 1.5f) ? 0 : pr, t.text.Alpha(0.05f)); // the pill at rest, as DrawMediaTile paints it unhovered; upright in a side dock
            DrawBars(0, 0, false);
        });
        if (!ok) {
            FreeDev();
            Retry();
            return;
        }
        Present();
        needsFrame = barsMoving;
        if (needsFrame) Arm(ambientSync / hz);
        else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
        return;
    }

    // two small passes, never the whole screen: the toolbar with a dropdown hanging from it, then the dock with whatever rises over it
    fullNext = needsFrame = false; // anything drawn below that asks for a redraw sets these again, and they stick
    canvas->NewFrame();
    ComPtr<ID2D1Geometry> shape = Shape();
    D2D1_RECT_F cardNow = CardOnScreen(), dockArea = DockArea(); // the dock, its shadow and its tooltips
    D2D1_RECT_F top = Union(barR, cardDown ? cardNow : D2D1_RECT_F{}), bottom = Union(sink < 0.999f ? dockArea : D2D1_RECT_F{}, cardDown ? D2D1_RECT_F{} : cardNow);
    D2D1_RECT_F topDirty = Inset(Union(top, drawnTop), -2, -2), botDirty = Inset(Union(bottom, drawnBottom), -2, -2);
    // each bar draws only when something there changed: hovering the dock leaves the top bar's pixels alone, and the other way round
    bool cardMoves = springs || Grown() > 0.5f;
    bool onTop = Inside(top, input.mouse), onBot = Inside(bottom, input.mouse) || Inside(wegHit, input.mouse);
    bool drawTop = allNext || topAsk || themes || topLive || onTop || overTop || (cardDown && cardMoves) || easing || !Same(top, drawnTop);
    bool drawBot = allNext || themes || botLive || onBot || overBot || (!cardDown && cardMoves) || easing || dragging || !Same(bottom, drawnBottom);
    if (top.right <= top.left && drawnTop.right <= drawnTop.left) drawTop = topLive = false; // nothing there and nothing left over: the pass would only clear clear pixels (the top bar is retired, so most frames)
    if (bottom.right <= bottom.left && drawnBottom.right <= drawnBottom.left) drawBot = botLive = false; // the dock tucked away and already wiped
    overTop = onTop, overBot = onBot;
    allNext = topAsk = false;
    barsMoving = StepBars(dt, now); // the song's little bars, stepped once whichever pass draws them
    ui.Begin(*canvas, input, dt, now);
    bool ok = true;
    if (drawTop) ok = Pass(topDirty, [&] {
        DrawToolbar(t);
        if (cardDown) DrawCard(t, shape.Get());
    });
    else ui.KeepHits(drawnTop);
    bool tb = ui.Busy(), ta = ui.Ambient();
    if (drawTop) topLive = tb || ta;
    ui.Quiet();
    if (drawBot) tipNow = {}, trayAt.clear(); // the dock's pass names whatever is hovered again, and places the tray icons again
    if (!drawBot) ui.KeepHits(Union(drawnBottom, wegHit));
    else ok = ok && Pass(botDirty, [&] {
        DrawWeg(t);
        if (!cardDown) DrawCard(t, shape.Get());
    });
    if (drawBot) botLive = ui.Busy() || ui.Ambient();
    ui.Merge(tb, ta);
    if (!drawTop) top = drawnTop; // untouched: the surface still holds what it held
    if (!drawBot) bottom = drawnBottom;
    ui.End();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    midClick = false;
    if (!ok) {
        FreeDev();
        Retry();
        return;
    }
    bool shrank = drawnTop.bottom > top.bottom + 1 || drawnBottom.top < bottom.top - 1 || drawnBottom.left < bottom.left - 1 || drawnBottom.right > bottom.right + 1;
    drawnTop = top, drawnBottom = bottom;
    if (shrank) { // a dropdown closed or the dock narrowed: hand its old tiles back
        RECT keep[2];
        UINT n = 0;
        for (const D2D1_RECT_F& k : {top, bottom}) {
            if (k.right <= k.left) continue;
            RECT r = {(LONG)std::floor((k.left - 2) * scale), (LONG)std::floor((k.top - 2) * scale), (LONG)std::ceil((k.right + 2) * scale), (LONG)std::ceil((k.bottom + 2) * scale)};
            r.left = (std::max)(r.left, 0L), r.top = (std::max)(r.top, 0L);
            r.right = (std::min)(r.right, (LONG)winPxW), r.bottom = (std::min)(r.bottom, (LONG)winPxH);
            keep[n++] = r;
        }
        TrimSurface(keep, n);
    }
    if (drawTop || drawBot) SyncMask(), Present();
    if (statsParked && !StatsUnseen()) statsParked = false, SetTimer(hwnd, kStatsTimer, 1, nullptr); // back in sight: the numbers tick again at once
    devTries = 0; // a frame made it to the screen
    Census(0xF0005, (drawTop ? 1 : 0) | (drawBot ? 2 : 0)); // which passes a full frame drew
    if (!took) { // really showing: only now does the windows taskbar step aside
        took = true;
        SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // once windows' taskbar has fully let go, the strip at the top is ours
        if (!moon) taskbar::Take(), ClaimTray(), HaloIcon(true), SyncKeyHook(), SetTimer(hwnd, kGhostTimer, 4000, nullptr); // apps had their chance to give us their icons: the rest come from windows' snapshot
        if (HMODULE u = GetModuleHandleW(L"user32.dll"); u && !moon) { // user32's unnamed-in-the-docs taskman: the window asked where a minimizing window flies
            using GetTm = HWND(WINAPI*)();
            using SetTm = BOOL(WINAPI*)(HWND);
            auto get = reinterpret_cast<GetTm>(GetProcAddress(u, "GetTaskmanWindow"));
            auto set = reinterpret_cast<SetTm>(GetProcAddress(u, "SetTaskmanWindow"));
            if (get && set) taskmanWas = get(), set(hwnd); // explorer's, handed back on exit
        }
        BelowIsland(); // taking the taskbar shuffled the topmost windows
    }
    SyncRegion();
    hand = ui.Hand();
    if (menuWanted) {
        menuWanted = false;
        PostMessageW(hwnd, WM_APP_MENU, 0, 0); // a menu runs its own loop: not from inside a frame
    }

    bool asked = needsFrame; // a click during the frame asked for another (the player opening, say)
    bool busy = springs || themes || easing || ui.Busy() || asked;
    bool ambient = ui.Ambient() || barsMoving;    fullNext = fullNext || busy || ui.Ambient(); // only the toolbar's bars can ride on a partial frame
    needsFrame = busy || ambient;
    bool wasBarsOnly = barsOnly;
    barsOnly = !busy && !ui.Ambient() && barsMoving; // nothing but the little bars: caches can still be handed back
    if (barsOnly && !wasBarsOnly) SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr); // armed once: re-arming every frame pushed it back forever
    UINT smooth = (UINT)(std::max)(1L, std::lround(hz / 60)); // the open player's wave and light read as smooth at 60, not every refresh of a fast screen
    UINT busySync = (UINT)(std::max)(1L, std::lround(hz / kBusyFps));
    if (needsFrame) Arm((busy ? (input.down ? 1u : busySync) : mediaOpen ? smooth : ambientSync) / hz); // a drag follows the mouse at the screen's own rate
    else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
    if (spectrum && now - spectrumAsked > 3) spectrum.reset(); // the player closed: capture stops
}

void Bar::StartMark(D2D1_POINT_2F o, float size, const Theme& t) { // the start button's face: windows' logo or halobar's own
    StartMark(o, size, cfg.startIcon, t.accentHi, t.accent);
}

void Bar::StartMark(D2D1_POINT_2F o, float size, int face, Color hi, Color lo) {
    if (face == 1 && ctx) {
        if (!haloLogo) { // the app's icon, read once at the dock's size, as a mask painted in the accent
            constexpr float kFaceMax = 20; // the dock's logo at rest: smaller ones scale down from it, never up
            int px = (int)std::lround(kFaceMax * kLogoGrow * scale);
            HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
            ComPtr<IWICBitmap> wb;
            ComPtr<IWICFormatConverter> conv;
            if (ico && gfx::Wic() && SUCCEEDED(gfx::Wic()->CreateBitmapFromHICON(ico, &wb)) && SUCCEEDED(gfx::Wic()->CreateFormatConverter(&conv)) &&
                SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
                ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &haloLogo);
            if (ico) DestroyIcon(ico);
        }
        ComPtr<ID2D1SolidColorBrush> ink;
        if (haloLogo && SUCCEEDED(ctx->CreateSolidColorBrush(hi.D2D(), &ink))) {
            float s = size * kLogoGrow;
            D2D1_RECT_F at = {o.x - s / 2, o.y - s / 2, o.x + s / 2, o.y + s / 2};
            D2D1_ANTIALIAS_MODE was = ctx->GetAntialiasMode();
            ctx->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED); // the mask call wants it
            ctx->FillOpacityMask(haloLogo.Get(), ink.Get(), D2D1_OPACITY_MASK_CONTENT_GRAPHICS, &at, nullptr);
            ctx->SetAntialiasMode(was);
            return;
        }
    }
    WinLogo(o, size, hi, lo);
}

void Bar::LoadIconPaths() {
    iconPaths.clear();
    customBmps.clear();
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\HaloBar\\Icons", 0, KEY_READ, &k) != ERROR_SUCCESS) return;
    wchar_t name[512], path[MAX_PATH];
    for (DWORD i = 0;; ++i) {
        DWORD nl = 512, pl = sizeof(path), type = 0;
        if (RegEnumValueW(k, i, name, &nl, nullptr, &type, reinterpret_cast<BYTE*>(path), &pl) != ERROR_SUCCESS) break;
        if (type == REG_SZ) iconPaths[name] = path;
    }
    RegCloseKey(k);
}

ID2D1Bitmap* Bar::CustomIcon(const std::wstring& key, UINT px) { // the picture the user gave this app: an icon, a png, or another program's icon
    auto p = iconPaths.find(key);
    if (p == iconPaths.end() || !ctx || !gfx::Wic()) return nullptr;
    if (auto b = customBmps.find(key); b != customBmps.end()) return b->second.Get();
    ComPtr<IWICBitmapSource> src;
    std::wstring ext = p->second.size() > 4 ? p->second.substr(p->second.size() - 4) : L"";
    for (auto& ch : ext) ch = (wchar_t)towlower(ch);
    if (ext == L".png") {
        ComPtr<IWICBitmapDecoder> dec;
        ComPtr<IWICBitmapFrameDecode> fr;
        ComPtr<IWICBitmapScaler> sc;
        if (SUCCEEDED(gfx::Wic()->CreateDecoderFromFilename(p->second.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) &&
            SUCCEEDED(gfx::Wic()->CreateBitmapScaler(&sc)) && SUCCEEDED(sc->Initialize(fr.Get(), px, px, WICBitmapInterpolationModeFant)))
            src = sc;
    } else {
        HICON ico = nullptr;
        if (ext == L".ico") ico = (HICON)LoadImageW(nullptr, p->second.c_str(), IMAGE_ICON, (int)px, (int)px, LR_LOADFROMFILE);
        else SHDefExtractIconW(p->second.c_str(), 0, 0, &ico, nullptr, MAKELONG(px, 0)); // a program: its own first icon
        ComPtr<IWICBitmap> wb;
        if (ico && SUCCEEDED(gfx::Wic()->CreateBitmapFromHICON(ico, &wb))) src = wb;
        if (ico) DestroyIcon(ico);
    }
    ComPtr<IWICFormatConverter> conv;
    ComPtr<ID2D1Bitmap> bmp;
    if (src && SUCCEEDED(gfx::Wic()->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        ctx->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &bmp);
    customBmps[key] = bmp; // a file that won't load stays empty: the app's own icon shows
    return bmp.Get();
}

void Bar::PickIcon(const std::wstring& key) {
    constexpr wchar_t kIcons[] = L"Software\\HaloBar\\Icons";
    ComPtr<IFileOpenDialog> dlg;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dlg)))) return;
    COMDLG_FILTERSPEC types[] = {{L"Icons and pictures", L"*.ico;*.png;*.exe"}};
    dlg->SetFileTypes(1, types);
    dlg->SetTitle(L"Pick an icon");
    ComPtr<IShellItem> item;
    PWSTR path = nullptr;
    if (SUCCEEDED(dlg->Show(hwnd)) && SUCCEEDED(dlg->GetResult(&item)) && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        RegSetKeyValueW(HKEY_CURRENT_USER, kIcons, key.c_str(), REG_SZ, path, (DWORD)((wcslen(path) + 1) * sizeof(wchar_t)));
        CoTaskMemFree(path);
    }
    LoadIconPaths();
    Redraw();
}

void Bar::SyncTrayOnly() { // tray icons whose app has no window on the taskbar: background apps, in the dock if asked
    trayOnly.clear();
    if (!cfg.trayApps || !tray || !tasks) return;
    std::vector<DWORD> pids;
    for (const auto& a : tasks->Apps())
        for (const auto& w : a.wins) {
            DWORD pid = 0;
            GetWindowThreadProcessId(w.hwnd, &pid);
            pids.push_back(pid);
        }
    const auto& its = tray->Items();
    for (size_t i = 0; i < its.size(); ++i) {
        if (!its[i].Shown() || !its[i].owner) continue;
        DWORD pid = 0;
        GetWindowThreadProcessId(its[i].owner, &pid);
        if (pid && pid != GetCurrentProcessId() && std::find(pids.begin(), pids.end(), pid) == pids.end()) trayOnly.push_back(i);
    }
}
