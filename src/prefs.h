#pragma once
#include <windows.h>

namespace prefs {

struct Look { // astraeus island's appearance, so the two read as one desktop
    int look = 0; // 0 dark, 1 light, 2 glass, 3 custom, 5 starfall
    bool accentFixed = false; // false: the accent comes from the wallpaper
    int accentHex = 0xB8C4FF;
    int customBg = 0x121826;
    int glassBlur = 100; // 0 clear glass .. 100 full backdrop blur
    int bgOpacity = 100; // the solid looks' body, percent; 100 while island follows the bar back, as its look falls to dark
    bool followsBar = false; // island's look is set to follow the bar's: following it back would only loop to dark
    bool reduceMotion = false; // island's switch, else windows' own "animation effects" off
};

struct Settings { // the bar's own choices; "follow" means whatever island uses
    static constexpr int kFollow = -1;
    int look = kFollow; // the astraeus theme's material: kFollow, or 0 dark, 1 light, 2 glass, 3 custom
    int blur = 100; // glass frost when the bar picks its own look
    static constexpr int kMinOpacity = 40; // island's floor: below this the dock floats on nothing
    int opacity = 100; // the solid looks: how much of the desktop shows through the body, when the bar picks its own look
    int accent = kFollow; // kFollow, 0 wallpaper, 1 the hex below
    int accentHex = 0x5B8CFF;
    int customBg = 0x121826; // the custom look's background, off the wheel
    int size = 1; // 0 compact, 1 normal, 2 large
    int pills = 1; // 1 new notifications rise from the island as pills, and windows' own banners stay quiet; 0 windows shows its banners
    int launcher = 1; // 1 the logo opens halobar's launcher, 0 windows' own start
    int room = 1; // the dock. 1 auto-hide: windows take the whole screen and it slips away under them; 0 always shown, its strip kept free
    int mods = 0xFFF; // which top bar modules show, a bit each: workspaces, app, tray, keyboard, bluetooth, network, volume, battery, bell, quick settings, clock
    int top = 2; // the top bar. 0 always shown, its strip kept free; 1 auto-hide; 2 off: gone, the dock carries everything and astraeus has the top edge to itself
    int widgets = 0x117; // the dock's widgets, a bit each: weather, stats, focus, water, clipboard, picker, screenshot, emoji, downloads, notes
    int song = 1; // the playing song sits in the bar
    int winKeys = 1; // win + 1..9 opens, shows or tucks away the app in that spot, as windows' taskbar
    int adaptive = 0; // the centered dock stretches across the screen while a maximized window sits on it
    int trayApps = 0; // apps that only live in the tray show among the dock's apps, a grey dot under them
    int startIcon = 0; // the start button's face: 0 windows' logo, 1 halobar's
    int edge = 0; // the screen edge the dock stands on: 0 bottom, 1 left, 2 right; astraeus reads it to stay off that side
    int align = 0; // 0 the dock in the middle, sized to what's in it; 1 across the screen, apps from the left and the status at the right end; 2 the same, mirrored
    int screens = 1; // 0 the dock on one screen, the one picked or the main one; 1 a dock on every screen, as windows' own taskbar
    int hoverPlay = 1; // resting on the song opens the player
    int date = 1; // the date under the time
    int clock24 = kFollow; // kFollow = the system's clock format, 0 12-hour, 1 24-hour
};

inline constexpr wchar_t kBarKeyPath[] = L"Software\\HaloBar", kIslandKeyPath[] = L"Software\\Astraeus"; // the bar's settings, and island's (read only)
Look IslandLook(); // island's settings, or its defaults when it isn't installed
bool IslandInstalled(); // astraeus island is on this pc: its installed copy, its autostart entry, or it running
HANDLE WatchIsland(); // auto-reset event set when island's settings change, null without island
void Rearm(HANDLE ev); // after the event fired: watch for the next change
void Unwatch(HANDLE ev);

void MoveOldKey(); // settings kept under the bar's old name come over to HaloBar, once
Settings Load(); // the bar's own settings, HKCU\Software\HaloBar, each clamped to its range
int ReadNum(const wchar_t* name, int fallback);
int ReadShared(const wchar_t* name, int fallback); // what halobar and astraeus share, under astraeus' key: do not disturb
void SaveShared(const wchar_t* name, int value);
void SaveNum(const wchar_t* name, int value);
void Delete(const wchar_t* name);

}
