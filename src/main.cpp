#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "bar.h"
#include "gfx.h"
#include "prefs.h"
#include "rest.h"
#include "taskbar.h"

void Census(UINT what, WPARAM sub, int add = 1); // bar.cpp: DebugLog's tally of what wakes the ui thread

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run"; // per-user startup apps
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run"; // their on/off switch in task manager

void StartWithWindows(const std::wstring& exe) { // a taskbar has to be there at sign-in: added once, re-pointed if the exe moved, never forced back on
    BYTE flags[12] = {};
    DWORD fsize = sizeof(flags);
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, L"HaloBar", RRF_RT_REG_BINARY, nullptr, flags, &fsize) == ERROR_SUCCESS && (flags[0] & 1))
        return; // switched off in task manager's startup apps: the user's choice stands
    std::wstring want = L"\"" + exe + L"\"";
    wchar_t now[MAX_PATH * 2] = {};
    DWORD size = sizeof(now);
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, L"HaloBar", RRF_RT_REG_SZ, nullptr, now, &size) == ERROR_SUCCESS && want == now) return; // already right
    RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, L"HaloBar", REG_SZ, want.c_str(), (DWORD)((want.size() + 1) * sizeof(wchar_t)));
}

constexpr wchar_t kExeName[] = L"HaloBar.exe";
constexpr wchar_t kOldExe[] = L"HaloBar.old"; // the name FinishUpdate clears at the next start
constexpr wchar_t kHanded[] = L"--installed"; // on the launch of the installed copy: it never hands over again, whatever its path looks like

std::wstring LongPath(const std::wstring& p) { // a short 8.3 spelling of the same file still compares equal
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetLongPathNameW(p.c_str(), buf, MAX_PATH * 2);
    return n && n < MAX_PATH * 2 ? std::wstring(buf, n) : p;
}

std::wstring InstallDir() { // %LOCALAPPDATA%\Programs\HaloBar: per-user, no admin, what startup and the start menu point at
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_UserProgramFiles, 0, nullptr, &base))) dir = std::wstring(base) + L"\\HaloBar";
    CoTaskMemFree(base);
    return dir;
}

bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

bool Newer(const std::wstring& from, const std::wstring& to) { // a build or download made after the installed copy
    WIN32_FILE_ATTRIBUTE_DATA a, b;
    if (!GetFileAttributesExW(from.c_str(), GetFileExInfoStandard, &a)) return false;
    if (!GetFileAttributesExW(to.c_str(), GetFileExInfoStandard, &b)) return true; // nothing installed yet
    return CompareFileTime(&a.ftLastWriteTime, &b.ftLastWriteTime) > 0; // same or newer there (the updater's) is kept
}

bool Install(const std::wstring& self, const std::wstring& dir) { // true once dir holds a copy worth running
    std::wstring inst = dir + L"\\" + kExeName;
    if (!Newer(self, inst)) return Exists(inst);
    int r = SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr); // parents too
    if (r != ERROR_SUCCESS && r != ERROR_ALREADY_EXISTS && r != ERROR_FILE_EXISTS) return false;
    if (!CopyFileW(self.c_str(), inst.c_str(), FALSE)) { // the installed copy is running: renamed aside, as the updater does
        std::wstring old = dir + L"\\" + kOldExe;
        if (!MoveFileExW(inst.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING)) return Exists(inst); // still locked: the old copy runs on
        if (!CopyFileW(self.c_str(), inst.c_str(), FALSE)) {
            MoveFileExW(old.c_str(), inst.c_str(), 0); // roll back so an exe is always there
            return Exists(inst);
        }
    }
    DeleteFileW((inst + L":Zone.Identifier").c_str()); // the download's internet mark: at sign-in windows may hold it for a prompt nobody sees
    return true;
}

bool HandOver(const std::wstring& self, PCWSTR args) { // run from a download or the build folder: install, and the installed copy runs instead
    if (args && wcsstr(args, kHanded)) return false; // already the handed-over launch: no loop, even if the paths never match
    std::wstring dir = InstallDir();
    if (dir.empty() || !_wcsicmp(LongPath(self).c_str(), LongPath(dir + L"\\" + kExeName).c_str())) return false; // we are the installed copy
    if (!Install(self, dir)) return false; // can't write there: this copy runs, as before
    std::wstring inst = dir + L"\\" + kExeName, more = std::wstring(args ? args : L"") + L" " + kHanded; // --after-crash and the like carry over
    return (INT_PTR)ShellExecuteW(nullptr, L"open", inst.c_str(), more.c_str(), dir.c_str(), SW_SHOWNORMAL) > 32;
}

void StartShortcut(const std::wstring& target) { // the start menu finds halobar again after it's closed; written only when it points elsewhere
    PWSTR programs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Programs, 0, nullptr, &programs))) return;
    std::wstring lnk = std::wstring(programs) + L"\\HaloBar.lnk";
    CoTaskMemFree(programs);
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> file;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link))) || FAILED(link.As(&file))) return;
    wchar_t now[MAX_PATH] = {};
    if (SUCCEEDED(file->Load(lnk.c_str(), STGM_READ)) && SUCCEEDED(link->GetPath(now, MAX_PATH, nullptr, SLGP_RAWPATH)) && !_wcsicmp(now, target.c_str()))
        return; // already right: nothing written
    std::wstring dir = target.substr(0, target.find_last_of(L'\\'));
    link->SetPath(target.c_str());
    link->SetArguments(L"");
    link->SetWorkingDirectory(dir.c_str());
    link->SetDescription(L"HaloBar");
    link->SetIconLocation(target.c_str(), 0);
    file->Save(lnk.c_str(), TRUE);
}

wchar_t crashExe[MAX_PATH] = {}; // filled at start: the crash handler mustn't allocate
constexpr int kRelaunchGapSec = 60; // a crash that keeps coming back stops relaunching
constexpr wchar_t kAfterCrash[] = L"--after-crash"; // the relaunched dock: windows' error reporting can hold the dead one, and its mutex, for many seconds
constexpr DWORD kMutexWaitMs = 3000, kCrashWaitMs = 30000;

LONG WINAPI Crashed(EXCEPTION_POINTERS*) {
    taskbar::Give(); // never leave the user without a taskbar
    RestoreBanners(); // nor without their notification banners
    int now = (int)time(nullptr);
    if (crashExe[0] && now - prefs::ReadNum(L"CrashAt", 0) > kRelaunchGapSec) { // and the dock comes back on its own
        prefs::SaveNum(L"CrashAt", now);
        ShellExecuteW(nullptr, L"open", crashExe, kAfterCrash, nullptr, SW_SHOWNORMAL); // waits on our mutex, which goes with this process
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

constexpr size_t kMaxDocks = 12; // four waits each, under windows' 64
constexpr UINT kCensusWait = 0xF0000, kCensusQueued = 0xF0001, kCensusLoop = 0xF0002, kCensusFrameUs = 0xF0006; // census kinds past any message: a wait that fired, a queued message's id, every wake, frame microseconds

int Run(const std::vector<Bar*>& bars) { // returns the quit code
    MSG msg;
    for (;;) {
        HANDLE waits[kMaxDocks * 4];
        struct Wait {
            Bar* bar;
            int kind; // 0 frame, 1 prefs, 2 pins, 3 notifications
        } what[kMaxDocks * 4];
        DWORD n = 0;
        for (Bar* b : bars) {
            if (b->NeedsFrame()) what[n] = {b, 0}, waits[n++] = b->FrameWait(); // only while something moves
            if (b->PrefsWait()) what[n] = {b, 1}, waits[n++] = b->PrefsWait();
            if (b->PinsWait()) what[n] = {b, 2}, waits[n++] = b->PinsWait();
            if (b->NotifWait() != INVALID_HANDLE_VALUE) what[n] = {b, 3}, waits[n++] = b->NotifWait();
        }
        DWORD r = MsgWaitForMultipleObjectsEx(n, n ? waits : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE); // asleep until there's work, 0 cpu idle
        Census(kCensusLoop, r == WAIT_OBJECT_0 + n ? 1 : 0);
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
            const Wait& w = what[r - WAIT_OBJECT_0];
            Census(kCensusWait, (WPARAM)w.kind);
            if (w.kind == 0) {
                LARGE_INTEGER a, b, f;
                QueryPerformanceCounter(&a);
                w.bar->Frame();
                QueryPerformanceCounter(&b);
                QueryPerformanceFrequency(&f);
                Census(kCensusFrameUs, 0, (int)((b.QuadPart - a.QuadPart) * 1000000 / f.QuadPart)); // the frames' own cost, summed
            }
            else if (w.kind == 1) w.bar->OnPrefs();
            else if (w.kind == 2) w.bar->OnPins();
            else w.bar->OnNotifWrite();
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { // drain all queued input
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            Census(kCensusQueued, msg.message);
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR args, int) {
    HEAP_OPTIMIZE_RESOURCES_INFORMATION heap = {HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &heap, sizeof(heap)); // heaps hand freed memory back sooner
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // crisp on mixed dpi screens
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) { // menus follow windows' dark mode: uxtheme's unnamed SetPreferredAppMode
        using SetMode = int(WINAPI*)(int);
        if (auto set = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)))) set(1); // 1: allow dark
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // smoother animation under load
    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exePath = exe;
    if (HandOver(exePath, args)) return 0; // before the mutex: the installed copy waits on it, not on us
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\HaloBar");
    DWORD wait = args && wcsstr(args, kAfterCrash) ? kCrashWaitMs : kMutexWaitMs;
    if (!mutex || WaitForSingleObject(mutex, wait) == WAIT_TIMEOUT) return 0; // wait for the old instance on restart, else bail
    sys::FinishUpdate(); // the exe an update replaced goes now that it isn't running
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1; // sta for wic and shell apis
    SetUnhandledExceptionFilter(Crashed);
    prefs::MoveOldKey(); // before anything reads them, the crash marker included

    wcscpy_s(crashExe, exe); // the installed copy comes back after a crash, as after a restart
    StartWithWindows(exePath);
    StartShortcut(exePath);
    PathRemoveFileSpecW(exe);
    int code = 1;
    if (gfx::Init(std::wstring(exe) + L"\\fonts")) { // shared d2d/dwrite factories + bundled fonts
        if (prefs::ReadNum(L"WindowsBar", 0)) { // windows has its own taskbar: only the tray icon and its dashboard
            taskbar::Give(); // a crash in the dock may have left it hidden
            code = rest::Run(instance);
            gfx::Shutdown();
            CoUninitialize();
            ReleaseMutex(mutex);
            CloseHandle(mutex);
            if (code == 2) ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // the dock back, or a restart
            return code == 2 ? 0 : code;
        }
        std::vector<PinItem> pinned;
        {
            Bar bar;
            std::vector<std::unique_ptr<Bar>> moons; // the other screens' docks, gone before the main one
            if (bar.Create(instance)) {
                std::vector<Bar*> all = {&bar};
                if (prefs::Load().screens) { // a dock on every screen, as a taskbar should be
                    for (HMONITOR m : Bar::Screens()) {
                        if (m == bar.Screen() || all.size() >= kMaxDocks) continue;
                        auto b = std::make_unique<Bar>();
                        b->SetMoon(m, &bar);
                        if (b->Create(instance)) all.push_back(b.get()), moons.push_back(std::move(b));
                    }
                }
                gfx::GiveBack(); // trim startup memory
                code = Run(all);
                pinned = bar.PinList();
            }
            moons.clear();
        } // the bar lets go of its strip before the taskbar comes back
        if (code != 2) taskbar::Give(); // a restart keeps it: the next dock takes over a hidden taskbar, so windows' never flashes up in between
        if (code != 2 && pins::Dirty()) pins::ReloadExplorer(pinned); // pins changed here: the windows taskbar starts again with the same ones
        gfx::Shutdown();
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    if (code == 2 || code == 3) { // restart from the menu, or back as a tray icon
        bool started = (INT_PTR)ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL) > 32;
        if (!started && code == 2) taskbar::Give(); // no dock coming: never leave the user without a taskbar
    }
    return code == 2 || code == 3 ? 0 : code;
}
