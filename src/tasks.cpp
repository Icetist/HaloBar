#include "tasks.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <propsys.h>
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <unordered_set>

#include "overflow.h"
#include "taskbar.h"

#pragma comment(lib, "version.lib") // GetFileVersionInfo, for an exe's friendly name

using Microsoft::WRL::ComPtr;

void Census(UINT what, WPARAM sub, int add = 1); // bar.cpp: DebugLog's tally of what wakes the ui thread

namespace {

Tasks* live = nullptr; // win event hooks call a plain function: this is who they're for

// what CTaskbarList in every app posts to the window named by the tray's TaskbandHWND property
constexpr UINT kTbProgress = WM_USER + 64, kTbState = WM_USER + 65, kTbOverlay = WM_USER + 79;
constexpr float kTbScale = 65534.0f; // progress arrives scaled to this
constexpr float kTbStep = 0.004f; // a quarter of a percent: finer than the ring can draw

std::wstring Title(HWND h) {
    wchar_t buf[256]; // longer titles are cut; the row never shows that much anyway
    int n = InternalGetWindowText(h, buf, 256); // never sends a message, so a hung app can't stall the bar
    return std::wstring(buf, n > 0 ? n : 0);
}

bool Cloaked(HWND h) { // on another virtual desktop, or a store app that's suspended
    DWORD c = 0;
    return SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_CLOAKED, &c, sizeof(c))) && c != 0; // any reason counts: app, shell or inherited
}

bool Eligible(HWND h) { // the rules the windows taskbar uses to give a window a button
    if (!IsWindowVisible(h) || Cloaked(h)) return false;
    LONG_PTR ex = GetWindowLongPtrW(h, GWL_EXSTYLE);
    if (!(ex & WS_EX_APPWINDOW)) { // appwindow forces a button, whatever else the style says
        if (ex & (WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE)) return false; // palettes, popups and overlays
        if (GetWindow(h, GW_OWNER)) return false; // dialogs ride on their owner's button
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return false; // the bar's own windows: menus, the preview card
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    // the desktop and explorer's taskbars are shell, not apps
    if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd") || !wcscmp(cls, L"Shell_SecondaryTrayWnd")) return false;
    wchar_t t[2];
    return InternalGetWindowText(h, t, 2) > 0; // untitled top-level windows are helpers, not apps
}

std::wstring AppId(HWND h) { // the explicit app user model id, what windows itself groups by
    ComPtr<IPropertyStore> store;
    if (FAILED(SHGetPropertyStoreForWindow(h, IID_PPV_ARGS(&store)))) return {}; // most classic apps set none: the exe groups them then
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring id;
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &v)) && v.vt == VT_LPWSTR && v.pwszVal) id = v.pwszVal;
    PropVariantClear(&v);
    return id;
}

DWORD PidOf(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    wchar_t cls[32] = {};
    GetClassNameW(h, cls, 32);
    if (!wcscmp(cls, L"ApplicationFrameWindow")) { // store apps: the frame belongs to a host process, the app runs in a child window
        struct Find { DWORD host, app; } f{pid, 0};
        EnumChildWindows(h, [](HWND c, LPARAM lp) -> BOOL {
            auto* f = reinterpret_cast<Find*>(lp);
            DWORD p = 0;
            GetWindowThreadProcessId(c, &p);
            if (p == f->host) return TRUE; // the frame's own chrome, keep looking
            f->app = p; // the first child from another process is the app
            return FALSE; // stop the enumeration
        }, reinterpret_cast<LPARAM>(&f));
        if (f.app) pid = f.app; // a suspended app has no child yet: the host stands in
    }
    return pid;
}

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); // keys are compared lowercased, so case never splits a group
    return s;
}

std::wstring FileName(const std::wstring& exe) { // the name people know an app by: its file description, else the file's name
    DWORD unused = 0, size = GetFileVersionInfoSizeW(exe.c_str(), &unused);
    if (size) {
        std::vector<BYTE> data(size);
        struct Lang { WORD lang, cp; }* langs = nullptr;
        UINT n = 0;
        if (GetFileVersionInfoW(exe.c_str(), 0, size, data.data()) &&
            VerQueryValueW(data.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&langs), &n) && n >= sizeof(Lang)) { // the first language listed
            wchar_t q[64];
            swprintf_s(q, L"\\StringFileInfo\\%04x%04x\\FileDescription", langs[0].lang, langs[0].cp);
            wchar_t* desc = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), q, reinterpret_cast<void**>(&desc), &len) && len > 1 && desc && *desc) return desc; // len counts the null
        }
    }
    size_t slash = exe.find_last_of(L"\\/"); // no description: "notepad" from its path
    std::wstring file = exe.substr(slash == std::wstring::npos ? 0 : slash + 1);
    size_t dot = file.find_last_of(L'.');
    return dot == std::wstring::npos ? file : file.substr(0, dot);
}

std::wstring ShellName(const std::wstring& aumid) { // a store or packaged app's own display name
    ComPtr<IShellItem> item;
    PWSTR name = nullptr;
    std::wstring out;
    if (SUCCEEDED(SHCreateItemFromParsingName((L"shell:AppsFolder\\" + aumid).c_str(), nullptr, IID_PPV_ARGS(&item))) && // the start menu's own entry
        SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)))
        out = name;
    CoTaskMemFree(name); // null is fine when the lookup failed
    return out;
}

}

Tasks::Tasks(HWND n, UINT m) : notify(n), msg(m) {
    live = this;
    shellMsg = RegisterWindowMessageW(L"SHELLHOOK"); // the message id the shell hook arrives under
    RegisterShellHookWindow(notify); // window created and destroyed, activated, and flashing for attention
    auto hook = [&](DWORD from, DWORD to) { // out of context: no dll injected, delivered on our thread
        if (HWINEVENTHOOK h = SetWinEventHook(from, to, nullptr, OnEvent, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS)) hooks.push_back(h);
    };
    hook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND); // the app in front gets the accent capsule
    hook(EVENT_OBJECT_DESTROY, EVENT_OBJECT_HIDE); // destroy, show, hide: separate ranges keep the cursor's location spam out
    hook(EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE); // titles, and windows that only become eligible once named
    hook(EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED); // virtual desktop switches
    fore = GetForegroundWindow();
    pinList = pins::Load(); // explorer's own pins, read from its registry lists
}

Tasks::~Tasks() {
    for (auto& [h, b] : badges)
        if (b.overlay) DestroyIcon(b.overlay); // our copies, made in OnTaskband
    for (void* h : hooks) UnhookWinEvent(static_cast<HWINEVENTHOOK>(h));
    DeregisterShellHookWindow(notify);
    for (HWND h : others) DeregisterShellHookWindow(h);
    if (live == this) live = nullptr; // a late event after unhooking finds nobody
}

void Tasks::Listen(HWND h) {
    if (!h || h == notify || std::find(others.begin(), others.end(), h) != others.end()) return;
    others.push_back(h);
    RegisterShellHookWindow(h); // its own minimize targets: windows asks every shell hook window where a window flies
}

void Tasks::Unlisten(HWND h) {
    auto it = std::find(others.begin(), others.end(), h);
    if (it == others.end()) return;
    DeregisterShellHookWindow(h);
    others.erase(it);
}

void Tasks::Ping(WPARAM what) const {
    PostMessageW(notify, msg, what, 0);
    if (what == kRescan || what == kTrayShown) return; // the main dock's alone: it walks the windows and hides explorer's taskbar
    for (HWND h : others) PostMessageW(h, msg, what, 0);
}

void CALLBACK Tasks::OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD) {
    Census(0xF0003, ev);
    if (!live || !h || obj != OBJID_WINDOW || child != CHILDID_SELF) return; // the window itself, not a control inside it
    live->Event(ev, h);
}

void Tasks::Event(DWORD ev, HWND h) {
    switch (ev) {
    case EVENT_SYSTEM_FOREGROUND:
        fore = h;
        if (TaskWin* w = Find(h)) {
            w->used = GetTickCount64(); // a click on its app goes back to the window used last
            w->flash = false; // it got what it asked for
        } else if (Eligible(h)) {
            Ping(kRescan); // a window we haven't seen yet
        }
        Ping(kChanged);
        return;
    case EVENT_OBJECT_NAMECHANGE:
        if (GetAncestor(h, GA_ROOT) != h) return; // a child's name, a tab or a field: not a title
        if (TaskWin* w = Find(h)) {
            std::wstring t = Title(h);
            if (t != w->title) {
                w->title = std::move(t);
                if (h == fore) Ping(kTitle); // only the front window's title is on screen
            }
        } else if (Eligible(h)) {
            Ping(kRescan); // got its first title
        }
        return;
    case EVENT_OBJECT_SHOW:
        if (overflow::Is(h)) {
            Ping(kOverflow); // explorer opened its flyout over its own hidden chevron: the bar moves it over its button
            return;
        }
        if (taskbar::IsTray(h) || taskbar::IsSecondary(h)) {
            Ping(kTrayShown); // explorer brought a taskbar back, on this screen or another: hide it again
            return;
        }
        if (GetAncestor(h, GA_ROOT) == h) { // astraeus started after us: the bar steps under it at once
            wchar_t cls[32] = {};
            if (GetClassNameW(h, cls, 32) && !wcscmp(cls, L"AstraeusDrawer")) {
                Ping(kIsland);
                return;
            }
        }
        [[fallthrough]];
    case EVENT_OBJECT_UNCLOAKED:
        if (GetAncestor(h, GA_ROOT) == h && !Find(h) && Eligible(h)) Ping(kRescan); // only new top-level windows are worth a scan
        return;
    case EVENT_OBJECT_HIDE:
        if (overflow::Is(h)) {
            Ping(kOverflow); // closed: the chevron turns back, focus goes home
            return;
        }
        [[fallthrough]];
    case EVENT_OBJECT_DESTROY:
    case EVENT_OBJECT_CLOAKED:
        if (Find(h)) Ping(kRescan); // ignored unless it was one of ours: hides and destroys are constant
        return;
    }
}

bool Tasks::OnShell(UINT m, WPARAM code, LPARAM lp) {
    if (m != shellMsg || !shellMsg) return false; // not ours: the window proc goes on with it
    HWND h = reinterpret_cast<HWND>(lp);
    if (code == HSHELL_FLASH) {
        if (TaskWin* w = Find(h)) {
            if (h != fore && !w->flash) { // repeat flashes of one call change nothing on screen
                w->flash = true;
                Ping(kChanged);
            }
        }
    } else if (code == HSHELL_WINDOWACTIVATED || code == HSHELL_RUDEAPPACTIVATED) { // rude: the same, sent when a fullscreen app is around
        if (TaskWin* w = Find(h); w && w->flash) {
            w->flash = false;
            Ping(kChanged);
        }
    } else if (code == HSHELL_WINDOWCREATED || code == HSHELL_WINDOWDESTROYED) {
        Ping(kRescan);
    }
    return true;
}

TaskWin* Tasks::Find(HWND h) {
    for (auto& a : apps)
        for (auto& w : a.wins)
            if (w.hwnd == h) return &w;
    return nullptr;
}

int Tasks::AppOf(HWND h) const {
    for (size_t i = 0; i < apps.size(); ++i)
        for (const auto& w : apps[i].wins)
            if (w.hwnd == h) return (int)i;
    return -1;
}

int Tasks::Calling(HWND* who) const {
    for (size_t i = 0; i < apps.size(); ++i)
        for (const auto& w : apps[i].wins)
            if (w.flash && w.hwnd != fore) { // the first app asking for attention, in row order
                if (who) *who = w.hwnd;
                return (int)i;
            }
    return -1;
}

void Tasks::Describe(HWND h, TaskApp& a) {
    DWORD pid = PidOf(h);
    auto it = exes.find(pid); // cached per process: opening it again for every window is wasteful
    if (it == exes.end()) {
        std::wstring path;
        if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) { // limited: the most an unelevated process gets on most apps
            wchar_t buf[MAX_PATH * 2]; // room for long paths under program files
            DWORD n = MAX_PATH * 2;
            if (QueryFullProcessImageNameW(p, 0, buf, &n)) path.assign(buf, n);
            CloseHandle(p);
        }
        it = exes.emplace(pid, std::move(path)).first; // kept even when empty, so a denied process isn't retried
    }
    a.exe = it->second;
    a.aumid = AppId(h);
    a.key = pins::WindowKey(h); // as explorer groups it, so its pin finds it
    if (a.key.empty()) a.key = Lower(!a.aumid.empty() ? a.aumid : !a.exe.empty() ? a.exe : L"pid:" + std::to_wstring(pid)); // elevated apps hide their path: one group per process
    auto n = names.find(a.key); // a name is looked up once per app, not per window
    if (n == names.end()) {
        std::wstring name = !a.aumid.empty() ? ShellName(a.aumid) : L"";
        if (name.empty() && !a.exe.empty()) name = FileName(a.exe);
        if (name.empty()) name = Title(h); // last resort: what the window calls itself
        n = names.emplace(a.key, std::move(name)).first;
    }
    a.name = n->second;
}

void Tasks::Rescan() {
    std::vector<HWND> found;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        if (Eligible(h)) reinterpret_cast<std::vector<HWND>*>(lp)->push_back(h);
        return TRUE;
    }, reinterpret_cast<LPARAM>(&found));
    std::unordered_set<HWND> alive(found.begin(), found.end()), kept; // kept: windows already placed under a known app

    std::vector<TaskApp> next; // known apps keep their place and their windows' order, so nothing jumps
    for (auto& a : apps) {
        TaskApp b{a.key, a.exe, a.aumid, a.name, {}, false, {}}; // pinned state and pidl are put back by Merge
        for (auto& w : a.wins)
            if (alive.count(w.hwnd)) {
                w.title = Title(w.hwnd); // name changes of background windows aren't tracked live, so refresh here
                b.wins.push_back(std::move(w));
                kept.insert(b.wins.back().hwnd);
            }
        if (!b.wins.empty()) next.push_back(std::move(b)); // all its windows closed: the app leaves the row (unless pinned)
    }
    for (auto it = found.rbegin(); it != found.rend(); ++it) { // enum runs front to back: the oldest windows come last, add them first
        HWND h = *it;
        if (kept.count(h)) continue;
        TaskApp desc;
        Describe(h, desc);
        auto app = std::find_if(next.begin(), next.end(), [&](const TaskApp& a) { return a.key == desc.key; }); // a new window of an app already open joins it
        if (app == next.end()) {
            next.push_back(std::move(desc));
            app = next.end() - 1;
        }
        app->wins.push_back({h, Title(h), false, h == fore ? GetTickCount64() : 0}); // not flashing; used now if it's the one in front
    }
    Merge(std::move(next));
    for (auto it = badges.begin(); it != badges.end();) { // a closed window's progress and badge go with it
        if (alive.count(it->first)) {
            ++it;
            continue;
        }
        if (it->second.overlay) DestroyIcon(it->second.overlay);
        it = badges.erase(it); // erase hands back the next one
    }

    std::unordered_set<DWORD> pids; // forget processes that are gone, a reused pid must not inherit a path
    for (HWND h : found) pids.insert(PidOf(h));
    for (auto it = exes.begin(); it != exes.end();) it = pids.count(it->first) ? std::next(it) : exes.erase(it);
    Ping(kChanged);
}

void Tasks::Activate(HWND h) {
    WINDOWPLACEMENT wp = {sizeof(wp)};
    if (GetWindowPlacement(h, &wp) && !MonitorFromRect(&wp.rcNormalPosition, MONITOR_DEFAULTTONULL)) { // lost off every screen (one was unplugged): back onto the main one
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(MonitorFromPoint({0, 0}, MONITOR_DEFAULTTOPRIMARY), &mi);
        LONG w = (std::min)(wp.rcNormalPosition.right - wp.rcNormalPosition.left, mi.rcWork.right - mi.rcWork.left);
        LONG ht = (std::min)(wp.rcNormalPosition.bottom - wp.rcNormalPosition.top, mi.rcWork.bottom - mi.rcWork.top);
        wp.rcNormalPosition = {mi.rcWork.left + 40, mi.rcWork.top + 40, mi.rcWork.left + 40 + w, mi.rcWork.top + 40 + ht};
        if (wp.showCmd == SW_SHOWMINIMIZED) wp.showCmd = SW_RESTORE;
        SetWindowPlacement(h, &wp);
    }
    if (IsIconic(h)) ShowWindowAsync(h, SW_RESTORE); // async: a hung app can't stall the bar
    if (!SetForegroundWindow(h)) SwitchToThisWindow(h, TRUE); // the click was ours, so windows normally lets us; this covers the rest
}

void Tasks::Merge(std::vector<TaskApp> running) {
    std::vector<TaskApp> out;
    out.reserve(pinList.size() + running.size());
    for (const auto& p : pinList) { // pins first, in their own order, as the windows taskbar
        auto r = std::find_if(running.begin(), running.end(), [&](const TaskApp& a) { return !a.wins.empty() && a.key == p.key; });
        TaskApp a;
        if (r != running.end()) {
            a = std::move(*r);
            r->wins.clear(); // taken
        } else {
            a.key = p.key; // not running: just the pin, no windows
        }
        a.pinned = true;
        a.pidl = p.pidl; // what a click starts when no window is open
        if (!p.name.empty()) a.name = p.name; // the shortcut's name, as the pin shows it
        out.push_back(std::move(a));
    }
    for (auto& a : running)
        if (!a.wins.empty()) {
            a.pinned = false; // it may have just been unpinned
            a.pidl.clear();
            out.push_back(std::move(a));
        }
    apps.swap(out);
}

void Tasks::ReloadPins() {
    if (pins::Dirty()) return; // ours are newer than what explorer last wrote: kept until it reads them
    pinList = pins::Load();
    Merge(std::move(apps)); // running windows regroup under the new pins
    Ping(kChanged);
}

void Tasks::SavePins() {
    pins::Save(pinList);
    pins::SetDirty(true); // explorer only reads them as it starts: it's restarted once the bar steps aside
}

bool Tasks::Pin(int i, int at) {
    if (i < 0 || i >= (int)apps.size() || apps[i].pinned) return false;
    const TaskApp& a = apps[i];
    PinItem p;
    if (!pins::Make(a.key, a.aumid, a.exe, a.name, p)) return false; // nothing to start it from, e.g. an elevated app with a hidden path
    p.key = a.key; // the running windows' own key, so they stay together under it
    int n = (int)pinList.size();
    pinList.insert(pinList.begin() + (at < 0 || at > n ? n : at), std::move(p)); // no place given: after the last pin
    SavePins();
    Merge(std::move(apps));
    Ping(kChanged);
    return true;
}

bool Tasks::Unpin(int i) {
    if (i < 0 || i >= (int)apps.size() || !apps[i].pinned) return false;
    auto it = std::find_if(pinList.begin(), pinList.end(), [&](const PinItem& p) { return p.key == apps[i].key; });
    if (it == pinList.end()) return false;
    PinItem gone = std::move(*it);
    pinList.erase(it);
    if (pins::Dirty() || !pins::UnpinLive(gone)) { // live when explorer's list is still the same as ours
        SavePins();
        pins::Trash(gone); // its shortcut goes too, as explorer's own unpin does
    }
    Merge(std::move(apps));
    Ping(kChanged);
    return true;
}

bool Tasks::MovePin(int from, int to) {
    int n = (int)pinList.size();
    if (from < 0 || from >= n || to < 0 || to >= n || from == to) return false;
    PinItem p = std::move(pinList[from]);
    pinList.erase(pinList.begin() + from);
    pinList.insert(pinList.begin() + to, std::move(p));
    SavePins();
    Merge(std::move(apps));
    Ping(kChanged);
    return true;
}

bool Tasks::OnTaskband(UINT m, WPARAM wp, LPARAM lp) {
    if (m != kTbProgress && m != kTbState && m != kTbOverlay) return false;
    HWND h = reinterpret_cast<HWND>(wp);
    if (!h || !IsWindow(h)) return true; // handled, just about a window that's gone
    Badge& b = badges[h];
    if (m == kTbProgress) {
        float v = (std::min)(1.0f, (float)(DWORD)lp / kTbScale);
        bool seen = std::fabs(v - b.value) >= kTbStep || (v != b.value && (v == 0 || v == 1)); // a download reports many times a second: only a step the ring shows wakes the docks
        if (seen) b.value = v;
        if (!seen) return true;
    } else if (m == kTbState) {
        if (b.state == (int)lp) return true; // apps repeat their state with every update
        b.state = (int)lp; // TBPF_ flags: 0 none, 1 indeterminate, 2 normal, 4 error, 8 paused
    } else {
        if (b.overlay) DestroyIcon(b.overlay);
        b.overlay = lp ? CopyIcon(reinterpret_cast<HICON>(lp)) : nullptr; // the sender may free its icon at any time
        ++b.ver; // tells the dock its cached bitmap is stale
    }
    Ping(kBadge);
    return true;
}

const Badge* Tasks::BadgeOf(const TaskApp& a) const {
    const Badge* best = nullptr;
    for (const auto& w : a.wins) {
        auto it = badges.find(w.hwnd);
        if (it == badges.end()) continue;
        const Badge& b = it->second;
        if (!best || (b.state && !best->state) || (b.overlay && !best->overlay)) best = &b; // one icon, several windows: the one with something to show wins
    }
    return best && (best->state || best->overlay) ? best : nullptr;
}

void Tasks::CloseAll(int i) {
    if (i < 0 || i >= (int)apps.size()) return;
    for (const auto& w : apps[i].wins) PostMessageW(w.hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); // as the window's own close button
}

void Tasks::Click(int i) {
    if (i < 0 || i >= (int)apps.size()) return;
    if (apps[i].wins.empty()) { // a pin that isn't running: start it
        Launch(i);
        return;
    }
    const auto& wins = apps[i].wins;
    auto front = std::find_if(wins.begin(), wins.end(), [&](const TaskWin& w) { return w.hwnd == fore; });
    if (front != wins.end() && IsIconic(fore)) { // in front only on paper: it was minimized and nothing took over, so bring it back
        Activate(fore);
        return;
    }
    if (front != wins.end()) {
        if (wins.size() == 1) ShowWindowAsync(fore, SW_MINIMIZE); // its only window is in front: tuck it away
        else Activate((front + 1 == wins.end() ? wins.front() : *(front + 1)).hwnd); // several: step to the next one
        return;
    }
    // not in front: bring back the window used last
    auto recent = std::max_element(wins.begin(), wins.end(), [](const TaskWin& a, const TaskWin& b) { return a.used < b.used; });
    Activate(recent->hwnd);
}

namespace {

std::wstring LinkTarget(const std::vector<BYTE>& pidl) { // a pin's program: the shortcut it is, followed
    wchar_t path[MAX_PATH] = {};
    if (pidl.empty() || !SHGetPathFromIDListW((PCIDLIST_ABSOLUTE)pidl.data(), path)) return L"";
    size_t n = wcslen(path);
    if (n < 4 || _wcsicmp(path + n - 4, L".lnk")) return path;
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    wchar_t target[MAX_PATH] = {};
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file)) || FAILED(file->Load(path, STGM_READ)) ||
        FAILED(link->GetPath(target, MAX_PATH, nullptr, 0)))
        return L"";
    return target;
}

}

bool Tasks::CanOpen(int i) const {
    if (i < 0 || i >= (int)apps.size()) return false;
    const TaskApp& a = apps[i];
    return !a.exe.empty() || !a.aumid.empty() || !a.pidl.empty();
}

bool Tasks::OpenWith(int i, const std::vector<std::wstring>& files) {
    if (files.empty() || !CanOpen(i)) return false;
    const TaskApp& a = apps[i];
    if (!a.aumid.empty()) { // a store app takes files through its activation, not a command line; a desktop one says no and falls through
        std::vector<PIDLIST_ABSOLUTE> ids;
        for (const auto& f : files)
            if (PIDLIST_ABSOLUTE id = nullptr; SUCCEEDED(SHParseDisplayName(f.c_str(), nullptr, &id, 0, nullptr))) ids.push_back(id);
        Microsoft::WRL::ComPtr<IShellItemArray> items;
        Microsoft::WRL::ComPtr<IApplicationActivationManager> am;
        DWORD pid = 0;
        bool ok = !ids.empty() && SUCCEEDED(SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE_ARRAY)ids.data(), &items)) &&
                  SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&am))) &&
                  SUCCEEDED(am->ActivateForFile(a.aumid.c_str(), items.Get(), L"open", &pid));
        for (auto id : ids) CoTaskMemFree(id);
        if (ok) return true;
    }
    std::wstring exe = !a.exe.empty() ? a.exe : LinkTarget(a.pidl);
    if (exe.empty()) return false;
    std::wstring args;
    for (const auto& f : files) args += L"\"" + f + L"\" "; // each path quoted: spaces stay inside one argument
    return (INT_PTR)ShellExecuteW(nullptr, L"open", exe.c_str(), args.c_str(), nullptr, SW_SHOWNORMAL) > 32;
}

void Tasks::Launch(int i, bool admin) {
    if (i < 0 || i >= (int)apps.size()) return;
    const TaskApp& a = apps[i];
    const wchar_t* verb = admin ? L"runas" : nullptr; // windows asks for consent itself
    if (!a.pidl.empty()) { // pinned: exactly what the windows taskbar would start
        SHELLEXECUTEINFOW ex = {sizeof(ex)};
        ex.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;
        ex.lpIDList = (void*)a.pidl.data();
        ex.lpVerb = verb;
        ex.nShow = SW_SHOWNORMAL;
        if (ShellExecuteExW(&ex)) return;
    }
    std::wstring target = !a.aumid.empty() ? L"shell:AppsFolder\\" + a.aumid : a.exe;
    if (!target.empty()) ShellExecuteW(nullptr, admin && a.aumid.empty() ? L"runas" : L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}
