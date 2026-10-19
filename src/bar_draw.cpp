#include "bar.h"

#include <cmath>
#include <cwctype>

#include "glyphs.h"
#include "overflow.h"

#include <psapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <unordered_set>

namespace {

constexpr double kPulseSec = 6; // an attention call pulses this long, then holds still: no endless frames

uint32_t TrayId(uint64_t key) { return (uint32_t)(key ^ (key >> 32)) & ~0xFu; } // low bits cleared: room for id + 1, + 8 and friends

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

}

float Bar::TrayRise() const {
    int n = TrayCount();
    int rows = (std::max)(1, (n + kTrayCols - 1) / kTrayCols); // rounded up; an empty tray still opens one row
    return rows * kTrayCell + 44; // and a line for the hovered icon's name
}

bool Bar::StepSprings(float dt) {
    callSpring.target = 0; // an app flashing for attention: its dock icon says so, nothing drops down
    peekSpring.target = pillUp && !AnyOpen() ? kPeekRise : 0; // a panel opening puts the pill away
    mediaSpring.target = mediaOpen && hasTrack ? kMediaRise : 0; // the player folds away when the song ends
    traySpring.target = trayOpen ? TrayRise() : 0;
    setSpring.target = setOpen ? kSetRise : 0;
    sheetSpring.target = sheetOpen ? SheetRise() : 0;
    qsSpring.target = qsOpen ? QuickRise() : 0;
    calSpring.target = calOpen ? CalRise() : 0;
    launchSpring.target = launchOpen ? LaunchRise() : 0;
    wgSpring.target = wgOpen ? WidgetRise() : 0;
    for (anim::Spring* s : {&peekSpring, &callSpring, &mediaSpring, &traySpring, &setSpring, &sheetSpring, &qsSpring, &calSpring, &launchSpring, &wgSpring})
        s->damp = s->target > 0 ? (look.reduceMotion ? 1.0f : 0.62f) : 0.92f; // island's feel: a little bounce opening, a calm settle closing
    bool a = callSpring.Step(dt) | peekSpring.Step(dt); // bitwise, so both always step; || would skip the second
    bool b = mediaSpring.Step(dt);
    bool c = traySpring.Step(dt);
    bool d = setSpring.Step(dt);
    bool e = sheetSpring.Step(dt);
    bool f = qsSpring.Step(dt);
    bool g = calSpring.Step(dt);
    bool h = launchSpring.Step(dt);
    bool i = wgSpring.Step(dt);
    return a || b || c || d || e || f || g || h || i; // any still moving keeps frames coming
}

bool Bar::Layout(float dt) {
    // the dock hides while a window overlaps it; the toolbar always stays
    float hideTo = AutoHide() && covered && !peek && !(AnyOpen() && !cardDown) && !card.Shown() && !Inside(wegHit, input.mouse) ? 1.0f : 0.0f;
    bool sinking = anim::Approach(sink, hideTo, hideTo > 0 ? 7.0f : 12.0f, dt); // slips away gently, comes back quick
    // the top bar: off, or slipping up while a window reaches under it, back when the mouse touches the top edge
    float topTo = TopAuto() && topCovered && !topPeek && !(AnyOpen() && cardDown) && !Inside(capL, input.mouse) && !Inside(capR, input.mouse) ? 1.0f : 0.0f;
    if (!TopOn()) topSink = 1;
    else sinking = anim::Approach(topSink, topTo, topTo > 0 ? 7.0f : 12.0f, dt) || sinking;
    float lift = topSink * (kBarH + 2);
    barR = TopOn() ? D2D1_RECT_F{0, -lift, W, kBarH - lift} : D2D1_RECT_F{};
    sinking = anim::Approach(shadeTopNow, shadeTop, 5, dt) || sinking; // the glass smokes and clears gently
    sinking = anim::Approach(shadeBotNow, shadeBot, 5, dt) || sinking;

    bool showMedia = cfg.song && hasTrack && !track.title.empty();
    std::wstring key = showMedia ? track.title + L"\n" + track.artist : L""; // a newline can't appear in either, so the pair is unambiguous
    if (showMedia && key != mediaMeasured) { // sized to the song, measured once per song
        mediaMeasured = key;
        std::wstring line = track.artist.empty() ? track.title : track.title + L" - " + track.artist;
        mediaTextW = (std::clamp)(TextW(line, Weight::Regular, kBarText) + 2, 40.0f, 220.0f); // a long one is cut with an ellipsis
    }
    bool moving = anim::Approach(mediaShow, showMedia ? 1.0f : 0.0f, 10, dt) || sinking;

    // the dock: start, show desktop, a separator, the apps, a separator, the recycle bin, the song; it packs closer before any app is left out
    int apps = tasks ? (int)tasks->Apps().size() : 0;
    float others = 0; // everything but the apps: start, the widgets with a gap after each (the apps' too), the status and show desktop
    for (const WgSlot& s : wgSlots) others += (s.kind == kWgApps ? 0 : WidgetW(s.kind, s.size)) + kWegGap;
    float ends = 2 * kWegPad + kSlot + kWegSep + others + (editing ? kIcon + kWegGap : 0) + (TopOn() ? 0 : kWegSep + statusW);
    float room = W - 2 * kIcon - ends; // the screen's edges keep a margin
    slotW = apps > 0 ? (std::clamp)(room / apps, kSlotMin, kAppSlot) : kAppSlot;
    dockCount = (std::clamp)((int)(room / slotW), 0, apps);
    if (dockCount < apps) dockCount = (std::clamp)((int)((room - kIcon - kWegGap) / slotW), 0, apps); // not all fit: one spot goes to the "+N" tile
    moreN = apps - dockCount;
    appsW = (dockCount > 0 ? dockCount * slotW - kWegGap : 0) + (moreN > 0 ? (dockCount > 0 ? kWegGap : 0) + kIcon : 0);
    float want = ends + appsW;
    if (rowW <= 0) rowW = want; // first frame: no ease in from nothing
    moving = anim::Approach(rowW, want, 14, dt) || moving;
    float floorY = winH - kWegMargin + sink * (kWegH + kWegMargin + 4); // tucked: the whole dock below the screen's edge
    rowTop = floorY - kWegH;
    wegR = {W / 2 - rowW / 2, rowTop, W / 2 + rowW / 2, floorY};
    wegHit = {wegR.left, rowTop, wegR.right, winH}; // down to the screen's edge, so the mouse can rest there

    // a dropdown: laid out where it always grew, centered and rising from rowTop; cardOff hangs it under the toolbar, or over the dock
    float open = anim::Clamp01(mediaSpring.value / kMediaRise), pk = anim::Clamp01(PeekH() / kPeekRise);
    float tr = anim::Clamp01(traySpring.value / TrayRise()), set = anim::Clamp01(setSpring.value / kSetRise);
    float sheet = anim::Clamp01(sheetSpring.value / SheetRise());
    float qs = anim::Clamp01(qsSpring.value / QuickRise()), cal = anim::Clamp01(calSpring.value / CalRise()), ln = anim::Clamp01(launchSpring.value / LaunchRise());
    float wg = anim::Clamp01(wgSpring.value / WidgetRise());
    float gridW = (std::clamp)(TrayCount(), 1, kTrayCols) * kTrayCell + 40; // the tray grid, with its margins
    float wide = 0, openK = 0;
    for (auto [to, k] : {std::pair{kMediaW, open}, std::pair{kPeekW, pk}, std::pair{gridW, tr}, std::pair{(std::min)(kSetW, W - 24), set}, std::pair{kSheetW, sheet},
                         std::pair{kQsW, qs}, std::pair{kCalW, cal}, std::pair{kLaunchW, ln}, std::pair{WidgetPanelW(), wg}})
        if (k > 0.001f) wide = (std::max)(wide, to * (0.25f + 0.75f * k)), openK = (std::max)(openK, k); // grows out of its icon: narrow at first, full width once up
    float grown = Grown();
    if (!AnyOpen() && grown - PeekH() - CallH() < 0.5f) cardDown = true, anchorX = W - 8 - kPeekW / 2; // a notification arrives in the top right corner; a closing popup keeps its place until it's gone
    pill = grown > 0.5f && wide > 1 ? D2D1_RECT_F{W / 2 - wide / 2, rowTop - grown, W / 2 + wide / 2, rowTop} : D2D1_RECT_F{};
    float cx = (std::clamp)(anchorX, wide / 2 + 8, (std::max)(wide / 2 + 8, W - wide / 2 - 8)); // kept on screen
    float cardWant = anchorX + (cx - anchorX) * openK; // centered on the icon while it's small, sliding to where it fits as it opens
    if (grown < 0.5f || cardX == 0) cardX = cardWant; // nothing showing: it starts on the icon
    else moving = anim::Approach(cardX, cardWant, 22, dt) || moving; // moved to another module while open: it glides there
    cardOff = {cardX - W / 2, cardDown ? kCardTop - pill.top : -10.0f};
    return moving;
}

void Bar::PaintShape(ID2D1Geometry* shape, const Theme& t) { // a panel: island's material, its gloss and rim
    if (!shape) return;
    Canvas& c = *canvas;
    c.FillGeometry(shape, t.bg);
    if (!t.glass) return;
    if (glassShade > 0.01f) c.FillGeometry(shape, Color{0.02f, 0.02f, 0.04f, 0.45f * glassShade}); // smoked over a white page
    D2D1_RECT_F o = pill; // island's glass: gloss from the top, a bright rim
    c.FillGeoV(shape, {o.left, o.top, o.right, o.top + (std::min)(Height(o), 90.0f)}, t.sheen, t.sheen.Alpha(0));
    c.StrokeGeo(shape, Color{1, 1, 1, 0.26f}, 1.2f);
}

ComPtr<ID2D1Geometry> Bar::Shape() const { // the open dropdown's outline, in its own layout
    if (pill.right <= pill.left) return nullptr;
    if (shapeCache && std::fabs(pill.left - shapeFor.left) < 0.01f && std::fabs(pill.top - shapeFor.top) < 0.01f && std::fabs(pill.right - shapeFor.right) < 0.01f &&
        std::fabs(pill.bottom - shapeFor.bottom) < 0.01f)
        return shapeCache; // at rest: the same outline every frame, built once
    shapeFor = pill;
    shapeCache = BuildShape();
    return shapeCache;
}

ComPtr<ID2D1Geometry> Bar::BuildShape() const {
    ComPtr<ID2D1RoundedRectangleGeometry> g;
    float r = (std::min)(CardRadius(), Height(pill) / 2);
    if (FAILED(gfx::D2D()->CreateRoundedRectangleGeometry({pill, r, r}, &g))) return nullptr;
    return g;
}

const TaskApp* Bar::FindApp(const std::wstring& key) const {
    if (!tasks || key.empty()) return nullptr;
    for (const auto& a : tasks->Apps())
        if (a.key == key) return &a;
    return nullptr;
}

float Bar::TextW(const std::wstring& s, Weight w, float size) {
    ComPtr<IDWriteTextLayout> lay;
    DWRITE_TEXT_METRICS m{};
    if (s.empty() || FAILED(gfx::DWrite()->CreateTextLayout(s.c_str(), (UINT32)s.size(), gfx::TextFmt(w, size, Align::Left), 4096, 64, &lay)) || // wide enough never to wrap
        FAILED(lay->GetMetrics(&m)))
        return 0;
    return m.widthIncludingTrailingWhitespace;
}

void Bar::DrawApp(const TaskApp* a, const D2D1_RECT_F& r, float alpha, const Theme& t, UINT px) {
    if (alpha <= 0.01f || Width(r) < 1) return; // invisible: skip the icon lookup too
    Canvas& c = ui.C();
    if (ID2D1Bitmap* bmp = a ? icons.Get(ctx.Get(), *a, px ? px : IconPx()) : nullptr) { // cached per app, at the largest pixel size asked for
        c.Pixels(bmp, r, alpha);
        return;
    }
    if (a && icons.Pending(a->key)) { // still being read: a faint plate where it'll be
        c.Fill(r, Width(r) * 0.3f, t.text.Alpha(0.06f * alpha));
        return;
    }
    c.Fill(r, Width(r) * 0.3f, t.cardHi.Alpha(alpha)); // no icon anywhere: its initial on a tile
    std::wstring letter(1, a && !a->name.empty() ? (wchar_t)std::towupper(a->name[0]) : L'?');
    float h = Height(r);
    c.Text(letter, Rect(r.left, r.top + h * 0.16f, Width(r), h * 0.75f), Weight::Semi, h * 0.5f, t.accentHi.Alpha(alpha), Align::Center);
}

void Bar::WinLogo(D2D1_POINT_2F o, float size, Color top, Color bottom) { // the windows logo: four panes
    float s = size * 0.92f, gap = s * 0.08f, q = (s - gap) / 2;
    for (int i = 0; i < 4; ++i) {
        float x = o.x - s / 2 + (i % 2) * (q + gap), y = o.y - s / 2 + (i / 2) * (q + gap);
        ui.C().FillV(Rect(x, y, q, q), q * 0.14f, top, bottom);
    }
}

bool Bar::LabelButton(const Theme& t, uint32_t id, const D2D1_RECT_F& r, int glyph, const wchar_t* label) {
    Canvas& c = ui.C();
    Ui::Hit h = ui.Button(id, r);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), cy = (r.top + r.bottom) / 2;
    c.Fill(r, Height(r) / 2, t.text.Alpha(0.06f + 0.07f * hov));
    Color ink = t.textDim.Mix(t.text, 0.4f + 0.6f * hov);
    if (glyph == kLogo) WinLogo({r.left + 18, cy}, 13, t.accentHi, t.accent); // the logo isn't in the icon font: drawn
    else c.Icon(glyph, {r.left + 18, cy}, 17, ink);
    c.Text(label, Rect(r.left + 32, cy - 9, Width(r) - 40, 18), Weight::Medium, 11.5f, ink);
    return h.clicked;
}

ID2D1Bitmap* Bar::TrayBitmap(const TrayItem& it) { return IconBitmap(it.icon, it.key, it.ver); }

ID2D1Bitmap* Bar::IconBitmap(HICON icon, uint64_t key, uint64_t ver) {
    if (!icon || !ctx) return nullptr; // no device yet, or the app sent no icon
    TrayBmp& b = trayBmps[key];
    if (b.bmp && b.ver == ver) return b.bmp.Get(); // ver bumps whenever the app swaps its icon
    b.bmp.Reset();
    b.ver = ver;
    ComPtr<IWICBitmap> wic;
    ComPtr<IWICBitmapSource> pbgra;
    if (SUCCEEDED(gfx::Wic()->CreateBitmapFromHICON(icon, &wic)) && SUCCEEDED(WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, wic.Get(), &pbgra)))
        ctx->CreateBitmapFromWicBitmap(pbgra.Get(), nullptr, &b.bmp);
    return b.bmp.Get();
}

void Bar::OpenOverflow() {
    if (overflowUp || overflow::Shown()) return overflow::Close(); // a second click puts it away, as windows' chevron
    SetPanel(Panel::None); // one thing open at a time
    bool dock = !TopOn(); // the tray sits in the dock: the flyout rises over it instead
    float cx = (trayR.left + trayR.right) / 2;
    POINT anchor = ToScreen({cx, dock ? wegR.top : capR.bottom});
    int edge = ToScreen({0, dock ? wegR.top - 8 : kCardTop}).y; // just under the top bar, or just over the dock, as the panels
    overflowFore = GetForegroundWindow();
    if (overflow::Open(anchor, edge, !dock)) return; // shown and placed once explorer says so: Tasks::kOverflow
    Anchor(cx, !dock);
    SetTrayOpen(true); // explorer's hidden icon menu is off: the bar's own grid instead
}

void Bar::SyncPromoted() {
    bool changed = overflow::PromotedChanged();
    uint64_t set = 1469598103934665603ull; // fnv over the icons' keys: the list is worked out again only when they change
    if (tray)
        for (const TrayItem& it : tray->Items())
            if (it.Shown()) set = (set ^ it.key) * 1099511628211ull;
    if (!changed && set == promotedFor) return;
    promotedFor = set;
    promoted.clear();
    if (tray)
        for (const TrayItem& it : tray->Items())
            if (it.Shown() && overflow::Promoted(it.owner, it.id)) promoted.push_back(it.key);
}

void Bar::DrawTrayGrid(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, traySpring.value);
    if (rise < 1 || !tray || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 16) / (TrayRise() - 16));
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr); // inside the island, fading in as it grows
    const auto& items = tray->Items();
    int n = TrayCount(), cols = (std::clamp)(n, 1, kTrayCols);
    float x0 = W / 2 - cols * kTrayCell / 2, y0 = pill.top + CallH() + 12; // centered, below a call strip if one is showing
    int k = 0, pick = -1;
    bool right = false;
    D2D1_POINT_2F at{};
    const std::wstring* tip = nullptr;
    for (size_t i = 0; i < items.size(); ++i) {
        if (!items[i].Shown()) continue;
        D2D1_RECT_F r = Rect(x0 + (k % cols) * kTrayCell, y0 + (k / cols) * kTrayCell, kTrayCell, kTrayCell);
        ++k;
        uint32_t id = TrayId(items[i].key) + 8; // keyed on the icon, not its cell, so hover survives a reshuffle
        Ui::Hit h = ui.Button(id, r);
        if (h.hover) tip = &items[i].tip;
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(Inset(r, 3, 3), (kTrayCell - 6) / 2, t.text.Alpha(0.09f * hov));
        if (ID2D1Bitmap* b = TrayBitmap(items[i])) c.Image(b, Rect((r.left + r.right) / 2 - 10, (r.top + r.bottom) / 2 - 10, 20, 20), 0, 1);
        if (h.clicked || (input.rclick && Inside(r, input.mouse))) pick = (int)i, right = !h.clicked, at = {(r.left + r.right) / 2, r.top}; // acted on after the layer pops
    }
    int ghostPick = -1;
    for (size_t g = 0; g < ghosts.size(); ++g) { // icons only explorer heard of: windows' own picture of them; a click brings the app forward
        D2D1_RECT_F r = Rect(x0 + (k % cols) * kTrayCell, y0 + (k / cols) * kTrayCell, kTrayCell, kTrayCell);
        ++k;
        uint32_t id = Ui::Id("tray.ghost", (int)g);
        Ui::Hit h = ui.Button(id, r);
        if (h.hover) tip = &ghosts[g].tip;
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(Inset(r, 3, 3), (kTrayCell - 6) / 2, t.text.Alpha(0.09f * hov));
        Ghost& gh = ghosts[g];
        if (!gh.bmp && gh.px && ctx) ctx->CreateBitmapFromWicBitmap(gh.px.Get(), nullptr, &gh.bmp); // made lazily, on the device that draws it
        if (gh.bmp) c.Image(gh.bmp.Get(), Rect((r.left + r.right) / 2 - 10, (r.top + r.bottom) / 2 - 10, 20, 20), 0, 1);
        if (h.clicked || (input.rclick && Inside(r, input.mouse))) ghostPick = (int)g; // its own menu lives with explorer: the app itself is the most we can open
    }
    if (tip && !tip->empty()) { // what the hovered icon is, in the strip below the grid
        std::wstring line = tip->substr(0, tip->find(L'\n')); // first line only: tooltips run long
        c.Text(line, Rect(pill.left + 20, rowTop - 22, Width(pill) - 40, 16), Weight::Medium, 11, t.textDim, Align::Center);
    }
    rt->PopLayer();
    if (pick >= 0) tray->Click((size_t)pick, right, ToScreen(at)); // the app may open a menu: never mid-draw
    if (ghostPick >= 0) {
        SetTrayOpen(false);
        GhostOpen(ghosts[ghostPick].exe);
    }
}

void Bar::GhostOpen(const std::wstring& exe) { // the app itself: its window if it has one, else start it, which brings an already running one forward
    if (tasks)
        for (int i = 0; i < (int)tasks->Apps().size(); ++i)
            if (!tasks->Apps()[i].wins.empty() && _wcsicmp(tasks->Apps()[i].exe.c_str(), exe.c_str()) == 0) return tasks->Click(i);
    ShellExecuteW(nullptr, L"open", exe.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Bar::DrawCalling(const Theme& t, ID2D1Geometry* shape) {
    HWND who = nullptr;
    int ai = tasks ? tasks->Calling(&who) : -1;
    if (ai >= 0) {
        const TaskApp& app = tasks->Apps()[ai];
        if (app.key != callKey || who != callWin) callSince = ui.Now(); // a new call: pulse again
        callKey = app.key;
        callName = app.name;
        callWin = who;
    }
    float h = CallH();
    if (h < 1 || callKey.empty() || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((h - 14) / (kCallRise - 14));
    D2D1_RECT_F pr = Rect(pill.left, pill.top, Width(pill), h);
    Ui::Hit hit = ui.Button(Ui::Id("calling"), pr);
    if (hit.clicked && callWin && IsWindow(callWin)) Tasks::Activate(callWin); // the window may have closed meanwhile
    bool pulsing = ai >= 0 && ui.Now() - callSince < kPulseSec;
    float pulse = pulsing ? 0.5f + 0.5f * std::sin((float)(ui.Now() - callSince) * 4.0f) : 0.6f; // about two-thirds of a beat a second
    if (pulsing) ui.KeepAlive(); // frames only while it actually pulses
    if (a <= 0.01f) return;

    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    float cy = pr.top + kCallRise / 2 + 2, x0 = W / 2 - kCallW / 2 + 24; // rides the top of the island as it grows
    c.Glow({x0 + 12, cy}, 48, 30, t.accent.Alpha(0.16f + 0.24f * pulse));
    DrawApp(FindApp(callKey), Rect(x0, cy - 13, 26, 26), 1, t);
    float x = x0 + 26 + 12, w = W / 2 + kCallW / 2 - 40 - x;
    c.Text(callName, Rect(x, cy - 17, w, 18), Weight::Medium, 13.5f, t.text);
    c.Text(L"wants your attention", Rect(x, cy + 1, w, 15), Weight::Light, 11, t.textDim);
    c.Dot({W / 2 + kCallW / 2 - 26, cy}, 3.5f + 1.5f * pulse, t.accentHi);
    rt->PopLayer();
}

void Bar::SyncGhosts() { // read when the tray opens and once after start: never on a timer
    std::vector<Ghost> keep;
    std::unordered_set<std::wstring> running, owned;
    DWORD pids[1024], got = 0; // plenty for a desktop; extras are just not seen
    if (EnumProcesses(pids, sizeof(pids), &got)) // every exe running now, by full path
        for (DWORD i = 0; i < got / sizeof(DWORD); ++i)
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pids[i])) { // the limited right works even on elevated processes
                wchar_t path[MAX_PATH];
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(p, 0, path, &n)) running.insert(Lower(path));
                CloseHandle(p);
            }
    if (tray)
        for (const auto& it : tray->Items()) { // apps that did give us their icon
            DWORD pid = 0;
            GetWindowThreadProcessId(it.owner, &pid);
            if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
                wchar_t path[MAX_PATH];
                DWORD n = MAX_PATH;
                if (QueryFullProcessImageNameW(p, 0, path, &n)) owned.insert(Lower(path));
                CloseHandle(p);
            }
        }
    HKEY root = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Control Panel\\NotifyIconSettings", 0, KEY_READ, &root) == ERROR_SUCCESS) {
        wchar_t sub[64];
        for (DWORD i = 0; i < 256; ++i) { // one subkey per icon windows has ever seen
            DWORD sn = 64;
            if (RegEnumKeyExW(root, i, sub, &sn, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; // ERROR_NO_MORE_ITEMS ends it
            wchar_t exe[MAX_PATH * 2] = {}, tip[256] = {};
            DWORD en = sizeof(exe), tn = sizeof(tip);
            if (RegGetValueW(root, sub, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, exe, &en) != ERROR_SUCCESS) continue;
            RegGetValueW(root, sub, L"InitialTooltip", RRF_RT_REG_SZ, nullptr, tip, &tn); // optional: stays empty if missing
            std::wstring path = exe;
            if (path.size() > 38 && path[0] == L'{') { // a known folder's id, then the rest of the path
                GUID g;
                PWSTR base = nullptr;
                if (SUCCEEDED(CLSIDFromString(path.substr(0, 38).c_str(), &g)) && SUCCEEDED(SHGetKnownFolderPath(g, 0, nullptr, &base))) path = std::wstring(base) + path.substr(38);
                CoTaskMemFree(base); // freed even on failure, as the docs ask
            }
            std::wstring low = Lower(path);
            if (!running.count(low) || owned.count(low)) continue; // not running, or we already have its real icon
            bool dup = false;
            for (const auto& gh : keep) dup = dup || Lower(gh.exe) == low;
            if (dup) continue; // one entry per exe, though windows may keep several
            DWORD bn = 0;
            if (RegGetValueW(root, sub, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, nullptr, &bn) != ERROR_SUCCESS || bn < 8 || bn > (1u << 20)) continue;
            std::vector<uint8_t> png(bn);
            if (RegGetValueW(root, sub, L"IconSnapshot", RRF_RT_REG_BINARY, nullptr, png.data(), &bn) != ERROR_SUCCESS) continue;
            Ghost gh;
            gh.exe = path;
            gh.tip = tip;
            gh.px = gfx::Decode(png.data(), png.size(), 64); // the snapshot is a png; 64 px is ample for a 20 dip cell
            if (gh.px) keep.push_back(std::move(gh));
        }
        RegCloseKey(root);
    }
    bool changed = keep.size() != ghosts.size();
    ghosts = std::move(keep);
    if (changed) Redraw();
}

void Bar::DrawPeek(const Theme& t, ID2D1Geometry* shape) {
    float h = PeekH();
    if (h < 1 || !shape || pill.right <= pill.left) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((h - 16) / (kPeekRise - 16));
    D2D1_RECT_F pr = Rect(pill.left, pill.top + CallH(), Width(pill), h); // under a call strip, if both are up
    Ui::Hit hit = ui.Button(Ui::Id("peek"), pr);
    if (hit.hover) SetTimer(hwnd, kPillTimer, kPillMs, nullptr); // read at leisure: it stays while the mouse rests on it
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    float cy = pr.top + kPeekRise / 2 + 3, x0 = W / 2 - kPeekW / 2 + 26;
    c.Dot({x0 + 13, cy}, 15, t.accent.Alpha(0.22f));
    c.Icon(glyph::Bell, {x0 + 13, cy}, 17, t.accentHi, true);
    float x = x0 + 36, w = W / 2 + kPeekW / 2 - 30 - x;
    std::wstring head = pillNote.app.empty() ? pillNote.title : pillNote.app + L"  \u00b7  " + pillNote.title; // "App  \u00b7  Title", as island's pill
    c.Text(head, Rect(x, cy - 17, w, 18), Weight::Medium, 13, t.text);
    std::wstring body = pillNote.body.substr(0, pillNote.body.find(L'\n'));
    c.Text(body.empty() ? std::wstring(L"Tap to open") : body, Rect(x, cy + 1, w, 15), Weight::Light, 11, t.textDim);
    rt->PopLayer();
    if (hit.clicked) { // open what it's about
        pillUp = false;
        KillTimer(hwnd, kPillTimer);
        sys::OpenNotifApp(pillNote);
        notifGone.insert(pillNote.id); // seen: not offered again
    } else if (ui.RClick(pr)) {
        pillUp = false; // right click: away with it
    }
}
