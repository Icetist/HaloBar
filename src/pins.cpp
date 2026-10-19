#include "pins.h"

#include <shlobj.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <propsys.h>
#include <propkey.h>
#include <propvarutil.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>

#include "prefs.h"
#include "taskbar.h"

#pragma comment(lib, "propsys.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr wchar_t kTaskband[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Taskband"; // where explorer keeps its pins, under hkcu
constexpr wchar_t kDirty[] = L"PinsDirty"; // in the bar's own key: set while explorer's list is stale
constexpr int kCallerExplorer = 4; // PINNEDLISTMODIFYCALLER: what chromium passes
constexpr UINT kExitExplorer = WM_USER + 436; // explorer's own "exit explorer", it saves its state on the way out
constexpr DWORD kExitWaitMs = 8000; // a busy explorer can take a while to save and quit

const CLSID kTaskbandPin = {0x90aa3a4e, 0x1cba, 0x4233, {0xb8, 0xbb, 0x53, 0x57, 0x73, 0xd4, 0x84, 0x49}}; // CLSID_TaskbandPin
const CLSID kAppResolver = {0x660b90c8, 0x73a9, 0x4b58, {0x8c, 0xae, 0x35, 0x5b, 0x7f, 0x55, 0x34, 0x1b}}; // CLSID_StartMenuCacheAndAppResolver

// undocumented, but stable from windows 10 1809 through 11 25h2; chromium and firefox pin through it
MIDL_INTERFACE("0DD79AE2-D156-45D4-9EEB-3B549769E940") IPinnedList3 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE EnumObjects() = 0; // unused slots keep the vtable order, so no real signatures
    virtual HRESULT STDMETHODCALLTYPE GetPinnableInfo() = 0;
    virtual HRESULT STDMETHODCALLTYPE IsPinnable() = 0;
    virtual HRESULT STDMETHODCALLTYPE Resolve() = 0;
    virtual HRESULT STDMETHODCALLTYPE LegacyModify() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetChangeCount(ULONG*) = 0;
    virtual HRESULT STDMETHODCALLTYPE IsPinned(PCIDLIST_ABSOLUTE) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPinnedItem() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForPinnedItem() = 0;
    virtual HRESULT STDMETHODCALLTYPE ItemChangeNotify() = 0;
    virtual HRESULT STDMETHODCALLTYPE UpdateForRemovedItemsAsNecessary() = 0;
    virtual HRESULT STDMETHODCALLTYPE PinShellLink() = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPinnedItemForAppID() = 0;
    virtual HRESULT STDMETHODCALLTYPE Modify(PCIDLIST_ABSOLUTE unpin, PCIDLIST_ABSOLUTE pin, int caller) = 0; // either side may be null
};

// what explorer itself uses to give windows and shortcuts the same app id, so a pin finds its windows
MIDL_INTERFACE("DE25675A-72DE-44B4-9373-05170450C140") IApplicationResolver8 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcut(IShellItem* item, LPWSTR* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForShortcutObject(IShellLinkW* link, IShellItem* item, LPWSTR* id) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForWindow(HWND h, LPWSTR* id, void*, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetAppIDForProcess(DWORD pid, LPWSTR* id, void*, void*, void*) = 0;
};

std::wstring Lower(std::wstring s) {
    if (!s.empty()) CharLowerBuffW(s.data(), (DWORD)s.size()); // locale-aware, as explorer compares app ids
    return s;
}

std::wstring Take(LPWSTR s) { // a co-task string, freed
    std::wstring out = s ? s : L"";
    CoTaskMemFree(s);
    return out;
}

IApplicationResolver8* Resolver() { // one per process, on the ui thread
    static ComPtr<IApplicationResolver8> r;
    static bool tried = false; // a failure is remembered too: no retrying every call
    if (!tried) {
        tried = true;
        CoCreateInstance(kAppResolver, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&r)); // may fail on a future windows: callers fall back
    }
    return r.Get();
}

std::vector<BYTE> ReadBin(const wchar_t* name) {
    DWORD size = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kTaskband, name, RRF_RT_REG_BINARY, nullptr, nullptr, &size) != ERROR_SUCCESS || !size) return {};
    std::vector<BYTE> buf(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kTaskband, name, RRF_RT_REG_BINARY, nullptr, buf.data(), &size) != ERROR_SUCCESS) return {}; // it may have grown meanwhile
    buf.resize(size);
    return buf;
}

std::vector<BYTE> Serialize(IShellLinkW* link) { // a shortcut as the bytes of a .lnk file, the taskband's second list
    ComPtr<IPersistStream> ps;
    ComPtr<IStream> s;
    s.Attach(SHCreateMemStream(nullptr, 0)); // a growable stream in memory, never touches disk
    if (!s || FAILED(link->QueryInterface(IID_PPV_ARGS(&ps))) || FAILED(ps->Save(s.Get(), TRUE))) return {};
    STATSTG st{};
    if (FAILED(s->Stat(&st, STATFLAG_NONAME)) || st.cbSize.QuadPart > (1u << 20)) return {}; // a shortcut over 1 mb is broken
    std::vector<BYTE> out((size_t)st.cbSize.QuadPart);
    LARGE_INTEGER zero{};
    ULONG got = 0;
    if (FAILED(s->Seek(zero, STREAM_SEEK_SET, nullptr)) || FAILED(s->Read(out.data(), (ULONG)out.size(), &got))) return {}; // Save left it at the end
    out.resize(got);
    return out;
}

std::wstring PinnedDir() { // %appdata%\microsoft\internet explorer\quick launch\user pinned\taskbar
    PWSTR p = nullptr;
    std::wstring dir = SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserPinned, 0, nullptr, &p)) ? std::wstring(p) + L"\\TaskBar" : L"";
    CoTaskMemFree(p);
    return dir;
}

std::wstring KeyOf(IShellItem* item, const std::wstring& fallback) { // the grouping key for a pin, the same one its windows get
    LPWSTR id = nullptr;
    if (IApplicationResolver8* r = Resolver(); r && SUCCEEDED(r->GetAppIDForShortcut(item, &id)) && id) return Lower(Take(id)); // best: explorer's own answer
    ComPtr<IShellItem2> si2;
    if (SUCCEEDED(item->QueryInterface(IID_PPV_ARGS(&si2))) && SUCCEEDED(si2->GetString(PKEY_AppUserModel_ID, &id)) && id) return Lower(Take(id)); // set on the link
    return Lower(fallback);
}

std::wstring SafeName(std::wstring s) {
    for (wchar_t& c : s)
        if (wcschr(L"<>:\"/\\|?*", c) || c < 32) c = L'_'; // characters a file name can't hold
    return s.empty() ? L"App" : s;
}

}

namespace pins {

std::vector<PinItem> Load() {
    std::vector<BYTE> fav = ReadBin(L"Favorites"), res = ReadBin(L"FavoritesResolve"); // the pidls, and the shortcuts behind them
    std::vector<std::vector<BYTE>> links; // parallel to the pins: { dword size; .lnk bytes }
    for (size_t o = 0; o + 4 <= res.size();) {
        DWORD cb = 0;
        memcpy(&cb, &res[o], 4); // unaligned, so memcpy, not a cast
        if (cb > res.size() - o - 4) break; // a size running past the end: stop, don't overread
        links.emplace_back(res.begin() + o + 4, res.begin() + o + 4 + cb);
        o += 4 + cb;
    }
    std::vector<PinItem> out;
    for (size_t o = 0; o + 5 <= fav.size() && fav[o] != 0xFF;) { // { byte 0; dword size; pidl } ... byte 0xff
        DWORD cb = 0;
        memcpy(&cb, &fav[o + 1], 4);
        if (cb < 2 || cb > fav.size() - o - 5) break; // damaged: keep what parsed
        PinItem p;
        p.pidl.assign(fav.begin() + o + 5, fav.begin() + o + 5 + cb);
        size_t index = out.size(); // the matching shortcut sits at the same position
        o += 5 + cb; // advance before any skip below
        auto pidl = reinterpret_cast<PCIDLIST_ABSOLUTE>(p.pidl.data());
        ComPtr<IShellItem> item;
        if (ILGetSize(pidl) > cb || FAILED(SHCreateItemFromIDList(pidl, IID_PPV_ARGS(&item)))) continue; // an app that's gone
        PWSTR s = nullptr;
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_NORMALDISPLAY, &s))) p.name = Take(s);
        if (SUCCEEDED(SHGetNameFromIDList(pidl, SIGDN_DESKTOPABSOLUTEPARSING, &s))) p.parse = Take(s); // a .lnk path, or shell:AppsFolder\ for store apps
        p.key = KeyOf(item.Get(), p.parse);
        if (index < links.size()) p.link = links[index]; // kept as bytes, so a save writes it back unchanged
        out.push_back(std::move(p));
    }
    return out;
}

namespace {
std::vector<BYTE> FavBytes(const std::vector<PinItem>& list) { // the Favorites value exactly as Save would write it
    std::vector<BYTE> fav;
    for (const auto& p : list) {
        DWORD cb = (DWORD)p.pidl.size();
        fav.push_back(0);
        fav.insert(fav.end(), (BYTE*)&cb, (BYTE*)&cb + 4);
        fav.insert(fav.end(), p.pidl.begin(), p.pidl.end());
    }
    fav.push_back(0xFF);
    return fav;
}
}

bool Assert(const std::vector<PinItem>& list) {
    if (ReadBin(L"Favorites") == FavBytes(list)) return false; // already ours: no write, so no echo from the watcher
    Save(list);
    return true;
}

bool Save(const std::vector<PinItem>& list) {
    std::vector<BYTE> fav, res;
    for (const auto& p : list) {
        DWORD cb = (DWORD)p.pidl.size(), lb = (DWORD)p.link.size(); // both lists stay in step, entry for entry
        fav.push_back(0);
        fav.insert(fav.end(), (BYTE*)&cb, (BYTE*)&cb + 4);
        fav.insert(fav.end(), p.pidl.begin(), p.pidl.end());
        res.insert(res.end(), (BYTE*)&lb, (BYTE*)&lb + 4);
        res.insert(res.end(), p.link.begin(), p.link.end());
    }
    fav.push_back(0xFF);
    DWORD changes = 0, size = sizeof(changes);
    RegGetValueW(HKEY_CURRENT_USER, kTaskband, L"FavoritesChanges", RRF_RT_REG_DWORD, nullptr, &changes, &size); // explorer's change counter
    ++changes; // bumped so explorer sees the list as newer than its cache
    bool ok = RegSetKeyValueW(HKEY_CURRENT_USER, kTaskband, L"Favorites", REG_BINARY, fav.data(), (DWORD)fav.size()) == ERROR_SUCCESS &&
              RegSetKeyValueW(HKEY_CURRENT_USER, kTaskband, L"FavoritesResolve", REG_BINARY, res.data(), (DWORD)res.size()) == ERROR_SUCCESS;
    RegSetKeyValueW(HKEY_CURRENT_USER, kTaskband, L"FavoritesChanges", REG_DWORD, &changes, sizeof(changes));
    return ok;
}

bool Make(const std::wstring& key, const std::wstring& aumid, const std::wstring& exe, const std::wstring& name, PinItem& out) {
    out = PinItem{};
    out.key = key;
    out.name = name;
    ComPtr<IShellLinkW> link;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) return false;
    bool packaged = !aumid.empty() && (aumid.find(L'!') != std::wstring::npos || Lower(exe).find(L"\\windowsapps\\") != std::wstring::npos); // family!app is a store id
    PIDLIST_ABSOLUTE abs = nullptr;
    if (packaged) { // store apps are pinned as their entry in the apps folder, no shortcut file
        out.parse = L"shell:AppsFolder\\" + aumid;
        if (FAILED(SHParseDisplayName(out.parse.c_str(), nullptr, &abs, 0, nullptr))) return false; // not installed for this user
        link->SetIDList(abs); // the resolve list still wants a shortcut, one that points at the apps folder entry
    } else { // desktop apps: a shortcut in the pinned folder, as the windows taskbar makes one
        std::wstring dir = PinnedDir();
        if (exe.empty() || dir.empty()) return false;
        SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); // a fresh profile may not have it yet
        out.parse = dir + L"\\" + SafeName(name) + L".lnk"; // same name as an existing one: overwritten, as windows does
        link->SetPath(exe.c_str());
        std::wstring work = exe.substr(0, exe.find_last_of(L'\\')); // start in the exe's folder, like a start menu shortcut
        link->SetWorkingDirectory(work.c_str());
        if (!aumid.empty()) { // the app's own id, so its windows group under this pin
            ComPtr<IPropertyStore> props;
            PROPVARIANT v;
            if (SUCCEEDED(link.As(&props)) && SUCCEEDED(InitPropVariantFromString(aumid.c_str(), &v))) {
                props->SetValue(PKEY_AppUserModel_ID, v);
                props->Commit(); // into the link object; the file save below writes it out
                PropVariantClear(&v);
            }
        }
        ComPtr<IPersistFile> file;
        if (FAILED(link.As(&file)) || FAILED(file->Save(out.parse.c_str(), TRUE))) return false;
        abs = ILCreateFromPathW(out.parse.c_str()); // the pin points at the shortcut file, not the exe
        if (!abs) return false;
    }
    out.pidl.assign((BYTE*)abs, (BYTE*)abs + ILGetSize(abs)); // ILGetSize includes the terminating null id
    ILFree(abs);
    out.link = Serialize(link.Get()); // explorer needs both halves or it drops the pin
    return !out.link.empty();
}

bool UnpinLive(const PinItem& p) {
    ComPtr<IPinnedList3> list;
    if (p.pidl.empty() || FAILED(CoCreateInstance(kTaskbandPin, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&list)))) return false;
    return SUCCEEDED(list->Modify(reinterpret_cast<PCIDLIST_ABSOLUTE>(p.pidl.data()), nullptr, kCallerExplorer)); // null pin: unpin only, explorer updates live
}

void Trash(const PinItem& p) {
    if (p.pidl.empty()) return;
    PWSTR s = nullptr;
    if (FAILED(SHGetNameFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(p.pidl.data()), SIGDN_FILESYSPATH, &s))) return; // a store app pin has no file
    std::wstring path = Take(s), dir = PinnedDir();
    if (!dir.empty() && Lower(path).rfind(Lower(dir) + L"\\", 0) == 0) DeleteFileW(path.c_str()); // only ever inside the pinned folder
}

std::wstring WindowKey(HWND h) {
    LPWSTR id = nullptr;
    IApplicationResolver8* r = Resolver();
    if (!r || FAILED(r->GetAppIDForWindow(h, &id, nullptr, nullptr, nullptr)) || !id) return {}; // empty: the caller falls back to the exe path
    return Lower(Take(id));
}

namespace {
HKEY watched = nullptr; // held open while watching: the notification is tied to the handle
}

HANDLE Watch() {
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kTaskband, 0, KEY_NOTIFY, &watched) != ERROR_SUCCESS) {
        watched = nullptr;
        return nullptr;
    }
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr); // auto-reset, waited on by the main loop
    if (ev) Rearm(ev);
    return ev;
}

void Rearm(HANDLE ev) {
    if (watched && ev) RegNotifyChangeKeyValue(watched, FALSE, REG_NOTIFY_CHANGE_LAST_SET, ev, TRUE); // one-shot: re-armed after every signal
}

void Unwatch(HANDLE ev) {
    if (watched) RegCloseKey(watched);
    watched = nullptr;
    if (ev) CloseHandle(ev);
}

bool Dirty() { return prefs::ReadNum(kDirty, 0) != 0; } // survives a crash, so the next start still writes the pins back

void SetDirty(bool on) {
    if (on) prefs::SaveNum(kDirty, 1);
    else prefs::Delete(kDirty);
}

void ReloadExplorer(const std::vector<PinItem>& list) {
    HWND tray = taskbar::ExplorerTray(); // explorer's own tray, not the one the bar hosts
    DWORD pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &pid);
    if (HANDLE proc = pid ? OpenProcess(SYNCHRONIZE | PROCESS_TERMINATE, FALSE, pid) : nullptr) {
        PostMessageW(tray, kExitExplorer, 0, 0); // it writes its own copy of the pins as it goes: ours are written after
        if (WaitForSingleObject(proc, kExitWaitMs) == WAIT_TIMEOUT) {
            TerminateProcess(proc, 0); // hung: its pending save is lost anyway, ours wins
            WaitForSingleObject(proc, 2000);
        }
        CloseHandle(proc);
    }
    Save(list); // with explorer gone nothing overwrites it
    wchar_t win[MAX_PATH] = {};
    GetWindowsDirectoryW(win, MAX_PATH); // the full path, never a search that could find another explorer.exe
    std::wstring exe = std::wstring(win) + L"\\explorer.exe";
    STARTUPINFOW si = {sizeof(si)};
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(exe.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, win, &si, &pi)) { // no shell running: it becomes the shell
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
    SetDirty(false); // explorer now has the same list as the bar
}

}
