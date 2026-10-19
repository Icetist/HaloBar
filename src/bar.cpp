#include "bar.h"

#include <dwmapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cwctype>

#include "overflow.h"
#include "taskbar.h"
#include "version.h"

#pragma comment(lib, "shcore.lib")

namespace {

constexpr UINT_PTR kClockTimer = 1, kRescanTimer = 2, kHoverTimer = 3, kCloseTimer = 4, kIdleTimer = 5, kDevTimer = 6, kStatusTimer = 7, kReserveTimer = 8;
Bar* coverBar = nullptr; // win event hooks carry no context
constexpr UINT kReserveSettleMs = 700; // explorer's taskbar takes a moment to really give up its strip once it auto-hides
constexpr UINT kStatusMs = 10000; // network, battery and volume in the row: windows doesn't tell us, so a slow look
constexpr UINT kRescanMs = 40; // a burst of window events lands in one walk of the windows
constexpr UINT kHoverMs = 260, kCloseMs = 420; // song: rest on it to open the player; leave the island this long to close what's open
constexpr UINT kSetCloseMs = 1400; // settings forgive a longer trip outside
constexpr UINT kIdleMs = 2500; // still this long: efficiency mode
constexpr int kDevTries = 30; // ~3 seconds of retries, 100 ms apart
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
    char line[256];
    int n = sprintf_s(line, "%llu %s\r\n", GetTickCount64(), what);
    DWORD w = 0;
    WriteFile(f, line, (DWORD)(n > 0 ? n : 0), &w, nullptr);
    CloseHandle(f);
}

double Seconds() {
    static LARGE_INTEGER freq = [] { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)now.QuadPart / (double)freq.QuadPart;
}

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
    tray.reset(); // apps fall back to explorer's tray, which has every icon
    HookCover(false);
    coverBar = nullptr;
    if (notifWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(notifWatch);
    if (cfg.pills) Banners(false);
    card.Hide();
    icons.Stop(); // its thread posts to the window: done before the window goes
    tasks.reset(); // unhook before the window goes
    spectrum.reset();
    FreeDev();
    StopPoller(); // it closes the status handles on its own thread
    prefs::Unwatch(prefsEv);
    pins::Unwatch(pinsEv);
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
    tasks = std::make_unique<Tasks>(hwnd, WM_APP_TASKS);
    Place();
    tasks->Rescan();
    coverBar = this;
    HookCover(true); // always: auto-hide and the glass's tint both follow the windows
    notifStamp = sys::NotifStamp();
    LoadNotifs(); // the counts start from what's there now
    wchar_t dir[MAX_PATH];
    if (ExpandEnvironmentStringsW(L"%LOCALAPPDATA%\\Microsoft\\Windows\\Notifications", dir, MAX_PATH))
        notifWatch = FindFirstChangeNotificationW(dir, FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE); // the kernel tells us when a toast lands
    if (cfg.pills) Banners(true);
    SetTimer(hwnd, kAppsTimer, 8000, nullptr);
    SetCoalescableTimer(hwnd, kNotifPollTimer, 2000, nullptr, 500); // windows writes its store lazily and the folder watch can lag: two attribute reads every 2 s catch a new toast fast
    cals.Sync(hwnd, WM_APP_CAL, 0); // and again whenever the calendar opens: nothing fetches while it's closed
    SetTimer(hwnd, kUpdateTimer, kUpdateFirstMs, nullptr);
    media.Start(hwnd, WM_APP_MEDIA);
    prefsEv = prefs::WatchIsland();
    pinsEv = pins::Watch();
    SyncLook();
    SyncUser(); // the toolbar's user module
    SyncDesktops();
    shellNow = shellTo; // start on the final colors, no fade at boot
    mediaNow = mediaTo;
    shellT = mediaT = 1;
    ArmClock();
    radios.Start(hwnd, WM_APP_RADIOS);
    SyncAudio();
    StartPoller();
    LoadSlots(); // the dock's widgets, in their order and sizes
    SyncWidgets(); // the clipboard listener, the stats timer, the forecast: only for the widgets in the dock
    PollStatus();
    SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
    BelowIsland(); // astraeus came up first: its strip stays over the top bar
    Redraw();
    return true;
}

void Bar::ClaimTray() {
    if (!tray) tray = std::make_unique<TrayHost>(hwnd, WM_APP_TRAY);
    tbSelfAt = Seconds(); // the broadcast comes back to us too
    tray->Claim();
}

void Bar::Place() {
    mon = MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(mon, &mi);
    monitor = mi.rcMonitor;
    UINT dx = 96, dy = 96;
    dpi = SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &dx, &dy)) ? dx / 96.0f : 1.0f;
    scale = dpi * Zoom(); // the size setting is just a bigger dip: everything, hit tests included, follows
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
    regionNow.clear();
    SyncRegion(); // right away: the window must never swallow clicks, even before the first frame
    Unreserve(); // the strip may have changed size, or gone: a fresh registration takes it as it is now
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
        sb.uEdge = ABE_BOTTOM;
        sb.rc = monitor;
        sb.rc.top = monitor.bottom - px;
        SHAppBarMessage(ABM_QUERYPOS, &sb);
        sb.rc.top = sb.rc.bottom - px;
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
    if (on) SetCoalescableTimer(hwnd, kFullTimer, 2000, nullptr, 1000); // the shell's "it's over" can go missing: never stay hidden on its word alone
    else KillTimer(hwnd, kFullTimer);
    if (!on) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        BelowIsland();
        Redraw();
    }
}

bool Bar::FullInFront() const {
    HWND f = GetForegroundWindow();
    if (!f || f == hwnd || IsIconic(f) || !IsWindowVisible(f)) return false;
    wchar_t cls[32] = {};
    GetClassNameW(f, cls, 32);
    for (const wchar_t* shell : {L"Progman", L"WorkerW", L"Shell_TrayWnd", L"Shell_SecondaryTrayWnd"})
        if (!wcscmp(cls, shell)) return false; // the desktop itself is screen-sized too
    LONG style = GetWindowLongW(f, GWL_STYLE);
    if ((style & WS_CAPTION) == WS_CAPTION || (IsZoomed(f) && (style & WS_THICKFRAME))) return false; // a maximized app, however it draws its title bar: fullscreen drops the sizing frame
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
    if (sink > 0.5f && !peek) want.push_back({0, winPxH - 2, winPxW, winPxH}); // dock tucked away: only the screen's bottom edge, to call it back
    else if (wegR.right > wegR.left) want.push_back(px(Union(wegR, wegHit.top < wegR.top && (Inside(wegHit, input.mouse) || dragging) ? wegHit : wegR))); // magnified: the room the grown icons take, only while they're grown
    if (D2D1_RECT_F c = CardOnScreen(); c.right > c.left) want.push_back(px(c));
    bool same = want.size() == regionNow.size();
    for (size_t i = 0; same && i < want.size(); ++i) same = EqualRect(&want[i], &regionNow[i]) != 0;
    if (same) return; // resting: set once, not every frame
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
    card.Show(hwnd, a->key, wins, ToScreen({(hoverSlot.left + hoverSlot.right) / 2, wegR.top}), scale, shellNow, med); // over the dock, centered on the app
}

void Bar::HookCover(bool on) {
    for (auto& h : coverHooks)
        if (h) UnhookWinEvent(h), h = nullptr;
    if (moveHook) UnhookWinEvent(moveHook), moveHook = nullptr;
    covered = false;
    if (!on) return;
    constexpr DWORD kFlags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
    coverHooks[0] = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnCoverEvent, 0, 0, kFlags);
    coverHooks[1] = SetWinEventHook(EVENT_SYSTEM_MOVESIZEEND, EVENT_SYSTEM_MOVESIZEEND, nullptr, OnCoverEvent, 0, 0, kFlags);
    coverHooks[2] = SetWinEventHook(EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND, nullptr, OnCoverEvent, 0, 0, kFlags);
    OnCoverEvent(nullptr, EVENT_SYSTEM_FOREGROUND, GetForegroundWindow(), OBJID_WINDOW, CHILDID_SELF, 0, 0);
}

void CALLBACK Bar::OnCoverEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    Bar* b = coverBar;
    if (!b || !b->hwnd || obj != OBJID_WINDOW || child != CHILDID_SELF) return; // carets, scrollbars and the like move all the time: not windows
    if (ev == EVENT_SYSTEM_FOREGROUND && h) { // only the app in front can be dragged or maximized over us: follow its moves alone, not the whole desktop's
        DWORD pid = 0;
        GetWindowThreadProcessId(h, &pid);
        if (b->moveHook) UnhookWinEvent(b->moveHook);
        b->moveHook = pid && pid != GetCurrentProcessId()
                          ? SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, OnCoverEvent, pid, 0, WINEVENT_OUTOFCONTEXT)
                          : nullptr;
    }
    if (ev == EVENT_OBJECT_LOCATIONCHANGE && GetAncestor(h, GA_ROOT) != h) return;
    SetTimer(b->hwnd, kCoverTimer, kCoverMs, nullptr); // a burst of moves, one check
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
        for (const auto& a : tasks->Apps())
            for (const auto& w : a.wins) {
                if (covered || !IsWindowVisible(w.hwnd) || IsIconic(w.hwnd)) continue;
                RECT wr{}, x{};
                if (FAILED(DwmGetWindowAttribute(w.hwnd, DWMWA_EXTENDED_FRAME_BOUNDS, &wr, sizeof(wr)))) GetWindowRect(w.hwnd, &wr); // without the invisible resize border
                covered = IntersectRect(&x, &wr, &spot) != FALSE;
            }
    }
    if (covered != was) Redraw();
}

void Bar::OnNotifWrite() {
    FindNextChangeNotification(notifWatch); // keep watching
    SetTimer(hwnd, kNotifReadTimer, 150, nullptr); // a toast writes in a burst: read once it settles
}

namespace {
constexpr wchar_t kPushKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\PushNotifications";
}

void RestoreBanners() {
    int was = prefs::ReadNum(L"ToastWas", -1);
    if (was < 0) return; // we never touched them
    DWORD v = (DWORD)was;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", REG_DWORD, &v, sizeof(v));
    prefs::Delete(L"ToastWas");
}

void Bar::Banners(bool hush) {
    if (!hush) {
        RestoreBanners();
        return;
    }
    if (prefs::ReadNum(L"ToastWas", -1) < 0) { // first time: remember the user's own setting, even across a crash
        DWORD v = 1, size = sizeof(v);
        RegGetValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", RRF_RT_REG_DWORD, nullptr, &v, &size);
        prefs::SaveNum(L"ToastWas", (int)v);
    }
    DWORD off = 0;
    RegSetKeyValueW(HKEY_CURRENT_USER, kPushKey, L"ToastEnabled", REG_DWORD, &off, sizeof(off)); // our pill shows it instead
}

bool Bar::Quiet() const { return cfg.pills ? prefs::ReadNum(L"Quiet", 0) != 0 : sys::DoNotDisturb(); }

void Bar::SetQuiet(bool on) {
    if (cfg.pills) prefs::SaveNum(L"Quiet", on ? 1 : 0); // windows' banners are already off: quiet means no pills
    else sys::SetDoNotDisturb(on);
    dnd = on;
    if (on) pillUp = false;
}

void Bar::ShowDesktop() { // win+d, which explorer still answers: toggles, as the windows corner button does
    INPUT in[4] = {};
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
    };
    static const Entry kEntries[] = {
        {L"Installed apps", L"ms-settings:appsfeatures", nullptr, false},
        {L"Mobility Center", L"mblctr.exe", nullptr, false}, // laptops only: skipped below without a battery
        {L"Power Options", L"ms-settings:powersleep", nullptr, false},
        {L"Event Viewer", L"eventvwr.msc", nullptr, false},
        {L"System", L"ms-settings:about", nullptr, false},
        {L"Device Manager", L"devmgmt.msc", nullptr, false},
        {L"Network Connections", L"ms-settings:network", nullptr, false},
        {L"Disk Management", L"diskmgmt.msc", nullptr, true},
        {L"Computer Management", L"compmgmt.msc", nullptr, false},
        {L"Terminal", L"wt.exe", nullptr, false},
        {L"Terminal (Admin)", L"wt.exe", nullptr, true},
        {nullptr, nullptr, nullptr, false},
        {L"Task Manager", L"taskmgr.exe", nullptr, false},
        {L"Settings", L"ms-settings:", nullptr, false},
        {L"File Explorer", L"explorer.exe", nullptr, false},
        {L"Search", nullptr, nullptr, false}, // keys, below
        {L"Run", nullptr, nullptr, false},
    };
    constexpr UINT kBase = 100, kOut = 200, kSleep = 201, kHibernate = 202, kOff = 203, kReboot = 204, kDesk = 205;
    HMENU m = CreatePopupMenu(), power = CreatePopupMenu(), own = CreatePopupMenu();
    if (!m || !power || !own) {
        if (m) DestroyMenu(m);
        if (power) DestroyMenu(power);
        if (own) DestroyMenu(own);
        return;
    }
    for (UINT i = 0; i < ARRAYSIZE(kEntries); ++i) {
        if (!kEntries[i].label) {
            AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        if (i == 1 && !status.battery) continue; // mobility center exists only on laptops
        AppendMenuW(m, MF_STRING, kBase + i, kEntries[i].label);
    }
    AppendMenuW(power, MF_STRING, kOut, L"Sign out");
    AppendMenuW(power, MF_STRING, kSleep, L"Sleep");
    SYSTEM_POWER_CAPABILITIES caps{};
    if (CallNtPowerInformation(SystemPowerCapabilities, nullptr, 0, &caps, sizeof(caps)) == 0 && caps.HiberFilePresent) AppendMenuW(power, MF_STRING, kHibernate, L"Hibernate");
    AppendMenuW(power, MF_STRING, kOff, L"Shut down");
    AppendMenuW(power, MF_STRING, kReboot, L"Restart");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)power, L"Shut down or sign out");
    AppendMenuW(m, MF_STRING, kDesk, L"Desktop");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    std::wstring head = L"HaloBar " APP_VER_W + (updateNote.empty() ? L"" : L"  ·  " + updateNote);
    AppendMenuW(own, MF_STRING | MF_GRAYED, 0, head.c_str());
    AppendMenuW(own, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(own, MF_STRING, kMenuStart, L"Windows Start");
    AppendMenuW(own, MF_STRING, kMenuSettings, L"HaloBar settings");
    AppendMenuW(own, MF_STRING, kMenuEdit, L"Customize dock");
    AppendMenuW(own, MF_STRING, kMenuUpdate, L"Check for updates");
    AppendMenuW(own, MF_STRING, kMenuRestart, L"Restart HaloBar");
    AppendMenuW(own, MF_STRING, kMenuExit, L"Exit HaloBar");
    AppendMenuW(m, MF_POPUP, (UINT_PTR)own, L"HaloBar");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY | (p.y > monitor.top + (monitor.bottom - monitor.top) / 2 ? TPM_BOTTOMALIGN : TPM_TOPALIGN), p.x, p.y, hwnd, nullptr);
    DestroyMenu(m); // its submenus go with it
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
    bool launchWas = launchOpen, noteWas = Typing();
    launchOpen = p == Panel::Launch;
    wgOpen = p == Panel::Widget;
    if (!wgOpen || wgKind != kWgGallery) editing = false, dragSlot = -1; // edit mode lasts as long as its gallery
    if (noteWas && !Typing()) SaveNote(), draft.clear(); // the note is written as it closes
    if (p == Panel::None && updateReady) PostQuitMessage(2); // an update waited for the panel to close
    if (trayOpen && !trayWas) SyncGhosts(); // the icons explorer kept, read fresh as the tray opens
    if (calOpen && !calWas) cals.Sync(hwnd, WM_APP_CAL, sys::cal::Calendars::kOnOpen), calPick = 0; // opening it shows changes made a minute ago; today picked
    if (p == Panel::Media || p == Panel::Settings) trimDue = true; // art and layers get cleared from d2d's cache once it's gone again
    ringDrag = false;
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
    if (&field == &cfg.pills) Banners(cfg.pills != 0);
    if (&field == &cfg.song && !cfg.song && mediaOpen) SetMediaOpen(false);
    SampleShade(); // a look change: glass measures what it sits over, others drop the tint
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
    taskbar::Give(); // this pc can't draw the bar: the user keeps a working taskbar
    PostQuitMessage(1);
}

void Bar::Arm(double sec) {
    LARGE_INTEGER due;
    due.QuadPart = -(LONGLONG)(sec * 1e7); // relative, 100 ns units; zero is already due
    SetWaitableTimer(frameWait, &due, 0, nullptr, nullptr, FALSE);
}

void Bar::Redraw() {
    fullNext = allNext = true;
    if (eco) sys::SetEco(eco = false);
    KillTimer(hwnd, kIdleTimer);
    needsFrame = true;
    Arm(0);
}

void Bar::Idle() {
    if ((needsFrame && !barsOnly) || AnyOpen() || Grown() > 0.5f) return;
    spectrum.reset(); // the player is gone, so is the capture
    if (trimDue && ctx) { // the player left blurred art and its layers in d2d's cache
        trimDue = false;
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        if (dev) dev->ClearResources(0);
        gfx::Settle(d3d.Get());
    }
    gfx::GiveBack();
    if (!eco && !barsOnly) sys::SetEco(eco = true); // the bars keep their beat at full priority
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
    if (fixed) shellSeed = theme::FromCol(Color::Rgb((uint32_t)(follow ? look.accentHex : cfg.accentHex)));
    else wallKey.clear(), SyncWall(); // read the wallpaper again even if it didn't change
    SetShell(Make(shellSeed));
    if (coverSrc) SetMedia(Make(mediaSeed));
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
    return true;
}

void Bar::SetShell(const Theme& t) {
    shellFrom = shellNow;
    shellTo = t;
    shellT = 0;
    if (!coverSrc) SetMedia(t); // media follows the shell unless the album tints it
}

void Bar::SetMedia(const Theme& t) {
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

void Bar::OnPins() {
    pins::Rearm(pinsEv);
    if (!tasks) return;
    if (pins::Dirty()) pins::Assert(tasks->PinList()); // explorer wrote its older list over ours: ours stays until it reads it
    else tasks->ReloadPins(); // pinned or unpinned from somewhere else
}

void Bar::OnPrefs() {
    prefs::Rearm(prefsEv);
    islandHere = prefs::IslandInstalled(); // its settings just changed: it may have just been installed
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
    return self ? self->Handle(msg, wp, lp) : DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT Bar::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    if (tasks && tasks->ShellMsg() && msg == tasks->ShellMsg() && wp == HSHELL_GETMINRECT && lp) { // a window minimizes or comes back: it flies to its icon here, not to explorer's hidden one
        auto* info = reinterpret_cast<SHELLHOOKINFO*>(lp);
        int ai = tasks->AppOf(info->hwnd);
        auto it = ai >= 0 ? slots.find(tasks->Apps()[ai].key) : slots.end();
        if (it != slots.end()) { // tucked away: still its spot, at the screen's edge, so windows animates instead of fading
            float lift = sink * (kWegH + kWegMargin + 4);
            POINT a = ToScreen({it->second.left, it->second.top - lift}), b = ToScreen({it->second.right, it->second.bottom - lift});
            info->rc = {a.x, a.y, b.x, b.y};
            return TRUE;
        }
    }
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
        if (took) {
            taskbar::Take();
            ClaimTray(); // the new explorer tray came up in front of ours
            SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // its strip goes once it auto-hides: drop back to the bottom then
        }
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return passFor.empty() && !launchOpen && !Typing() ? MA_NOACTIVATE : MA_ACTIVATE; // clicks never steal focus from the user's app, except while a password is typed here
    case WM_CHAR:
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
        if (LOWORD(wp) == WA_INACTIVE && (launchOpen || Typing())) SetPanel(Panel::None); // clicked away: the launcher goes, as start does
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
        if (sink > 0.5f && !peek && p.y > winH / 2) { // tucked away and the mouse came to the bottom edge: rise
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
        if (peek || topPeek) SetTimer(hwnd, kPeekTimer, kPeekMs, nullptr); // called up from the edge: tuck away again a moment after the mouse goes
        KillTimer(hwnd, kHoverTimer);
        hoverArmed = false;
        if (AnyOpen() && !closeArmed && GetCapture() != hwnd) SetTimer(hwnd, kCloseTimer, setOpen ? kSetCloseMs : kCloseMs, nullptr), closeArmed = true;
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
            }
            SetCoalescableTimer(hwnd, kNotifPollTimer, full ? 10000 : 2000, nullptr, full ? 5000 : 500); // a fullscreen app shows no pills: the folder watch is soon enough
            break;
        case kAppsTimer:
            LoadApps(false); // names only, a while after start: the launcher's first search is instant, its icons load when it opens
            break;
        case kNotifReadTimer:
            LoadNotifs();
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
        case kFullTimer:
            if (!FullInFront()) SetFull(false);
            break;
        case kCoverTimer:
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
            WidgetTick();
            break;
        case kLangTimer: // the app switched layouts: the label follows
            KillTimer(hwnd, kLangTimer);
            SyncLang();
            Redraw();
            break;
        case kFocusTimer:
            StepFocus();
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
            Redraw();
            break;
        case kRescanTimer:
            tasks->Rescan();
            icons.Keep(tasks->Apps());
            for (const auto& a : tasks->Apps()) icons.Get(nullptr, a, IconPx()); // read new icons now, not in the middle of a frame
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
            if (GetCapture() != hwnd && !OnChrome({pt.x / scale, pt.y / scale})) CloseAll();
            break;
        }
        case kIdleTimer: Idle(); break;
        case kDevTimer: Redraw(); break;
        case kStatusTimer: {
            PollStatus();
            if (HasWg(kWgWeather)) weather.Refresh(hwnd, WM_APP_WX); // cached for half an hour: fetches only when that's up
            if (HasWg(kWgPhoto) && shadeTick % 360 == 0) PickPhoto(); // another picture each hour
            if (HasWg(kWgFiles) && shadeTick % 6 == 0 && !(wgOpen && wgKind == kWgFiles)) LoadFiles(L""); // the newest download, each minute
            if (++shadeTick % 3 == 0) SampleShade(); // a page can scroll from dark to white without any window moving; a screen read is costly, so every 30 s
            BelowIsland(); // astraeus may have started since: its hover strip goes back over the top bar
            if (auto was = promoted; SyncPromoted(), promoted != was) Redraw(); // an icon moved in or out of the taskbar in windows' settings
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
            taskbar::Hide();
            if (tray) tray->Reassert();
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
        if (wp == 0) media.Previous();
        else if (wp == 1) media.TogglePlay();
        else media.Next();
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
        if (trayOpen || (!promoted.empty() && !Unseen()) || promoted != was || (shown > 0) != (trayShown > 0)) Redraw(); // an app animating its icon costs nothing unless it's in sight, or the button comes or goes
        trayShown = shown;
        return 0;
    }
    case WM_APP_CAL:
        if (calOpen) Redraw();
        return 0;
    case WM_APP_UPDATE:
        OnUpdate(wp);
        return 0;
    case WM_APP_CALMENU:
        CalMenu();
        return 0;
    case WM_CLIPBOARDUPDATE:
        OnClipboard();
        OnClipboard2(); // the clipboard widget's history
        return 0;
    case WM_APP_HOTSPOT:
        if (!hotspot.Busy()) hotspotSwitching = false;
        if (qsOpen) Redraw();
        return 0;
    case WM_APP_APPS:
        OnApps(wp, lp);
        return 0;
    case WM_KEYDOWN:
        if (launchOpen && (wp == VK_LEFT || wp == VK_RIGHT || wp == VK_UP || wp == VK_DOWN || wp == VK_ESCAPE || wp == VK_RETURN)) LaunchKey(wp);
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
                if (cfg.pills && !Quiet() && !full) {
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
        wp == 1 ? ShowPowerMenu() : wp == 2 ? BinMenu() : wp == 3 ? ShowUserMenu() : wp == 4 ? AppMenu() : ShowMenu();
        BelowIsland(); // a menu brings its owner to the front: astraeus goes back on top
        return 0;
    case WM_WINDOWPOSCHANGED: // something raised the bar (a menu, the shell, our own topmost call): astraeus' strip goes back on top
        if (!lowering && !(reinterpret_cast<WINDOWPOS*>(lp)->flags & SWP_NOZORDER)) BelowIsland();
        break;
    case WM_APP_WX:
        wx = weather.Get();
        if (HasWg(kWgWeather)) Redraw();
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
    bool fullFrame = fullNext || springs || themes || easing || Grown() > 0.5f || OnChrome(input.mouse); // under the mouse, hovers can change

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
        bool ok = Pass(Inset(barsRect, -2, -3), [&] { // only the bars' own square: the plate, the pill's tint, the bars; cover and text stay as drawn
            PaintPlate(wegR, kWegRadius, t, shadeBotNow);
            canvas->Fill(mediaR, Height(mediaR) / 2, t.text.Alpha(0.05f)); // the pill at rest, as DrawMediaTile paints it unhovered
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
    D2D1_RECT_F cardNow = CardOnScreen(), dockArea = Inset(wegR, -24, -36); // the dock, its shadow and its tooltips
    D2D1_RECT_F top = Union(barR, cardDown ? cardNow : D2D1_RECT_F{}), bottom = Union(sink < 0.999f ? dockArea : D2D1_RECT_F{}, cardDown ? D2D1_RECT_F{} : cardNow);
    D2D1_RECT_F topDirty = Inset(Union(top, drawnTop), -2, -2), botDirty = Inset(Union(bottom, drawnBottom), -2, -2);
    // each bar draws only when something there changed: hovering the dock leaves the top bar's pixels alone, and the other way round
    bool cardMoves = springs || Grown() > 0.5f;
    bool onTop = Inside(top, input.mouse), onBot = Inside(bottom, input.mouse) || Inside(wegHit, input.mouse);
    bool drawTop = allNext || topAsk || themes || topLive || onTop || overTop || (cardDown && cardMoves) || easing || !Same(top, drawnTop);
    bool drawBot = allNext || themes || botLive || onBot || overBot || (!cardDown && cardMoves) || easing || dragging || !Same(bottom, drawnBottom);
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
    if (drawTop || drawBot) GlassMask(Chrome().Get()); // the blur follows the capsules, the dock and the panel, in the same batch as the frame
    Present();
    devTries = 0; // a frame made it to the screen
    Log("frame: full presented");
    if (!took) { // really showing: only now does the windows taskbar step aside
        took = true;
        taskbar::Take();
        ClaimTray();
        SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // once windows' taskbar has fully let go, the strip at the top is ours
        SetTimer(hwnd, kGhostTimer, 4000, nullptr); // apps had their chance to give us their icons: the rest come from windows' snapshot
        if (HMODULE u = GetModuleHandleW(L"user32.dll")) { // user32's unnamed-in-the-docs taskman: the window asked where a minimizing window flies
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
    bool ambient = ui.Ambient() || barsMoving;
    fullNext = fullNext || busy || ui.Ambient(); // only the toolbar's bars can ride on a partial frame
    needsFrame = busy || ambient;
    bool wasBarsOnly = barsOnly;
    barsOnly = !busy && !ui.Ambient() && barsMoving; // nothing but the little bars: caches can still be handed back
    if (barsOnly && !wasBarsOnly) SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr); // armed once: re-arming every frame pushed it back forever
    UINT smooth = (UINT)(std::max)(1L, std::lround(hz / 60)); // the open player's wave and light read as smooth at 60, not every refresh of a fast screen
    if (needsFrame) Arm((busy ? 1.0 : mediaOpen ? smooth : ambientSync) / hz);
    else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
    if (spectrum && now - spectrumAsked > 3) spectrum.reset(); // the player closed: capture stops
}
