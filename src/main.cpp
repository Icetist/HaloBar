#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <shlwapi.h>

#include <string>

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

LONG WINAPI Crashed(EXCEPTION_POINTERS*) {
    taskbar::Give(); // never leave the user without a taskbar
    RestoreBanners(); // nor without their notification banners
    return EXCEPTION_CONTINUE_SEARCH;
}

int Run(Bar& bar) { // returns the quit code
    MSG msg;
    for (;;) {
        HANDLE waits[4];
        DWORD n = 0, frameAt = MAXDWORD, prefsAt = MAXDWORD, pinsAt = MAXDWORD, notifAt = MAXDWORD;
        if (bar.NeedsFrame()) frameAt = n, waits[n++] = bar.FrameWait(); // only while something moves
        if (bar.PrefsWait()) prefsAt = n, waits[n++] = bar.PrefsWait();
        if (bar.PinsWait()) pinsAt = n, waits[n++] = bar.PinsWait();
        if (bar.NotifWait() != INVALID_HANDLE_VALUE) notifAt = n, waits[n++] = bar.NotifWait();
        DWORD r = MsgWaitForMultipleObjectsEx(n, n ? waits : nullptr, INFINITE, QS_ALLINPUT, MWMO_INPUTAVAILABLE); // asleep until there's work, 0 cpu idle
        if (frameAt != MAXDWORD && r == WAIT_OBJECT_0 + frameAt) bar.Frame(); // guarded: MAXDWORD + 0 is WAIT_FAILED
        else if (prefsAt != MAXDWORD && r == WAIT_OBJECT_0 + prefsAt) bar.OnPrefs();
        else if (pinsAt != MAXDWORD && r == WAIT_OBJECT_0 + pinsAt) bar.OnPins();
        else if (notifAt != MAXDWORD && r == WAIT_OBJECT_0 + notifAt) bar.OnNotifWrite();
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { // drain all queued input
            if (msg.message == WM_QUIT) return (int)msg.wParam;
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }
}

}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HEAP_OPTIMIZE_RESOURCES_INFORMATION heap = {HEAP_OPTIMIZE_RESOURCES_CURRENT_VERSION, 0};
    HeapSetInformation(nullptr, HeapOptimizeResources, &heap, sizeof(heap)); // heaps hand freed memory back sooner
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2); // crisp on mixed dpi screens
    if (HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) { // menus follow windows' dark mode: uxtheme's unnamed SetPreferredAppMode
        using SetMode = int(WINAPI*)(int);
        if (auto set = reinterpret_cast<SetMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)))) set(1); // 1: allow dark
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL); // smoother animation under load
    HANDLE mutex = CreateMutexW(nullptr, FALSE, L"Local\\HaloBar");
    if (!mutex || WaitForSingleObject(mutex, 3000) == WAIT_TIMEOUT) return 0; // wait for the old instance on restart, else bail
    sys::FinishUpdate(); // the exe an update replaced goes now that it isn't running
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) return 1; // sta for wic and shell apis
    SetUnhandledExceptionFilter(Crashed);
    prefs::MoveOldKey(); // before anything reads them, the crash marker included

    wchar_t exe[MAX_PATH];
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring exePath = exe;
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
            if (bar.Create(instance)) {
                gfx::GiveBack(); // trim startup memory
                code = Run(bar);
                pinned = bar.PinList();
            }
        } // the bar lets go of its strip before the taskbar comes back
        taskbar::Give();
        if (code != 2 && pins::Dirty()) pins::ReloadExplorer(pinned); // pins changed here: the windows taskbar starts again with the same ones
        gfx::Shutdown();
    }
    CoUninitialize();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    if (code == 2 || code == 3) ShellExecuteW(nullptr, L"open", exePath.c_str(), nullptr, nullptr, SW_SHOWNORMAL); // restart from the menu, or back as a tray icon
    return code == 2 || code == 3 ? 0 : code;
}
