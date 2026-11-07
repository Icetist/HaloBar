// The dock's editor, over the wallpaper as astraeus' long pill's: the screen dims, the dock stays live where it is to move and
// remove its widgets, and a panel above it holds its shape, body, size, status, widgets, clock and start button, with undo and reset.
#include "bar.h"

#include <cctype>
#include <cstdlib>

#include "setui.h"

using namespace setui;

namespace {

constexpr float kScrim = 0.34f; // the dim over the screen
constexpr float kPanelW = 780, kPanelH = 476, kEdge = 24, kToDock = 40, kRise = 24; // the panel; clear of the dock's name pills; how far it comes up from
constexpr float kPad = 18, kHeadH = 32, kTabsH = 30, kTabsW = 390, kGap = 12; // its header, then the page picker
constexpr float kDoneW = 84, kResetW = 76, kHistBtn = 30, kHistGap = 6, kHistIcon = 20; // the header's buttons
constexpr float kCardGap = 10;
constexpr float kLookH = 196, kModsH = 92; // the dock page: its look and size, then the status chips
constexpr float kGripOff = 9, kGripLong = 48, kGripHit = 18, kGripBar = 34, kGripThin = 5; // the dock's handles: off its edge, their hit area and the bar drawn in it
constexpr float kRowGap = 8; // between the look card's choices
constexpr float kLenSnap = 14; // a dragged end this near the dock's fitting length snaps to fitting
constexpr float kClockH = 122, kStartH = 88; // the clock's two switches and its note; the start button's row and note
constexpr float kLabelH = 16, kSegH = 28, kMiniGap = 6, kMiniDockH = 14; // the look card: a label over each choice, a strip of wallpaper with the dock on it
constexpr size_t kDepth = 64; // undo steps kept
constexpr size_t kHeadN = 11; // a snapshot: the choices below, then each widget's kind and size

// material symbols' undo and redo, outlined, weight 400 (apache 2.0, as the bundled icon font): the subset font never held them
constexpr const char* kUndoPath = "M280-200v-80h284q63 0 109.5-40T720-420q0-60-46.5-100T564-560H312l104 104-56 56-200-200 200-200 56 56-104 104h252q97 0 166.5 63T800-420q0 94-69.5 157T564-200H280Z";
constexpr const char* kRedoPath = "M396-200q-97 0-166.5-63T160-420q0-94 69.5-157T396-640h252L544-744l56-56 200 200-200 200-56-56 104-104H396q-63 0-109.5 40T240-420q0 60 46.5 100T396-280h284v80H396Z";
constexpr float kGrid = 960; // the symbols' own grid: 0..960 across, -960..0 down

ComPtr<ID2D1PathGeometry> Symbol(const char* d, D2D1_POINT_2F o, float px) { // an svg path of moves, lines and quadratics, centred on o
    ComPtr<ID2D1PathGeometry> path;
    ComPtr<ID2D1GeometrySink> sink;
    if (FAILED(gfx::D2D()->CreatePathGeometry(&path)) || FAILED(path->Open(&sink))) return nullptr;
    float k = px / kGrid;
    auto at = [&](float x, float y) { return D2D1_POINT_2F{o.x + (x - kGrid / 2) * k, o.y + (y + kGrid / 2) * k}; };
    float cx = 0, cy = 0, sx = 0, sy = 0, qx = 0, qy = 0; // the pen, the figure's start, the last control point
    bool open = false;
    char cmd = 0, prev = 0;
    const char* s = d;
    auto num = [&]() {
        while (*s == ' ' || *s == ',') ++s;
        char* end = nullptr;
        float v = std::strtof(s, &end);
        s = end;
        return v;
    };
    while (*s) {
        while (*s == ' ' || *s == ',') ++s;
        if (!*s) break;
        if (std::isalpha((unsigned char)*s)) cmd = *s++;
        bool rel = std::islower((unsigned char)cmd) != 0;
        float bx = rel ? cx : 0, by = rel ? cy : 0;
        switch (std::toupper((unsigned char)cmd)) {
        case 'M': {
            float x = bx + num(), y = by + num();
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            sink->BeginFigure(at(x, y), D2D1_FIGURE_BEGIN_FILLED);
            open = true, cx = sx = x, cy = sy = y;
            cmd = rel ? 'l' : 'L'; // more pairs after a move are lines
            break;
        }
        case 'L': cx = bx + num(), cy = by + num(), sink->AddLine(at(cx, cy)); break;
        case 'H': cx = bx + num(), sink->AddLine(at(cx, cy)); break;
        case 'V': cy = by + num(), sink->AddLine(at(cx, cy)); break;
        case 'Q':
            qx = bx + num(), qy = by + num();
            cx = bx + num(), cy = by + num();
            sink->AddQuadraticBezier({at(qx, qy), at(cx, cy)});
            break;
        case 'T': { // the control mirrors the last one's
            bool chained = prev == 'Q' || prev == 'T';
            qx = chained ? 2 * cx - qx : cx, qy = chained ? 2 * cy - qy : cy;
            cx = bx + num(), cy = by + num();
            sink->AddQuadraticBezier({at(qx, qy), at(cx, cy)});
            break;
        }
        case 'Z':
            if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
            open = false, cx = sx, cy = sy;
            break;
        default: return nullptr; // nothing else is used
        }
        prev = (char)std::toupper((unsigned char)cmd);
    }
    if (open) sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    if (FAILED(sink->Close())) return nullptr;
    return path;
}

bool Pill(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool enabled) { // the header's buttons: done in the accent, the rest quiet
    Canvas& c = ui.C();
    Ui::Hit h = enabled ? ui.Button(id, r) : Ui::Hit{};
    if (enabled) ui.Name(label); // a greyed one registers nothing to name
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), on = ui.Smooth(id + 2, enabled ? 1.0f : 0, 14);
    Color fill = primary ? t.accent.Mix(t.accentHi, 0.35f * hov) : t.text.Alpha(0.07f + 0.06f * hov);
    c.Fill(r, Height(r) / 2, fill.Alpha(fill.a * (0.45f + 0.55f * on)));
    c.Text(label, r, Weight::Medium, 12, primary ? t.onAccent : t.text.Alpha(0.45f + 0.55f * on), Align::Center);
    return h.clicked;
}

}

D2D1_RECT_F Bar::PlateR(const D2D1_RECT_F& r) const {
    if (cfg.dockFloat || r.right <= r.left) return r;
    constexpr float kPast = kWegRadius + 2; // its edge-side corners off the screen, where nobody sees them round
    D2D1_RECT_F p = r;
    if (cfg.edge == 1) p.left -= kPast;
    else if (cfg.edge == 2) p.right += kPast;
    else p.bottom += kPast;
    return p;
}

void Bar::Rescale() {
    float was = scale;
    scale = dpi * Zoom();
    if (std::fabs(scale - was) < 1e-4f) return;
    anchorX *= was / scale, cardX *= was / scale; // an open panel stays over the same spot
    W = winPxW / scale, winH = winPxH / scale;
    rowW = 0, pill = {}, drawnTop = drawnBottom = {};
    shapeCache.Reset();
    fullNext = allNext = true;
    Redraw();
}

D2D1_RECT_F Bar::GripRect(int g) const { // where each handle sits, just off the dock: its hit area, the drawn bar inside it
    float mx = (wegR.left + wegR.right) / 2, my = (wegR.top + wegR.bottom) / 2;
    bool across = (g == kGripSize) != Vert(); // the bar lies along the dock's length: the bottom dock's size handle, a side dock's ends
    D2D1_POINT_2F o{};
    if (g == kGripSize) o = !Vert() ? D2D1_POINT_2F{mx, wegR.top - kGripOff} : cfg.edge == 1 ? D2D1_POINT_2F{wegR.right + kGripOff, my} : D2D1_POINT_2F{wegR.left - kGripOff, my};
    else if (!Vert()) o = {g == kGripStart ? wegR.left - kGripOff : wegR.right + kGripOff, my};
    else o = {mx, g == kGripStart ? wegR.top - kGripOff : wegR.bottom + kGripOff};
    return across ? Rect(o.x - kGripLong / 2, o.y - kGripHit / 2, kGripLong, kGripHit) : Rect(o.x - kGripHit / 2, o.y - kGripLong / 2, kGripHit, kGripLong);
}

void Bar::StudioGrips() { // pressed before the dock's own controls: the first button under a press takes it
    if (!editing) {
        studioGrip = kGripNone;
        return;
    }
    POINT pt{};
    GetCursorPos(&pt); // in pixels: a resize changes what a dip is under the mouse
    for (int g : {kGripSize, kGripStart, kGripEnd}) {
        if (g != kGripSize && Fixed()) continue; // across the screen: no length of its own
        uint32_t id = Ui::Id("st.grip", g);
        Ui::Hit h = ui.Button(id, GripRect(g));
        ui.Name(g == kGripSize ? L"Dock size" : L"Dock length");
        if (h.hover || ui.IsActive(id)) cursorNow = (g == kGripSize) != Vert() ? IDC_SIZENS : IDC_SIZEWE;
        if (ui.IsActive(id) && input.pressed) {
            studioGrip = g, gripFrom = pt;
            gripZoom = (float)cfg.zoom, gripLen = Vert() ? Height(wegR) : Width(wegR);
            iconScale = scale; // icons keep their pixels until it's let go
        }
    }
    if (studioGrip == kGripNone) return;
    ui.Wake();
    if (input.down) {
        if (studioGrip == kGripSize) { // the inner edge follows the mouse: the dock as thick as it's pulled
            float inward = !Vert() ? (float)(gripFrom.y - pt.y) : cfg.edge == 1 ? (float)(pt.x - gripFrom.x) : (float)(gripFrom.x - pt.x);
            float thick = kWegH * iconScale; // in pixels, as it was taken
            int z = (std::clamp)((int)std::lround(gripZoom * (thick + inward) / thick), prefs::Settings::kZoomMin, prefs::Settings::kZoomMax);
            if (z != (zoomLive > 0 ? zoomLive : cfg.zoom)) zoomLive = z, Rescale();
        } else { // both ends move: it stays centered
            float along = (float)(Vert() ? pt.y - gripFrom.y : pt.x - gripFrom.x) / scale;
            float len = gripLen + 2 * (studioGrip == kGripEnd ? along : -along), span = Vert() ? winH : W;
            int pm = std::fabs(len - fitLen) < kLenSnap ? prefs::Settings::kLenFit : (std::clamp)((int)std::lround(len / span * prefs::Settings::kLenMax), 1, prefs::Settings::kLenMax);
            if (pm != LenNow()) lenLive = pm, Redraw();
        }
        return;
    }
    int grip = studioGrip; // let go: kept, in every dock, and the strip claimed at its new size
    studioGrip = kGripNone, iconScale = 0;
    if (grip == kGripSize && zoomLive > 0) {
        int z = zoomLive;
        zoomLive = 0;
        SetDock(&prefs::Settings::zoom, L"Zoom", z);
    } else if (grip != kGripSize && lenLive >= 0) {
        int len = lenLive;
        lenLive = -1;
        SetDock(&prefs::Settings::dockLen, L"DockLen", len);
    }
    Redraw();
}

Theme Bar::DockTheme(const Theme& t) const { // the accent as the body, its own ink on it, as on any accent fill
    Theme d = t;
    Color ink = t.onAccent;
    ink.a = 1;
    d.bg = t.accent;
    d.bg.a = 1;
    d.text = ink, d.textDim = ink.Alpha(0.74f), d.textLow = ink.Alpha(0.5f);
    d.accent = ink, d.accentHi = ink, d.onAccent = d.bg; // what was lit in the accent is lit in the ink
    d.card = d.bg.Mix(ink, 0.08f), d.cardHi = d.bg.Mix(ink, 0.14f), d.track = ink.Alpha(0.18f);
    d.hairTop = ink.Alpha(0.24f), d.hairBot = ink.Alpha(0.08f), d.line = ink.Alpha(0.14f), d.sheen = ink.Alpha(0.12f);
    d.glass = d.stars = false; // a solid body: no frost, no sky
    return d;
}

void Bar::SetDock(int prefs::Settings::*field, const wchar_t* name, int value) {
    if (cfg.*field == value) return;
    prefs::SaveNum(name, value);
    bool place = field == &prefs::Settings::zoom || field == &prefs::Settings::dockFloat; // a new size, or a new strip kept free: placed again
    for (Bar* b : Docks()) {
        b->cfg.*field = value;
        if (field == &prefs::Settings::align || field == &prefs::Settings::dockLen) b->rowW = 0; // straight to its new length: no slow stretch across the screen
        if (field == &prefs::Settings::startIcon) b->haloLogo.Reset();
        if (field == &prefs::Settings::song && !value && b->mediaOpen) b->SetMediaOpen(false);
        PostMessageW(b->hwnd, WM_APP_RESTYLE, place ? 1 : 0, 0); // never mid-frame
        b->SampleShade(); // a clear dock reads its ink off the wallpaper
        b->Redraw();
    }
    if (field == &prefs::Settings::launcher) SyncKeyHook(); // the win key opens the launcher or windows' start
}

std::vector<int> Bar::StudioSnap() const {
    std::vector<int> s = {cfg.dockFloat, cfg.dockBody, cfg.zoom, cfg.dockLen, cfg.align, cfg.song, cfg.date, cfg.clock24, cfg.startIcon, cfg.launcher, cfg.mods};
    s.reserve(kHeadN + 2 * wgSlots.size());
    for (const WgSlot& w : wgSlots) s.push_back(w.kind), s.push_back(w.size);
    return s;
}

std::vector<int> Bar::StudioDefaults() const { // as a first run has it: the choices' defaults, the apps, the song, the first widgets, the bin
    prefs::Settings d;
    std::vector<int> s = {d.dockFloat, d.dockBody, d.zoom, d.dockLen, d.align, d.song, d.date, d.clock24, d.startIcon, d.launcher, d.mods};
    auto slot = [&](int kind, int size) { s.push_back(kind), s.push_back(size); };
    slot(kWgApps, 0);
    slot(kWgDivider, 0);
    slot(kWgMedia, 1);
    constexpr int kOldWidgets = 10; // the widgets picked by bit before the dock had slots, as LoadSlots reads them
    for (int old = 0; old < kOldWidgets; ++old)
        if ((d.widgets >> old) & 1) slot(old + 2, old < 2 ? 1 : 0); // weather and stats medium, the rest small
    slot(kWgBin, 0);
    return s;
}

void Bar::StudioLoad(const std::vector<int>& s) {
    if (s.size() < kHeadN) return;
    struct Choice {
        int prefs::Settings::*field;
        const wchar_t* name;
    };
    static const Choice kChoices[kHeadN] = {{&prefs::Settings::dockFloat, L"DockFloat"}, {&prefs::Settings::dockBody, L"DockBody"}, {&prefs::Settings::zoom, L"Zoom"},
                                            {&prefs::Settings::dockLen, L"DockLen"}, {&prefs::Settings::align, L"Align"}, {&prefs::Settings::song, L"Song"}, {&prefs::Settings::date, L"Date"}, {&prefs::Settings::clock24, L"Clock24"},
                                            {&prefs::Settings::startIcon, L"StartIcon"}, {&prefs::Settings::launcher, L"Launcher"}, {&prefs::Settings::mods, L"Mods"}};
    for (size_t i = 0; i < kHeadN; ++i) SetDock(kChoices[i].field, kChoices[i].name, s[i]); // only what differs is written
    std::vector<WgSlot> slots;
    for (size_t i = kHeadN; i + 1 < s.size(); i += 2) slots.push_back({s[i], s[i + 1]});
    bool same = slots.size() == wgSlots.size();
    for (size_t i = 0; same && i < slots.size(); ++i) same = slots[i].kind == wgSlots[i].kind && slots[i].size == wgSlots[i].size;
    if (same) return;
    wgSlots = std::move(slots);
    wgSel = dragSlot = -1;
    SaveSlots();
}

bool Bar::StudioHistory(const Theme& t, D2D1_POINT_2F at) { // two round chips, the arrows in the accent; greyed with nothing to step to
    Canvas& c = ui.C();
    bool acted = false;
    for (int i = 0; i < 2; ++i) {
        bool back = i == 0, can = editing && (back ? !studioBack.empty() : !studioAhead.empty());
        D2D1_POINT_2F o = {at.x + kHistBtn / 2 + i * (kHistBtn + kHistGap), at.y};
        uint32_t id = Ui::Id("st.hist", i);
        Ui::Hit h = can ? ui.Button(id, Rect(o.x - kHistBtn / 2, o.y - kHistBtn / 2, kHistBtn, kHistBtn)) : Ui::Hit{};
        if (can) ui.Name(back ? L"Undo" : L"Redo");
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), press = ui.Smooth(id + 2, h.down ? 0.88f : 1.0f, 26), on = ui.Smooth(id + 3, can ? 1.0f : 0, 14);
        c.Dot(o, kHistBtn / 2, t.text.Alpha((0.07f + 0.06f * hov) * (0.45f + 0.55f * on)));
        if (ComPtr<ID2D1PathGeometry> g = Symbol(back ? kUndoPath : kRedoPath, o, kHistIcon * press)) c.FillGeometry(g.Get(), t.textLow.Alpha(0.6f).Mix(t.accentHi, on));
        if (!h.clicked) continue;
        std::vector<std::vector<int>>& from = back ? studioBack : studioAhead;
        (back ? studioAhead : studioBack).push_back(studioNow);
        studioNow = from.back();
        from.pop_back();
        StudioLoad(studioNow); // the next frame sees it as it is: not a new step
        acted = true;
    }
    return acted;
}

D2D1_RECT_F Bar::StudioRect() const {
    if (Vert()) { // beside the column, toward the screen's middle
        float x = cfg.edge == 1 ? wegR.right + kToDock : kEdge;
        float room = cfg.edge == 1 ? W - kEdge - x : wegR.left - kToDock - kEdge;
        float w = (std::min)(kPanelW, room), h = (std::min)(kPanelH, winH - 2 * kEdge);
        if (cfg.edge == 2) x = wegR.left - kToDock - w;
        return Rect(x, (winH - h) / 2, w, h);
    }
    float w = (std::min)(kPanelW, W - 2 * kEdge), bottom = wegR.top - kToDock, h = (std::min)(kPanelH, bottom - kEdge);
    return Rect(W / 2 - w / 2, bottom - h, w, h);
}

void Bar::DrawStudio(const Theme& t, const Theme& dock, const Theme* ink) {
    Canvas& c = *canvas;
    ID2D1RenderTarget* rt = c.Target();
    c.Fill({0, 0, W, winH}, 0, Color{0, 0, 0, kScrim * studioK});
    StudioGrips(); // first: a handle wins the press over the widgets under its edge
    inkNow = ink;
    DrawWeg(dock); // as it is, live: its widgets are moved and taken off right there
    inkNow = nullptr;
    for (int g : {kGripSize, kGripStart, kGripEnd}) { // the handles over it, as the long pill's: a short bar off each edge that pulls
        if (!editing || (g != kGripSize && Fixed())) continue;
        D2D1_RECT_F gr = GripRect(g);
        uint32_t id = Ui::Id("st.grip", g);
        float hot = ui.Smooth(id + 1, ui.IsActive(id) || Inside(gr, input.mouse) ? 1.0f : 0, 16) * studioK;
        D2D1_POINT_2F o = {(gr.left + gr.right) / 2, (gr.top + gr.bottom) / 2};
        D2D1_RECT_F bar = Width(gr) > Height(gr) ? Rect(o.x - kGripBar / 2, o.y - kGripThin / 2, kGripBar, kGripThin) : Rect(o.x - kGripThin / 2, o.y - kGripBar / 2, kGripThin, kGripBar);
        c.Fill(Inset(bar, -1, -1), kGripThin, Color{0, 0, 0, 0.3f * studioK}); // a dark rim: it reads over a pale wallpaper
        c.Fill(bar, kGripThin / 2, Color{1, 1, 1, (0.7f + 0.3f * hot) * studioK}.Mix(t.accentHi.Alpha(studioK), hot));
    }
    if (studioGrip != kGripNone) { // what it's being pulled to, over the handle
        int len = LenNow();
        std::wstring say = studioGrip == kGripSize ? std::to_wstring(zoomLive > 0 ? zoomLive : cfg.zoom) + L"%" : len == prefs::Settings::kLenFit ? std::wstring(L"Fits") : std::to_wstring((len + 5) / 10) + L"%";
        D2D1_RECT_F gr = GripRect(studioGrip);
        float tw = TextW(say, Weight::Medium, 12) + 22;
        D2D1_POINT_2F o = {(gr.left + gr.right) / 2, (gr.top + gr.bottom) / 2};
        D2D1_RECT_F tag = !Vert() ? Rect(o.x - tw / 2, wegR.top - 46, tw, 24) : Rect(cfg.edge == 1 ? wegR.right + 26 : wegR.left - 26 - tw, o.y - 12, tw, 24);
        c.Fill(tag, 12, Color{0, 0, 0, 0.62f});
        c.Text(say, tag, Weight::Medium, 12, Color{1, 1, 1, 1}, Align::Center);
    }
    D2D1_RECT_F r = StudioRect();
    float rise = (1 - anim::EaseOut(studioK)) * kRise; // it comes up after the dim
    r.top += rise, r.bottom += rise;
    bool fading = studioK < 0.999f;
    if (fading) rt->PushLayer(D2D1::LayerParameters(Inset(r, -60, -60), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), studioK), nullptr);
    c.Glow({(r.left + r.right) / 2, r.bottom - 10}, Width(r) * 0.55f, 46, Color{0, 0, 0, 0.35f});
    c.Fill(r, kStudioRadius, t.glass ? Body(t) : t.bg.Alpha(1)); // glass: frosted by the blur under it; the solid looks opaque, read over anything
    if (t.glass) {
        if (glassShade > 0.01f) c.Fill(r, kStudioRadius, Tint(glassShade));
        c.FillV({r.left, r.top, r.right, r.top + 90}, kStudioRadius, t.sheen, t.sheen.Alpha(0));
        c.Stroke(r, kStudioRadius, Color{1, 1, 1, 0.26f}, 1.2f);
    } else {
        c.StrokeV(r, kStudioRadius, t.hairTop, t.hairBot, 1);
    }
    StudioPanel(t, r);
    if (fading) rt->PopLayer();
    if (!editing) return;
    bool kept = Inside(r, input.pressPos) || Inside(DockArea(), input.pressPos); // the press began on the panel or the dock
    if (input.released && !kept && !ui.Captured()) SetPanel(Panel::None); // a click on the dim: done, as astraeus' editor
    else if (input.rclick && !Inside(r, input.mouse) && !Inside(wegHit, input.mouse)) SetPanel(Panel::None);
}

void Bar::StudioPanel(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    bool live = editing;
    if (live && !input.down && dragSlot < 0 && studioGrip == kGripNone) { // a change counts once the mouse lets go: a drag is one step
        std::vector<int> s = StudioSnap();
        if (s != studioNow) {
            studioBack.push_back(std::move(studioNow));
            if (studioBack.size() > kDepth) studioBack.erase(studioBack.begin());
            studioAhead.clear();
            studioNow = std::move(s);
        }
    }
    D2D1_RECT_F in = Inset(r, kPad, kPad);
    float doneX = in.right - kDoneW, resetX = doneX - 8 - kResetW, histX = resetX - 10 - (2 * kHistBtn + kHistGap);
    c.Text(L"Customize dock", Rect(in.left + 2, in.top, histX - in.left - 10, kHeadH), Weight::Medium, 15, t.text);
    if (Pill(ui, t, Ui::Id("st.done"), Rect(doneX, in.top + 1, kDoneW, kHeadH - 2), L"Done", true, live)) {
        SetPanel(Panel::None);
        return;
    }
    std::vector<int> defaults = StudioDefaults();
    if (Pill(ui, t, Ui::Id("st.reset"), Rect(resetX, in.top + 1, kResetW, kHeadH - 2), L"Reset", false, live && StudioSnap() != defaults)) StudioLoad(defaults); // undo brings it back
    StudioHistory(t, {histX, in.top + kHeadH / 2});
    float y = in.top + kHeadH + kGap;
    static const wchar_t* const kTabs[] = {L"Dock", L"Widgets", L"Clock and start"};
    int picked = Segmented(ui, t, "st.tabs", Rect(in.left, y, (std::min)(kTabsW, Width(in)), kTabsH), kTabs, 3, studioTab);
    if (picked >= 0) studioTab = picked, ++studioTabNth;
    y += kTabsH + kGap + 4;
    D2D1_RECT_F body = {in.left, y, in.right, in.bottom};
    float show = ui.SmoothFrom(Ui::Id("st.page", studioTabNth), 1, 0, 16); // each page fades in afresh
    bool layer = show < 0.999f;
    if (layer) rt->PushLayer(D2D1::LayerParameters(Inset(body, -8, -8), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), show), nullptr);
    if (studioTab == 1) DrawGallery(t, Inset(body, 6, 6)); // the song's switch, the search, the picked widget, every widget by category
    else if (studioTab == 2) StudioClock(t, body);
    else StudioDock(t, body);
    if (layer) rt->PopLayer();
}

void Bar::StudioDock(const Theme& t, const D2D1_RECT_F& body) {
    Canvas& c = ui.C();
    float cw = (Width(body) - 2 * kCardGap) / 3;
    D2D1_RECT_F look = Rect(body.left, body.top, 2 * cw + kCardGap, kLookH);
    D2D1_RECT_F in = Box(c, t, look, L"Look");
    float pw = Width(in) * 0.42f; // the dock on a strip of wallpaper, as the choices make it
    D2D1_RECT_F wall = {in.left, in.top + 2, in.left + pw, in.bottom - kNoteLine - 4};
    c.FillV(wall, 10, t.track.Alpha(0.9f), t.track.Alpha(0.45f));
    c.Stroke(wall, 10, t.hairTop, 1);
    float gap = ui.Smooth(Ui::Id("st.mini.gap"), cfg.dockFloat ? kMiniGap : 0.0f, 14);
    float accent = ui.Smooth(Ui::Id("st.mini.accent"), AccentDock() ? 1.0f : 0, 14), solid = ui.Smooth(Ui::Id("st.mini.solid"), ClearDock() ? 0.0f : 1.0f, 14);
    float dw = Width(wall) * 0.72f;
    D2D1_RECT_F dock = Rect((wall.left + wall.right - dw) / 2, wall.bottom - gap - kMiniDockH, dw, kMiniDockH);
    c.PushClip(Inset(wall, 1, 1));
    D2D1_RECT_F plate = gap < 0.5f ? D2D1_RECT_F{dock.left, dock.top, dock.right, dock.bottom + kMiniDockH} : dock; // stuck: its foot past the edge
    c.Fill(plate, kMiniDockH / 2, t.bg.Alpha(1).Mix(t.accent, accent).Alpha(solid));
    Color dot = t.accentHi.Mix(t.onAccent, accent).Mix(t.text, 1 - solid);
    for (int k = -3; k <= 3; ++k) c.Dot({(dock.left + dock.right) / 2 + k * 10.0f, (dock.top + dock.bottom) / 2}, 2.3f, dot);
    c.PopClip();
    static const wchar_t* const kBodyNotes[] = {L"The look picked in Settings", L"Your accent color", L"Only its icons, black or white"};
    Note(c, t, wall, in.bottom - kNoteLine, kBodyNotes[cfg.dockBody]);
    float x = wall.right + 18, w = in.right - x, y = in.top;
    c.Text(L"Shape", Rect(x, y, w, kLabelH), Weight::Medium, 11, t.textDim);
    static const wchar_t* const kShapes[] = {L"Floating", L"Stuck"};
    int shape = Segmented(ui, t, "st.shape", Rect(x, y + kLabelH + 2, w, kSegH), kShapes, 2, cfg.dockFloat ? 0 : 1);
    y += kLabelH + 2 + kSegH + kRowGap;
    c.Text(L"Body", Rect(x, y, w, kLabelH), Weight::Medium, 11, t.textDim);
    static const wchar_t* const kBodies[] = {L"Theme", L"Accent", L"Clear"};
    int bodyPick = Segmented(ui, t, "st.body", Rect(x, y + kLabelH + 2, w, kSegH), kBodies, 3, cfg.dockBody);
    y += kLabelH + 2 + kSegH + kRowGap;
    c.Text(L"Position", Rect(x, y, w, kLabelH), Weight::Medium, 11, t.textDim);
    int place = -1;
    if (Vert()) { // along a side it's always centered: which edge is Settings' choice
        c.Text(L"Centered along the side", Rect(x, y + kLabelH + 2, w, kSegH), Weight::Regular, 11.5f, t.textDim);
    } else { // along the bottom: where it sits on it. 0 centered, 1 from the left, 2 from the right
        static const wchar_t* const kPlaces[] = {L"Left", L"Center", L"Right"};
        static const int kAlignOf[] = {1, 0, 2}, kShownAs[] = {1, 0, 2};
        int picked = Segmented(ui, t, "st.align", Rect(x, y + kLabelH + 2, w, kSegH), kPlaces, 3, kShownAs[cfg.align]);
        if (picked >= 0) place = kAlignOf[picked];
    }
    if (shape >= 0) SetDock(&prefs::Settings::dockFloat, L"DockFloat", shape == 0 ? 1 : 0);
    if (bodyPick >= 0) SetDock(&prefs::Settings::dockBody, L"DockBody", bodyPick);
    if (place >= 0) SetDock(&prefs::Settings::align, L"Align", place);
    StudioSize(t, Rect(look.right + kCardGap, body.top, cw, kLookH));
    SetMods(t, Rect(body.left, body.top + kLookH + kCardGap, Width(body), kModsH)); // what sits at its end
}

void Bar::StudioSize(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Size");
    int z = zoomLive > 0 ? zoomLive : cfg.zoom, len = LenNow();
    c.Text(std::to_wstring(z) + L"%", Rect(in.left, in.top, Width(in), 36), Weight::Light, 30, t.text);
    std::wstring line = Fixed() ? std::wstring(L"Runs the screen's width") : len == prefs::Settings::kLenFit ? std::wstring(L"Fits what's in it") : std::to_wstring((len + 5) / 10) + L"% of the screen long";
    c.Text(line, Rect(in.left, in.top + 40, Width(in), 16), Weight::Regular, 11.5f, t.textDim);
    float bw = (Width(in) - 8) / 2, by = in.top + 68;
    if (Pill(ui, t, Ui::Id("st.compact"), Rect(in.left, by, bw, 28), L"Compact", false, editing && z != prefs::Settings::kZoomDef)) SetDock(&prefs::Settings::zoom, L"Zoom", prefs::Settings::kZoomDef);
    if (Pill(ui, t, Ui::Id("st.fit"), Rect(in.left + bw + 8, by, bw, 28), L"Fit", false, editing && len != prefs::Settings::kLenFit && !Fixed())) SetDock(&prefs::Settings::dockLen, L"DockLen", prefs::Settings::kLenFit);
    Note(c, t, in, in.bottom - 2 * kNoteLine, Vert() ? L"Drag the dock's inner edge" : L"Drag the dock's top edge");
    Note(c, t, in, in.bottom - kNoteLine, L"or its ends to resize it");
}

void Bar::StudioClock(const Theme& t, const D2D1_RECT_F& body) {
    SetClock(t, Rect(body.left, body.top, Width(body), kClockH));
    SetStart(t, Rect(body.left, body.top + kClockH + kCardGap, Width(body), kStartH));
}
