#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "pins.h"

struct TaskWin {
    HWND hwnd = nullptr;
    std::wstring title;
    bool flash = false; // asked for attention, cleared once it comes to the front
    uint64_t used = 0; // when it was last in front: a click brings back the most recent one
};

struct TaskApp {
    std::wstring key; // what groups windows: the app id windows resolves, lowercased
    std::wstring exe, aumid, name;
    std::vector<TaskWin> wins;
    bool pinned = false;
    std::vector<BYTE> pidl; // pinned: the taskband's entry, what launches it and where its icon comes from
};

struct Badge { // what an app told the taskbar about one of its windows through ITaskbarList3
    int state = 0; // TBPFLAG: 0 none, 1 indeterminate, 2 normal, 4 error, 8 paused
    float value = 0; // 0..1
    HICON overlay = nullptr; // our own copy
    uint64_t ver = 0; // bumps when the overlay changes
};

// Every window that would get a taskbar button, grouped by app, kept current from window events: nothing polls.
// The pinned apps come first, in the windows taskbar's own order, whether they run or not.
class Tasks {
public:
    enum : WPARAM { kChanged, kRescan, kTrayShown, kTitle, kOverflow, kIsland }; // kisland: astraeus island's window appeared // wparam of the posted message; ktitle: only the front window's title moved; koverflow: windows' tray flyout opened or closed
    Tasks(HWND notify, UINT msg);
    ~Tasks();
    Tasks(const Tasks&) = delete;
    Tasks& operator=(const Tasks&) = delete;

    void Rescan(); // walk the windows again, after a burst of kRescan
    bool OnShell(UINT msg, WPARAM wp, LPARAM lp); // true if it was the shell hook message
    bool OnTaskband(UINT msg, WPARAM wp, LPARAM lp); // true if it was an ITaskbarList3 message
    const std::vector<TaskApp>& Apps() const { return apps; }
    HWND Fore() const { return fore; }
    int AppOf(HWND h) const; // index into Apps, -1 if none
    UINT ShellMsg() const { return shellMsg; } // the shell hook's message, for the minimize target
    int Calling(HWND* who = nullptr) const; // an app with a window asking for attention, -1 if none
    const Badge* BadgeOf(const TaskApp& a) const; // the most telling badge of its windows, null if none
    void Click(int app); // bring back, minimize or cycle its windows, or start it, like the windows taskbar
    void Launch(int app, bool admin = false); // another window of it; admin asks for elevation, as ctrl+shift+click does
    void CloseAll(int app);
    static void Activate(HWND h);

    void ReloadPins(); // the taskband's list changed outside the bar
    bool Pin(int app, int at = -1); // at: place among the pins, -1 last
    bool Unpin(int app);
    bool MovePin(int from, int to); // both indexes among the pins
    int PinCount() const { return (int)pinList.size(); }
    const std::vector<PinItem>& PinList() const { return pinList; }

private:
    static void CALLBACK OnEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    void Event(DWORD ev, HWND h);
    TaskWin* Find(HWND h);
    void Ping(WPARAM what) const { PostMessageW(notify, msg, what, 0); }
    void Describe(HWND h, TaskApp& out); // key, exe, app id and name of a new window's app
    void Merge(std::vector<TaskApp> running); // pins first, then the rest, into apps
    void SavePins(); // written for the windows taskbar, which reads them once it's back

    HWND notify;
    UINT msg, shellMsg = 0;
    std::vector<void*> hooks;
    std::vector<TaskApp> apps;
    std::vector<PinItem> pinList;
    std::unordered_map<HWND, Badge> badges;
    std::unordered_map<DWORD, std::wstring> exes; // pid -> image path, a rescan opens no process twice
    std::unordered_map<std::wstring, std::wstring> names; // key -> display name, read once per app
    HWND fore = nullptr;
};
