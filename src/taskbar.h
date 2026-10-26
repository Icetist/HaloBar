#pragma once
#include <windows.h>

// The windows taskbar, put away while the bar runs and handed back exactly as it was.
namespace taskbar {

void Take(); // remember its state, set it to auto-hide so it frees the screen edge, then hide it
void Hide(); // hide it again, and the other screens' ones: explorer shows them after a restart, a new screen or a peek at the edge
void Give(); // the user's own state back, visible; safe to call more than once and from a crash handler
bool IsTray(HWND h); // the primary taskbar window
bool IsSecondary(HWND h); // explorer's taskbar on another screen
HWND ExplorerTray(); // explorer's own taskbar window, never the bar's stand-in of the same class

}
