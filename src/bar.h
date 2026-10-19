#pragma once
#include <windows.h>
#include <d3d11.h>
#include <dcomp.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "anim.h"
#include "bt.h"
#include "radios.h"
#include "status.h"
#include "sys.h"
#include "wifi.h"
#include "gfx.h"
#include "icons.h"
#include "lyrics.h"
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

class Bar {
public:
    Bar();
    ~Bar();
    Bar(const Bar&) = delete;
    Bar& operator=(const Bar&) = delete;

    bool Create(HINSTANCE instance);
    HWND Hwnd() const { return hwnd; }
    bool NeedsFrame() const { return needsFrame; }
    HANDLE FrameWait() const { return frameWait; } // waitable timer that paces frames while something moves
    HANDLE PrefsWait() const { return prefsEv; } // set when island's settings change, null without island
    void OnPrefs();
    HANDLE NotifWait() const { return notifWatch; } // set when windows' notification store is written: a new one may have come
    void OnNotifWrite();
    HANDLE PinsWait() const { return pinsEv; } // set when the windows taskbar's pins change
    void OnPins();
    std::vector<PinItem> PinList() const { return tasks ? tasks->PinList() : std::vector<PinItem>{}; }
    void Frame();

    static constexpr float kGap = 0; // the island rises straight out of the screen's bottom edge, as island hangs from the top one
    static constexpr float kRowH = 48; // the resting island: one row, as tall as windows' own taskbar
    static constexpr float kFillet = 26, kCorner = 34; // island's shape: concave shoulders where it meets the bezel, round free corners
    static constexpr float kPadX = 14; // content inset from the island's sides, the same both ways
    static constexpr float kRoom = 600; // window room for the grown island and its bounce: the tallest panel, quick settings, plus a little
    static constexpr float kSlot = 38, kSlotMin = 31, kIcon = 26; // one app in the row (closer when crowded), and its icon
    static constexpr float kClockW = 64;
    static constexpr float kArt = 30, kBarsW = 22; // the song's cover and mini visualizer in the row
    static constexpr float kTrayBtn = 30; // the mini tray: one button, every icon lives in what it opens
    static constexpr int kTrayCols = 8;
    static constexpr float kTrayCell = 38; // one icon in the grown tray
    static constexpr float kCallRise = 48, kCallW = 360; // the island grows this much taller when an app wants attention
    static constexpr float kMediaRise = 196, kMediaW = 860; // and this much into the player, its lyrics on the right
    static constexpr float kSetRise = 300, kSetW = 760; // and into its settings
    static constexpr float kSheetW = 380; // and into an app's sheet
    static constexpr float kLaunchW = 600; // and into the launcher
    static constexpr float kQsW = 460, kCalW = 720, kNotifW = 360; // and into quick settings, or the clock: notifications beside the calendar
    static constexpr UINT_PTR kPreviewTimer = 9, kPreviewHideTimer = 10, kCoverTimer = 11, kPeekTimer = 12, kLyricTimer = 13, kMixTimer = 14, kMixCloseTimer = 15, kGhostTimer = 16, kNotifReadTimer = 17, kPillTimer = 18, kAppsTimer = 19, kNotifPollTimer = 20, kFullTimer = 21, kPosTimer = 22, kCalWaitTimer = 24, kUpdateTimer = 25;
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
    bool AutoHide() const { return cfg.room != 0; } // windows take the whole screen; the bar slips away while one covers it
    bool Unseen() const { return full || (sink >= 0.98f && !peek && !AnyOpen()); } // hidden or tucked away: coming back redraws everything anyway
    void CheckCovered(); // does a window sit where the island rests
    void HookCover(bool on);
    static void CALLBACK OnCoverEvent(HWINEVENTHOOK, DWORD ev, HWND h, LONG obj, LONG child, DWORD, DWORD);
    void ShowDesktop();
    void DrawDesktopBtn(const Theme& t);
    void DrawDividers(const Theme& t);
    static constexpr int kLogo = 0; // LabelButton's glyph for the windows logo
    bool LabelButton(const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label); // a pill with an icon and words, the same everywhere
    void WinLogo(D2D1_POINT_2F c, float size, Color top, Color bottom); // hairlines between the row's groups: song, apps, tray, clock
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
    enum class Panel { None, Media, Tray, Settings, Sheet, Quick, Calendar, Launch }; // what the island has grown into: one at a time
    void SetPanel(Panel p);
    void SetMediaOpen(bool open) { open ? SetPanel(Panel::Media) : mediaOpen ? SetPanel(Panel::None) : void(); }
    void SetTrayOpen(bool open) { open ? SetPanel(Panel::Tray) : trayOpen ? SetPanel(Panel::None) : void(); }
    void SetSettingsOpen(bool open) { open ? SetPanel(Panel::Settings) : setOpen ? SetPanel(Panel::None) : void(); }
    void SetSheet(const std::wstring& key); // an app's sheet, empty closes it
    void CloseAll() { SetPanel(Panel::None); }
    bool AnyOpen() const { return mediaOpen || trayOpen || setOpen || sheetOpen || qsOpen || calOpen || launchOpen; }
    void SetPref(int& field, const wchar_t* name, int value); // saved, and applied right away
    void ClaimTray(); // apps' tray icons come to the bar from now on

    bool EnsureDev();
    void FreeDev();
    void Retry(); // the device or a draw failed: try again shortly, give the taskbar back if it keeps failing
    UINT IconPx() const { return (UINT)(std::max)(16L, std::lround(kIcon * scale)); } // the resting size in pixels: drawn 1:1 there, so it stays sharp
    void Redraw();
    void Arm(double seconds);
    void Idle(); // still for a while: efficiency mode, caches handed back

    int LookNow() const; // the look in use: 0 dark, 1 light, 2 glass, 3 island's custom
    float Blur() const;
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
               (std::max)(0.0f, sheetSpring.value) + (std::max)(0.0f, qsSpring.value) + (std::max)(0.0f, calSpring.value) + (std::max)(0.0f, launchSpring.value);
    }
    float TrayRise() const;
    bool Layout(float dt); // where everything sits this frame, before drawing: true while widths still ease
    ComPtr<ID2D1Geometry> Shape() const;
    ComPtr<ID2D1Geometry> BuildShape() const;
    mutable ComPtr<ID2D1Geometry> shapeCache; // the outline for shapeFor, reused while the island holds still
    mutable D2D1_RECT_F shapeFor{};
    D2D1_RECT_F Outer() const { return Inset(pill, -kFillet, 0); } // the island with its shoulders, all it paints
    const TaskApp* FindApp(const std::wstring& key) const;
    float TextW(const std::wstring& s, Weight w, float size);
    void DrawApp(const TaskApp* a, const D2D1_RECT_F& r, float alpha, const Theme& t); // its icon, or its initial on a tile
    void DrawStart(const Theme& t);
    void DrawDock(const Theme& t); // bar_dock.cpp, with the two below
    void DrawSheet(const Theme& t, ID2D1Geometry* shape);
    void DrawProgress(const Badge& b, float cx, float y, float w, float grow, const Theme& t);
    float SheetRise() const;
    ID2D1Bitmap* IconBitmap(HICON icon, uint64_t key, uint64_t ver);
    void DrawCalling(const Theme& t, ID2D1Geometry* shape);
    void DrawPeek(const Theme& t, ID2D1Geometry* shape); // a new notification, as a pill rising out of the island
    float PeekH() const { return (std::max)(0.0f, peekSpring.value); }
    bool Quiet() const; // do not disturb: no pills (and, with pills off, no windows banners)
    void SetQuiet(bool on);
    void Banners(bool hush); // windows' own toast banners off while our pills show them, back as they were after
    void DrawClock(const Theme& t);
    void DrawTrayButton(const Theme& t);
    void PaintShape(ID2D1Geometry* shape, const Theme& t);
    void DrawTrayGrid(const Theme& t, ID2D1Geometry* shape);
    ID2D1Bitmap* TrayBitmap(const TrayItem& it);
    int TrayCount() const { return (tray ? tray->Visible() : 0) + (int)ghosts.size(); } // real icons and the ones windows remembers
    void SyncGhosts(); // apps whose icon went to explorer before we took the tray: shown from windows' own snapshot of it

    // bar_quick.cpp: status in the row, quick settings, calendar
    float StatusW() const;
    void SyncLang(); // the keyboard layout in front, shown only when there is more than one
    void DrawLang(const Theme& t);
    void SyncAudio();
    void PollStatus();
    void DrawStatus(const Theme& t);
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
    void SetSong(const Theme& t, const D2D1_RECT_F& r);
    void SetClock(const Theme& t, const D2D1_RECT_F& r);

    // bar_glass.cpp: the glass look, the island over a live blur of what's behind it
    struct Glass;
    bool OpenGlass();
    void FreeGlass();
    void GlassMask(ID2D1Geometry* shape);
    void SampleBehind(); // how bright the screen is where the island grows: bright pages get island's smoked glass
    HRESULT BeginSurface(const RECT& px, IDXGISurface** out, POINT* off);
    void EndSurface();
    void TrimSurface(const RECT& keep);
    void Present();

    // bar_media.cpp
    void OnMedia();
    void SyncCover();
    bool StepBars(float dt, double now);
    void DrawMediaRow(const Theme& t);
    void DrawBars(float dt, double now, bool step = true);
    void DrawPlayer(ID2D1Geometry* shape);
    void Wave(const D2D1_RECT_F& r, const Theme& t);
    void DrawLyrics(const D2D1_RECT_F& r, const Theme& t); // the song's synced lyrics, as island's: the line being sung bright, the words filling in
    bool StepSwell(float dt, bool playing);
    void DrawSwell(const D2D1_RECT_F& r, const Theme& t, double now);

    HWND hwnd = nullptr;
    HMONITOR mon = nullptr;
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

    std::unique_ptr<Tasks> tasks;
    std::unique_ptr<TrayHost> tray;
    Icons icons;
    sys::Media media;
    sys::Meter meter;
    std::unique_ptr<sys::Spectrum> spectrum; // only while the player shows the music
    double spectrumAsked = 0;

    HANDLE frameWait = nullptr, prefsEv = nullptr, pinsEv = nullptr;
    bool needsFrame = false, fullNext = true, trimDue = false, barsOnly = false;
    double lastFrame = 0;
    int devTries = 0;
    D2D1_RECT_F drawn{}; // what the surface holds: a full frame repaints this and the island's new bounds
    D2D1_RECT_F barsRect{}; // the row's little bars: all a frame repaints while only they move

    Input input;
    Ui ui;
    bool midClick = false, menuWanted = false;

    D2D1_RECT_F pill{}; // the island this frame
    D2D1_RECT_F startR{}, clockR{}, mediaR{}, trayR{}, statusR{}, langR{}, deskR{}; // parts of its row
    std::wstring lang, langSub, langName; // "ENG" over "US", and the layout's full name; empty with a single layout
    uint64_t langKey = 0; // the layout and installed list those were read for: naming a variant walks the whole registry list
    static constexpr float kLangW = 36, kDeskW = 14; // the language mark; the show-desktop sliver at the island's right end
    float rowTop = 0, rowW = 0, dockX = 0;
    float mediaShow = 0, mediaTextW = 0;
    int dockCount = 0; // apps that fit in the row on this screen
    float slotW = kSlot; // each app's room in the row this frame
    std::wstring mediaMeasured; // title and artist the row was last sized for
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
    HWINEVENTHOOK coverHooks[3] = {}, moveHook = nullptr; // what moves windows: focus, drags, minimizes; and the app in front's own moves

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
    D2D1_POINT_2F lastLaunchMouse{};
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

    prefs::Look look;
    bool islandHere = false; // astraeus island is installed: only then can the bar follow its look
    Seed shellSeed, mediaSeed;
    Theme shellFrom, shellTo, shellNow, mediaFrom, mediaTo, mediaNow; // from/to lerp into now
    float shellT = 1, mediaT = 1;
    std::wstring wallKey;

    sys::Track track;
    bool hasTrack = false;
    uint64_t coverId = 0;
    ComPtr<IWICBitmapSource> coverSrc;
    ComPtr<ID2D1Bitmap1> cover;
    ComPtr<ID2D1Bitmap> coverBlur;
    float bars[4] = {}; // the row's mini visualizer, eased per frame
    float barsDrawn[4] = {}; // as last put on screen: a frame that wouldn't move them is skipped
    bool barsMoving = false;
    float preview = -1; // seek drag preview, -1 = none
    Swell swell;
    sys::LyricSrc lyrics; // island's engine and island's disk cache: songs either app has seen show at once
    std::wstring lyrKey; // the song lyrics were last asked for
    int lyrLine = -2; // the line being sung, measured once each time it changes
    const void* lyrDoc = nullptr; // the lyrics the line sizes below were measured for
    std::vector<float> lyrFit; // each line's size so it fits the column whole, never cut off with an ellipsis
    ComPtr<IDWriteTextLayout> lyrLay; // the line being sung, laid out once: the fill finds each word on it exactly

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
