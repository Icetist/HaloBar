#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <ctime>
#include <memory>
#include <string>
#include <vector>

#include "bar.h"
#include "gfx.h"
#include "prefs.h"
#include "rest.h"
#include "taskbar.h"

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
        if (r >= WAIT_OBJECT_0 && r < WAIT_OBJECT_0 + n) {
            const Wait& w = what[r - WAIT_OBJECT_0];
            if (w.kind == 0) w.bar->Frame();
            else if (w.kind == 1) w.bar->OnPrefs();
            else if (w.kind == 2) w.bar->OnPins();
            else w.bar->OnNotifWrite();
        }
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { // drain all queued input
            if (msg.message == WM_QUIT) return (int)msg.wParam;
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
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\HaloBar");
    DWORD wait = args && wcsstr(args, kAfterCrash) ? kCrashWaitMs : kMutexWaitMs;
    if (!mutex || WaitForSingleObject(mutex, wait) == WAIT_TIMEOUT) return 0; // wait for the old instance on restart, else bail
    sys::FinishUpdate(); // the exe an update replaced goes now that it isn't running
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1; // sta for wic and shell apis
    SetUnhandledExceptionFilter(Crashed);
    prefs::MoveOldKey(); // before anything reads them, the crash marker included

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exePath = exe;
    wcscpy_s(crashExe, exe);
    StartWithWindows(exePath);
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
                    taskbar::Everywhere(true);
                    for (HMONITOR m : Bar::Screens()) {
                        if (m == bar.Screen() || all.size() >= kMaxDocks) continue;
                        auto b = std::make_unique<Bar>();
                        b->SetMoon(m);
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
