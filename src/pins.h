#pragma once
#include <windows.h>

#include <string>
#include <vector>

// The pinned apps, read from and written to the same place the windows taskbar keeps them, so switching back to it
// shows exactly the same pins in the same order.
struct PinItem {
    std::wstring key; // the app id windows itself resolves for it, lowercased: matches its windows' keys
    std::wstring name;
    std::wstring parse; // parsing name: what to launch and where its icon comes from
    std::vector<BYTE> pidl; // the taskband's own entry
    std::vector<BYTE> link; // its serialized shortcut, the taskband's second list
};

namespace pins {

std::vector<PinItem> Load(); // the taskband's pins, in order
bool Save(const std::vector<PinItem>& list); // writes both lists the taskband reads at start
bool Assert(const std::vector<PinItem>& list); // writes them only if what's there differs: true if it wrote
bool Make(const std::wstring& key, const std::wstring& aumid, const std::wstring& exe, const std::wstring& name, PinItem& out); // a new pin for a running app
bool UnpinLive(const PinItem& p); // tells the running windows taskbar right away
void Trash(const PinItem& p); // its shortcut in the pinned folder, once it's no longer pinned anywhere

std::wstring WindowKey(HWND h); // the app id windows groups this window under, lowercased; empty if it can't tell
HANDLE Watch(); // auto-reset event, set when the taskband's pins change; null on failure
void Rearm(HANDLE ev);
void Unwatch(HANDLE ev);

// pins changed while the bar ran: windows' taskbar has to read them again once it's back
bool Dirty();
void SetDirty(bool on);
void ReloadExplorer(const std::vector<PinItem>& list); // explorer leaves gracefully, the pins are written, explorer starts again

}
