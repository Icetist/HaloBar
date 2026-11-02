#pragma once
#include <windows.h>

// Show desktop, done by the bar itself: every app window minimized, then the very same ones back in their order, the last one in front focused again.
namespace desktop {

bool Toggle(); // false: nothing could be minimized here (every window elevated above us, say), so the caller tries the shell's own

}
