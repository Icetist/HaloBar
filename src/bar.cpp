#include "bar.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <windowsx.h>

#include <cmath>
#include <cstdio>
#include <cwctype>

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
constexpr UINT kMenuRestart = 1, kMenuExit = 2, kMenuSettings = 3, kMenuTaskMgr = 4, kMenuDesktop = 5, kMenuStart = 6, kMenuUpdate = 7;

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

D2D1_RECT_F Union(const D2D1_RECT_F& a, const D2D1_RECT_F& b) {
    if (a.right <= a.left) return b;
    if (b.right <= b.left) return a;
    return {(std::min)(a.left, b.left), (std::min)(a.top, b.top), (std::max)(a.right, b.right), (std::max)(a.bottom, b.bottom)};
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
    tray.reset(); // apps fall back to explorer's tray, which has every icon
    HookCover(false);
    coverBar = nullptr;
    if (notifWatch != INVALID_HANDLE_VALUE) FindCloseChangeNotification(notifWatch);
    if (cfg.pills) Banners(false);
    card.Hide();
    tasks.reset(); // unhook before the window goes
    spectrum.reset();
    FreeDev();
    sys::CloseStatus();
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
    tbMsg = RegisterWindowMessageW(L"TaskbarCreated");
    tasks = std::make_unique<Tasks>(hwnd, WM_APP_TASKS);
    Place();
    tasks->Rescan();
    coverBar = this;
    HookCover(AutoHide());
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
    shellNow = shellTo; // start on the final colors, no fade at boot
    mediaNow = mediaTo;
    shellT = mediaT = 1;
    ArmClock();
    radios.Start(hwnd, WM_APP_RADIOS);
    SyncAudio();
    PollStatus();
    SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
    ShowWindow(hwnd, SW_SHOWNOACTIVATE);
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
        constexpr double kAmbientFps = 20; // the row's bars and other slow motion: lively at that size, a third of the cost of 30
        hz = (double)ti.rateRefresh.uiNumerator / ti.rateRefresh.uiDenominator;
        ambientSync = (UINT)(std::clamp)((int)std::lround(hz / kAmbientFps), 1, 4);
    }
    winPxW = monitor.right - monitor.left;
    winPxH = (int)std::lround(kRoom * scale);
    barPx = cfg.room ? 0 : (int)std::lround((kGap + kRowH) * scale); // reserved: maximized windows stop just above the island, as tall as the windows taskbar
    W = winPxW / scale;
    winH = winPxH / scale;
    SetWindowPos(hwnd, HWND_TOPMOST, monitor.left, monitor.bottom - winPxH, winPxW, winPxH, SWP_NOACTIVATE);
    pill = {};
    drawn = {};
    rowW = 0; // re-fit at the new size
    regionNow.clear();
    SyncRegion(); // right away: the window must never swallow clicks, even before the first frame
    Unreserve(); // the strip may have changed size, or gone: a fresh registration takes it as it is now
    Reserve();
    int needW = winPxW, needH = (int)std::lround(kRoom * dpi * kZoomMax);
    if (ctx && (needW != surfPxW || needH != surfPxH)) FreeDev(); // a new screen: the surface is sized for the old one
    else if (ctx) TrimSurface({}), fullNext = true; // only a new size: the same surface, its old pixels dropped and drawn again
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
        abd.uEdge = ABE_BOTTOM;
        abd.rc = monitor;
        abd.rc.top = monitor.bottom - barPx;
        SHAppBarMessage(ABM_QUERYPOS, &abd); // another bottom bar may push us up
        abd.rc.top = abd.rc.bottom - barPx;
        SHAppBarMessage(ABM_SETPOS, &abd); // explorer keeps the work area from here on, as it does for its own taskbar
        reserving = false;
    }
    if (tray) tray->Reassert();
}


void Bar::Unreserve() {
    if (!appbar) return;
    if (tray) tray->StepBack();
    APPBARDATA abd = {sizeof(abd)};
    abd.hWnd = hwnd;
    SHAppBarMessage(ABM_REMOVE, &abd); // or the shell keeps the strip reserved
    if (tray) tray->Reassert();
    appbar = false;
}


void Bar::SetFull(bool on) {
    if (on == full) return;
    full = on;
    ShowWindow(hwnd, on ? SW_HIDE : SW_SHOWNOACTIVATE); // games and videos get the whole screen
    if (on) SetCoalescableTimer(hwnd, kFullTimer, 2000, nullptr, 1000); // the shell's "it's over" can go missing: never stay hidden on its word alone
    else KillTimer(hwnd, kFullTimer);
    if (!on) {
        SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
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

void Bar::SyncRegion() {
    RECT r = {(LONG)std::floor(pill.left * scale) - 1, (LONG)std::floor(pill.top * scale) - 1, (LONG)std::ceil(pill.right * scale) + 1,
              (LONG)std::ceil(pill.bottom * scale) + 1};
    if (pill.right <= pill.left) r = {0, 0, 0, 0}; // nothing laid out yet: take no clicks at all
    else if (sink > 0.5f && !peek) r = {0, winPxH - 2, winPxW, winPxH}; // tucked away: only the screen's bottom edge, to call it back
    RECT corner = {winPxW - CornerPx(), winPxH - (LONG)std::lround(kRowH * scale), winPxW, winPxH}; // windows' show-desktop corner, far right
    if (regionNow.size() == 2 && EqualRect(&r, &regionNow[0]) && EqualRect(&corner, &regionNow[1])) return; // resting: set once, not every frame
    HRGN rg = CreateRectRgnIndirect(&r), rc = CreateRectRgnIndirect(&corner);
    if (rg && rc) CombineRgn(rg, rg, rc, RGN_OR);
    if (rc) DeleteObject(rc);
    if (rg && SetWindowRgn(hwnd, rg, FALSE)) regionNow = {r, corner}; // the system owns rg now
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
    card.Show(hwnd, a->key, wins, ToScreen({(hoverSlot.left + hoverSlot.right) / 2, pill.top}), scale, shellNow, med);
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
    bool was = covered;
    covered = false;
    if (AutoHide() && tasks) {
        D2D1_RECT_F o = Outer();
        RECT spot = {monitor.left + (LONG)std::floor(o.left * scale), monitor.bottom - (LONG)std::lround(kRowH * scale), monitor.left + (LONG)std::ceil(o.right * scale),
                     monitor.bottom}; // where the island rests
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

void Bar::ShowMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    std::wstring head = L"HaloBar " APP_VER_W + (updateNote.empty() ? L"" : L"  ·  " + updateNote);
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, head.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuStart, L"Windows Start");
    AppendMenuW(m, MF_STRING, kMenuTaskMgr, L"Task Manager");
    AppendMenuW(m, MF_STRING, kMenuDesktop, L"Show desktop");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kMenuSettings, L"HaloBar settings");
    AppendMenuW(m, MF_STRING, kMenuUpdate, L"Check for updates");
    AppendMenuW(m, MF_STRING, kMenuRestart, L"Restart");
    AppendMenuW(m, MF_STRING, kMenuExit, L"Exit");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_NONOTIFY, p.x, p.y, hwnd, nullptr);
    DestroyMenu(m);
    HWND back = tasks ? tasks->Fore() : nullptr; // the bar never keeps the keyboard: give it back to the app that had it
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick == kMenuStart) StartMenu();
    else if (pick == kMenuTaskMgr) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
    else if (pick == kMenuDesktop) ShowDesktop();
    else if (pick == kMenuSettings) SetSettingsOpen(true);
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
    bool launchWas = launchOpen;
    launchOpen = p == Panel::Launch;
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
    if (launchWas && !launchOpen) { // the keyboard goes back to the app that had it
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
    bool place = &field == &cfg.size || &field == &cfg.room;
    PostMessageW(hwnd, WM_APP_RESTYLE, place ? 1 : 0, 0); // never mid-frame: the canvas drawing this very button would be freed under it
    if (&field == &cfg.pills) Banners(cfg.pills != 0);
    if (&field == &cfg.song && !cfg.song && mediaOpen) SetMediaOpen(false);
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
    drawn = {}; // a new surface holds nothing yet
    fullNext = true;
    return true;
}

void Bar::FreeDev() {
    FreeGlass();
    canvas.reset();
    icons.Drop();
    trayBmps.clear();
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
    fullNext = true;
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

Theme Bar::Make(Seed s) const {
    s.look = LookNow();
    s.bg = cfg.look == 3 ? cfg.customBg : look.customBg; // the bar's own custom look, or island's
    return Theme::From(s);
}

void Bar::SyncLook() {
    look = prefs::IslandLook();
    bool follow = cfg.accent == prefs::Settings::kFollow;
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
            float lift = sink * (kRowH + 6);
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
        if (took) {
            taskbar::Take();
            ClaimTray(); // the new explorer tray came up in front of ours
            SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // its strip goes once it auto-hides: drop back to the bottom then
        }
        return 0;
    }
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return passFor.empty() && !launchOpen ? MA_NOACTIVATE : MA_ACTIVATE; // clicks never steal focus from the user's app, except while a password is typed here
    case WM_CHAR:
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
        if (LOWORD(wp) == WA_INACTIVE && launchOpen) SetPanel(Panel::None); // clicked away: the launcher goes, as start does
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
        if (sink > 0.5f && !peek) { // tucked away and the mouse came to the edge: rise
            peek = true;
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
        if (AnyOpen() && closeArmed && Inside(pill, p)) KillTimer(hwnd, kCloseTimer), closeArmed = false;
        if (ui.Captured() || ui.HitAt(p) != ui.Hovered()) Redraw(); // only when what's under the mouse changes
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = false;
        input.mouse = {-1e6f, -1e6f}; // park offscreen to clear hover
        if (peek) SetTimer(hwnd, kPeekTimer, kPeekMs, nullptr); // called up from the edge: tuck away again a moment after the mouse goes
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
        if (GET_X_LPARAM(lp) >= winPxW - CornerPx() && !Inside(Outer(), ToDip(lp))) { // the far corner: show the desktop, as windows' does
            ReleaseCapture();
            input.down = false;
            ShowDesktop();
            return 0;
        }
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
        case kLyricTimer:
            if (mediaOpen) Redraw(); // lyrics may have arrived
            break;
        case kPosTimer: {
            KillTimer(hwnd, kPosTimer);
            RECT wa{};
            SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
            Log("appbar moved");
            if (barPx > 0 && wa.bottom > monitor.bottom - barPx) Reserve(); // only if our strip really went: claiming it again tells every bar, and they answer
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
            break;
        case kPeekTimer:
            peek = false;
            Redraw();
            break;
        case kReserveTimer:
            Unreserve(); // registered while windows' taskbar still stood: explorer stacked us above it, so take the bottom again now it's hidden
            Reserve();
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
            if (Inside(mediaR, {pt.x / scale, pt.y / scale})) SetMediaOpen(true);
            break;
        }
        case kCloseTimer: {
            closeArmed = false;
            POINT pt;
            GetCursorPos(&pt);
            ScreenToClient(hwnd, &pt); // leave messages can lie, the cursor can't
            if (GetCapture() != hwnd && !Inside(pill, {pt.x / scale, pt.y / scale})) CloseAll();
            break;
        }
        case kIdleTimer: Idle(); break;
        case kDevTimer: Redraw(); break;
        case kStatusTimer:
            PollStatus();
            SetCoalescableTimer(hwnd, kStatusTimer, kStatusMs, nullptr, 1000);
            break;
        }
        return 0;
    case WM_APP_TASKS:
        if (wp == Tasks::kRescan) SetTimer(hwnd, kRescanTimer, kRescanMs, nullptr); // restarting it coalesces a burst
        else if (wp == Tasks::kTitle) { // a terminal or player can retitle every second: nothing moved, nothing to cover or relayout
            if (card.Shown()) ShowPreview();
            if (sheetOpen) Redraw(); // the app sheet is the only place a title is drawn
        } else if (wp == Tasks::kTrayShown) {
            taskbar::Hide();
            if (tray) tray->Reassert();
        } else {
            if (card.Shown()) ShowPreview(); // titles and windows change under the card
            CheckCovered();
            SyncLang(); // a different app in front may type in a different layout
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
        int shown = TrayCount();
        if (trayOpen || (shown > 0) != (trayShown > 0)) Redraw(); // an app animating its icon costs nothing unless the grid is open or the button comes or goes
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
    case WM_APP_MENU:
        ShowMenu();
        return 0;
    case WM_APP_RESTYLE:
        if (ctx && (LookNow() == 2) != (glass != nullptr)) FreeDev(); // into or out of glass: a different composition tree
        if (wp) {
            Place(); // a new size or strip
            HookCover(AutoHide());
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
    bool easing = Layout(dt); // the island's bounds for this frame, known before anything is drawn
    bool fullFrame = fullNext || springs || themes || easing || Grown() > 0.5f || Inside(Inset(pill, -4, -4), input.mouse); // under the mouse, hovers can change; a panel paints under the row too, so the bars alone would leave a patch

    if (!fullFrame) { // only the bars could have moved: step them first, and draw nothing at all if no bar moved by half a pixel
        barsMoving = StepBars(dt, now);
        bool visible = false;
        for (int i = 0; i < 4; ++i) visible = visible || std::fabs(bars[i] - barsDrawn[i]) * 11 > 0.5f;
        if (!visible) {
            needsFrame = barsMoving;
            if (needsFrame) Arm(ambientSync / hz);
            else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
            return;
        }
    }
    // a full frame repaints the island and whatever it covered last time; otherwise only the row's bars moved
    D2D1_RECT_F dirty = fullFrame ? Inset(Union(Outer(), drawn), -2, -2) : Inset(barsRect, -2, -2);
    RECT px = {(LONG)std::floor(dirty.left * scale), (LONG)std::floor(dirty.top * scale), (LONG)std::ceil(dirty.right * scale),
               (LONG)std::ceil(dirty.bottom * scale)};
    px.left = (std::max)(px.left, 0L), px.top = (std::max)(px.top, 0L);
    px.right = (std::min)(px.right, (LONG)winPxW), px.bottom = (std::min)(px.bottom, (LONG)winPxH);
    if (px.right <= px.left || px.bottom <= px.top) {
        Log("frame: nothing to draw");
        needsFrame = false;
        SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr); // Redraw took efficiency mode and the trim away: tucked out of sight, both come back
        return;
    }
    ComPtr<IDXGISurface> dxgi;
    POINT off{};
    ComPtr<ID2D1Bitmap1> tgt;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96 * scale, 96 * scale);
    if (FAILED(BeginSurface(px, &dxgi, &off))) { // device lost or display changed: rebuild
        Log("frame: begin surface failed");
        FreeDev();
        Retry();
        return;
    }
    if (FAILED(ctx->CreateBitmapFromDxgiSurface(dxgi.Get(), &bp, &tgt))) {
        EndSurface();
        FreeDev();
        Retry();
        return;
    }
    fullNext = needsFrame = false; // anything drawn below that asks for a redraw sets these again, and they stick
    ctx->SetTarget(tgt.Get());
    ctx->SetDpi(96, 96); // draw in dips, scaled by the transform: the atlas offset stays a whole pixel count, so nothing jitters at sizes like 0.9
    ctx->BeginDraw();
    ctx->SetTransform(D2D1::Matrix3x2F::Scale(scale, scale) * D2D1::Matrix3x2F::Translation((float)(off.x - px.left), (float)(off.y - px.top))); // the update sits at off in the surface's atlas
    ctx->PushAxisAlignedClip({px.left / scale, px.top / scale, px.right / scale, px.bottom / scale}, D2D1_ANTIALIAS_MODE_ALIASED);
    ctx->Clear(D2D1::ColorF(0, 0, 0, 0)); // transparent: the desktop shows through around the island
    canvas->NewFrame();
    const Theme& t = shellNow;
    ComPtr<ID2D1Geometry> shape = Shape();
    if (!fullFrame) { // only the little bars moved: the island's surface under them, then the bars, nothing else
        PaintShape(shape.Get(), t);
        DrawBars(dt, now, false); // stepped above
        ctx->PopAxisAlignedClip();
        HRESULT part = ctx->EndDraw();
        ctx->SetTarget(nullptr);
        EndSurface();
        if (part == D2DERR_RECREATE_TARGET) {
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
    ui.Begin(*canvas, input, dt, now);
    PaintShape(shape.Get(), t);
    DrawPlayer(shape.Get());
    DrawTrayGrid(t, shape.Get());
    DrawSettings(t, shape.Get());
    DrawSheet(t, shape.Get());
    DrawQuick(t, shape.Get());
    DrawCalendar(t, shape.Get());
    DrawLauncher(t, shape.Get());
    DrawCalling(t, shape.Get());
    DrawPeek(t, shape.Get());
    if (Grown() > 1) canvas->Line({pill.left + 24, rowTop}, {pill.right - 24, rowTop}, t.line.Alpha(anim::Clamp01(Grown() / 30)), 1); // island's divider ink
    DrawDividers(t);
    DrawMediaRow(t);
    DrawStart(t);
    DrawDock(t);
    DrawTrayButton(t);
    DrawLang(t);
    DrawStatus(t);
    DrawClock(t);
    DrawDesktopBtn(t);
    ui.End();
    ctx->PopAxisAlignedClip();
    HRESULT hr = ctx->EndDraw();
    ctx->SetTarget(nullptr);
    EndSurface();
    input.pressed = input.released = input.rclick = false;
    input.wheel = 0;
    midClick = false;
    if (hr == D2DERR_RECREATE_TARGET) {
        FreeDev();
        Retry();
        return;
    }
    if (fullFrame) {
        D2D1_RECT_F before = drawn;
        D2D1_RECT_F o = Outer();
        drawn = o;
        if (before.left < o.left - 1 || before.top < o.top - 1 || before.right > o.right + 1) { // the island shrank: hand its old tiles back
            RECT keep = {(LONG)std::floor((o.left - 2) * scale), (LONG)std::floor((o.top - 2) * scale), (LONG)std::ceil((o.right + 2) * scale),
                         (LONG)std::ceil((o.bottom + 2) * scale)};
            keep.left = (std::max)(keep.left, 0L), keep.top = (std::max)(keep.top, 0L);
            keep.right = (std::min)(keep.right, (LONG)winPxW), keep.bottom = (std::min)(keep.bottom, (LONG)winPxH);
            TrimSurface(keep);
        }
    }
    if (fullFrame) GlassMask(shape.Get()); // the blur follows the island's outline, in the same batch as the frame
    Present();
    devTries = 0; // a frame made it to the screen
    Log(fullFrame ? "frame: full presented" : "frame: bars presented");
    if (!took) { // the bar is really showing: only now does the windows taskbar step aside
        took = true;
        taskbar::Take();
        ClaimTray();
        SetTimer(hwnd, kReserveTimer, kReserveSettleMs, nullptr); // once windows' taskbar has fully let go, the strip at the very bottom is ours
        SetTimer(hwnd, kGhostTimer, 4000, nullptr); // apps had their chance to give us their icons: the rest come from windows' snapshot
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
    fullNext = fullNext || busy || ui.Ambient(); // only the row's bars can ride on a partial frame
    needsFrame = busy || ambient;
    bool wasBarsOnly = barsOnly;
    barsOnly = !busy && !ui.Ambient() && barsMoving; // nothing but the row's little bars: caches can still be handed back
    if (barsOnly && !wasBarsOnly) SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr); // armed once: re-arming every frame pushed it back forever
    UINT smooth = (UINT)(std::max)(1L, std::lround(hz / 60)); // the open player's wave and light read as smooth at 60, not every refresh of a fast screen
    if (needsFrame) Arm((busy ? 1.0 : mediaOpen ? smooth : ambientSync) / hz);
    else SetTimer(hwnd, kIdleTimer, kIdleMs, nullptr);
    if (spectrum && now - spectrumAsked > 3) spectrum.reset(); // the player closed: capture stops
}
