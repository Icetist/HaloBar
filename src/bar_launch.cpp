// The launcher: click the logo and type. Every app the start menu knows, found as you type, Enter starts the best one;
// the pins show while the field is empty. The list and its icons are read once on a worker, then kept until the bar idles.
#include "bar.h"

#include <cwctype>
#include <thread>

#include "glyphs.h"

namespace {

constexpr int kCols = 6, kRows = 3; // a grid of results, as many as fit without scrolling
constexpr float kHead = 56, kTileW = 92, kTileH = 82, kPowerH = 44; // field and gap above the grid, one result, the session row
constexpr float kLnStartW = 136, kLnSetW = 150; // the named buttons on the bottom row: windows start, halobar settings

struct AppsLoad { // what the worker brings back
    std::vector<sys::AppEntry> appList;
    std::vector<ComPtr<IWICBitmapSource>> icons; // same order as appList, null where the shell had none
};

}

float Bar::LaunchRise() const { return kHead + kRows * kTileH + kPowerH + 22; } // fixed height: the panel doesn't jump as results come and go

void Bar::OpenLauncher() {
    query.clear(); // each open starts on an empty field, showing the pins
    launchSel = 0;
    SetPanel(Panel::Launch);
    SetForegroundWindow(hwnd); // typing needs the keyboard; it goes back to the app when the launcher closes
    SetFocus(hwnd);
    bool stale = appList.empty() || GetTickCount64() / 1000.0 - appsAt > 600; // ten minutes: long enough to skip the rescan, short enough to see new installs
    if (stale || !appIconsDone) LoadApps(true); // icons may have been trimmed by an idle
}

void Bar::LoadApps(bool withIcons) { // two steps on a worker: the names first, so search works at once; the icons after, filling in
    if (appsLoading) return; // one load at a time; the running one will post its result
    appsLoading = true;
    HWND to = hwnd; // captured by value: the worker never touches the bar itself
    UINT px = IconPx() + 6; // a little over the drawn size, so the tile's larger icon stays sharp
    std::thread([to, px, withIcons] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the shell's app folder needs com on this thread too
        auto* list = new AppsLoad{sys::ListApps(), {}};
        std::vector<sys::AppEntry> names = list->appList; // the worker's own copy, for the icons
        if (!PostMessageW(to, WM_APP_APPS, withIcons ? 1 : 0, (LPARAM)list)) delete list; // the window owns it once posted; a failed post would leak it
        if (withIcons) {
            auto* icons = new AppsLoad{{}, {}};
            icons->icons.resize(names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                HBITMAP hb = sys::AppIcon(names[i], (int)px);
                if (!hb) continue; // left null: the tile draws a plain square instead
                ComPtr<IWICBitmap> bmp;
                ComPtr<IWICBitmapSource> pb;
                if (SUCCEEDED(gfx::Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &bmp)) && // shell icons carry alpha
                    SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, bmp.Get(), &pb))) { // the one format direct2d takes from wic
                    ComPtr<IWICBitmap> baked; // its own pixels, independent of the hbitmap freed below
                    if (SUCCEEDED(gfx::Wic()->CreateBitmapFromSource(pb.Get(), WICBitmapCacheOnLoad, &baked))) icons->icons[i] = baked;
                }
                DeleteObject(hb);
            }
            if (!PostMessageW(to, WM_APP_APPS, 2, (LPARAM)icons)) delete icons;
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void Bar::OnApps(WPARAM step, LPARAM lp) { // step 0: names, all done; 1: names, icons to come; 2: the icons
    auto* got = reinterpret_cast<AppsLoad*>(lp);
    if (!got) return;
    if (step == 2) {
        if (got->icons.size() == appList.size()) appIcons = std::move(got->icons), appIconsDone = true; // a size mismatch means the list was reloaded meanwhile
        appBmps.clear(); // device bitmaps are made again from the new icons on the next draw
        appsLoading = false;
    } else {
        appList = std::move(got->appList);
        appIcons.assign(appList.size(), nullptr); // old icons no longer line up with the new list
        appIconsDone = false;
        appBmps.clear();
        appsAt = GetTickCount64() / 1000.0;
        if (step == 0) appsLoading = false;
    }
    delete got;
    if (launchOpen) Redraw(); // closed: nothing to show, so no frame
}

void Bar::LaunchKey(WPARAM vk) {
    int n = (int)launchHits.size();
    if (vk == VK_ESCAPE) SetPanel(Panel::None);
    else if (vk == VK_RETURN) LaunchPick(launchSel);
    else if (n > 0 && vk == VK_RIGHT) launchSel = (launchSel + 1) % n; // left and right wrap; up and down stop at the edges
    else if (n > 0 && vk == VK_LEFT) launchSel = (launchSel + n - 1) % n;
    else if (n > 0 && vk == VK_DOWN) launchSel = (std::min)(n - 1, launchSel + kCols);
    else if (n > 0 && vk == VK_UP) launchSel = (std::max)(0, launchSel - kCols);
    Redraw();
}

void Bar::LaunchChar(wchar_t ch) {
    if (ch == VK_BACK) {
        if (!query.empty()) query.pop_back();
    } else if (ch >= 32 && query.size() < 64) { // printable only; the cap keeps the field on one line
        query.push_back(ch);
    } else {
        return;
    }
    launchSel = 0; // a new query puts the best match back under enter
    Redraw();
}

void Bar::LaunchPick(int i) {
    if (i < 0 || i >= (int)launchHits.size()) return; // enter with no results does nothing
    int h = launchHits[i];
    bool admin = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0; // ctrl+shift+enter, as start: as administrator
    SetPanel(Panel::None); // closed first, so the started app can take the foreground
    if (h >= 0 && h < (int)appList.size()) sys::LaunchApp(appList[h], admin);
    else if (h < 0 && tasks) tasks->Click(-h - 1); // a pin: as clicking it in the row
}

void Bar::DrawLauncher(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, launchSpring.value);
    if (rise < 1 || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 40) / (LaunchRise() - 40));
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    D2D1_RECT_F r = {W / 2 - kLaunchW / 2 + 22, pill.top + CallH() + 16, W / 2 + kLaunchW / 2 - 22, rowTop - 8};

    // the field: what's typed, a caret, the lens
    D2D1_RECT_F f = Rect(r.left, r.top, Width(r), 40);
    c.Fill(f, 20, t.text.Alpha(0.07f));
    c.Icon(glyph::Search, {f.left + 22, (f.top + f.bottom) / 2}, 19, t.accentHi);
    float tw = query.empty() ? 0 : TextW(query, Weight::Medium, 14);
    if (query.empty()) c.Text(L"Type to search apps", Rect(f.left + 42, f.top, Width(f) - 60, Height(f)), Weight::Light, 13.5f, t.textLow);
    else c.Text(query, Rect(f.left + 42, f.top, Width(f) - 60, Height(f)), Weight::Medium, 14, t.text);
    if (std::fmod(ui.Now(), 1.0) < 0.55) c.Fill(Rect(f.left + 42 + tw + 1, f.top + 11, 1.5f, Height(f) - 22), 0.75f, t.accentHi);
    ui.KeepAlive(); // the caret blinks while it's open

    // results: matches while typing, the pins while empty
    launchHits.clear();
    if (!query.empty()) {
        for (int i : sys::Search(appList, query, kCols * kRows)) launchHits.push_back(i);
    } else if (tasks) {
        const auto& all = tasks->Apps();
        for (int i = 0; i < (int)all.size() && (int)launchHits.size() < kCols * kRows; ++i)
            if (all[i].pinned) launchHits.push_back(-i - 1); // negative: a pin, by its place in the row
    }
    if (launchSel >= (int)launchHits.size()) launchSel = (std::max)(0, (int)launchHits.size() - 1);
    float gy = r.top + kHead, gx = (r.left + r.right) / 2 - kCols * kTileW / 2;
    if (!query.empty() && launchHits.empty()) {
        c.Text(appsLoading ? L"Finding your apps…" : L"No apps match", Rect(r.left, gy + 60, Width(r), 18), Weight::Light, 13, t.textLow, Align::Center);
    }
    if (query.empty()) c.Text(L"PINNED", Rect(gx + 8, gy - 14, 200, 12), Weight::Medium, 9.5f, t.textLow);
    int pick = -1;
    for (int k = 0; k < (int)launchHits.size(); ++k) {
        int h = launchHits[k];
        D2D1_RECT_F tile = Rect(gx + (k % kCols) * kTileW, gy + (k / kCols) * kTileH, kTileW - 4, kTileH - 4);
        uint32_t id = Ui::Id("ln.tile", k);
        Ui::Hit hit = ui.Button(id, tile);
        if (hit.hover && input.mouse.x != lastLaunchMouse.x) launchSel = k; // the mouse moving picks, a still mouse leaves the keys in charge
        bool sel = k == launchSel;
        float hov = ui.Smooth(id + 1, sel ? 1.0f : 0, 18);
        if (hov > 0.01f) c.Fill(tile, 16, (k == 0 && !query.empty() ? t.accent.Alpha(0.16f) : t.text.Alpha(0.08f)).Alpha(hov)); // the one enter starts
        D2D1_RECT_F ic = Rect((tile.left + tile.right) / 2 - kIcon * 0.65f, tile.top + 10, kIcon * 1.3f, kIcon * 1.3f);
        std::wstring name;
        if (h >= 0) {
            name = appList[h].name;
            ComPtr<ID2D1Bitmap>& bmp = appBmps[h];
            if (!bmp && h < (int)appIcons.size() && appIcons[h] && ctx) ctx->CreateBitmapFromWicBitmap(appIcons[h].Get(), nullptr, &bmp);
            if (bmp) c.Pixels(bmp.Get(), ic);
            else c.Fill(ic, Width(ic) * 0.3f, t.cardHi);
        } else if (tasks) {
            const TaskApp& ta = tasks->Apps()[-h - 1];
            name = ta.name;
            DrawApp(&ta, ic, 1, t);
        }
        c.Text(name, Rect(tile.left + 4, ic.bottom + 6, Width(tile) - 8, 30), Weight::Regular, 11, sel ? t.text : t.textDim, Align::Center);
        if (hit.clicked) pick = k;
    }
    lastLaunchMouse = input.mouse;

    // the session, as start's power menu
    float py = r.bottom - kPowerH / 2;
    c.Line({r.left, py - kPowerH / 2 + 2}, {r.right, py - kPowerH / 2 + 2}, t.line, 1);
    if (LabelButton(t, Ui::Id("ln.winstart"), Rect(r.left, py - 15, kLnStartW, 30), kLogo, L"Windows Start")) SetPanel(Panel::None), StartMenu(); // windows' own, for what the launcher doesn't do
    if (LabelButton(t, Ui::Id("ln.set"), Rect(r.left + kLnStartW + 8, py - 15, kLnSetW, 30), glyph::Settings, L"HaloBar settings")) SetSettingsOpen(true);
    struct P {
        int glyph;
        void (*run)();
    };
    const P ps[] = {{glyph::Lock, sys::power::Lock}, {glyph::Sleep, sys::power::Sleep}, {glyph::SignOut, sys::power::SignOut},
                    {glyph::Restart, sys::power::Restart}, {glyph::Power, sys::power::Shutdown}};
    for (int i = 0; i < 5; ++i)
        if (w::IconBtn(ui, t, Ui::Id("ln.pw", i), {r.right - 16 - (4 - i) * 40.0f, py}, 32, ps[i].glyph, 19)) {
            SetPanel(Panel::None);
            ps[i].run();
        }
    rt->PopLayer();
    if (pick >= 0) LaunchPick(pick);
}
