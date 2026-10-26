#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>
#include <ole2.h>

#include <algorithm>
#include <atomic>
#include <thread>
#include <functional>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <map>
#include <vector>

#include "anim.h"
#include "bt.h"
#include "radios.h"
#include "status.h"
#include "sys.h"
#include "wifi.h"
#include "gfx.h"
#include "icons.h"
#include "media.h"
#include "mixer.h"
#include "notifs.h"
#include "apps.h"
#include "calendar.h"
#include "toggles.h"

#include <unordered_set>
#include "prefs.h"
#include "preview.h"
#include "spectrum.h"
#include "tasks.h"
#include "theme.h"
#include "tray.h"
#include "ui.h"

constexpr UINT WM_APP_TASKS = WM_APP + 1; // wparam: Tasks::kChanged, kRescan or kTrayShown
constexpr UINT WM_APP_MEDIA = WM_APP + 2;
constexpr UINT WM_APP_APPBAR = WM_APP + 3;
constexpr UINT WM_APP_MENU = WM_APP + 4; // the bar's own menu, opened after the frame that saw the right click
constexpr UINT WM_APP_TRAY = WM_APP + 5; // a tray icon came, changed or went
constexpr UINT WM_APP_RADIOS = WM_APP + 6; // wi-fi or bluetooth went on or off
constexpr UINT WM_APP_BRIGHT = WM_APP + 7; // wparam: the screen's brightness, -1 if it can't be set
constexpr UINT WM_APP_WIFI = WM_APP + 8; // networks found or a connection changed
constexpr UINT WM_APP_BT = WM_APP + 9; // bluetooth devices came, went or connected
constexpr UINT WM_APP_HOTSPOT = WM_APP + 12; // the mobile hotspot's state is known, or a switch finished
constexpr UINT WM_APP_APPS = WM_APP + 11; // lparam: the app list and icons, read off the ui thread
constexpr UINT WM_APP_UPDATE = WM_APP + 17; // wparam: sys::Updater::Result
constexpr UINT WM_APP_POLLED = WM_APP + 18; // lparam: a new Bar::Polled, from the poller thread
constexpr UINT WM_APP_WX = WM_APP + 19; // the forecast arrived
constexpr UINT WM_APP_PHOTO = WM_APP + 24; // lparam: a photo's pixels, decoded off the ui thread
constexpr WORD kIslandBack = 120; // astraeus sends this as its fullscreen player, opened from our cover, closes
constexpr UINT WM_APP_HALOICON = WM_APP + 25; // clicks on halobar's own tray icon
constexpr UINT WM_APP_WINKEY = WM_APP + 26; // win + a number: wparam the spot, from 0
constexpr UINT WM_APP_ICON = WM_APP + 22; // lparam: an app icon read on the icon thread
constexpr UINT WM_APP_STATSRC = WM_APP + 21; // lparam: a new sys::StatSrc, made off the ui thread
constexpr UINT WM_APP_PICK = WM_APP + 20; // the color picker finished: wparam 0x00RRGGBB, lparam 1 picked, 0 cancelled
constexpr UINT WM_APP_CAL = WM_APP + 15; // calendar events fetched
constexpr UINT WM_APP_CALMENU = WM_APP + 16; // the connected calendars' menu, opened after the frame
constexpr UINT WM_APP_RESTYLE = WM_APP + 13; // wparam 1: re-place too. a setting that rebuilds the device, applied after the frame that clicked it
constexpr UINT WM_APP_NOTIFS = WM_APP + 10; // lparam: a new std::vector<sys::Notif>*, read off the ui thread

struct Swell { // soft light that rises from the floor and swells with the music, as on astraeus island's media page
    static constexpr int kMax = 8;
    float level[kMax] = {}, gain[kMax] = {}, heat = 0; // eased blob sizes, each blob's recent loudest, overall warmth
};

// The taskbar as one floating island: the song, the apps and the tray in a single black pill that grows up
// into the player, the whole tray, or an app asking for attention. One window, one composition surface;
// frames are drawn only while something moves, and then only where it moved.
void RestoreBanners(); // windows' toast banners back as the user had them: on exit and from the crash handler
void Keys(std::initializer_list<WORD> keys); // bar_quick.cpp: a windows shortcut, as if typed
int WifiGlyph(const sys::Status& s);
int BatteryGlyph(const sys::Status& s);
int VolumeGlyph(float level, bool muted);

class Bar {
public:
    void SetMoon(HMONITOR m, Bar* main) { moon = true, moonScreen = m, lead = main; } // another screen's dock: the taskbar, the tray and updates stay with the main one, which shares its windows, song, status and forecast
    bool SameScreen() const { // a moon's screen is still there, where it was
        MONITORINFO mi = {sizeof(mi)};
        return GetMonitorInfoW(moonScreen, &mi) && EqualRect(&mi.rcMonitor, &monitor);
    }
    HMONITOR Screen() const { return mon; }
    static HMONITOR MainScreen(); // the main dock's screen: the one picked in settings, else windows' main one
    static std::vector<HMONITOR> Screens(); // every screen, left to right
    Bar();
    ~Bar();
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    bool Create(HINSTANCE instance);
    HWND Hwnd() const { return hwnd; }
    bool NeedsFrame() const { return needsFrame; }
    HANDLE FrameWait() const { return frameWait; } // waitable timer that paces frames while something moves
    HANDLE PrefsWait() const { return prefsEv; } // set when island's settings change, null without island
    DWORD DragOver(POINT screen, bool files); // a file held over the dock: the app under it comes up; says whether a drop opens it there
    void DragGone();
    DWORD Drop(POINT screen, const std::vector<std::wstring>& files);
    void OnPrefs();
    HANDLE NotifWait() const { return notifWatch; } // set when windows' notification store is written: a new one may have come
    void OnNotifWrite();
    HANDLE PinsWait() const { return pinsEv; } // set when the windows taskbar's pins change
    void OnPins();
    std::vector<PinItem> PinList() const { return tasks ? tasks->PinList() : std::vector<PinItem>{}; }
    void Frame();
    bool Pass(const D2D1_RECT_F& area, const std::function<void()>& draw); // one region of the surface, drawn; false when the device went
    void DrawCard(const Theme& t, ID2D1Geometry* shape);

    // the top bar: two floating capsules, one each side, the middle left free for astraeus island
    static constexpr float kBarH = 32, kItemH = 24, kItemPad = 6, kItemGap = 4; // the strip, edge to edge; one module's hover plate
    static constexpr float kBarIcon = 15, kBarText = 13;
    // the dock: an island-like pill over the bottom edge
    static constexpr float kIcon = 40, kWegPad = 8, kWegGap = 6, kWegMargin = 8, kWegRadius = 22; // an item's spot, inner padding, between items, off the screen's edge
    static constexpr float kAppIcon = 28, kTileRadius = 12; // the app's icon, and its hover plate's corners
    static constexpr float kWegSep = kWegGap + 20, kMediaTileW = 176; // a separator's slot, room either side of its hairline; the song's wide spot
    static constexpr float kWegZoom = 1.0f, kWegReach = 2.6f; // no magnification unless raised here
    static constexpr float kWegH = kIcon + 2 * kWegPad;
    static constexpr float kShelfH = kWegH + 2 * kWegMargin; // the dock with its margins: the strip kept free when it doesn't auto-hide
    static constexpr float kSlot = kIcon + kWegGap, kSlotMin = 30;
    static constexpr float kAppSlot = kIcon + 14; // an app's spot: wider than the rest, so the icons breathe
    static constexpr float kRowH = kWegH; // the dock's row, for the code that rests on it
    static constexpr float kCardTop = kBarH + 6, kCardRadius = 22; // panels hang just under the top bar, island-round
    static constexpr float kArt = 30, kBarsW = 18; // the song's cover (player) and the toolbar's mini visualizer
    static constexpr float kPinCell = 26; // a tray icon kept out on the taskbar in windows' settings, in the toolbar
    static constexpr float kRoom = 600; // the tallest a dropdown gets: quick settings, plus a little
    static constexpr int kTrayCols = 8;
    static constexpr float kTrayCell = 38; // one icon in the grown tray
    static constexpr float kCallRise = 48, kCallW = 360; // the island grows this much taller when an app wants attention
    static constexpr float kMediaRise = 196, kMediaW = 520; // and this much into the player
    static constexpr float kSetRise = 470, kSetW = 760; // and into its settings
    static constexpr float kWgW = 420, kGalleryW = 660; // and into a widget's panel, or edit mode's gallery
    static constexpr float kSheetW = 380; // and into an app's sheet
    static constexpr float kLaunchW = 800; // and into the apps menu
    static constexpr float kQsW = 460, kCalW = 720, kNotifW = 360; // and into quick settings, or the clock: notifications beside the calendar
    static constexpr UINT_PTR kPreviewTimer = 9, kPreviewHideTimer = 10, kCoverTimer = 11, kPeekTimer = 12, kMixTimer = 14, kMixCloseTimer = 15, kGhostTimer = 16, kNotifReadTimer = 17, kPillTimer = 18, kAppsTimer = 19, kNotifPollTimer = 20, kFullTimer = 21, kPosTimer = 22, kCalWaitTimer = 24, kUpdateTimer = 25, kShadeTimer = 26, kTitleTimer = 27, kStatsTimer = 28, kFocusTimer = 29, kLangTimer = 30, kCaretTimer = 32;
    static constexpr UINT kTitleMs = 200; // title changes: at most five top bar frames a second
    static constexpr UINT kUpdateFirstMs = 60 * 1000, kUpdateEveryMs = 6 * 60 * 60 * 1000; // a minute after start, then a few times a day
    static constexpr UINT kCalWaitMs = 5 * 60 * 1000;
    static constexpr float kPeekRise = 52, kPeekW = 380; // the notification pill
    static constexpr UINT kPillMs = 6000; // it shows this long, longer while the mouse is on it
    static constexpr UINT kPreviewMs = 450, kPreviewHideMs = 300, kCoverMs = 60, kPeekMs = 700; // rest before the card shows; grace before it goes; event burst; stay up after the mouse leaves

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);

    void Place(); // screen, dpi, window and the reserved strip
    void Reserve();
    void Unreserve();
    void SetFull(bool on); // a fullscreen app in front: step aside
    bool FullInFront() const; // the window in front really fills the screen, with no frame: a game or a video, not a maximized app
    void SyncRegion(); // the window takes clicks only on the island
    D2D1_POINT_2F ToDip(LPARAM lp) const;
    POINT ToScreen(D2D1_POINT_2F p) const;
    void TrackLeave();
    void ArmClock();
    void StartMenu();
    void ShowPreview(); // the hovered app's window thumbnails, over its slot
    bool AutoHide() const { return cfg.room != 0; } // windows take the whole screen; the dock slips away while one covers it
    enum Mod { kModDesks, kModTitle, kModTray, kModLang, kModBt, kModNet, kModVol, kModBat, kModBell, kModQs, kModClock, kModDesktop, kModCount }; // the status modules, in order, a bit each in cfg.mods; desktop: show desktop, at the dock's far right
    bool Shows(int m) const { return (cfg.mods >> m) & 1; }
    float modAt[kModCount] = {}; // each status module's middle this frame, negative when it isn't shown: panels opened from elsewhere hang from it
    bool TopOn() const { return false; } // the top bar is retired: its status sits at the dock's end, the top edge is astraeus'
    bool TopAuto() const { return false; } // it slips up out of the way while a window covers it
    void SampleShade(); // how bright the screen is around the top bar and the dock: glass smokes over white pages
    bool Unseen() const { return full; } // behind a fullscreen app: the toolbar is otherwise always in sight
    bool Hidden() const { return full || (sink > 0.999f && Grown() < 0.5f && !AnyOpen()); } // nothing of this dock on screen: tucked away with nothing risen, or behind a fullscreen app
    bool StatsUnseen() const { return full || (sink > 0.5f && !wgOpen); } // the load and network numbers can't be seen: not read
    bool statsParked = false, shadeStale = false; // a tick or a screen read skipped while unseen: done as the dock comes back
    bool ScreensMoved() const; // a screen came or went since start, or a moon's own one moved
    static const std::vector<Bar*>& Docks(); // every dock, main one first
    Bar* lead = nullptr; // a moon's main dock: one window list, one song, one status read and one forecast for every screen
    sys::Media& Player() { return lead ? lead->media : media; }
    sys::Meter& Peaks() { return lead ? lead->meter : meter; }
    void RefreshWx(); // the main dock fetches; the answer goes to every dock
    void TakeStatus(const sys::Status& s, int bin); // the poller's read, applied to this dock
    void FollowNotifs(); // a moon: the main dock saw windows' notification store change
    bool PillsLive() const; // a new notification would rise as a pill right now: the store is looked at often
    bool notifPolling = false; // the quick look at the store is armed
    double hideAt = 0; // explorer showing its taskbar back: this second's count, and how many seconds in a row it kept on
    int hideCount = 0, hideStrikes = 0;
    void CheckCovered(); // does a window sit where the island rests
    void HookCover(bool on);
    static void CALLBACK OnCoverEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    void ShowDesktop();
    static constexpr int kLogo = 0; // LabelButton's glyph for the windows logo
    bool LabelButton(const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label); // a pill with an icon and words, the same everywhere
    void WinLogo(D2D1_POINT_2F c, float size, Color top, Color bottom); // windows' four panes, drawn: the icon font has no logo
    float LaunchRise() const; // bar_launch.cpp: the app launcher
    void OpenLauncher();
    void OnApps(WPARAM step, LPARAM lp);
    void LoadApps(bool withIcons);
    void LaunchKey(WPARAM vk);
    void LaunchChar(wchar_t ch);
    void LaunchPick(int i);
    void DrawLauncher(const Theme& t, ID2D1Geometry* shape);
    LONG CornerPx() const { return (LONG)std::lround(8 * scale); } // the show-desktop strip at the screen's far right
    void ShowMenu();
    void GhostOpen(const std::wstring& exe);
    enum class Panel { None, Media, Tray, Settings, Sheet, Quick, Calendar, Launch, Widget }; // what the island has grown into: one at a time
    void SetPanel(Panel p);
    void SetMediaOpen(bool open) { open ? SetPanel(Panel::Media) : mediaOpen ? SetPanel(Panel::None) : void(); }
    void SetTrayOpen(bool open) { open ? SetPanel(Panel::Tray) : trayOpen ? SetPanel(Panel::None) : void(); }
    void SetSettingsOpen(bool open) { open ? SetPanel(Panel::Settings) : setOpen ? SetPanel(Panel::None) : void(); }
    void SetSheet(const std::wstring& key); // an app's sheet, empty closes it
    void CloseAll() { SetPanel(Panel::None); }
    bool AnyOpen() const { return mediaOpen || trayOpen || setOpen || sheetOpen || qsOpen || calOpen || launchOpen || wgOpen; }
    void SetPref(int& field, const wchar_t* name, int value); // saved, and applied right away
    void SetEdge(int e); // the dock moves to another screen edge, in every dock, with no restart
    void ToEdge(int e); // this dock's share of it
    void ClaimTray(); // apps' tray icons come to the bar from now on

    bool EnsureDev();
    void FreeDev();
    void Retry(); // the device or a draw failed: try again shortly, give the taskbar back if it keeps failing
    UINT IconPx() const { return (UINT)(std::max)(16L, std::lround(kAppIcon * scale)); } // the size on the dock's tiles, in pixels: drawn 1:1 there, so it stays sharp
    void Redraw();
    void Arm(double seconds);
    bool Caret(); // a text field's blinking caret: shown now or not, with a frame booked for its next flip
    void Idle(); // still for a while: efficiency mode, caches handed back

    int LookNow() const; // the look in use: 0 dark, 1 light, 2 glass, 3 island's custom
    float WegRadius() const { return kWegRadius; }
    float CardRadius() const { return kCardRadius; }
    float TileRadius() const { return kTileRadius; }
    void BelowIsland(); // astraeus hangs over the toolbar's middle: the bar keeps just under it
    void KeepOnTop(); // windows can lift a maximized app over topmost windows: the dock climbs back
    void HaloIcon(bool show); // halobar's own icon in the tray, its dashboard a click away
    float Blur() const;
    bool OwnOpacity() const { return cfg.look != prefs::Settings::kFollow || !islandHere; } // following island: its opacity, as its look
    float Opacity() const { return (OwnOpacity() ? cfg.opacity : look.bgOpacity) / 100.0f; }
    Color Body(const Theme& t) const { return t.glass ? t.bg : t.bg.Alpha(Opacity()); } // the material's body: the solid looks let the desktop through, glass is the blur's
    static constexpr float kZoomMax = 1.12f; // large: the surface is made this tall, so a size change never rebuilds it
    float Zoom() const { return cfg.size == 0 ? 0.9f : cfg.size == 2 ? kZoomMax : 1.0f; }
    Theme Make(Seed s) const; // a palette in the bar's current look
    void SyncLook();
    bool SyncWall(); // true if the wallpaper gave a new seed
    void SetShell(const Theme& t);
    void SetMedia(const Theme& t);
    bool StepTheme(float dt);

    // bar_draw.cpp
    bool StepSprings(float dt);
    float CallH() const { return (std::max)(0.0f, callSpring.value); }
    float Grown() const { // height above the row
        return CallH() + PeekH() + (std::max)(0.0f, mediaSpring.value) + (std::max)(0.0f, traySpring.value) + (std::max)(0.0f, setSpring.value) +
               (std::max)(0.0f, sheetSpring.value) + (std::max)(0.0f, qsSpring.value) + (std::max)(0.0f, calSpring.value) + (std::max)(0.0f, launchSpring.value) +
               (std::max)(0.0f, wgSpring.value);
    }
    float TrayRise() const;
    bool Layout(float dt); // where everything sits this frame, before drawing: true while widths still ease
    ComPtr<ID2D1Geometry> Shape() const;
    ComPtr<ID2D1Geometry> BuildShape() const;
    mutable ComPtr<ID2D1Geometry> shapeCache; // the outline for shapeFor, reused while the island holds still
    mutable D2D1_RECT_F shapeFor{};
    D2D1_RECT_F Outer() const { return Union(Union(barR, DockArea()), CardOnScreen()); } // all a frame paints: the dock's tooltips rise above it
    const TaskApp* FindApp(const std::wstring& key) const;
    float TextW(const std::wstring& s, Weight w, float size);
    void DrawApp(const TaskApp* a, const D2D1_RECT_F& r, float alpha, const Theme& t, UINT px = 0); // its icon (px: the pixels wanted, 0 the dock's), or its initial on a tile
    void DrawStart(const Theme& t);
    void DrawDock(const Theme& t); // bar_dock.cpp, with the two below
    void DrawApps(const Theme& t, const D2D1_RECT_F& r, bool live); // the apps as one block among the widgets, and a "+N" for those without room
    void DrawSheet(const Theme& t, ID2D1Geometry* shape);
    void DrawProgress(const Badge& b, float cx, float y, float w, float grow, const Theme& t);
    float SheetRise() const;
    ID2D1Bitmap* IconBitmap(HICON icon, uint64_t key, uint64_t ver);
    void DrawCalling(const Theme& t, ID2D1Geometry* shape);
    void DrawPeek(const Theme& t, ID2D1Geometry* shape); // a new notification, as a pill rising out of the island
    float PeekH() const { return (std::max)(0.0f, peekSpring.value); }
    bool Quiet() const; // do not disturb: no pills (and, with pills off, no windows banners)
    bool Hush() const; // windows' banners hidden: our pills or astraeus' show them, or quiet
    void SetQuiet(bool on);
    void Banners(bool hush); // windows' own toast banners off while our pills show them, back as they were after
    void PaintShape(ID2D1Geometry* shape, const Theme& t);

    // bar_tool.cpp: the toolbar across the top and the dock at the bottom
    void DrawToolbar(const Theme& t);
    void DrawWeg(const Theme& t);
    void PaintPlate(const D2D1_RECT_F& r, float radius, const Theme& t, float shade, const D2D1_RECT_F* only = nullptr); // a capsule or the dock: the material, its gloss, a hairline rim; only: the one box a pass repaints
    static bool Within(const D2D1_RECT_F& in, const D2D1_RECT_F& r, float radius, float depth); // a box at least depth inside a rounded rect
    float DrawStatus(const Theme& t, float x, float cy, float h, bool dock); // tray, keyboard, the status cluster, the clock, left to right from x; returns the width
    void DrawMediaTile(const Theme& t, bool live); // the song's spot in the dock; live false: pixels only, for the visualizer's own frames
    void SyncClockText(); // the clock's words, formatted once a minute
    ComPtr<ID2D1Geometry> Chrome(); // toolbar, dock and an open dropdown, on screen: what the blur shows through
    bool Module(uint32_t id, const D2D1_RECT_F& r, bool lit); // one toolbar module's hover plate; true when clicked
    float ModuleW(const std::wstring& text, int icons, Weight weight = Weight::Regular); // padding, icons and text
    void Anchor(float x, bool down) { anchorX = x, cardDown = down; } // where the next dropdown hangs from: a point along the dock
    bool Vert() const { return cfg.edge == 1 || cfg.edge == 2; } // the dock stands at the left or right edge
    float Along(D2D1_POINT_2F p) const { return Vert() ? p.y : p.x; } // where along the dock a point is
    float Mid(const D2D1_RECT_F& r) const { return Vert() ? (r.top + r.bottom) / 2 : (r.left + r.right) / 2; } // an item's middle along the dock
    D2D1_RECT_F Spot(float at, float len, float cross) const { return Vert() ? Rect(cross, at, kIcon, len) : Rect(at, cross, len, kIcon); } // an item's tile: along the row, or down the column
    D2D1_RECT_F TipRect(const D2D1_RECT_F& over, float tw) const;
    mutable D2D1_RECT_F tipNow{}; // the name pills drawn this frame: they sit off the dock, so the window region takes them in // a hovered item's name pill: over the row, or beside the column
    D2D1_RECT_F DockArea() const; // the dock, its shadow and its tooltips
    void DrawWegCol(const Theme& t); // the dock standing at a side edge
    void ShowPowerMenu();
    void ShowUserMenu();
    void AppMenu(); // menuApp's menu, after the frame that right clicked it
    std::wstring menuApp;
    void SyncUser();
    void OpenQuick(int page, float x);
    void DrawTile(const Theme& t, const D2D1_RECT_F& tile, float hover, float alpha);
    std::wstring userName;
    ComPtr<IWICBitmapSource> avatarSrc;
    ComPtr<ID2D1Bitmap> avatarBmp;
    void SyncDesktops(); // virtual desktops: how many, and which is in front
    D2D1_RECT_F CardOnScreen() const { return pill.right > pill.left ? D2D1_RECT_F{pill.left + cardOff.x, pill.top + cardOff.y, pill.right + cardOff.x, pill.bottom + cardOff.y} : D2D1_RECT_F{}; }
    bool OnChrome(D2D1_POINT_2F p) const { return Inside(capL, p) || Inside(capR, p) || Inside(wegHit, p) || Inside(CardOnScreen(), p); }
    void DrawTrayGrid(const Theme& t, ID2D1Geometry* shape);
    ID2D1Bitmap* TrayBitmap(const TrayItem& it);
    int TrayCount() const { return (tray ? tray->Visible() : 0) + (int)ghosts.size(); } // real icons and the ones windows remembers
    void SyncGhosts(); // apps whose icon went to explorer before we took the tray: shown from windows' own snapshot of it
    void OpenOverflow(); // windows' own flyout over the tray button, or the bar's grid when explorer has none
    void SyncPromoted(); // the icons windows' settings keep out on the taskbar: those sit in the row, beside the button
    void DrawPromoted(const Theme& t);
    float PromotedW() const { return (float)promoted.size() * kPinCell; }
    int HiddenCount() const { return (std::max)(0, TrayCount() - (int)promoted.size()); } // what the flyout holds

    // bar_quick.cpp: quick settings, calendar
    void SyncLang(); // the keyboard layout in front, shown only when there is more than one
    void SyncAudio();
    void PollStatus();
    struct Polled {
        sys::Status status;
        int bin = -1; // -1 not read this time, else 1 full
    };
    void StartPoller();
    void StopPoller();
    void OnPolled(LPARAM lp);
    std::thread poller; // network, battery and the bin, read off the ui thread
    HANDLE pollWake = nullptr;
    std::atomic<bool> pollStop{false}, binAsk{false};
    float QuickRise() const;
    void OpenQuickPage(int page); // 0 main, 1 wi-fi, 2 bluetooth, 3 sound output
    void DrawQuick(const Theme& t, ID2D1Geometry* shape);
    void QuickMain(const Theme& t, const D2D1_RECT_F& r);
    void QuickList(const Theme& t, const D2D1_RECT_F& r);
    float MixRows(const Theme& t, const D2D1_RECT_F& r, float y, int rows); // every app's own volume, under the outputs
    int MixFit() const;
    D2D1_RECT_F lastTile{}; // the quick tile just drawn, for a badge over its icon
    void BeginPass(const sys::WifiNet& n); // a password field under that network; the bar takes the keyboard while it's typed
    void EndPass(); // the field goes, its text wiped, the keyboard handed back
    void SubmitPass(); // every app's own volume, island's mixer
    float CalRise() const;
    void LoadNotifs(); // reads them on a worker; WM_APP_NOTIFS brings them back
    void DrawNotifs(const Theme& t, const D2D1_RECT_F& r);
    void DrawCalendar(const Theme& t, ID2D1Geometry* shape);

    // bar_widgets.cpp, bar_wgdraw.cpp, bar_wgpanel.cpp: the dock's widgets, picked, ordered and sized in its edit mode; the song and the bin are widgets too
    enum Wg { kWgMedia, kWgBin, kWgWeather, kWgStats, kWgFocus, kWgWater, kWgClip, kWgPicker, kWgShot, kWgEmoji, kWgFiles, kWgNotes, kWgCalendar, kWgTodo,
              kWgClock, kWgProgress, kWgToggles, kWgAudio, kWgSearch, kWgSwitch, kWgFolder, kWgNet, kWgBattery, kWgHabit, kWgRetired, kWgPhoto, kWgDivider, kWgApps, kWgDrives, kWgSpace, kWgCount }; // retired: once crypto, its number kept so saved layouts still line up
    static constexpr int kSpaceMin = 8, kSpaceMax = 480, kSpaceDef = 48, kSpaceStep = 16; // the space widget: its width in dips lives in its slot's size
    static bool Many(int kind) { return kind == kWgDivider || kind == kWgSpace; } // added again on every pick, never "in"
    int spaceFrom = 0; // the space's width as its edge was grabbed
    float spaceCap = 1e9f, spaceSlack = 0; // no space wider than the cap, so every app fits; the room still free for them to grow into
    std::vector<float> spaceWs; // Layout's scratch: each space's own width this frame
    static constexpr int kWgGallery = 100; // wgKind while edit mode's gallery is up
    struct WgSlot {
        int kind = 0, size = 1; // size: 0 small, 1 medium, 2 large
    };
    std::vector<WgSlot> wgSlots; // in dock order
    float fitW[kWgCount] = {}; // net, battery and weather: as wide as their words, measured as the numbers change; 0 until first measured
    double fitLow[kWgCount] = {}; // since when a shorter text has wanted less room
    void SyncFit(); // grows a fitted widget at once, shrinks it only once a shorter text has held a while
    bool HasWg(int w) const;
    float WidgetW(int kind, int size) const;
    float WidgetsW() const; // the group, without separators; 0 with none
    void LoadSlots();
    void SaveSlots();
    void DrawWidgets(const Theme& t, float x, float top); // the group, left to right from x; in edit mode, each one movable and removable
    void DrawWg(const Theme& t, int kind, int size, const D2D1_RECT_F& r, bool live); // one widget's face; live: its own buttons work
    void WgClick(int kind, bool right, float cx); // a click on a widget outside edit mode
    std::wstring WgTip(int kind) const;
    void DrawWidgetPanel(const Theme& t, ID2D1Geometry* shape);
    void DrawGallery(const Theme& t, const D2D1_RECT_F& r);
    float WidgetRise() const;
    float WidgetPanelW() const;
    void OpenWidget(int w, float x);
    void StartEdit(); // the dock's edit mode: the gallery rises over it
    bool editing = false;
    int wgSel = -1, dragSlot = -1; // the slot picked in edit mode, and the one being dragged
    float wgGrab = 0; // where in the held widget the mouse took it
    int galleryCat = 0;
    float galleryScroll = 0;
    void SyncWidgets(); // starts and stops what the widgets in the dock need, nothing for the rest
    void WidgetTick(); // the 2 s tick, only while a live widget is in sight
    void OnClipboard2();
    void OnPick(WPARAM rgb, LPARAM ok);
    void StepFocus();
    void LoadFiles(const std::wstring& dir);
    struct DriveRow {
        wchar_t letter = 0;
        std::wstring label;
        ULONGLONG total = 0, free = 0;
        bool removable = false;
    };
    std::vector<DriveRow> drives; // read when the drives widget opens or windows says a drive came or went
    void LoadDrives();
    static void Eject(wchar_t letter); // the shell's own eject: windows says when it's safe
    bool Typing() const { return wgOpen && (wgKind == kWgNotes || wgKind == kWgTodo || wgKind == kWgHabit); } // a panel that takes the keyboard
    void WgChar(wchar_t ch);
    void SaveNote();
    void SaveLists();
    void PickPhoto();
    bool wgOpen = false;
    int wgKind = -1;
    anim::Spring wgSpring;
    std::unique_ptr<sys::StatSrc> statSrc; // made only while the stats widget is in the dock
    bool statMaking = false; // a stat source is being made on its thread
    sys::Stats stats;
    float cpuHist[30] = {}; // the last minute of load, for the large card's graph
    int cpuAt = 0;
    sys::WxSvc weather;
    sys::Wx wx;
    double focusEnd = 0, focusLeft = 0; // a running session ends at focusEnd; a paused one keeps focusLeft seconds
    int water = 0; // glasses today
    struct Clip { // text, or an image kept compressed with a small preview; in memory only, never written anywhere
        std::wstring text;
        std::vector<BYTE> png;
        UINT w = 0, h = 0;
        ComPtr<IWICBitmap> pixels; // the preview, made once
        ComPtr<ID2D1Bitmap> thumb; // and on the device, made when first drawn
    };
    std::vector<Clip> clips; // newest first
    std::wstring ClipLine(const Clip& cl) const; // what a row says: the text's first line, or the image's size
    void CopyClip(int i);
    bool clipListening = false;
    int pickHex = -1; // the last picked color
    struct FileRow {
        std::wstring name, path;
        ComPtr<ID2D1Bitmap> icon;
        ComPtr<IWICBitmapSource> pixels;
    };
    std::vector<FileRow> files;
    std::wstring filesDir;
    std::wstring note, draft; // the note; the todo or habit being typed
    bool noteLoaded = false, listsLoaded = false;
    struct Todo {
        std::wstring text;
        bool done = false;
    };
    std::vector<Todo> todos;
    struct Habit {
        std::wstring name;
        int last = 0, streak = 0; // the day (yyyymmdd) last done, and days in a row
    };
    std::vector<Habit> habits;
    ComPtr<IWICBitmapSource> photoSrc;
    ComPtr<ID2D1Bitmap> photoBmp;
    std::map<std::wstring, std::wstring> iconPaths; // an app's own icon picked by the user, by app key
    std::map<std::wstring, ComPtr<ID2D1Bitmap>> customBmps; // and made on the device, once
    ID2D1Bitmap* CustomIcon(const std::wstring& key, UINT px);
    void LoadIconPaths();
    void PickIcon(const std::wstring& key); // a file picker; empty key: the choice is cleared
    ComPtr<ID2D1Bitmap> haloLogo; // the start button's face when it's halobar's own
    void StartMark(D2D1_POINT_2F o, float size, const Theme& t);
    void StartMark(D2D1_POINT_2F o, float size, int face, Color hi, Color lo); // either face, in any ink: settings shows both
    int dockKey = -1; // win + t: the app the keyboard is on, -1 none
    HWND keyBack = nullptr; // where the keyboard came from, and goes back to
    void FocusDock(int step); // win + t, with shift backwards
    void DockKey(WPARAM vk);
    void LeaveDock(bool back);
    int dropApp = -1; // the app a dragged file is held over
    ULONGLONG dropSince = 0;
    bool dropRaised = false;
    ComPtr<IDropTarget> dropper;
    void SyncDrop();
    int AppAt(POINT screen) const;
    std::unordered_map<uint64_t, D2D1_RECT_F> trayAt; // each tray icon's place in the last full frame: apps ask where theirs is
    bool ScreenRect(const D2D1_RECT_F& r, RECT& out) const;
    HHOOK keyHook = nullptr; // win + 1..9, only while the taskbar is ours and the setting is on
    void SyncKeyHook();
    static LRESULT CALLBACK OnKey(int code, WPARAM wp, LPARAM lp);
    bool maxHere = false; // a maximized window on this screen: an adaptive dock runs the full width
    float rowContent = 0; // what the dock's items need along it, whatever width it's drawn at
    std::vector<size_t> trayOnly; // tray icons of apps with no window on the taskbar: shown among the apps
    void SyncTrayOnly();
    std::wstring photoPath;
    std::atomic<bool> photoBusy{false};
    uint64_t netIn = 0, netOut = 0; // byte counters at the last tick
    double netDown = 0, netUp = 0; // bytes a second
    double netAt = 0;
    int focusMin = 25, waterGoal = 8; // per widget options, from edit mode
    bool fahrenheit = false;
    int folderPick = 0; // documents, desktop, pictures, music, videos
    int mediaSize = 1; // the song widget's size, for DrawMediaTile

    // bar_settings.cpp
    void DrawSettings(const Theme& t, ID2D1Geometry* shape);
    void SetHero(const Theme& t, const D2D1_RECT_F& r);
    void SetMaterial(const Theme& t, const D2D1_RECT_F& r);
    void SetWheel(const Theme& t, D2D1_POINT_2F o, float r); // the custom look: accent and background, any color, off one wheel
    void SetChip(const Theme& t, const D2D1_RECT_F& r, int which, const wchar_t* name);
    void SetShade(const Theme& t, const D2D1_RECT_F& r);
    int& WheelColor(int which) { return which == 0 ? cfg.accentHex : cfg.customBg; }
    void StoreWheel(int which, int hex, bool done);
    int wheelPick = 0; // the handle being edited: 0 accent, 1 background
    void SetSize(const Theme& t, const D2D1_RECT_F& r);
    void SetRoom(const Theme& t, const D2D1_RECT_F& r);
    void SetMods(const Theme& t, const D2D1_RECT_F& r); // which status modules the dock shows
    void SetWidgets(const Theme& t, const D2D1_RECT_F& r); // the way into the dock's edit mode
    void SetApp(const Theme& t, const D2D1_RECT_F& r); // updates, and handing the taskbar back to windows
    void SetExtras(const Theme& t, const D2D1_RECT_F& r); // keys, width, tray apps
    void SetStart(const Theme& t, const D2D1_RECT_F& r); // the start button's face
    void SetScreens(const Theme& t, const D2D1_RECT_F& r); // one screen or every screen, and which is the main one
    void SetPlace(const Theme& t, const D2D1_RECT_F& r); // a little monitor: tap where the dock goes, an edge or a spot along the bottom
    float setScroll = 0; // how far the settings sheet is scrolled, eased toward
    void SetSong(const Theme& t, const D2D1_RECT_F& r);
    void SetClock(const Theme& t, const D2D1_RECT_F& r);

    // bar_glass.cpp: the glass look, the island over a live blur of what's behind it
    struct Glass;
    bool OpenGlass();
    void FreeGlass();
    void GlassMask(ID2D1Geometry* shape);
    void SyncMask(); // GlassMask with this frame's chrome, only when it moved
    void SampleBehind(); // how bright the screen is where the island grows: bright pages get island's smoked glass
    HRESULT BeginSurface(const RECT& px, IDXGISurface** out, POINT* off);
    void EndSurface();
    void TrimSurface(const RECT* keep, UINT n);
    void Present();

    // bar_media.cpp
    void OnMedia();
    void SyncCover();
    bool StepBars(float dt, double now);
    void DrawBars(float dt, double now, bool step = true);
    void DrawPlayer(ID2D1Geometry* shape);
    void Wave(const D2D1_RECT_F& r, const Theme& t);
    bool StepSwell(float dt, bool playing);
    void DrawSwell(const D2D1_RECT_F& r, const Theme& t, double now);

    HWND hwnd = nullptr;
    HMONITOR mon = nullptr, moonScreen = nullptr; // where the dock is; a moon's screen, fixed
    bool moon = false;
    int screensAt = 0; // screens at start: every screen's docks are made again when that changes
    RECT monitor{};
    float dpi = 1; // the screen's own scale; scale adds the bar's size setting on top
    float scale = 1, W = 0, winH = 0; // dips; the window spans the screen's width, the island floats kGap above its bottom
    int winPxW = 0, winPxH = 0, barPx = 0;
    int surfPxW = 0, surfPxH = 0; // the drawing surface: the window at the largest size
    double hz = 60;
    UINT ambientSync = 2; // refreshes per frame for slow motion, ~30 fps at any refresh rate
    bool appbar = false, full = false, tracking = false, eco = false, hand = false;
    bool reserving = false; // inside Reserve: the shell's own notice about it is ignored
    bool took = false; // the windows taskbar is put away only once a frame of ours is really on screen
    UINT tbMsg = 0; // TaskbarCreated: explorer restarted, or the tray host asked apps to re-add their icons
    double tbSelfAt = -10; // when we sent TaskbarCreated ourselves, so we don't answer our own call

    std::shared_ptr<Tasks> tasks; // the main dock's, shared by the moons: one walk of the windows, one pin list
    std::unique_ptr<TrayHost> tray;
    Icons icons;
    sys::Media media;
    sys::Meter meter;
    std::unique_ptr<sys::Spectrum> spectrum; // only while the player shows the music
    double spectrumAsked = 0;

    HANDLE frameWait = nullptr, prefsEv = nullptr, pinsEv = nullptr;
    bool needsFrame = false, fullNext = true, trimDue = false, barsOnly = false;
    bool allNext = true; // something changed that either bar may show: both passes draw, nothing skipped
    bool topLive = true, botLive = true; // that pass still animates: it draws again next frame
    bool overTop = false, overBot = false; // the mouse was over that bar last frame: its hover has to fade out
    double lastFrame = 0;
    int devTries = 0;
    D2D1_RECT_F drawnTop{}, drawnBottom{}; // what the surface holds at the top and the bottom: a full frame repaints these and the new bounds
    D2D1_RECT_F barsRect{}; // the media spot's little bars: all a frame repaints while only they move

    Input input;
    Ui ui;
    bool midClick = false, menuWanted = false;

    D2D1_RECT_F pill{}; // an open dropdown, laid out as it always was (growing up from rowTop, centered); cardOff moves it on screen
    D2D1_POINT_2F cardOff{};
    float anchorX = 0; // the module or dock item it hangs from
    bool cardDown = true; // from the toolbar, or rising over the dock
    D2D1_RECT_F barR{}, wegR{}, wegHit{}, wegDrawn{}; // the top bar's strip; the dock's pill at rest, the room its magnified icons take, and the pill as drawn
    D2D1_RECT_F capL{}, capR{}; // the top bar's strip as drawn, and (unused while it's one strip) a second part
    float topSink = 0; // auto-hide: 0 showing, 1 slipped up off the screen
    bool topCovered = false, topPeek = false; // a window reaches under the top bar; the mouse came to the top edge
    float statusW = 0; // the status group's width in the dock, last frame: the dock sizes to it
    float shadeTop = 0, shadeBot = 0, shadeTopNow = 0, shadeBotNow = 0; // brightness behind the glass, measured and eased
    std::wstring clockTime, clockDate; // formatted for clockMin
    float clockWBar = 0, clockWDock = 0; // their widths: on one line in the top bar, stacked in the dock
    float cardX = 0; // where the open panel's middle is drawn: glides to anchorX when it moves to another module
    bool qsRight = false; // a quick tile took this frame's right click: the panel doesn't also step back
    int clockMin = -1;
    int shadeTick = 0; // status ticks: the glass looks behind itself every third
    HWND shelf = nullptr; // an unseen window holding the dock's strip: one appbar per window, and the bar's own holds the top
    bool shelfBar = false;
    bool lowering = false; // inside BelowIsland's own z-order move
    HWND taskmanWas = nullptr; // explorer's taskman window, given back on exit
    bool titleArmed = false, topAsk = false; // a title change waits on kTitleTimer; then only the top bar draws
    std::vector<float> wegX, wegW, wegS; // each app's place in the dock this frame: left edge, width, magnification
    ComPtr<ID2D1Bitmap> binBmp[2]; // the recycle bin, empty and full
    bool binFull = false;
    void SyncBin(); // empty or not: asked on the slow timer, windows doesn't say
    void BinMenu();
    std::unordered_map<uint64_t, std::pair<std::wstring, float>> textWs; // strings' widths, measured once each, by a hash of words, weight and size
    D2D1_RECT_F mediaR{}, trayR{}, startR{}; // modules other code aims at: the song, the tray chevron, the start button
    std::wstring lang, langSub, langName; // "ENG" over "US", and the layout's full name; empty with a single layout
    uint64_t langKey = 0; // the layout and installed list those were read for: naming a variant walks the whole registry list
    int desks = 1, desk = 0; // virtual desktops, for the workspaces module
    float rowTop = 0, rowW = 0, dockX = 0;
    float mediaShow = 0, mediaTextW = 0;
    int dockCount = 0; // apps that fit in the row on this screen
    int moreN = 0; // and those that don't: behind the "+N" tile
    float appsW = 0; // the apps block's width, worked out in Layout
    float slotW = kSlot; // each app's room in the row this frame
    std::wstring mediaMeasured, mediaMeasuredBy; // title and artist the row was last sized for
    anim::Spring peekSpring; // the notification pill
    anim::Spring callSpring, mediaSpring, traySpring, setSpring, sheetSpring, qsSpring, calSpring, launchSpring; // how much taller the island has grown for each
    bool mediaOpen = false, trayOpen = false, setOpen = false, sheetOpen = false, qsOpen = false, calOpen = false, launchOpen = false; // set together by SetPanel
    bool hoverArmed = false, closeArmed = false;
    std::wstring sheetKey; // whose sheet, kept while it shrinks away
    struct Recent {
        std::wstring name; // what the file is called
        std::vector<BYTE> pidl; // where it is, to open it as the jump list would
    };
    std::vector<Recent> sheetRecent; // the app's own recent files, read as its sheet opens
    void ReadRecent(const TaskApp& a);
    int dragFrom = -1; // the app being dragged along the row
    bool dragging = false, dragged = false;
    prefs::Settings cfg;

    Preview card; // the hovered app's window thumbnails
    std::wstring hoverApp, previewMuted; // the app under the mouse; one whose card was just used, quiet until the mouse moves on
    D2D1_RECT_F hoverSlot{};
    std::unordered_map<std::wstring, D2D1_RECT_F> slots; // each app's place in the row this frame: thumbnails and minimize animations aim there
    float sink = 0; // auto-hide: 0 showing, 1 tucked below the screen's edge
    bool covered = false, peek = false; // a window sits where the island rests; the mouse came to the edge to call it back
    POINT parkedAt{LONG_MIN, LONG_MIN}; // the cursor as the dock tucked away: the edge strip appearing under a still cursor is no call back

    sys::Radios radios;
    sys::Bright bright;
    sys::Wifi wifi; // both lists open only while their page shows
    sys::Bt bt;
    sys::Hotspot hotspot;
    bool hotspotSwitching = false; // the user flipped it and windows hasn't answered yet: only then does the tile say so
    struct Ghost {
        std::wstring exe, tip; // the running app, and what its icon said
        ComPtr<IWICBitmapSource> px;
        ComPtr<ID2D1Bitmap> bmp;
    };
    std::vector<Ghost> ghosts;
    std::wstring wheelApp; // the app whose own volume the wheel last turned, and when: its ring shows a moment
    double wheelAt = -10;
    float wheelVol = 0;
    sys::Mixer mixer; // per-app volume: holds nothing until its page or a wheel over an app needs it
    std::unordered_map<std::wstring, ComPtr<ID2D1Bitmap>> mixIcons; // each app's icon on the mixer page, made once per device
    std::unique_ptr<sys::AudioDev> spk, mic;
    sys::Status status;
    int brightPct = -1; // -1: this screen's brightness can't be set
    float volShown = 0;
    bool muteShown = false, powerRow = false;
    int qsPage = 0, calYear = 0, calMonth = 0;
    int calPick = 0; // the day tapped in the shown month, 0 = today
    sys::cal::Calendars cals; // the user's own calendars, from their private links
    sys::Updater updater;
    bool updateReady = false; // a new exe is in place: restart into it once nothing is open
    std::wstring updateNote; // what the last check asked from the menu found
    void OnUpdate(WPARAM result);
    bool calWaiting = false; // "connect" was pressed: the next calendar link copied anywhere is taken
    void StartCalWait(bool openGoogle);
    void StopCalWait();
    void OnClipboard();
    void CalMenu();
    void DrawAgenda(const Theme& t, const D2D1_RECT_F& r, int y, int m, int d);
    bool qsMore = false; // quick settings' rarer tiles shown
    std::vector<sys::WifiNet> nets;
    std::vector<sys::AppEntry> appList; // everything start can launch, read once on a worker
    std::vector<ComPtr<IWICBitmapSource>> appIcons; // their icons, same order
    std::unordered_map<int, ComPtr<ID2D1Bitmap>> appBmps; // gpu copies of the ones shown
    bool appsLoading = false, appIconsDone = false;
    double appsAt = 0;
    std::wstring query; // what's typed in the launcher
    std::vector<int> launchHits; // what it shows: indices into apps, or -(row place + 1) for a pin
    int launchSel = 0;
    bool launchAll = false; // all apps rather than the favourites
    struct LaunchRow {
        int kind = 0, idx = 0, sel = -1; // kind: 0 a letter (idx is it), 1 an app, 2 a folder, 3 an app inside an open folder; sel: its place among the pickable rows
        float y = 0, h = 0;
    };
    std::vector<LaunchRow> launchRows; // all apps as start lists it: letters, apps and folders, laid out once per change
    std::vector<int> launchListHits; // the pickable rows' targets, as launchHits holds them
    std::vector<std::wstring> launchFolders, launchOpenDirs; // the folders in the list; the ones opened
    double launchRowsAt = -1; // the list the rows were made from
    int launchDirsVer = 0, launchRowsVer = -1;
    void BuildLaunchRows();
    void DrawLaunchList(const Theme& t, const D2D1_RECT_F& body, int& pick);
    bool LaunchList() const { return launchAll && query.empty(); } // all apps: start's list, not the grid
    float launchScroll = 0;
    D2D1_POINT_2F lastLaunchMouse{};
    std::unordered_map<std::wstring, ComPtr<IDWriteTextLayout>> launchLabels; // the tiles' two-line names, laid out once while the launcher is open
    std::vector<sys::Notif> notifs; // what windows' notification centre holds, newest first, read when the clock opens
    std::unordered_set<int64_t> notifGone; // dismissed here
    int64_t notifStamp = 0; // the database's last change, checked on the slow timer: a new one lights the clock's dot
    bool notifUnread = false, notifLoading = false, dnd = false;
    float notifScroll = 0;
    HANDLE notifWatch = INVALID_HANDLE_VALUE; // the notifications folder, watched by the kernel: free while nothing changes
    int64_t pillSeen = 0; // newest notification already shown as a pill
    sys::Notif pillNote; // the one rising now
    bool pillUp = false;
    std::unordered_map<std::wstring, int64_t> notifSeen; // per app (its id, lowercased): the newest notification already seen
    std::unordered_map<std::wstring, int> unread; // per app: notifications since, drawn as a count on its icon
    void CountUnread();
    void SeeNotifs(const std::wstring& appKey); // that app came to the front: its count clears; empty clears them all
    std::string passFor; // the network a password is being typed for (its raw ssid), empty when none
    std::wstring pass; // what's typed so far: never logged, zeroed when done
    std::vector<sys::BtDev> devs;
    std::vector<sys::OutDev> outs;
    std::wstring outId;
    bool ringDrag = false; // dragging around the settings' hue ring

    std::wstring callKey, callName; // who wants attention, kept while the island shrinks back
    HWND callWin = nullptr;
    double callSince = 0;

    struct TrayBmp {
        uint64_t ver = 0;
        ComPtr<ID2D1Bitmap> bmp;
    };
    std::unordered_map<uint64_t, TrayBmp> trayBmps; // per icon, rebuilt when its image changes
    int trayShown = 0; // icons the row knew of at the last redraw
    std::vector<uint64_t> promoted; // keys of the tray icons sitting in the row
    uint64_t promotedFor = 0; // the icon set they were worked out for
    HWND overflowFore = nullptr; // in front when windows' flyout opened: it gets the focus back after
    bool overflowUp = false;
    D2D1_RECT_F pinsR{}; // the promoted icons, left of the tray button

    prefs::Look look;
    bool islandHere = false; // astraeus island is installed: only then can the bar follow its look
    Seed shellSeed, mediaSeed;
    Theme shellFrom, shellTo, shellNow, mediaFrom, mediaTo, mediaNow; // from/to lerp into now
    float shellT = 1, mediaT = 1;
    std::wstring wallKey;

    sys::Track track;
    bool hasTrack = false;
    uint64_t coverId = 0;
    ComPtr<IWICBitmapSource> coverSrc; // the decoded art, only until it's on the device: decoded again from the song's bytes if the device goes
    bool coverTint = false; // the song has art, and its colors tint the media
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
    float bars[4] = {}; // the row's mini visualizer, eased per frame
    bool mediaCalm = false; // the song's pill was last drawn at rest: only then may the bars' fast frames repaint their square
    double quietSince = -1; // playing yet silent since then: maybe the sound moved to another output
    float barsDrawn[4] = {}; // as last put on screen: a frame that wouldn't move them is skipped
    bool barsMoving = false;
    float preview = -1; // seek drag preview, -1 = none
    Swell swell;

    ComPtr<ID3D11Device> d3d;
    ComPtr<ID2D1DeviceContext> ctx;
    ComPtr<IDCompositionDevice> dcomp;
    ComPtr<IDCompositionTarget> target;
    ComPtr<IDCompositionVisual> visual;
    ComPtr<IDCompositionVirtualSurface> surface; // keeps what was drawn: a frame updates only the part that changed
    Glass* glass = nullptr; // while the glass look is on: composition draws the blur and our surface over it
    bool glassFailed = false; // this pc has no backdrop blur: glass wears its dark twin
    float glassShade = 0; // 0 over dark or colourful screens, 1 over a white page
    std::unique_ptr<Canvas> canvas;
    std::vector<RECT> regionNow;
};
