#pragma once
#include <windows.h>

namespace rest { // windows has its taskbar back: halobar waits as a tray icon with astraeus' little dashboard, no dock, no surface

int Run(HINSTANCE instance); // until bring back, restart or exit; returns the quit code, 2 relaunches

}
