// The tour: a few steps over the dock, a soft light on each part and a card saying what it does and how it's made your own.
// It plays a moment after the first start while its switch is on, and Settings can play it any time.
#include "bar.h"

#include <cmath>

#include "glyphs.h"
#include "setui.h"

using namespace setui;

namespace {

enum Spot { kSpotNone, kSpotStart, kSpotApps, kSpotWidgets, kSpotStatus, kSpotDock };
struct Step {
    const wchar_t* title;
    const wchar_t* body;
    int spot; // a step whose part this dock doesn't show is left out
};
const Step kSteps[] = {
    {L"Welcome to HaloBar", L"A quick look around your new taskbar.\nIt takes about a minute.", kSpotNone},
    {L"Start", L"Click for your apps and search.\nRight-click for Windows' power menu.", kSpotStart},
    {L"Your apps", L"Pinned and running apps, as on Windows.\nHover for previews, right-click for recent\nfiles, scroll to change an app's volume.", kSpotApps},
    {L"Widgets", L"Music, weather, notes and more. Click one\nfor its panel.", kSpotWidgets},
    {L"Status", L"Wi-Fi, sound and quick settings. Click\nthe clock for notifications and the calendar.", kSpotStatus},
    {L"Make it yours", L"Right-click an empty spot on the dock to\nresize it, change its shape and body, and\npick its widgets.", kSpotDock},
    {L"Settings", L"Open Settings from the gear in the\nlauncher, or from quick settings.", kSpotNone},
    {L"You're all set", L"Take the tour again any time from\nSettings, at the bottom.", kSpotNone},
};
constexpr int kStepCount = (int)(sizeof(kSteps) / sizeof(kSteps[0]));

constexpr float kDim = 0.55f, kSpotPad = 6, kSpotRad = 16, kRing = 2, kPulseGrow = 9, kPulseSec = 1.6f; // the light
constexpr float kCardW = 320, kCardPad = 18, kCardGap = 16, kCardEdge = 16, kCardRad = 18, kLineH = 17, kTitleH = 24, kBtnH = 30, kDotsH = 10, kSwitchH = 30; // the card
constexpr float kNextW = 92, kAgainW = 116, kBackW = 70, kSkipW = 64, kBtnGap = 8, kDotR = 3, kDotOnW = 14, kDotGap = 6, kRise = 8;
constexpr float kTakeW = 150; // settings' card: its button

int Lines(const wchar_t* s) {
    int n = 1;
    for (; *s; ++s) n += *s == L'\n';
    return n;
}

bool Pill(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const wchar_t* label, bool primary, bool live) { // the card's buttons: next in the accent, the rest quiet
    Canvas& c = ui.C();
    Ui::Hit h = live ? ui.Button(id, r) : Ui::Hit{};
    if (live) ui.Name(label);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(r, Height(r) / 2, primary ? t.accent.Mix(t.accentHi, 0.35f * hov) : t.text.Alpha(0.07f + 0.06f * hov));
    c.Text(label, r, Weight::Medium, 12, primary ? t.onAccent : t.text, Align::Center);
    return h.clicked;
}

}

bool Bar::TourShows(int i) const {
    switch (kSteps[i].spot) {
    case kSpotApps: return spotApps.right > spotApps.left;
    case kSpotWidgets: return spotWidgets.right > spotWidgets.left;
    case kSpotStatus: return spotStatus.right > spotStatus.left;
    default: return true;
    }
}

void Bar::TourGo(int from, int dir) {
    for (int i = from; i >= 0 && i < kStepCount; i += dir) {
        if (!TourShows(i)) continue;
        tourStep = i;
        ++tourNth;
        Redraw();
        return;
    }
}

void Bar::TourStart() {
    if (moon) return; // the main dock's: one tour, whatever the screens
    SetPanel(Panel::None); // the dock itself, nothing over it
    tourAgain = false; // the last card asks again each time
    ++tourRun;
    TourGo(0, 1);
}

void Bar::TourEnd() {
    tourStep = -1;
    tourNext = tourAgain ? 1 : 0;
    prefs::SaveNum(L"TourNext", tourNext);
    Redraw();
}

void Bar::DrawTour(const Theme& t) {
    Canvas& c = *canvas;
    ID2D1RenderTarget* rt = c.Target();
    int step = tourStep >= 0 ? tourStep : kStepCount - 1; // fading out: the last card it showed
    const Step& st = kSteps[step];
    D2D1_RECT_F want = st.spot == kSpotStart ? startR : st.spot == kSpotApps ? spotApps : st.spot == kSpotWidgets ? spotWidgets : st.spot == kSpotStatus ? spotStatus : st.spot == kSpotDock ? wegR : D2D1_RECT_F{};
    bool lit = Width(want) > 1 && Height(want) > 1;
    D2D1_RECT_F full = {0, 0, W, winH};
    float show = tourK;
    float hole = ui.Smooth(Ui::Id("tour.hole"), lit ? 1.0f : 0, 10);
    D2D1_POINT_2F mid = {W / 2, winH / 2};
    want = lit ? Inset(want, -kSpotPad, -kSpotPad) : D2D1_RECT_F{mid.x, mid.y, mid.x, mid.y}; // nothing to point at: the light closes to the middle
    D2D1_RECT_F r = {ui.Smooth(Ui::Id("tour.l"), want.left, 12), ui.Smooth(Ui::Id("tour.t"), want.top, 12), ui.Smooth(Ui::Id("tour.r"), want.right, 12), ui.Smooth(Ui::Id("tour.b"), want.bottom, 12)}; // it glides from step to step
    float rad = (std::min)({kSpotRad, Width(r) / 2, Height(r) / 2});
    Color dim = Color{0, 0, 0, kDim * show};
    ComPtr<ID2D1RectangleGeometry> outer;
    ComPtr<ID2D1RoundedRectangleGeometry> inner;
    ComPtr<ID2D1PathGeometry> veil;
    ComPtr<ID2D1GeometrySink> sink;
    bool cut = hole > 0.01f && Width(r) > 1 && Height(r) > 1 && SUCCEEDED(gfx::D2D()->CreateRectangleGeometry(full, &outer)) &&
               SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry({r, rad, rad}, &inner)) && SUCCEEDED(gfx::D2D()->CreatePathGeometry(&veil)) && SUCCEEDED(veil->Open(&sink)) &&
               SUCCEEDED(outer->CombineWithGeometry(inner.Get(), D2D1_COMBINE_MODE_EXCLUDE, nullptr, sink.Get())) && SUCCEEDED(sink->Close());
    if (cut) c.FillGeometry(veil.Get(), dim);
    else c.Fill(full, 0, dim);
    if (cut) { // a ring around the light, and a softer one breathing out from it
        float phase = (float)std::fmod(ui.Now(), (double)kPulseSec) / kPulseSec, grow = kPulseGrow * phase;
        c.Stroke(r, rad, t.accentHi.Alpha(0.9f * hole * show), kRing);
        c.Stroke(Inset(r, -grow, -grow), rad + grow, t.accentHi.Alpha(0.45f * (1 - phase) * hole * show), 1.5f);
        ui.KeepAlive();
    }

    int shown = 0, at = 0;
    for (int i = 0; i < kStepCount; ++i)
        if (TourShows(i)) at = i == step ? shown : at, ++shown;
    bool first = at == 0, last = at + 1 == shown;
    float cw = (std::min)(kCardW, W - 2 * kCardEdge);
    float ch = 2 * kCardPad + kTitleH + Lines(st.body) * kLineH + kCardGap + kDotsH + kCardGap + (last ? kSwitchH + kBtnGap : 0) + kBtnH;
    float cx, cy;
    if (lit && !Vert() && want.top - kCardGap - ch >= kCardEdge) cx = (want.left + want.right - cw) / 2, cy = want.top - kCardGap - ch; // over the dock
    else if (lit && want.right + kCardGap + cw <= W - kCardEdge) cx = want.right + kCardGap, cy = (want.top + want.bottom - ch) / 2; // beside a side dock
    else if (lit && want.left - kCardGap - cw >= kCardEdge) cx = want.left - kCardGap - cw, cy = (want.top + want.bottom - ch) / 2;
    else cx = (W - cw) / 2, cy = (winH - ch) / 2;
    cx = (std::clamp)(cx, kCardEdge, (std::max)(kCardEdge, W - cw - kCardEdge));
    cy = (std::clamp)(cy, kCardEdge, (std::max)(kCardEdge, winH - ch - kCardEdge));
    cx = ui.Smooth(Ui::Id("tour.cx"), cx, 12), cy = ui.Smooth(Ui::Id("tour.cy"), cy, 12);
    float h = ui.Smooth(Ui::Id("tour.ch"), ch, 14);
    float in = ui.SmoothFrom(Ui::Id("tour.in", tourNth), 1, 0, 14) * show; // each step's words drift up into place
    D2D1_RECT_F card = Rect(cx, cy, cw, h);
    c.Glow({(card.left + card.right) / 2, card.bottom - 6}, cw * 0.55f, 30, Color{0, 0, 0, 0.3f * show});
    Color fill = t.bg;
    fill.a = 0.98f;
    c.Fill(card, kCardRad, fill.Alpha(0.98f * show));
    c.StrokeV(card, kCardRad, t.hairTop.Alpha(show), t.hairBot.Alpha(show), 1);
    c.PushClip(card);
    bool layer = in < 0.999f;
    if (layer) rt->PushLayer(D2D1::LayerParameters(card, nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), in), nullptr);
    float x = card.left + kCardPad, y = card.top + kCardPad + (1 - in) * kRise, w = cw - 2 * kCardPad;
    c.Text(st.title, Rect(x, y, w, kTitleH), Weight::Semi, 15, t.text);
    y += kTitleH;
    c.Text(st.body, Rect(x, y, w, Lines(st.body) * kLineH), Weight::Regular, 12, t.textDim);
    y += Lines(st.body) * kLineH + kCardGap;
    float dx = x;
    for (int i = 0; i < shown; ++i) { // where it is: the current one a short pill
        float on = ui.Smooth(Ui::Id("tour.dot", i), i == at ? 1.0f : 0, 14), dw = 2 * kDotR + (kDotOnW - 2 * kDotR) * on;
        c.Fill(Rect(dx, y + kDotsH / 2 - kDotR, dw, 2 * kDotR), kDotR, t.textLow.Mix(t.accent, on));
        dx += dw + kDotGap;
    }
    y += kDotsH + kCardGap;
    bool live = tourStep >= 0 && in > 0.5f;
    int go = 0; // -1 back, 1 next, 2 done, 3 again, 4 skip to the last card
    if (last) { // the switch for next time, then done or once more
        bool flip = false;
        Toggle(ui, t, "tour.again", Rect(x, y, w, kSwitchH), L"Show the tour next time", tourAgain, flip);
        if (flip && live) tourAgain = !tourAgain;
        y += kSwitchH + kBtnGap;
        if (Pill(ui, t, Ui::Id("tour.done"), Rect(card.right - kCardPad - kNextW, y, kNextW, kBtnH), L"Got it", true, live)) go = 2;
        if (Pill(ui, t, Ui::Id("tour.redo"), Rect(card.right - kCardPad - kNextW - kBtnGap - kAgainW, y, kAgainW, kBtnH), L"Take it again", false, live)) go = 3;
    } else {
        float bx = card.right - kCardPad - kNextW;
        if (Pill(ui, t, Ui::Id("tour.next"), Rect(bx, y, kNextW, kBtnH), first ? L"Let's go" : L"Next", true, live)) go = 1;
        if (!first && Pill(ui, t, Ui::Id("tour.back"), Rect(bx - kBtnGap - kBackW, y, kBackW, kBtnH), L"Back", false, live)) go = -1;
        if (Pill(ui, t, Ui::Id("tour.skip"), Rect(x, y, kSkipW, kBtnH), L"Skip", false, live)) go = 4;
    }
    if (layer) rt->PopLayer();
    c.PopClip();
    if (tourStep >= 0) ui.Button(Ui::Id("tour.veil"), full), ui.NoStop(); // last: the card's buttons take their presses first; nothing under it can be pressed
    if (go == 1) TourGo(tourStep + 1, 1);
    else if (go == -1) TourGo(tourStep - 1, -1);
    else if (go == 2) TourEnd();
    else if (go == 3) TourGo(0, 1), ++tourRun;
    else if (go == 4) TourGo(kStepCount - 1, -1); // its switch and its buttons still have the say
}

void Bar::SetTour(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Tour");
    float cy = (in.top + in.bottom) / 2;
    if (tourNext < 0) tourNext = prefs::ReadNum(L"TourNext", 1) != 0 ? 1 : 0; // read once
    if (LabelButton(t, Ui::Id("set.tour"), Rect(in.left, cy - 15, kTakeW, 30), glyph::Play, L"Take the tour")) {
        SetSettingsOpen(false);
        TourStart();
        return;
    }
    bool flip = false;
    Toggle(ui, t, "set.tour.next", Rect(in.left + kTakeW + 24, cy - 17, Width(in) - kTakeW - 24, 34), L"Show it when HaloBar starts", tourNext != 0, flip);
    if (flip) tourNext = tourNext ? 0 : 1, prefs::SaveNum(L"TourNext", tourNext), Redraw();
}
