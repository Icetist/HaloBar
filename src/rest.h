#pragma once
#include <windows.h>

#include <string>

namespace rest { // windows has its taskbar back: halobar waits as a tray icon with astraeus' little dashboard, no dock, no surface

enum Cmd : UINT { kSepCmd = 0, kBack = 100, kUpdate, kRestart, kExit, kSettings, kCustomize, kWindows }; // posted to the owner as WM_COMMAND

int Run(HINSTANCE instance); // until bring back, restart or exit; returns the quit code, 2 relaunches
void ShowMenu(HWND owner); // the same dashboard for the dock's own tray icon: settings, editing, handing the taskbar to windows
void Note(WPARAM result, const std::wstring& tag); // the dock's updater answered: the open dashboard's update row follows

}
