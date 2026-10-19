#include "tray.h"

#include <shellapi.h>

#include <algorithm>
#include <cstddef>

#include "prefs.h"
#include "taskbar.h"

namespace {

constexpr DWORD kTraySig = 0x34753423; // what Shell_NotifyIcon stamps its messages with
constexpr ULONG_PTR kNotifyData = 1; // WM_COPYDATA kinds the tray gets: 0 appbar, 1 notify icon, 3 icon position

struct NotifyData32 { // NOTIFYICONDATAW as Shell_NotifyIcon sends it: handles as 32 bits, from 32 and 64 bit apps alike
    DWORD cbSize;
    DWORD hWnd; // window handles are 32 bit values on every windows, so this is lossless
    UINT uID;
    UINT uFlags;
    UINT uCallbackMessage;
    DWORD hIcon; // icons too: they live in a table every process shares
    WCHAR szTip[128];
    DWORD dwState;
    DWORD dwStateMask;
    WCHAR szInfo[256]; // balloon text: windows turns these into toasts, so the bar ignores them
    UINT uVersion; // shares its slot with uTimeout
    WCHAR szInfoTitle[64];
    DWORD dwInfoFlags;
    GUID guidItem; // set with NIF_GUID: the icon's identity then, not owner and id
    DWORD hBalloonIcon;
};

struct TrayData {
    DWORD sig; // kTraySig, or it's not a notify icon call
    DWORD msg; // NIM_*
    NotifyData32 nid;
};

constexpr DWORD kV1Size = offsetof(NotifyData32, szTip) + 64 * sizeof(WCHAR); // the oldest apps: a 64 character tip, nothing after
constexpr DWORD kGuidEnd = offsetof(NotifyData32, guidItem) + sizeof(GUID); // a smaller struct predates guids, whatever its flags claim

uint64_t KeyOf(const TrayItem& it) {
    uint64_t h = 1469598103934665603ull; // fnv-1a over what identifies the icon
    auto mix = [&](const void* p, size_t n) {
        for (size_t i = 0; i < n; ++i) h = (h ^ static_cast<const uint8_t*>(p)[i]) * 1099511628211ull;
    };
    if (it.hasGuid) mix(&it.guid, sizeof(GUID)); // a guid survives the app restarting
    else mix(&it.owner, sizeof(it.owner)), mix(&it.id, sizeof(it.id)); // otherwise the window and the app's own number
    return h;
}

std::wstring ExeOf(HWND w) { // the full path of the app behind a window, empty if it can't be read
    DWORD pid = 0;
    GetWindowThreadProcessId(w, &pid);
    wchar_t exe[MAX_PATH * 2] = {};
    DWORD n = MAX_PATH * 2;
    HANDLE p = pid ? OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid) : nullptr; // fails for elevated apps: then empty
    bool ok = p && QueryFullProcessImageNameW(p, 0, exe, &n);
    if (p) CloseHandle(p);
    return ok ? std::wstring(exe, n) : std::wstring();
}

std::wstring SavePath() { // %LOCALAPPDATA%\HaloBar\tray.dat
    wchar_t base[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base, MAX_PATH);
    if (!n || n >= MAX_PATH) return L""; // unset or too long: the tray just isn't remembered
    std::wstring dir = std::wstring(base) + L"\\HaloBar";
    CreateDirectoryW(dir.c_str(), nullptr); // fails harmlessly when it's already there
    return dir + L"\\tray.dat";
}

constexpr uint32_t kSaveMagic = 0x31544248; // "HBT1"
constexpr int kSaveMaxPx = 64; // icons are 16 to 32 px: anything bigger is a broken file

bool IconPixels(HICON icon, int& w, int& h, std::vector<uint32_t>& px) { // premultiplied bgra, alpha from the mask for old icons
    ICONINFO ii{};
    if (!icon || !GetIconInfo(icon, &ii)) return false; // GetIconInfo hands us copies of both bitmaps, freed below
    BITMAP bm{};
    bool ok = ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(bm), &bm) && bm.bmWidth > 0 && bm.bmWidth <= kSaveMaxPx && bm.bmHeight > 0 && bm.bmHeight <= kSaveMaxPx;
    if (ok) {
        w = bm.bmWidth, h = bm.bmHeight;
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), w, -h, 1, 32, BI_RGB}; // negative height: rows top down
        px.assign((size_t)w * h, 0);
        HDC dc = GetDC(nullptr);
        ok = GetDIBits(dc, ii.hbmColor, 0, h, px.data(), &bi, DIB_RGB_COLORS) == h; // every row, or the icon isn't saved
        bool alpha = std::any_of(px.begin(), px.end(), [](uint32_t p) { return (p >> 24) != 0; });
        if (ok && !alpha && ii.hbmMask) { // no alpha channel: the mask says what's opaque
            std::vector<uint32_t> mask((size_t)w * h, 0);
            if (GetDIBits(dc, ii.hbmMask, 0, h, mask.data(), &bi, DIB_RGB_COLORS) == h)
                for (size_t i = 0; i < px.size(); ++i) px[i] = (mask[i] & 0xFFFFFF) ? 0 : px[i] | 0xFF000000; // a set mask bit is transparent
        }
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask) DeleteObject(ii.hbmMask);
    return ok;
}

HICON IconFromPixels(int w, int h, const std::vector<uint32_t>& px) {
    BITMAPV5HEADER bh{}; // a v5 header, so gdi keeps the alpha
    bh.bV5Size = sizeof(bh), bh.bV5Width = w, bh.bV5Height = -h, bh.bV5Planes = 1, bh.bV5BitCount = 32, bh.bV5Compression = BI_BITFIELDS;
    bh.bV5RedMask = 0x00FF0000, bh.bV5GreenMask = 0x0000FF00, bh.bV5BlueMask = 0x000000FF, bh.bV5AlphaMask = 0xFF000000;
    void* bits = nullptr;
    HDC dc = GetDC(nullptr);
    HBITMAP color = CreateDIBSection(dc, reinterpret_cast<BITMAPINFO*>(&bh), DIB_RGB_COLORS, &bits, nullptr, 0);
    ReleaseDC(nullptr, dc);
    if (!color) return nullptr;
    memcpy(bits, px.data(), px.size() * sizeof(uint32_t));
    HBITMAP mask = CreateBitmap(w, h, 1, 1, nullptr); // all opaque: the alpha channel decides
    ICONINFO ii{TRUE, 0, 0, mask, color};
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(color);
    DeleteObject(mask);
    return icon;
}

struct Writer { // the save file is raw little-endian fields, read back by Reader in the same order
    std::vector<uint8_t> buf;
    void Raw(const void* p, size_t n) { buf.insert(buf.end(), static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n); }
    template <class T> void Put(const T& v) { Raw(&v, sizeof(v)); }
    void Str(const std::wstring& s) { Put((uint32_t)s.size()), Raw(s.data(), s.size() * sizeof(wchar_t)); } // length first, no terminator
};

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true; // sticks at false: one short read spoils the rest, checked once per icon
    void Raw(void* out, size_t n) {
        if (!ok || (size_t)(end - p) < n) { ok = false; return; }
        memcpy(out, p, n), p += n;
    }
    template <class T> T Get() { T v{}; Raw(&v, sizeof(v)); return v; }
    std::wstring Str() {
        uint32_t n = Get<uint32_t>();
        if (!ok || n > 4096) { ok = false; return {}; } // a path or tip is never that long: a damaged file
        std::wstring s(n, L'\0');
        Raw(s.data(), n * sizeof(wchar_t));
        return s;
    }
};

}

TrayHost::TrayHost(HWND n, UINT m) : notify(n), msg(m) {
    debug = prefs::ReadNum(L"DebugTray", 0) != 0; // off unless set by hand in the registry
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = Proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"Shell_TrayWnd"; // the class Shell_NotifyIcon looks for
    RegisterClassExW(&wc); // explorer's own class lives in its process, so ours registers fine
    hwnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, wc.lpszClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr,
                           wc.hInstance, this); // never shown: it only receives
    Restore(); // last run's icons, before any app hears of us
}

TrayHost::~TrayHost() {
    Save(); // the next start picks these up: apps that keep running never add their icons again
    if (hwnd) RemovePropW(hwnd, L"TaskbandHWND"), DestroyWindow(hwnd); // apps go back to explorer's tray, which has every icon already
    for (auto& it : items) Free(it);
}

void TrayHost::Save() const {
    std::wstring path = SavePath();
    if (path.empty()) return;
    Writer w;
    w.Put(kSaveMagic);
    w.Put((uint32_t)items.size());
    for (const auto& it : items) {
        int iw = 0, ih = 0;
        std::vector<uint32_t> px;
        if (!IconPixels(it.icon, iw, ih, px)) iw = ih = 0, px.clear(); // saved without a picture: the app's next update brings one
        w.Put((uint64_t)(ULONG_PTR)it.owner), w.Put(it.id), w.Put(it.guid), w.Put((uint8_t)it.hasGuid);
        w.Put(it.callback), w.Put(it.version), w.Put((uint8_t)it.hidden), w.Put((uint8_t)it.system);
        w.Str(ExeOf(it.owner)), w.Str(it.tip); // the exe proves on restore that the handle still means the same app
        w.Put((int32_t)iw), w.Put((int32_t)ih), w.Raw(px.data(), px.size() * sizeof(uint32_t));
    }
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr); // rewritten whole each time; it's a few kb
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD done = 0;
    WriteFile(f, w.buf.data(), (DWORD)w.buf.size(), &done, nullptr);
    CloseHandle(f);
}

void TrayHost::Restore() { // icons from the last run whose app still runs, with the very window that owns them: clicks and menus reach the app itself
    std::wstring path = SavePath();
    HANDLE f = path.empty() ? INVALID_HANDLE_VALUE : CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER size{};
    std::vector<uint8_t> buf;
    if (GetFileSizeEx(f, &size) && size.QuadPart > 0 && size.QuadPart < (8 << 20)) { // 8 mb: far beyond any real tray
        buf.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) || got != buf.size()) buf.clear();
    }
    CloseHandle(f);
    Reader r{buf.data(), buf.data() + buf.size()};
    if (buf.empty() || r.Get<uint32_t>() != kSaveMagic) return; // an older format or another file: start empty
    uint32_t n = r.Get<uint32_t>();
    for (uint32_t i = 0; i < n && r.ok && i < 256; ++i) { // the count comes from the file, so it's capped
        TrayItem it;
        it.owner = (HWND)(ULONG_PTR)r.Get<uint64_t>();
        it.id = r.Get<UINT>(), it.guid = r.Get<GUID>(), it.hasGuid = r.Get<uint8_t>() != 0;
        it.callback = r.Get<UINT>(), it.version = r.Get<UINT>(), it.hidden = r.Get<uint8_t>() != 0, it.system = r.Get<uint8_t>() != 0;
        std::wstring exe = r.Str();
        it.tip = r.Str();
        int iw = r.Get<int32_t>(), ih = r.Get<int32_t>();
        if (!r.ok || iw < 0 || ih < 0 || iw > kSaveMaxPx || ih > kSaveMaxPx) break; // the rest can't be trusted either
        std::vector<uint32_t> px((size_t)iw * ih);
        r.Raw(px.data(), px.size() * sizeof(uint32_t));
        if (!r.ok) break;
        if (exe.empty() || !IsWindow(it.owner) || _wcsicmp(ExeOf(it.owner).c_str(), exe.c_str()) != 0) continue; // gone, or the handle now belongs to another app
        bool dup = std::any_of(items.begin(), items.end(), [&](const TrayItem& o) {
            return it.hasGuid ? o.hasGuid && IsEqualGUID(o.guid, it.guid) : !o.hasGuid && o.owner == it.owner && o.id == it.id;
        });
        if (dup) continue; // the same icon twice in a damaged file
        if (iw > 0) it.icon = IconFromPixels(iw, ih, px);
        it.ver = 1; // non zero, so the row makes a bitmap for it
        it.key = KeyOf(it);
        items.push_back(std::move(it));
    }
    if (!items.empty()) PostMessageW(notify, msg, 0, 0); // the bar draws them once its window is ready
}

void TrayHost::Free(TrayItem& it) {
    if (it.icon) DestroyIcon(it.icon);
    it.icon = nullptr;
}

void TrayHost::Reassert() {
    if (!hwnd) return;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // first in z order: FindWindow finds us, not explorer
    if (HWND ex = taskbar::ExplorerTray()) SetWindowPos(ex, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // and explorer's behind us
}

void TrayHost::StepBack() {
    if (!hwnd) return;
    if (HWND ex = taskbar::ExplorerTray()) SetWindowPos(ex, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // hidden, so nothing shows
    SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE); // apps that look now find explorer first
}

void TrayHost::Claim() {
    if (!hwnd) return;
    Reassert(); // in front first, or the broadcast below reaches explorer again
    SetPropW(hwnd, L"TaskbandHWND", notify); // apps' ITaskbarList3 (progress, badges) find the taskband through the first tray window: us
    for (UINT m : {WM_USER + 64, WM_USER + 65, WM_USER + 79}) ChangeWindowMessageFilterEx(notify, m, MSGFLT_ALLOW, nullptr); // even if the bar runs elevated
    UINT created = RegisterWindowMessageW(L"TaskbarCreated");
    PostMessageW(HWND_BROADCAST, created, 0, 0); // every app adds its icons again, now to us
    for (HWND h = nullptr; (h = FindWindowExW(HWND_MESSAGE, h, nullptr, nullptr));) PostMessageW(h, created, 0, 0); // and the ones that keep their icon on a message-only window, which a broadcast never reaches
}

int TrayHost::Visible() const {
    return (int)std::count_if(items.begin(), items.end(), [](const TrayItem& it) { return it.Shown(); });
}

void TrayHost::Prune() {
    size_t before = items.size();
    for (auto& it : items)
        if (!IsWindow(it.owner)) Free(it); // an app that quit or crashed without removing its icon
    items.erase(std::remove_if(items.begin(), items.end(), [](const TrayItem& it) { return !IsWindow(it.owner); }), items.end());
    if (items.size() != before) PostMessageW(notify, msg, 0, 0); // only a real change costs a frame
}

LRESULT CALLBACK TrayHost::Proc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    if (m == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)((CREATESTRUCTW*)lp)->lpCreateParams); // the TrayHost passed to CreateWindowExW
    auto* self = (TrayHost*)GetWindowLongPtrW(h, GWLP_USERDATA);
    if (self && m == WM_COPYDATA && lp) return self->OnCopy((HWND)wp, (const COPYDATASTRUCT*)lp);
    return DefWindowProcW(h, m, wp, lp);
}

LRESULT TrayHost::Forward(WPARAM wp, LPARAM lp) const {
    HWND ex = taskbar::ExplorerTray();
    DWORD_PTR r = 0;
    if (!ex || !SendMessageTimeoutW(ex, WM_COPYDATA, wp, lp, SMTO_NORMAL | SMTO_ABORTIFHUNG, 1000, &r)) return 0; // a hung explorer costs a second at most
    return (LRESULT)r;
}

LRESULT TrayHost::OnCopy(HWND from, const COPYDATASTRUCT* cds) {
    WPARAM wp = (WPARAM)from; // the real sender: explorer's appbar code checks who is asking
    LPARAM lp = (LPARAM)cds;
    if (cds->dwData != kNotifyData) return Forward(wp, lp); // appbars and icon positions: explorer answers those
    LRESULT mine = Notify(cds->lpData, cds->cbData);
    Forward(wp, lp); // explorer keeps its copy of every icon
    if (debug && cds->cbData >= sizeof(DWORD) * 2 + 4 * sizeof(DWORD)) LogNotify(cds->lpData, cds->cbData, mine);
    return mine;
}

bool TrayHost::IsSystemIcon(const GUID& g, HWND owner) { // explorer's volume, network and power share one guid family
    if ((g.Data1 & 0xFFFFFFF0u) != 0x7820AE70u || g.Data2 != 0x23E3 || g.Data3 != 0x4229) return false; // only the last digit of Data1 differs between them
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    wchar_t exe[MAX_PATH] = {};
    DWORD n = MAX_PATH;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    bool ok = p && QueryFullProcessImageNameW(p, 0, exe, &n);
    if (p) CloseHandle(p);
    const wchar_t* name = wcsrchr(exe, L'\\');
    return ok && name && !_wcsicmp(name + 1, L"explorer.exe"); // another app reusing the guid keeps its icon
}

void TrayHost::LogNotify(const void* data, DWORD size, LRESULT result) const { // temporary: every icon request and our answer, to see why an app's icon goes missing
    TrayData d{};
    memcpy(&d, data, (std::min)((size_t)size, sizeof(d)));
    DWORD pid = 0;
    GetWindowThreadProcessId((HWND)(ULONG_PTR)d.nid.hWnd, &pid);
    wchar_t exe[MAX_PATH] = L"?", tmp[MAX_PATH], line[MAX_PATH + 96];
    DWORD n = MAX_PATH;
    if (HANDLE pr = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) QueryFullProcessImageNameW(pr, 0, exe, &n), CloseHandle(pr);
    swprintf_s(line, L"%ls msg=%lu flags=0x%lx cb=%lu size=%lu sig=0x%lx -> %ld\r\n", exe, d.msg, d.nid.uFlags, d.nid.cbSize, size, d.sig, (long)result);
    GetTempPathW(MAX_PATH, tmp);
    HANDLE f = CreateFileW((std::wstring(tmp) + L"HaloBar-traylog.txt").c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, line, (DWORD)(wcslen(line) * sizeof(wchar_t)), &w, nullptr);
    CloseHandle(f);
}

LRESULT TrayHost::Notify(const void* data, DWORD size) {
    if (!data || size < offsetof(TrayData, nid) + offsetof(NotifyData32, szTip)) return FALSE; // too short to even name the icon
    TrayData d{}; // zeroed, so fields an old app didn't send read as unset
    memcpy(&d, data, (std::min)((size_t)size, sizeof(d)));
    if (d.sig != kTraySig) return FALSE;
    const NotifyData32& n = d.nid;
    DWORD cb = (std::min)(n.cbSize, (DWORD)sizeof(NotifyData32)); // the app's own idea of the struct's size tells its version
    bool byGuid = (n.uFlags & NIF_GUID) && cb >= kGuidEnd;
    HWND owner = (HWND)(ULONG_PTR)n.hWnd;
    auto found = std::find_if(items.begin(), items.end(), [&](const TrayItem& it) {
        return byGuid ? it.hasGuid && IsEqualGUID(it.guid, n.guidItem) : !it.hasGuid && it.owner == owner && it.id == n.uID;
    });

    auto apply = [&](TrayItem& it) {
        it.owner = owner ? owner : it.owner; // a guid icon may move to a new window
        it.id = n.uID;
        if (n.uFlags & NIF_MESSAGE) it.callback = n.uCallbackMessage;
        if ((n.uFlags & NIF_ICON) && n.hIcon && !(it.icon && it.from == n.hIcon)) { // a handle value is never reused while it lives, and icons don't change: same handle, same picture
            HICON copy = CopyIcon((HICON)(ULONG_PTR)n.hIcon); // icons live in a shared table: copy it before the app frees it
            if (copy) {
                Free(it);
                it.icon = copy;
                it.from = n.hIcon;
                ++it.ver; // the row remakes its bitmap
            }
        }
        if (n.uFlags & NIF_TIP) it.tip.assign(n.szTip, wcsnlen(n.szTip, cb <= kV1Size ? 64 : 128)); // bounded: the tip may not be terminated
        if ((n.uFlags & NIF_STATE) && (n.dwStateMask & NIS_HIDDEN)) it.hidden = (n.dwState & NIS_HIDDEN) != 0;
    };

    switch (d.msg) {
    case NIM_MODIFY:
        if (found != items.end()) {
            apply(*found);
            break;
        }
        if (!(n.uFlags & NIF_ICON) || !n.hIcon) return FALSE; // nothing to show yet: apps answer this by adding it
        [[fallthrough]]; // an icon added before we took the tray, from an app that never heard our call: its next update brings it in
    case NIM_ADD: {
        if (found != items.end()) return FALSE; // already there: the app will modify it instead
        TrayItem it;
        it.hasGuid = byGuid;
        if (byGuid) it.guid = n.guidItem;
        it.system = byGuid && IsSystemIcon(n.guidItem, owner); // kept, but never drawn: the bar has its own status glyphs
        apply(it);
        it.key = KeyOf(it);
        items.push_back(std::move(it));
        Save(); // rare, and a crash must not lose how to reach this app
        break;
    }
    case NIM_DELETE:
        if (found == items.end()) return FALSE;
        Free(*found);
        items.erase(found);
        Save();
        break;
    case NIM_SETVERSION:
        if (found == items.end()) return FALSE;
        found->version = n.uVersion;
        return TRUE; // nothing visible changed
    default: // NIM_SETFOCUS: the keyboard isn't in the tray
        return TRUE;
    }
    if (!pending) pending = PostMessageW(notify, msg, 0, 0) != FALSE; // a burst of updates, one message
    return TRUE;
}

void TrayHost::Click(size_t i, bool right, POINT pt) {
    if (i >= items.size()) return;
    const TrayItem& it = items[i];
    if (!it.callback || !IsWindow(it.owner)) return; // an icon that never asked for messages can't be clicked
    DWORD pid = 0;
    GetWindowThreadProcessId(it.owner, &pid);
    if (right) SetForegroundWindow(notify); // the bar never takes focus, so it can't pass any on: in front for this one click, as explorer is when its tray opens a menu
    AllowSetForegroundWindow(pid); // and let the app bring up its window or menu: a menu whose owner can't come forward never shows, or never closes
    bool v4 = it.version >= NOTIFYICON_VERSION_4; // v4 packs the event and id into lparam, and the point into wparam
    auto send = [&](UINT m) {
        if (v4) PostMessageW(it.owner, it.callback, MAKEWPARAM(pt.x, pt.y), MAKELPARAM(m, it.id));
        else PostMessageW(it.owner, it.callback, it.id, m);
    };
    if (right) {
        GetCursorPos(&pt); // menus open where the click was, as with windows' own tray
        send(WM_RBUTTONDOWN);
        send(WM_RBUTTONUP);
        if (v4) send(WM_CONTEXTMENU);
        return;
    }
    uint64_t now = GetTickCount64();
    bool dbl = it.key == lastKey && now - lastClick < GetDoubleClickTime(); // the bar's window doesn't get double clicks, so they're made here
    lastKey = it.key;
    lastClick = dbl ? 0 : now;
    send(dbl ? WM_LBUTTONDBLCLK : WM_LBUTTONDOWN);
    send(WM_LBUTTONUP);
    if (v4 && !dbl) send(NIN_SELECT); // what v4 apps act on for a plain left click
}
