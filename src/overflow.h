#pragma once
#include <windows.h>

// Windows' own hidden-icons flyout, opened from the bar's tray button.
// Explorer keeps a full copy of every tray icon (the tray host forwards everything), so its flyout is always complete;
// the bar only presses explorer's hidden chevron for it and moves the flyout over its own button.
namespace overflow {

bool Open(POINT anchor, int edge, bool below); // anchor: the button's center on screen; the flyout hangs from edge (below) or sits on it; false when explorer has no chevron (its hidden icon menu is off)
void Close();
bool Shown();
bool Is(HWND h); // explorer's flyout window, known once it has been opened
void Place(); // over the button again: explorer puts it over its own, hidden, chevron
void Forget(); // explorer restarted: its taskbar and flyout are new windows

// the icons the user pinned out of the flyout, right onto the taskbar, in windows' settings
bool Promoted(HWND owner, UINT id);
bool PromotedChanged(); // windows' list changed since the last look: a registry watch, free to ask

}
