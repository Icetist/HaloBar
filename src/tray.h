#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>

struct TrayItem {
    HWND owner = nullptr; // the app's window that gets the clicks
    UINT id = 0;
    GUID guid{};
    bool hasGuid = false;
    UINT callback = 0; // message the app wants its clicks as, 0 = none
    UINT version = 0; // NOTIFYICON_VERSION_4 apps get the click position and a context menu message
    HICON icon = nullptr; // our own copy, the app may destroy its one
    uint32_t from = 0; // the app's handle it was copied from: many apps resend the same one every second
    std::wstring tip;
    bool hidden = false;
    bool system = false; // windows' own volume, network and power: the bar's quick settings have those
    bool Shown() const { return !hidden && !system; }
    uint64_t key = 0; // stable per icon, for ui ids and bitmap caching
    uint64_t ver = 0; // bumps whenever the icon image changes
};

// The notification area: the bar takes the place explorer's taskbar has for apps' tray icons.
// Every message is also handed on to explorer, so its own copy stays complete if the bar goes away.
class TrayHost {
public:
    TrayHost(HWND notify, UINT msg); // msg is posted whenever an icon comes, changes or goes
    void Posted() { pending = false; } // the bar took the change message
    ~TrayHost();
    TrayHost(const TrayHost&) = delete;
    TrayHost& operator=(const TrayHost&) = delete;

    void Claim(); // become the tray apps talk to, and ask every app to add its icons again
    void Reassert(); // stay in front of explorer's tray window, without asking again
    void StepBack(); // step behind explorer's tray for a moment, so a shell call made right now reaches explorer itself
    void Prune(); // drop icons whose app quit without removing them
    const std::vector<TrayItem>& Items() const { return items; }
    int Visible() const;
    void Click(size_t i, bool right, POINT screen); // as if the icon itself was clicked

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT OnCopy(HWND from, const COPYDATASTRUCT* cds);
    LRESULT Notify(const void* data, DWORD size);
    void LogNotify(const void* data, DWORD size, LRESULT result) const;
    bool pending = false; // a change message is already on its way to the bar
    bool debug = false; // DebugTray set: every icon request goes to a log in %TEMP%
    static bool IsSystemIcon(const GUID& g, HWND owner);
    LRESULT Forward(WPARAM wp, LPARAM lp) const; // hand a message on to explorer's tray
    void Free(TrayItem& it);
    void Save() const; // every icon and how to reach its app, for the next start
    void Restore();

    HWND hwnd = nullptr, notify = nullptr;
    UINT msg = 0;
    std::vector<TrayItem> items;
    uint64_t lastKey = 0, lastClick = 0; // double clicks
};
