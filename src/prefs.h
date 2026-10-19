#pragma once
#include <windows.h>

namespace prefs {

struct Look { // astraeus island's appearance, so the two read as one desktop
    int look = 0; // 0 dark, 1 light, 2 glass, 3 custom
    bool accentFixed = false; // false: the accent comes from the wallpaper
    int accentHex = 0xB8C4FF;
    int customBg = 0x121826;
    int glassBlur = 100; // 0 clear glass .. 100 full backdrop blur
    bool reduceMotion = false; // island's switch, else windows' own "animation effects" off
};

struct Settings { // the bar's own choices; "follow" means whatever island uses
    static constexpr int kFollow = -1;
    int look = kFollow; // kFollow, or 0 dark, 1 light, 2 glass, 3 custom
    int blur = 100; // glass frost when the bar picks its own look
    int accent = kFollow; // kFollow, 0 wallpaper, 1 the hex below
    int accentHex = 0x5B8CFF;
    int customBg = 0x121826; // the custom look's background, off the wheel
    int size = 1; // 0 compact, 1 normal, 2 large
    int pills = 1; // 1 new notifications rise from the island as pills, and windows' own banners stay quiet; 0 windows shows its banners
    int launcher = 1; // 1 the logo opens halobar's launcher, 0 windows' own start
    int room = 1; // 1 auto-hide: windows take the whole screen and the bar slips away under them; 0 always shown, maximized windows stop above it
    int song = 1; // the playing song sits in the bar
    int hoverPlay = 1; // resting on the song opens the player
    int date = 1; // the date under the time
    int clock24 = kFollow; // kFollow = the system's clock format, 0 12-hour, 1 24-hour
};

Look IslandLook(); // island's settings, or its defaults when it isn't installed
bool IslandInstalled(); // astraeus island is on this pc: its installed copy, its autostart entry, or it running
HANDLE WatchIsland(); // auto-reset event set when island's settings change, null without island
void Rearm(HANDLE ev); // after the event fired: watch for the next change
void Unwatch(HANDLE ev);

void MoveOldKey(); // settings kept under the bar's old name come over to HaloBar, once
Settings Load(); // the bar's own settings, HKCU\Software\HaloBar, each clamped to its range
int ReadNum(const wchar_t* name, int fallback);
void SaveNum(const wchar_t* name, int value);
void Delete(const wchar_t* name);

}
