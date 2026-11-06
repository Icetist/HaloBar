// Settings, grown out of the island like the player: island's instruments, a color orb and small cards you touch
// instead of rows of options. Every choice can also just follow astraeus island.
#include "bar.h"

#include <shellapi.h>

#include <cmath>
#include <cwctype>

#include "clips.h"
#include "glyphs.h"
#include "report.h"
#include "starfall.h"
#include "setui.h"
#include "version.h"

namespace setui { // settings' card, switch and segmented control: the clipboard panel's options wear them too

constexpr float kCardLift = 0.25f; // toward the track: a card that reads against the panel with no border, while its controls still read on it
constexpr float kLightLift = 0.7f; // a pale panel: toward white instead, as a darker card would swallow the slider tracks
constexpr float kPaleBody = 0.5f; // luma past this is a light look, its own or a custom one
Color CardFill(const Theme& t) {
    if (t.glass) return t.cardHi.Alpha(0.55f);
    if (theme::Luma(t.bg) > kPaleBody) return t.bg.Alpha(1).Mix(Color{1, 1, 1, 1}, kLightLift);
    return t.cardHi.Mix(t.track, kCardLift);
}

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title) { // island's card shell, returns its inner area
    c.Fill(r, 16, CardFill(t));
    if (theme::Contrast()) c.StrokeV(r, 16, t.hairTop, t.hairBot, 1); // card and panel are one color there: only an edge shows it
    c.Text(title, Rect(r.left + 14, r.top + 9, Width(r) - 28, 16), Weight::Medium, 12, t.text);
    return {r.left + 12, r.top + 30, r.right - 12, r.bottom - 10};
}

int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur) {
    Canvas& c = ui.C();
    float seg = Width(r) / n, rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    float x = ui.Smooth(Ui::Id(id, 99), r.left + seg * cur, 18); // the thumb slides between options
    c.Fill({x + 3, r.top + 3, x + seg - 3, r.bottom - 3}, rad - 3, t.accent); // solid, as every other picked control in settings
    int picked = -1;
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F cell = {r.left + seg * i, r.top, r.left + seg * (i + 1), r.bottom};
        Ui::Hit h = ui.Button(Ui::Id(id, i), cell);
        ui.Mark(Ui::Role::Toggle, i == cur);
        if (h.clicked && i != cur) picked = i; // the current one again changes nothing
        c.Text(labels[i], cell, Weight::Medium, 11.5f, i == cur ? t.onAccent : t.textDim.Mix(t.text, h.hover ? 0.6f : 0), Align::Center);
    }
    return picked;
}

void Toggle(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& row, const wchar_t* label, bool on, bool& flip) { // island's switch row: the words flip it too
    Canvas& c = ui.C();
    Ui::Hit h = ui.Button(Ui::Id(id, 1), {row.left, row.top, row.right - 52, row.bottom});
    ui.NoStop(); // the keyboard stops on the switch; the words only widen the mouse's target
    c.Text(label, Rect(row.left, row.top, Width(row) - 56, Height(row)), Weight::Regular, 11.5f, t.text);
    if (w::Switch(ui, t, Ui::Id(id), {row.right - 24, (row.top + row.bottom) / 2}, on) || h.clicked) flip = true; // reported, so the caller saves it after drawing
}

void Note(Canvas& c, const Theme& t, const D2D1_RECT_F& in, float y, const wchar_t* text) { // a quiet line under the controls, as island's
    c.Text(text, Rect(in.left, y, Width(in), kNoteLine), Weight::Light, 10.5f, t.textDim);
}

}


using namespace setui;

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHeroW = 210; // the orb's column
constexpr float kPad = 16, kCardGap = 10; // the sheet's margin, and the space between cards
constexpr float kSliderRow = 66; // under the theme swatches: their names, then the blur or opacity slider's label and its roomy row
constexpr float kSlideH = 32; // a slider's row: the whole height takes the mouse
constexpr float kInstH = 140; // the instrument cards: size, position, screens, dock
constexpr float kTabsH = 34; // the row over the cards
constexpr float kThemeH = 202; // the first row: the theme card's swatches, glass' own row, its slider
constexpr float kGlassRow = 34, kGlassRowH = 26; // that row, between the swatches' names and the slider
constexpr float kBelowRow = 49; // the slider and its label, under it
constexpr float kTintDot = 9; // the tint's plain choices
// a switch card hugs its rows: island's density. not kRowH: the bar has its own, the dock's height, and it would win inside Bar's members
constexpr float kLineH = 34, kNoteH = 14, kSegRow = 32; // a switch line, a quiet note line, a segmented control with its gap
constexpr float kHead = 30, kFoot = 10; // Box's title band above the rows, its padding below them
constexpr float CardH(int lines, int notes, int segs = 0) { return kHead + lines * kLineH + notes * kNoteH + segs * kSegRow + kFoot; }
constexpr float kBarH2 = CardH(2, 1), kClockH = CardH(2, 1), kStartH = CardH(1, 1); // each card's rows, as drawn below
constexpr float kAdvHeadH = 56; // the advanced section's bar, as island's
bool advOpen = false; // folded each launch: the everyday settings come first
float advBodyH = 0; // the unfolded cards' height, as measured last frame
constexpr float kAdvSlideLabelH = 14, kAdvSlideTrackH = 32; // a slider row in an advanced card: its label line, then the track's tall target
constexpr float kAdvSlideRowH = kAdvSlideLabelH + kAdvSlideTrackH + 4;
constexpr int kAdvSlideStep = 5; // percent per notch
constexpr float kAccessSeg = 16 + 28 + 6, kAccessLeft = kAccessSeg + kLineH + kAdvSlideLabelH + kAdvSlideTrackH; // the accessibility card's left column
constexpr float kAccessH = kHead + kAccessLeft + kFoot;
constexpr float kReportH = kHead + 38 + 3 * kNoteH + kFoot; // its buttons, then three notes
constexpr wchar_t kIssues[] = L"https://github.com/Icetist/HaloBar/issues/new"; // never prefilled: a report only travels if the user pastes it
constexpr double kCopiedSec = 2.5; // how long the button says what happened
constexpr int kTextStep = 10; // percent, the text size slider
constexpr float kPairH = (std::max)(kBarH2, kClockH); // cards side by side share the taller height
constexpr float kColGap = 24; // between a wide card's two columns
constexpr float kModsH = 92, kWideH = 64; // the status chips, two rows; one row of buttons across
constexpr float kNotch = 60; // dips per wheel notch, as island's settings
constexpr int kPresets[] = {0x5B8CFF, 0x9B7BFF, 0xFF6FAE, 0xFF6B6B, 0xFF9F43, 0xF5C542, 0x3DDC97, 0x2EC4D6}; // island's curated accents

float Rad(float deg) { return deg * kPi / 180; }

int ToHex(Color c) { // 0xRRGGBB, the form the registry keeps
    auto ch = [](float v) { return (int)(anim::Clamp01(v) * 255 + 0.5f); }; // rounded, so a color survives the round trip
    return (ch(c.r) << 16) | (ch(c.g) << 8) | ch(c.b);
}

// ---- custom look: island's wheel with two handles, accent and background, any color at all ----
struct Hsv {
    float h = 0, s = 0, v = 0;
};

Hsv ToHsv(Color c) {
    float mx = (std::max)(c.r, (std::max)(c.g, c.b)), mn = (std::min)(c.r, (std::min)(c.g, c.b)), d = mx - mn;
    Hsv o{0, mx > 0 ? d / mx : 0, mx}; // black has no saturation either
    if (d > 1e-5f) { // grey: hue is undefined, left at 0
        if (mx == c.r) o.h = 60 * std::fmod((c.g - c.b) / d, 6.0f);
        else if (mx == c.g) o.h = 60 * ((c.b - c.r) / d + 2);
        else o.h = 60 * ((c.r - c.g) / d + 4);
        if (o.h < 0) o.h += 360; // fmod keeps the sign
    }
    return o;
}

Color FromHsv(Hsv x) {
    float c = x.v * x.s, hp = std::fmod(x.h, 360.0f) / 60, k = c * (1 - std::fabs(std::fmod(hp, 2.0f) - 1)), m = x.v - c;
    float r = 0, g = 0, b = 0;
    if (hp < 1) r = c, g = k;
    else if (hp < 2) r = k, g = c;
    else if (hp < 3) g = c, b = k;
    else if (hp < 4) g = k, b = c;
    else if (hp < 5) r = k, b = c;
    else r = c, b = k;
    return {r + m, g + m, b + m, 1}; // always opaque: these are fills, not tints
}

ComPtr<ID2D1Bitmap> wheelBmp;
ComPtr<ID2D1Device> wheelOwner; // held, so a new device can't reuse its address; let go in FreeWheel, or the old device lives on

ID2D1Bitmap* WheelBmp(ID2D1RenderTarget* rt) { // hue around, saturation outward, at full brightness; made once per device
    ComPtr<ID2D1Bitmap>& bmp = wheelBmp;
    ComPtr<ID2D1Device>& owner = wheelOwner;
    ComPtr<ID2D1DeviceContext> dc;
    ComPtr<ID2D1Device> dev;
    if (SUCCEEDED(rt->QueryInterface(IID_PPV_ARGS(&dc)))) dc->GetDevice(&dev);
    if (bmp && dev == owner) return bmp.Get(); // bitmaps belong to a device: a lost one means a fresh bitmap
    bmp.Reset();
    owner = dev;
    constexpr UINT kN = 160;
    std::vector<uint32_t> px(kN * kN); // zero is transparent outside the circle
    for (UINT y = 0; y < kN; ++y)
        for (UINT x = 0; x < kN; ++x) {
            float dx = (x + 0.5f) / kN * 2 - 1, dy = (y + 0.5f) / kN * 2 - 1, r = std::hypot(dx, dy);
            float a = anim::Clamp01((1 - r) * kN / 2); // a soft pixel at the rim
            if (a <= 0) continue;
            Color c = FromHsv({std::fmod(std::atan2(dy, dx) * 180 / kPi + 90 + 360, 360.0f), (std::min)(r, 1.0f), 1}); // +90: red at the top, as the ring
            auto ch = [a](float v) { return (uint32_t)(anim::Clamp01(v) * a * 255 + 0.5f); }; // premultiplied
            px[y * kN + x] = (uint32_t)(a * 255 + 0.5f) << 24 | ch(c.r) << 16 | ch(c.g) << 8 | ch(c.b); // bgra in memory
        }
    rt->CreateBitmap(D2D1::SizeU(kN, kN), px.data(), kN * 4,
                     D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED)), &bmp);
    return bmp.Get();
}

Hsv handles[2]; // each handle's place on the wheel, kept apart from its hex so black doesn't lose its hue
int handleFor[2] = {-1, -1}; // the hex each place belongs to

Hsv HandleOf(int which, int hex) {
    hex &= 0xFFFFFF;
    if (handleFor[which] != hex) { // changed elsewhere: find it on the wheel
        Hsv fresh = ToHsv(Color::Rgb((uint32_t)hex));
        if (fresh.v < 0.01f || fresh.s < 0.01f) fresh.h = handles[which].h; // black or grey has no hue: keep where it was
        if (fresh.v < 0.01f) fresh.s = handles[which].s; // black has no saturation either: the handle stays put
        handles[which] = fresh;
        handleFor[which] = hex;
    }
    return handles[which];
}

bool ParseHex(const std::wstring& text, int& rgb) { // #rgb, #rrggbb, with or without the #
    std::wstring h;
    for (wchar_t c : text)
        if (iswxdigit(c)) h += c;
        else if (c != L'#' && !iswspace(c)) return false; // anything else isn't a color
    if (h.size() == 3) h = {h[0], h[0], h[1], h[1], h[2], h[2]}; // #abc -> #aabbcc
    if (h.size() != 6) return false;
    rgb = (int)wcstoul(h.c_str(), nullptr, 16);
    return true;
}

std::wstring HexText(int hex) {
    wchar_t b[8];
    swprintf_s(b, L"%06X", hex & 0xFFFFFF);
    return b;
}

bool SystemClock24() { // what windows' own clock does, for "follow"
    wchar_t v[4] = {};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, v, 4) > 0 && v[0] == L'1'; // "1" means 24-hour
}


void Monitor(Canvas& c, const Theme& t, const D2D1_RECT_F& s) { // a little screen on a stand, as island's
    float mx = (s.left + s.right) / 2;
    c.Fill(Rect(mx - 6, s.bottom, 12, 5), 2, t.track);
    c.Fill(Rect(mx - 15, s.bottom + 5, 30, 3), 1.5f, t.track);
    c.FillV(s, 7, t.track.Alpha(0.9f), t.track.Alpha(0.5f));
    c.Stroke(s, 7, t.hairTop, 1);
}

D2D1_RECT_F MiniDock(const D2D1_RECT_F& q, int edge, int align, float thick) { // where the dock sits on a little screen
    float w = Width(q), h = Height(q), len = w * 0.36f;
    if (edge == 1) return {q.left + 3, q.top + h * 0.3f, q.left + 3 + thick, q.bottom - h * 0.3f};
    if (edge == 2) return {q.right - 3 - thick, q.top + h * 0.3f, q.right - 3, q.bottom - h * 0.3f};
    float x = align == 1 ? q.left + 4 : align == 2 ? q.right - 4 - len : (q.left + q.right - len) / 2;
    return {x, q.bottom - 3 - thick, x + len, q.bottom - 3};
}

}

void Bar::DrawSettings(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, setSpring.value); // the spring overshoots below zero on close
    if (rise < 1 || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 60) / (kSetRise - 60)); // stays hidden for the first 60 dips, so the content doesn't cram into a sliver
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr); // inside the island, fading in as it grows
    float w = (std::min)(kSetW, W - 24), x0 = W / 2 - w / 2, y0 = pill.top + CallH(); // laid out at full size, so nothing slides as it opens
    D2D1_RECT_F area = {x0 + kPad, y0 + kPad, x0 + w - kPad, y0 + kSetRise - 6};
    SetHero(t, {area.left, area.top, area.left + kHeroW, area.bottom});
    float gx = area.left + kHeroW + 14;
    c.Line({gx - 8, area.top + 8}, {gx - 8, area.bottom - 8}, t.text.Alpha(0.06f), 1); // a faint divider between the orb and the cards
    c.Text(L"Settings", Rect(gx + 4, area.top, 200, 20), Weight::Medium, 15, t.text);
    if (w::IconBtn(ui, t, Ui::Id("set.close"), {area.right - 12, area.top + 10}, 28, glyph::Close, 17)) SetSettingsOpen(false);

    D2D1_RECT_F view = {gx, area.top + 30, area.right, area.bottom}; // what shows; the cards below it scroll into it
    float cw = (Width(view) - 2 * kCardGap) / 3, half = (Width(view) - kCardGap) / 2;
    float content = kTabsH + kThemeH + kInstH + kPairH + kStartH + kModsH + 2 * kWideH + kAdvHeadH + 8 * kCardGap;
    float advShown = advBodyH * ui.Smooth(Ui::Id("set.adv", 2), advOpen ? 1.0f : 0, 11); // the folded section's share, as it unfolds
    content += advShown; // island's order: the instruments, then switch cards two across, then the wide ones
    float most = (std::max)(0.0f, content - Height(view));
    uint32_t sid = Ui::Id("set.scroll");
    bool overSlide = slideHot; // the wheel over a slider steps it, not the sheet
    slideHot = false;
    if (float notch = overSlide ? 0 : ui.Wheel(view); notch != 0) setScroll = (std::clamp)(setScroll - notch * kNotch, 0.0f, most);
    setScroll = (std::min)(setScroll, most); // a bigger size leaves less to scroll
    float scroll = ui.Smooth(sid, setScroll, 18);
    ui.Reveal(view, scroll, setScroll); // the keyboard moved to something out of view
    setScroll = (std::clamp)(setScroll, 0.0f, most);
    D2D1_RECT_F grid = {view.left, view.top - scroll, view.right, view.bottom - scroll};
    D2D1_POINT_2F mouse = input.mouse;
    bool masked = !Inside(view, ui.In().mouse) && !ui.Captured(); // cards scrolled above the fold can't take a click meant for the header; a drag already under way keeps going
    constexpr D2D1_POINT_2F kAway = {1e4f, 1e4f};
    if (masked) input.mouse = {-1e4f, -1e4f}, ui.Shift(kAway); // the ui hit-tests its own copy of the mouse: moved off too
    rt->PushAxisAlignedClip(Inset(view, -4, 0), D2D1_ANTIALIAS_MODE_ALIASED);
    ui.List(true);
    float y = grid.top;
    SetThemeTabs(t, Rect(grid.left, y, Width(grid), kTabsH));
    y += kTabsH + kCardGap;
    SetMaterial(t, Rect(grid.left, y, 2 * cw + kCardGap, kThemeH)); // materials need room for five swatches: two cells wide
    SetSize(t, Rect(grid.left + 2 * (cw + kCardGap), y, cw, kThemeH));
    y += kThemeH + kCardGap;
    SetPlace(t, Rect(grid.left, y, cw, kInstH)); // island's own trio: where it sits, on which screens
    SetScreens(t, Rect(grid.left + cw + kCardGap, y, cw, kInstH));
    SetRoom(t, Rect(grid.left + 2 * (cw + kCardGap), y, cw, kInstH));
    y += kInstH + kCardGap;
    SetSong(t, Rect(grid.left, y, half, kPairH));
    SetClock(t, Rect(grid.left + half + kCardGap, y, half, kPairH));
    y += kPairH + kCardGap;
    SetStart(t, Rect(grid.left, y, Width(grid), kStartH)); // the extras moved into advanced: the start card takes the row
    y += kStartH + kCardGap;
    SetMods(t, Rect(grid.left, y, Width(grid), kModsH));
    y += kModsH + kCardGap;
    SetWidgets(t, Rect(grid.left, y, Width(grid), kWideH));
    y += kWideH + kCardGap;
    SetAdvanced(t, Rect(grid.left, y, Width(grid), kAdvHeadH)); // a bar that unfolds its cards under it, just before halobar's own buttons
    y += kAdvHeadH + kCardGap + advShown;
    SetApp(t, Rect(grid.left, y, Width(grid), kWideH));
    ui.List(false);
    rt->PopAxisAlignedClip();
    if (masked) ui.Shift({-kAway.x, -kAway.y});
    input.mouse = mouse;
    float k = most > 0 ? scroll / most : 0, lane = Height(view) - 16, thumb = lane * Height(view) / (Height(view) + most); // a slim thumb at the right edge: there's more below
    c.Fill(Rect(view.right + 5, view.top + 8 + (lane - thumb) * k, 3, thumb), 1.5f, t.text.Alpha(0.10f + 0.12f * ui.Smooth(sid + 1, Inside(view, mouse) ? 1.0f : 0, 10)));
    if (ui.RClick(pill) && Inside({x0, y0, x0 + w, rowTop}, input.mouse)) SetSettingsOpen(false); // right click closes, as island's sheets
    rt->PopLayer();
}

void Bar::SetHero(const Theme& t, const D2D1_RECT_F& r) { // the accent: a hue ring around a glowing orb, where it comes from, island's presets
    Canvas& c = ui.C();
    float hx = (r.left + r.right) / 2;
    c.Text(std::wstring(L"HaloBar  ·  ") + APP_VER_W, Rect(r.left, r.top, Width(r), 16), Weight::Medium, 12, t.textDim, Align::Center);

    if (cfg.look == 3) { // custom: any accent and any background, straight off the wheel, as island's
        SetWheel(t, {hx, r.top + 80}, 54);
        SetChip(t, Rect(hx - 96, r.top + 146, 94, 26), 0, L"Accent");
        SetChip(t, Rect(hx + 2, r.top + 146, 94, 26), 1, L"Background");
        SetShade(t, Rect(hx - 90, r.top + 188, 180, 12));
    } else {
        D2D1_POINT_2F o = {hx, r.top + 84};
        const float R = 46, ringW = R * 0.17f;
        bool own = cfg.accent == 1; // 1: the bar's own accent, not island's or the wallpaper's
        for (int i = 0; i < 36; ++i) { // rainbow ring, dimmed unless the bar picks its own accent
            float deg = i * 10.0f;
            c.Arc(o, R, Rad(deg - 90.5f), Rad(11), theme::Hsl(deg, 0.80f, 0.60f).Alpha(own ? 1.0f : 0.45f), ringW); // 11 degree arcs overlap a little, so no seams
        }
        uint32_t id = Ui::Id("set.orb");
        float reach = R + ringW;
        ui.Button(id, {o.x - reach, o.y - reach, o.x + reach, o.y + reach}); // only to own the drag; the ring test below decides
        D2D1_POINT_2F ms = input.mouse, ps = input.pressPos;
        float face = R - ringW - 3; // the orb inside the ring
        if (ui.IsActive(id) && input.pressed && std::fabs(std::hypot(ps.x - o.x, ps.y - o.y) - R) < ringW + 8) ringDrag = true; // grabbed the ring
        else if (ui.IsActive(id) && input.pressed && std::hypot(ps.x - o.x, ps.y - o.y) < face) BeginHex(0, own ? cfg.accentHex : ToHex(t.accent)); // tapped the orb: type a color
        if (hexFor == 0 && input.pressed && std::hypot(ps.x - o.x, ps.y - o.y) >= face) EndHex(true); // clicked away: kept if it's a color
        if (ringDrag && ui.IsActive(id) && input.down) { // the whole bar recolors live while you drag
            float hue = std::fmod(std::atan2(ms.y - o.y, ms.x - o.x) * 180 / kPi + 90 + 360, 360.0f);
            cfg.accentHex = ToHex(theme::Hsl(hue, 0.78f, 0.62f)); // the ring picks hue only; island's own saturation and lightness
            cfg.accent = 1;
            SyncLook();
        }
        if (ringDrag && input.released) { // written once on release, not every frame of the drag
            ringDrag = false;
            prefs::SaveNum(L"AccentHex", cfg.accentHex);
            prefs::SaveNum(L"Accent", 1);
        }
        Seed cur = theme::FromCol(t.accent);
        c.Glow(o, R * 1.5f, R * 1.5f, t.accent.Alpha(0.24f));
        c.Dot(o, R - ringW - 3, t.accent);
        D2D1_POINT_2F k = {o.x + R * std::cos(Rad(cur.hue - 90)), o.y + R * std::sin(Rad(cur.hue - 90))}; // the knob sits at the current hue
        c.Dot(k, ringW * 0.75f + 3, t.text); // a rim in the text color, so it reads on any hue
        c.Dot(k, ringW * 0.75f, theme::Hsl(cur.hue, 0.80f, 0.60f));
        static const wchar_t* const kNames[] = {L"Astraeus", L"Wallpaper", L"Custom"};
        int skip = islandHere ? 0 : 1; // without island there's nothing to follow: wallpaper and custom only
        int src = (std::max)(cfg.accent + 1, skip); // -1 follow island, 0 wallpaper, 1 custom; following a missing island looks like wallpaper
        float segW = 64.0f * (3 - skip);
        int v = Segmented(ui, t, "set.accent", Rect(hx - segW / 2, r.top + 146, segW, 24), kNames + skip, 3 - skip, src - skip);
        D2D1_RECT_F orbText = {o.x - R, o.y - 9, o.x + R, o.y + 9};
        if (hexFor == 0) {
            std::wstring shown = L"#" + hexEdit;
            c.Text(shown, orbText, Weight::Medium, 12, t.onAccent, Align::Center);
            if (Caret()) {
                float cx = o.x + TextW(shown, Weight::Medium, 12) / 2 + 1;
                c.Line({cx, o.y - 8}, {cx, o.y + 8}, t.onAccent, 1.5f);
            }
        } else {
            c.Text(own ? L"#" + HexText(cfg.accentHex) : std::wstring(kNames[src]), orbText, Weight::Medium, own ? 12.0f : 11.5f, t.onAccent, Align::Center); // your own color: its code, as astraeus shows it
        }
        if (v >= 0) {
            v += skip;
            if (v == 2 && cfg.accent != 1) cfg.accentHex = ToHex(t.accent), prefs::SaveNum(L"AccentHex", cfg.accentHex); // custom starts from what's showing
            SetPref(cfg.accent, L"Accent", v - 1);
        }
        for (int i = 0; i < 8; ++i) { // one-tap accents
            D2D1_POINT_2F p = {hx - 77 + i * 22, r.top + 190};
            Ui::Hit h = ui.Button(Ui::Id("set.preset", i), {p.x - 10, p.y - 10, p.x + 10, p.y + 10});
            bool on = cfg.accent == 1 && (cfg.accentHex & 0xFFFFFF) == kPresets[i];
            if (on || h.hover) c.Ring(p, 10, t.text.Alpha(on ? 0.9f : 0.4f), 1.5f);
            c.Dot(p, 7, Color::Rgb((uint32_t)kPresets[i]));
            if (h.clicked) {
                cfg.accentHex = kPresets[i];
                prefs::SaveNum(L"AccentHex", cfg.accentHex);
                if (cfg.accent == 1) SyncLook(), Redraw(); // already custom: only the color changed
                else SetPref(cfg.accent, L"Accent", 1); // switches to custom, which redraws on its own
            }
        }
    }
    D2D1_RECT_F btn = Rect(r.left + 14, r.bottom - 32, Width(r) - 28, 30); // pinned to the column's foot in both modes
    Ui::Hit h = ui.Button(Ui::Id("set.win"), btn);
    float hov = ui.Smooth(Ui::Id("set.win", 1), h.hover ? 1.0f : 0, 16);
    c.Fill(btn, 15, t.accent.Alpha(0.18f + 0.12f * hov));
    c.Icon(glyph::OpenNew, {btn.left + 22, (btn.top + btn.bottom) / 2}, 15, t.accentHi);
    c.Text(L"Taskbar settings", Rect(btn.left + 34, btn.top, Width(btn) - 40, Height(btn)), Weight::Medium, 11.5f, t.text);
    if (h.clicked) {
        SetSettingsOpen(false); // out of the way of the window that opens
        ShellExecuteW(nullptr, L"open", L"ms-settings:taskbar", nullptr, nullptr, SW_SHOWNORMAL);
    }
}

void Bar::SetGlassRow(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    constexpr float kSegW = 124, kAfterSeg = 16, kDotGap = 8, kAfterDots = 12;
    static const wchar_t* const kKinds[] = {L"Glass", L"Clear"};
    float cy = (r.top + r.bottom) / 2;
    int v = Segmented(ui, t, "set.glasskind", Rect(r.left, r.top + 1, kSegW, Height(r) - 2), kKinds, 2, cfg.look == prefs::Settings::kClear ? 1 : 0);
    if (v >= 0) {
        SetPref(cfg.look, L"Look", v == 1 ? prefs::Settings::kClear : 2);
        return; // the row's look changes next frame
    }
    float x = r.left + kSegW + kAfterSeg;
    if (cfg.look == prefs::Settings::kClear) { // nothing to pick: its ink looks after itself, its panels' tint follows the wallpaper
        c.Text(L"Icons turn black or white to suit what's behind", {x, r.top, r.right, r.bottom}, Weight::Regular, 11, t.textDim);
        return;
    }
    struct { int hex; Color fill; const wchar_t* name; } plain[] = {{-1, Color{0.05f, 0.05f, 0.07f, 1}, L"Dark tint"}, {0xFFFFFF, Color{1, 1, 1, 1}, L"White tint"}}; // the two that aren't on the strip
    for (const auto& p : plain) {
        D2D1_POINT_2F o = {x + kTintDot, cy};
        uint32_t id = Ui::Id("set.tint", p.hex < 0 ? 0 : 1);
        Ui::Hit h = ui.Button(id, {o.x - kTintDot - 2, r.top, o.x + kTintDot + 2, r.bottom});
        ui.Name(p.name);
        float on = ui.Smooth(id + 10, cfg.glassTint == p.hex ? 1.0f : 0, 14);
        c.Dot(o, kTintDot, p.fill);
        c.Ring(o, kTintDot, t.text.Alpha(0.30f), 1);
        if (on > 0.01f) c.Ring(o, kTintDot + 3, t.accentHi.Alpha(on), 1.5f);
        if (h.clicked) SetPref(cfg.glassTint, L"GlassTint", p.hex);
        x += 2 * kTintDot + kDotGap;
    }
    x += kAfterDots - kDotGap;
    constexpr float kStripH = 12;
    constexpr int kStops = 48;
    constexpr float kStripS = 0.75f, kStripL = 0.55f; // bright enough to read as a color through the frost
    D2D1_RECT_F strip = {x + kStripH / 2, cy - kStripH / 2, r.right - kStripH / 2, cy + kStripH / 2}; // any hue: the bar recolors live under the mouse
    float sw = Width(strip) / kStops;
    c.Dot({strip.left, cy}, kStripH / 2, theme::Hsl(0, kStripS, kStripL));
    c.Dot({strip.right, cy}, kStripH / 2, theme::Hsl(360, kStripS, kStripL));
    for (int i = 0; i < kStops; ++i) c.Fill(Rect(strip.left + i * sw, strip.top, sw + 0.6f, kStripH), 0, theme::Hsl(360.0f * i / kStops, kStripS, kStripL));
    uint32_t id = Ui::Id("set.tint.hue");
    ui.Button(id, {strip.left - kStripH / 2, r.top, strip.right + kStripH / 2, r.bottom});
    ui.Name(L"Tint color");
    if (ui.IsActive(id) && (input.down || input.released)) {
        float hue = anim::Clamp01((input.mouse.x - strip.left) / Width(strip)) * 360;
        cfg.glassTint = ToHex(theme::Hsl(hue, kStripS, kStripL));
        SyncLook();
        Redraw();
        if (input.released) prefs::SaveNum(L"GlassTint", cfg.glassTint), SampleShade(); // written once; the new rule looks behind the glass again
    }
    if (cfg.glassTint >= 0 && cfg.glassTint != 0xFFFFFF) { // the knob, at the tint's hue
        Seed cur = theme::FromCol(Color::Rgb((uint32_t)cfg.glassTint));
        float kx = strip.left + cur.hue / 360 * Width(strip);
        c.Dot({kx, cy + 1}, kStripH / 2 + 3, Color{0, 0, 0, 0.30f});
        c.Dot({kx, cy}, kStripH / 2 + 3, Color{1, 1, 1, 1});
        c.Dot({kx, cy}, kStripH / 2, Color::Rgb((uint32_t)cfg.glassTint));
    }
}

void Bar::SetThemeTabs(const Theme& t, const D2D1_RECT_F& r) {
    constexpr float kWordsShare = 0.56f, kSegGap = 12;
    bool on = cfg.appsTheme != 0, flip = false;
    float words = Width(r) * kWordsShare;
    Toggle(ui, t, "set.appstheme", Rect(r.left, r.top, words, Height(r)), L"Different theme when not on desktop", on, flip);
    if (flip) {
        SetPref(cfg.appsTheme, L"AppsTheme", on ? 0 : 1);
        CheckCovered();
        SyncThemeSet();
        return;
    }
    if (!on) return;
    static const wchar_t* const kSets[] = {L"Desktop", L"Not on desktop"};
    int v = Segmented(ui, t, "set.themetab", Rect(r.left + words + kSegGap, r.top + 3, Width(r) - words - kSegGap, Height(r) - 6), kSets, 2, setNow);
    if (v >= 0) setTab = v, SyncThemeSet(); // it previews as you edit it
}

void Bar::SetMaterial(const Theme& t, const D2D1_RECT_F& r) { // island's materials, or following island itself; swatches drawn in the actual materials
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Theme");
    bool ownGlass = cfg.look == 2 || cfg.look == prefs::Settings::kClear; // only the bar's own glass gets a blur slider, clear's panels included; island's glass is island's to tune
    bool ownBody = !ownGlass && OwnOpacity() && LookNow() != 2 && !Clear(); // the solid looks get island's opacity slider; glass has no body to thin
    Seed s = theme::FromCol(t.accent); // every swatch is tinted with the live accent
    static const wchar_t* const kNames[] = {L"Astraeus", L"Dark", L"Light", L"Glass", L"Custom"};
    static const int kLooks[] = {-1, 0, 1, 2, 3}; // -1 follows island
    constexpr int kLookCount = 5;
    int order[kLookCount], n = 0;
    for (int i = 0; i < kLookCount; ++i)
        if (kLooks[i] != -1 || (islandHere && (!look.followsBar || cfg.look == kLooks[i]))) order[n++] = i; // without island, or with island following us, no swatch to follow it; kept while it's the pick
    bool glassy = ownGlass && !theme::Contrast(); // following astraeus: its glass is picked and tinted over there
    float rowOpen = ui.Smooth(Ui::Id("set.mat.row"), glassy ? 1.0f : 0, 14); // the swatches give way to glass' own row
    float sw = (Width(in) - (n - 1) * 8) / n, sh = Height(in) - kSliderRow - kGlassRow * rowOpen; // the names, then the blur or opacity slider, or a word on why there's none
    int chosen = cfg.look == prefs::Settings::kClear ? 2 : cfg.look < 0 && !islandHere ? look.look : cfg.look; // clear lives under glass. following a missing island: its defaults, shown on their own swatch
    for (int k = 0; k < n; ++k) {
        int i = order[k], pick = kLooks[i];
        D2D1_RECT_F q = Rect(in.left + k * (sw + 8), in.top + 2, sw, sh);
        uint32_t id = Ui::Id("set.mat", i);
        Ui::Hit h = ui.Button(id, q);
        float on = ui.Smooth(id + 10, chosen == pick ? 1.0f : 0, 14), hov = ui.Smooth(id + 20, h.hover ? 1.0f : 0, 16); // offset ids: separate animation state per swatch
        int shown = pick < 0 ? look.look : pick; // island's swatch wears whatever island wears
        Theme th = Theme::From(Seed{s.hue, s.chroma, shown, s.rgb, pick == 3 ? cfg.customBg : look.customBg});
        if (shown == 2) { // glass: color under frost, a light streak, a bright rim
            ComPtr<ID2D1RoundedRectangleGeometry> round;
            if (SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(q, 6, 6), &round))) {
                c.Target()->PushLayer(D2D1::LayerParameters(q, round.Get()), nullptr); // clip the blobs and streak to the swatch's corners
                c.Dot({q.left + sw * 0.3f, q.top + sh * 0.7f}, sh * 0.6f, th.accent.Alpha(0.65f));
                c.Dot({q.right - sw * 0.25f, q.top + sh * 0.25f}, sh * 0.5f, theme::Hsl(s.hue + 60, 0.7f, 0.6f, 0.55f));
                c.FillV(q, 6, Color{1, 1, 1, 0.22f}, Color{1, 1, 1, 0.06f});
                c.Line({q.left + sw * 0.15f, q.bottom + 6}, {q.left + sw * 0.75f, q.top - 6}, Color{1, 1, 1, 0.22f}, 8);
                c.Target()->PopLayer();
            }
            c.Stroke(q, 6, Color{1, 1, 1, 0.40f}, 1.2f);
        } else {
            c.FillV(q, 6, th.cardHi, th.bg.Alpha(1)); // opaque: the swatch shows the material, not what's behind the bar
            c.Fill(Rect(q.left, q.top, sw, 8), 0, th.bg.Alpha(1).Mix(th.text, 0.06f)); // a little toolbar along its top
            c.Fill(Rect(q.left + sw * 0.25f, q.bottom - 14, sw * 0.5f, 8), 3, th.bg.Alpha(1).Mix(th.text, 0.12f)); // and a little dock
            if (th.stars) starfall::Sky(c, q, 6, th, 0, 1, 1, nullptr, 6); // island's starfall, a denser field to read at swatch size
            c.Dot({q.left + sw / 2, q.top + 4}, 2, th.accent);
            c.StrokeV(q, 6, th.hairTop, th.hairBot, 1);
        }
        if (on > 0.01f || hov > 0.01f) c.Stroke(Inset(q, -3, -3), 8, t.accent.Alpha((std::max)(on, 0.5f * hov)), 2); // a halo outside the swatch, hover at half strength
        c.Text(kNames[i], Rect(q.left - 4, q.bottom + 3, sw + 8, 14), Weight::Medium, 11, t.textDim.Mix(t.text, on), Align::Center);
        if (!h.clicked) continue;
        if (pick == 3 && cfg.accent != 1) { // the wheel starts from the accent you're looking at
            cfg.accentHex = ToHex(t.accent), cfg.accent = 1;
            prefs::SaveNum(L"AccentHex", cfg.accentHex), prefs::SaveNum(L"Accent", 1);
        }
        SetPref(cfg.look, L"Look", pick); // into or out of glass happens after this frame
    }
    if (rowOpen > 0.01f) { // uncovered as the swatches shrink, never over them
        float rowTop = in.bottom - kBelowRow - kGlassRow, namesEnd = in.top + 2 + sh + 18;
        c.PushClip({in.left - 2, namesEnd, in.right + 2, in.bottom - kBelowRow});
        if (glassy) SetGlassRow(t, Rect(in.left, rowTop + (kGlassRow - kGlassRowH) / 2, Width(in), kGlassRowH));
        c.PopClip();
    }
    if (!ownGlass && !ownBody) { // island's look: its blur and opacity are tuned there, and shared
        const wchar_t* why = OwnOpacity() && !Frosted() ? L"Glass is see-through: no opacity to set" : Frosted() ? L"Blur follows Astraeus" : L"Opacity follows Astraeus";
        c.Text(why, Rect(in.left, in.bottom - 18, Width(in), 14), Weight::Medium, 11, t.textDim, Align::Center);
        return;
    }
    // glass: how frosted, clear to windows' full backdrop blur; the solid looks: how much desktop shows through, as island's slider
    int& field = ownGlass ? cfg.blur : cfg.opacity;
    int lo = ownGlass ? 0 : prefs::Settings::kMinOpacity;
    float v = (field - lo) / (float)(100 - lo);
    uint32_t id = Ui::Id(ownGlass ? "set.blur" : "set.opacity");
    D2D1_RECT_F row = Rect(in.left - 4, in.bottom - kSlideH, Width(in) + 8, kSlideH); // the full width: room to place it finely
    c.Text(ownGlass ? L"Blur" : L"Opacity", Rect(in.left, row.top - 17, 120, 16), Weight::Regular, 11.5f, t.text);
    c.Text(std::to_wstring(field) + L"%", Rect(in.right - 60, row.top - 17, 60, 16), Weight::Semi, 12, t.accentHi, Align::Right);
    w::SlideLook lk;
    lk.bubble = std::to_wstring(field) + L"%";
    lk.step = 5.0f / (100 - lo); // 5% a notch
    if (Inside(row, input.mouse)) slideHot = true;
    if (w::Slide(ui, t, id, row, v, lk)) {
        field = lo + (int)(v * (100 - lo) + 0.5f), Redraw(); // live
        if (!ui.IsActive(id)) prefs::SaveNum(ownGlass ? L"GlassBlur" : L"Opacity", field); // a wheel notch: kept at once
    }
    if (ui.IsActive(id) && input.released) prefs::SaveNum(ownGlass ? L"GlassBlur" : L"Opacity", field); // a drag: saved once, on release; astraeus following the bar picks it up
}

void Bar::SetSize(const Theme& t, const D2D1_RECT_F& r) { // three little islands, small to large
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Size");
    static const wchar_t* const kNames[] = {L"Compact", L"Normal", L"Large"};
    float cw = Width(in) / 3, cy = in.top + (Height(in) - 16) / 2; // centered above the 14 dip labels
    for (int i = 0; i < 3; ++i) {
        float pw = 26 + 7.0f * i, ph = 9 + 2.5f * i, cx = in.left + cw * (i + 0.5f); // each a bit wider and taller than the last
        D2D1_RECT_F hit = Rect(in.left + cw * i, in.top, cw, Height(in));
        uint32_t id = Ui::Id("set.size", i);
        Ui::Hit h = ui.Button(id, hit);
        float on = ui.Smooth(id + 10, cfg.size == i ? 1.0f : 0, 14);
        D2D1_RECT_F mini = Rect(cx - pw / 2, cy - ph / 2, pw, ph);
        if (on > 0.01f) c.Glow({cx, cy}, pw, ph * 2, t.accent.Alpha(0.25f * on));
        c.Fill(mini, ph / 2, Color{0.02f, 0.02f, 0.03f, 0.95f}); // always the dark island, whatever the material
        c.Stroke(mini, ph / 2, t.accent.Alpha(on).Mix(t.text.Alpha(0.4f), h.hover ? 0.6f * (1 - on) : 0), 1.2f);
        c.Dot({mini.left + ph / 2 + 1, cy}, ph * 0.22f, t.accent);
        c.Text(kNames[i], Rect(hit.left, in.bottom - 14, cw, 14), Weight::Medium, 10.5f, t.textDim.Mix(t.text, on), Align::Center);
        if (h.clicked) SetPref(cfg.size, L"Size", i);
    }
}

void Bar::SetRoom(const Theme& t, const D2D1_RECT_F& r) { // the dock: always there with its strip kept free, or out of the way under windows
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Dock");
    float hide = ui.Smooth(Ui::Id("set.dock", 98), cfg.room ? 1.0f : 0, 14);
    D2D1_RECT_F q = {in.left + 14, in.top + 4, in.right - 14, in.bottom - 26 - kNoteH - 12}; // a little screen: a maximized window and the dock
    c.FillV(q, 6, t.track.Alpha(0.9f), t.track.Alpha(0.5f));
    c.Stroke(q, 6, t.hairTop, 1);
    constexpr float kThick = 4, kKeep = 9; // the dock on the little screen; the strip it keeps free
    float keep = kKeep * (1 - hide); // shown: the window stops short of the dock; auto-hide: it fills the screen
    D2D1_RECT_F win = Inset(q, 4, 4);
    if (cfg.edge == 1) win.left += keep;
    else if (cfg.edge == 2) win.right -= keep;
    else win.bottom -= keep;
    c.Fill(win, 3, t.text.Alpha(0.10f));
    c.Fill(Rect(win.left, win.top, Width(win), 6), 3, t.text.Alpha(0.08f)); // its title bar
    c.Fill(MiniDock(q, cfg.edge, cfg.align, kThick), 2, t.accent.Alpha(1 - 0.75f * hide)); // tucked: a faint line at the edge
    static const wchar_t* const kDock[] = {L"Show", L"Auto-hide"};
    int d = Segmented(ui, t, "set.dock", Rect(in.left, in.bottom - 26 - kNoteH - 6, Width(in), 26), kDock, 2, cfg.room);
    Note(c, t, in, in.bottom - kNoteH, cfg.room ? L"Slips away under windows" : L"Windows stop short of it");
    if (d >= 0) SetPref(cfg.room, L"Room", d);
}

void Bar::SetMods(const Theme& t, const D2D1_RECT_F& r) { // a chip per status module at the dock's end: lit ones show
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Status");
    static const wchar_t* const kNames[kModCount] = {L"Workspaces", L"App in front", L"Tray", L"Keyboard", L"Bluetooth", L"Network", L"Volume", L"Battery", L"Notifications", L"Quick settings", L"Clock", L"Show desktop"};
    float x = in.left, y = in.top + 2;
    constexpr float kChipH = 24, kChipGap = 6;
    int flip = -1;
    for (int m = kModTray; m < kModCount; ++m) { // workspaces and the app's title lived in the top bar
        float w = TextW(kNames[m], Weight::Medium, 11.5f) + 26;
        if (x + w > in.right) x = in.left, y += kChipH + kChipGap; // wraps onto the next row
        D2D1_RECT_F chip = Rect(x, y, w, kChipH);
        x += w + kChipGap;
        uint32_t id = Ui::Id("set.mod", m);
        Ui::Hit h = ui.Button(id, chip);
        float on = ui.Smooth(id + 10, Shows(m) ? 1.0f : 0, 16), hov = ui.Smooth(id + 20, h.hover ? 1.0f : 0, 16);
        c.Fill(chip, kChipH / 2, t.track.Alpha(0.55f).Mix(t.accent.Alpha(0.85f), on));
        if (hov > 0.01f) c.Fill(chip, kChipH / 2, t.text.Alpha(0.08f * hov));
        c.Text(kNames[m], chip, Weight::Medium, 11.5f, t.textDim.Mix(t.onAccent, on), Align::Center);
        if (h.clicked) flip = m;
    }
    if (flip >= 0) SetPref(cfg.mods, L"Mods", cfg.mods ^ (1 << flip));
}

void Bar::SetWidgets(const Theme& t, const D2D1_RECT_F& r) { // widgets are picked on the dock itself, in its edit mode
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Widgets");
    size_t n = std::count_if(wgSlots.begin(), wgSlots.end(), [&](const WgSlot& w) { return w.kind != kWgApps && !Many(w.kind); }); // the apps, dividers and spaces aren't widgets
    std::wstring line = std::to_wstring(n) + (n == 1 ? L" widget" : L" widgets") + L" in the dock  \u00b7  right click the dock to edit it any time";
    c.Text(line, Rect(in.left, in.top, Width(in) - 170, Height(in)), Weight::Regular, 11.5f, t.textDim);
    if (LabelButton(t, Ui::Id("set.edit"), Rect(in.right - 160, (in.top + in.bottom) / 2 - 15, 160, 30), glyph::Edit, L"Customize dock")) StartEdit();
}

void Bar::SetApp(const Theme& t, const D2D1_RECT_F& r) { // halobar itself: its version and updates, and stepping aside for windows' own taskbar
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"HaloBar");
    float cy = (in.top + in.bottom) / 2;
    float backW = TextW(L"Go back to Windows taskbar", Weight::Medium, 11.5f) + 48;
    D2D1_RECT_F upd = Rect(in.left, cy - 15, TextW(L"Check for updates", Weight::Medium, 11.5f) + 48, 30), back = Rect(in.right - backW, cy - 15, backW, 30); // both sized to their words: the note between them keeps room for "up to date"
    if (LabelButton(t, Ui::Id("set.update"), upd, glyph::Refresh, L"Check for updates")) updateNote = L"checking…", updater.Check(hwnd, WM_APP_UPDATE, true);
    std::wstring note = L"v" APP_VER_W + (updateNote.empty() ? std::wstring() : L"  ·  " + updateNote);
    c.Text(note, Rect(upd.right + 12, in.top, back.left - upd.right - 24, Height(in)), Weight::Regular, 11.5f, t.textDim);
    if (LabelButton(t, Ui::Id("set.windows"), back, glyph::Desktop, L"Go back to Windows taskbar")) { // halobar rests in the tray; its dashboard brings it back
        prefs::SaveNum(L"WindowsBar", 1);
        PostQuitMessage(3);
    }
}

void Bar::SetAdvanced(const Theme& t, const D2D1_RECT_F& head) { // island's advanced section: a bar, and under it a card per part, each only while that part is in use
    Canvas& c = ui.C();
    uint32_t id = Ui::Id("set.adv");
    float open = ui.Smooth(id + 2, advOpen ? 1.0f : 0, 11);
    Ui::Hit h = ui.Button(id, head);
    float hover = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(head, 16, CardFill(t).Mix(t.track, 0.4f * hover));
    if (theme::Contrast()) c.StrokeV(head, 16, t.hairTop, t.hairBot, 1);
    else if (open > 0.001f) c.Stroke(head, 16, t.accent.Alpha(0.6f * open), 1);
    D2D1_POINT_2F badge = {head.left + 30, (head.top + head.bottom) / 2};
    c.Dot(badge, 16, t.track.Mix(t.accent.Alpha(0.3f), open));
    c.Icon(glyph::Settings, badge, 19, t.textDim.Mix(t.accentHi, open), open > 0.5f);
    c.Text(L"Advanced", Rect(head.left + 56, head.top + 9, Width(head) - 110, 20), Weight::Medium, 13, t.text);
    c.Text(L"Accessibility, dock keys, the player, the clipboard, reports", Rect(head.left + 56, head.top + 29, Width(head) - 110, 16), Weight::Light, 10.5f, t.textDim);
    D2D1_POINT_2F o = {head.right - 26, (head.top + head.bottom) / 2}; // a chevron that turns over as it opens
    float turnA = open * kPi, s = 5, cs = std::cos(turnA), sn = std::sin(turnA);
    auto turn = [&](float x, float y) { return D2D1_POINT_2F{o.x + x * cs - y * sn, o.y + x * sn + y * cs}; };
    Color ink = t.textDim.Mix(t.text, hover);
    c.Line(turn(-s, -s * 0.5f), turn(0, s * 0.5f), ink, 2);
    c.Line(turn(0, s * 0.5f), turn(s, -s * 0.5f), ink, 2);
    if (h.clicked) advOpen = !advOpen, Redraw();
    if (open <= 0.001f) return;

    clip::Store& st = clip::History();
    clip::Options& o2 = st.opt; // the clipboard's own switches, here as well as in its panel
    bool player = cfg.song != 0, clipUsed = false;
    for (Bar* b : Docks()) player = player || b->HasWg(kWgMedia), clipUsed = clipUsed || b->HasWg(kWgClip);
    struct AdvLine {
        const char* id;
        const wchar_t* label;
        bool on;
    };
    struct AdvCard {
        const wchar_t* title;
        bool shown;
        AdvLine lines[3];
        int n;
        int sliders = 0; // slider rows under the switches
    };
    const AdvCard cards[] = {
        {L"Dock", true, {{"set.winkeys", L"Win + number opens apps", cfg.winKeys != 0}, {"set.adaptive", L"Full width over maximized", cfg.adaptive != 0}, {"set.trayapps", L"Tray apps in the dock", cfg.trayApps != 0}}, 3},
        {L"Player", player, {{"set.hoverplay", L"Open the player on hover", cfg.hoverPlay != 0}, {"set.artbg", L"Album art background", artBg}}, 2, 2}, // shared with astraeus, its two sliders too
        {L"Clipboard", clipUsed, {{"set.clipclean", L"Clean tracking from links", o2.clean != 0}, {"set.clipwipe", L"Clear clips on restart", o2.wipe != 0}, {"set.cliptitles", L"Fetch link titles online", o2.titles != 0}}, 3},
    };
    constexpr int kPlayerCard = 1;
    auto slider = [&](const D2D1_RECT_F& in, float sy, const char* id, const wchar_t* label, int Bar::*field, int lo, int hi, const wchar_t* key) { // a shared percent, live on every dock while dragged, saved on release
        int val = this->*field;
        c.Text(label, Rect(in.left, sy, Width(in) - 60, kAdvSlideLabelH), Weight::Regular, 11.5f, t.text);
        c.Text(std::to_wstring(val) + L"%", Rect(in.right - 60, sy - 1, 60, kAdvSlideLabelH + 2), Weight::Semi, 12, t.accentHi, Align::Right);
        if (open <= 0.99f) return; // unfolding: nothing to catch a stray drag
        D2D1_RECT_F row = Rect(in.left - 4, sy + kAdvSlideLabelH + 2, Width(in) + 8, kAdvSlideTrackH);
        float v = (float)(val - lo) / (hi - lo);
        w::SlideLook lk;
        lk.bubble = std::to_wstring(val) + L"%";
        lk.step = (float)kAdvSlideStep / (hi - lo);
        uint32_t sid = Ui::Id(id);
        if (Inside(row, input.mouse)) slideHot = true; // the wheel steps the slider, not the sheet
        bool save = false;
        if (w::Slide(ui, t, sid, row, v, lk)) {
            int nv = lo + (int)std::lround(v * (hi - lo) / kAdvSlideStep) * kAdvSlideStep;
            for (Bar* b : Docks()) b->*field = nv, b->Redraw();
            if (!ui.IsActive(sid)) save = true; // a wheel notch or a click: no release to wait for
        }
        if (ui.IsActive(sid) && input.released) save = true;
        if (save) prefs::SaveShared(key, this->*field); // astraeus hears it and follows
    };
    constexpr int kCards = sizeof(cards) / sizeof(cards[0]);
    float top = head.bottom + kCardGap, half = (Width(head) - kCardGap) / 2, y = top - (1 - open) * 24; // rises into place as it unfolds
    c.PushClip({head.left - 4, top - 4, head.right + 4, top + advBodyH * open});
    bool flip[kCards][3] = {};
    int col = 0;
    float rowH = 0;
    for (int i = 0; i < kCards; ++i) {
        const AdvCard& cd = cards[i];
        if (!cd.shown) continue; // that part isn't in the dock: nothing of it here
        float h2 = CardH(cd.n, 0) + cd.sliders * kAdvSlideRowH;
        D2D1_RECT_F in = Box(c, t, Rect(head.left + col * (half + kCardGap), y, half, h2), cd.title);
        for (int k = 0; k < cd.n; ++k) {
            bool f = false;
            if (open > 0.99f) Toggle(ui, t, cd.lines[k].id, Rect(in.left, in.top + k * kLineH, Width(in), kLineH), cd.lines[k].label, cd.lines[k].on, f);
            else c.Text(cd.lines[k].label, Rect(in.left, in.top + k * kLineH, Width(in) - 56, kLineH), Weight::Regular, 11.5f, t.text); // unfolding: the words only, nothing to catch a stray click
            flip[i][k] = f;
        }
        if (i == kPlayerCard) {
            float sy = in.top + cd.n * kLineH + 2;
            if (artBg) slider(in, sy, "set.vizalpha", L"Visualizer opacity", &Bar::vizAlpha, prefs::kVizAlphaMin, prefs::kVizAlphaMax, prefs::kVizAlpha);
            else c.Text(L"Visualizer opacity: album art off", Rect(in.left, sy, Width(in), kAdvSlideLabelH), Weight::Light, 10.5f, t.textDim); // it only sits over the art
            slider(in, sy + kAdvSlideRowH, "set.vizgain", L"Visualizer sensitivity", &Bar::vizGain, prefs::kVizGainMin, prefs::kVizGainMax, prefs::kVizGain);
        }
        rowH = (std::max)(rowH, h2);
        if (++col == 2) y += rowH + kCardGap, col = 0, rowH = 0;
    }
    if (col) y += rowH + kCardGap;
    ui.Block(open <= 0.99f); // unfolding: nothing to catch a stray click
    SetAccessCard(t, Rect(head.left, y, Width(head), kAccessH));
    y += kAccessH + kCardGap;
    SetReport(t, Rect(head.left, y, Width(head), kReportH));
    y += kReportH + kCardGap;
    ui.Block(false);
    c.PopClip();
    advBodyH = y - (top - (1 - open) * 24);
    if (flip[0][0]) SetPref(cfg.winKeys, L"WinKeys", cfg.winKeys ? 0 : 1), SyncKeyHook();
    if (flip[0][1]) SetPref(cfg.adaptive, L"Adaptive", cfg.adaptive ? 0 : 1), CheckCovered();
    if (flip[0][2]) SetPref(cfg.trayApps, L"TrayApps", cfg.trayApps ? 0 : 1);
    if (flip[1][0]) SetPref(cfg.hoverPlay, L"HoverPlay", cfg.hoverPlay ? 0 : 1);
    if (flip[1][1]) {
        bool on = !artBg;
        prefs::SaveShared(prefs::kMediaArtBg, on ? 1 : 0); // astraeus hears it and follows
        for (Bar* b : Docks()) b->SetArtBg(on); // every screen's player, without waiting on the watch
    }
    if (flip[2][0]) o2.clean ^= 1;
    if (flip[2][1]) o2.wipe ^= 1;
    if (flip[2][2]) o2.titles ^= 1;
    if (flip[2][0] || flip[2][1] || flip[2][2]) st.SaveOptions(), SyncClip(), Redraw();
}

void Bar::SetAccessCard(const Theme& t, const D2D1_RECT_F& r) { // for eyes that need more, and for hands and ears instead of the mouse
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Accessibility");
    float colW = (Width(in) - kColGap) / 2;
    D2D1_RECT_F left = {in.left, in.top, in.left + colW, in.bottom}, right = {in.right - colW, in.top, in.right, in.bottom};
    c.Text(L"High contrast", Rect(left.left, left.top, colW, 16), Weight::Regular, 11.5f, t.text);
    static const wchar_t* const kModes[] = {L"With Windows", L"On", L"Off"};
    int pick = Segmented(ui, t, "set.a11y.hc", Rect(left.left, left.top + 16, colW, 28), kModes, 3, cfg.a11yContrast);
    if (pick >= 0) SetAccess(&prefs::Settings::a11yContrast, L"A11yContrast", pick);
    float y = left.top + kAccessSeg;
    bool follow = false, keys = false, reader = false;
    Toggle(ui, t, "set.a11y.follow", Rect(left.left, y, colW, kLineH), L"Follow Windows text size", cfg.a11yFollow != 0, follow);
    y += kLineH;
    static int textDrag = -1; // shown while dragging, applied on release: the panel resizing under the mouse would fight the drag
    uint32_t tid = Ui::Id("set.a11y.text");
    int shown = textDrag >= 0 ? textDrag : cfg.a11yText;
    c.Text(L"Text size", Rect(left.left, y, colW - 60, kAdvSlideLabelH), Weight::Regular, 11.5f, t.text);
    c.Text(std::to_wstring(shown) + L"%", Rect(left.right - 60, y - 1, 60, kAdvSlideLabelH + 2), Weight::Semi, 12, t.accentHi, Align::Right);
    D2D1_RECT_F row = Rect(left.left - 4, y + kAdvSlideLabelH, colW + 8, kAdvSlideTrackH);
    float span = (float)(prefs::Settings::kTextMax - prefs::Settings::kTextMin), v = (shown - prefs::Settings::kTextMin) / span;
    w::SlideLook lk;
    lk.bubble = std::to_wstring(shown) + L"%";
    lk.step = kTextStep / span;
    if (Inside(row, input.mouse)) slideHot = true;
    bool held = ui.IsActive(tid), changed = w::Slide(ui, t, tid, row, v, lk);
    if (changed) textDrag = prefs::Settings::kTextMin + (int)std::lround(v * span / kTextStep) * kTextStep;
    if (textDrag >= 0 && ((changed && !ui.IsActive(tid)) || (held && input.released))) { // a release, a wheel notch or the keyboard
        SetAccess(&prefs::Settings::a11yText, L"A11yText", textDrag);
        textDrag = -1;
    }
    y = right.top;
    Toggle(ui, t, "set.a11y.keys", Rect(right.left, y, colW, kLineH), L"Keyboard navigation", cfg.a11yKeys != 0, keys);
    y += kLineH;
    Toggle(ui, t, "set.a11y.reader", Rect(right.left, y, colW, kLineH), L"Screen reader support", cfg.a11yReader != 0, reader);
    y += kLineH + 4;
    Note(c, t, right, y, L"Win + T, then S opens settings");
    Note(c, t, right, y + kNoteH, L"Tab moves, Enter presses");
    Note(c, t, right, y + 2 * kNoteH, L"Arrows adjust, Esc steps back");
    Note(c, t, right, y + 3 * kNoteH, L"For Narrator, NVDA and JAWS");
    if (follow) SetAccess(&prefs::Settings::a11yFollow, L"A11yFollowText", cfg.a11yFollow ? 0 : 1);
    if (keys) SetAccess(&prefs::Settings::a11yKeys, L"A11yKeys", cfg.a11yKeys ? 0 : 1);
    if (reader) SetAccess(&prefs::Settings::a11yReader, L"A11yReader", cfg.a11yReader ? 0 : 1);
}

void Bar::SetReport(const Theme& t, const D2D1_RECT_F& r) { // a report the user reads and shares themselves: nothing here touches the network
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Report a problem");
    float bw = (Width(in) - 8) / 2;
    static double copiedAt = -10;
    static bool copyOk = false;
    bool told = ui.Now() - copiedAt < kCopiedSec;
    if (told) ui.Wake(); // it has to turn back with nothing else going on
    const wchar_t* label = !told ? L"Copy report" : copyOk ? L"Copied" : L"Couldn't copy";
    if (LabelButton(t, Ui::Id("set.report"), Rect(in.left, in.top + 2, bw, 30), glyph::Copy, label)) {
        copyOk = sys::CopyText(sys::ProblemReport());
        copiedAt = ui.Now();
    }
    if (LabelButton(t, Ui::Id("set.report.open"), Rect(in.left + bw + 8, in.top + 2, bw, 30), glyph::OpenNew, L"Open GitHub")) {
        SetSettingsOpen(false);
        sys::OpenUri(kIssues);
    }
    Note(c, t, in, in.top + 38, L"Version, PC specs, main settings and recent problems,");
    Note(c, t, in, in.top + 38 + kNoteH, L"with your name taken out. Nothing is sent:");
    Note(c, t, in, in.top + 38 + 2 * kNoteH, L"read it, then paste it where you choose.");
}

void Bar::SetStart(const Theme& t, const D2D1_RECT_F& r) { // the start button: its face on the left, what it opens on the right, each with a note: kStartH
    Canvas& c = ui.C();
    D2D1_RECT_F box = Box(c, t, r, L"Start button");
    float colW = (Width(box) - kColGap) / 2;
    D2D1_RECT_F in = {box.left, box.top, box.left + colW, box.bottom}, right = {box.right - colW, box.top, box.right, box.bottom};
    static const wchar_t* const kFaces[] = {L"", L""}; // the logos themselves, drawn over the cells
    D2D1_RECT_F seg = Rect(in.left, in.top + (kLineH - kSegRow + 6) / 2, Width(in), kSegRow - 6); // centred on the switch line beside it
    int f = Segmented(ui, t, "set.startface", seg, kFaces, 2, cfg.startIcon);
    for (int i = 0; i < 2; ++i) {
        D2D1_POINT_2F o = {seg.left + Width(seg) * (i + 0.5f) / 2, (seg.top + seg.bottom) / 2};
        Color ink = i == cfg.startIcon ? t.onAccent : t.textDim;
        StartMark(o, i == 1 ? 11.0f : 13.0f, i, ink, ink); // the halo logo carries its own margin: drawn a touch smaller
    }
    bool b = false;
    Toggle(ui, t, "set.launch", Rect(right.left, right.top, Width(right), kLineH), L"Opens the launcher", cfg.launcher != 0, b); // off: the logo opens windows' own start
    Note(c, t, right, right.top + kLineH, L"Win key too. Off: Windows' own start");
    if (f >= 0) SetPref(cfg.startIcon, L"StartIcon", f), haloLogo.Reset();
    if (b) SetPref(cfg.launcher, L"Launcher", cfg.launcher ? 0 : 1), SyncKeyHook();
}

void Bar::SetScreens(const Theme& t, const D2D1_RECT_F& r) { // island's screens: your monitors as they stand, the dock on one or on all
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Screens");
    std::vector<HMONITOR> all = Screens();
    HMONITOR main = MainScreen();
    std::vector<RECT> rcs;
    RECT box = {0, 0, 16, 10};
    for (HMONITOR m : all) { // everything, to scale
        MONITORINFO mi = {sizeof(mi)};
        GetMonitorInfoW(m, &mi);
        if (rcs.empty()) box = mi.rcMonitor;
        UnionRect(&box, &box, &mi.rcMonitor);
        rcs.push_back(mi.rcMonitor);
    }
    D2D1_RECT_F map = {in.left + 4, in.top + 2, in.right - 4, in.bottom - 26};
    float bw = (float)(std::max)(1L, box.right - box.left), bh = (float)(std::max)(1L, box.bottom - box.top);
    float k = (std::min)(Width(map) / bw, Height(map) / bh);
    float ox = (map.left + map.right - bw * k) / 2, oy = (map.top + map.bottom - bh * k) / 2;
    float every = ui.Smooth(Ui::Id("set.screens", 99), cfg.screens ? 1.0f : 0, 14);
    int pick = -1, mainAt = 0;
    for (size_t i = 0; i < rcs.size(); ++i) {
        const RECT& s = rcs[i];
        D2D1_RECT_F q = {ox + (s.left - box.left) * k + 2, oy + (s.top - box.top) * k + 2, ox + (s.right - box.left) * k - 2, oy + (s.bottom - box.top) * k - 2};
        uint32_t id = Ui::Id("set.screen", (int)i);
        Ui::Hit h = ui.Button(id, q);
        if (all[i] == main) mainAt = (int)i;
        float on = (std::max)(every, ui.Smooth(id + 10, all[i] == main ? 1.0f : 0, 14));
        c.FillV(q, 6, t.track.Alpha(0.9f).Mix(t.accent.Alpha(0.35f), on), t.track.Alpha(0.5f).Mix(t.accent.Alpha(0.18f), on));
        c.Stroke(q, 6, t.accent.Alpha(on).Mix(t.text.Alpha(0.5f), h.hover ? 0.5f * (1 - on) : 0), on > 0.5f ? 1.5f : 1);
        c.Fill(MiniDock(q, cfg.edge, cfg.align, 3), 1.5f, t.accentHi.Alpha(on)); // the dock, where it stands on that screen
        c.Text(std::to_wstring(i + 1), q, Weight::Medium, (std::min)(14.0f, Height(q) * 0.45f), t.text.Alpha(0.5f + 0.5f * on), Align::Center);
        if (h.clicked && (cfg.screens || all[i] != main)) pick = (int)i; // every screen: tapping one keeps the dock to it alone, as island's
    }
    const wchar_t* line = cfg.screens ? L"Every screen" : L"One screen";
    D2D1_RECT_F pillR = {in.right - 44, in.bottom - 18, in.right, in.bottom}; // every screen: a dock on each, as windows' own taskbar
    c.Text(line, Rect(in.left, in.bottom - 18, Width(in) - 50, 18), Weight::Medium, 11, t.textDim);
    Ui::Hit ph = ui.Button(Ui::Id("set.screens.all"), pillR);
    c.Fill(pillR, 9, t.track.Alpha(0.8f).Mix(t.accent, every));
    c.Text(L"All", pillR, Weight::Medium, 10.5f, t.textDim.Mix(t.onAccent, every).Mix(t.text, ph.hover ? 0.4f * (1 - every) : 0), Align::Center);
    if (ph.clicked) pick = cfg.screens ? mainAt : -2; // off again: the main screen alone
    if (pick == -1) return;
    if (pick == -2) { // made afresh: each screen's dock starts clean, windows' taskbar stays hidden through it
        prefs::SaveNum(L"Screens", 1);
        PostQuitMessage(2);
        return;
    }
    MONITORINFOEXW mi = {};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(all[(size_t)pick], &mi)) return;
    sys::SaveText(L"DockScreen", mi.szDevice);
    prefs::SaveNum(L"Screens", 0);
    PostQuitMessage(2);
}

void Bar::SetPlace(const Theme& t, const D2D1_RECT_F& shell) { // island's position picker: a little monitor, the dock lit where it sits
    Canvas& c = ui.C();
    D2D1_RECT_F r = Box(c, t, shell, L"Position");
    struct Spot {
        int edge, align; // align only along the bottom: center 0, left 1, right 2
        const wchar_t* name;
    };
    static const Spot kSpots[] = {{0, 1, L"Bottom, from the left"}, {0, 0, L"Bottom, centered"}, {0, 2, L"Bottom, from the right"}, {1, 0, L"Left edge"}, {2, 0, L"Right edge"}};
    constexpr float kStand = 8, kCaption = 14, kRatio = 0.6f; // the monitor's stand and the line under it; a 16:10ish screen
    float mh = (std::min)(Height(r) - kStand - kCaption - 10, (Width(r) - 16) * kRatio), mw = mh / kRatio; // room left and right for the sides' hit areas
    D2D1_RECT_F scr = Rect((r.left + r.right - mw) / 2, r.top + 4, mw, mh);
    Monitor(c, t, scr);
    int cur = cfg.edge == 1 ? 3 : cfg.edge == 2 ? 4 : cfg.align == 1 ? 0 : cfg.align == 2 ? 2 : 1;
    float low = scr.bottom - mh * 0.34f, third = mw / 3; // the bottom's three spots below this line, the sides above it
    int hovered = -1, picked = -1;
    for (int i = 0; i < 5; ++i) {
        D2D1_RECT_F hot = i < 3 ? D2D1_RECT_F{scr.left + third * i - (i == 0 ? 8 : 0), low, scr.left + third * (i + 1) + (i == 2 ? 8 : 0), scr.bottom + 4}
                        : i == 3 ? D2D1_RECT_F{scr.left - 8, scr.top - 4, scr.left + mw * 0.3f, low}
                                 : D2D1_RECT_F{scr.right - mw * 0.3f, scr.top - 4, scr.right + 8, low}; // a little past the rim: easy to hit
        uint32_t id = Ui::Id("set.place", i);
        Ui::Hit h = ui.Button(id, hot);
        float on = ui.Smooth(id + 10, i == cur ? 1.0f : 0, 14), hov = ui.Smooth(id + 20, h.hover && i != cur ? 1.0f : 0, 16);
        if (h.hover) hovered = i;
        if (h.clicked && i != cur) picked = i;
        Color col = t.accent.Alpha(0.25f + 0.75f * on).Mix(t.text.Alpha(0.55f), hov * (1 - on));
        float d = 3.5f + 4.5f * on + 1.5f * hov; // the lit spot stands out as a little dock
        float len = (i < 3 ? mw * 0.28f : mh * 0.46f) + 4 * hov;
        D2D1_RECT_F bar = i == 0 ? Rect(scr.left + 5, scr.bottom - 4 - d, len, d)
                        : i == 1 ? Rect((scr.left + scr.right - len) / 2, scr.bottom - 4 - d, len, d)
                        : i == 2 ? Rect(scr.right - 5 - len, scr.bottom - 4 - d, len, d)
                        : i == 3 ? Rect(scr.left + 4, scr.top + mh * 0.40f - len / 2, d, len)
                                 : Rect(scr.right - 4 - d, scr.top + mh * 0.40f - len / 2, d, len); // the sides' column centered a touch high: clear of the bottom's spots
        if (i != 1 && i < 3 && on > 0.01f) c.Fill(Rect(scr.left + 5, bar.top, mw - 10, d), d / 2, t.accent.Alpha(0.22f * on)); // to one side, the dock runs the screen's width
        if (on > 0.01f) c.Glow({(bar.left + bar.right) / 2, (bar.top + bar.bottom) / 2}, Width(bar) * 0.7f + 6, Height(bar) * 0.7f + 6, t.accent.Alpha(0.30f * on));
        c.Fill(bar, d / 2, col);
        for (int k = -1; k <= 1 && on > 0.5f; ++k) { // three app dots: it reads as a dock
            D2D1_POINT_2F p = i < 3 ? D2D1_POINT_2F{(bar.left + bar.right) / 2 + k * 6.0f, (bar.top + bar.bottom) / 2} : D2D1_POINT_2F{(bar.left + bar.right) / 2, (bar.top + bar.bottom) / 2 + k * 6.0f};
            c.Dot(p, 1.3f, t.onAccent.Alpha((on - 0.5f) * 2));
        }
    }
    int shown = hovered >= 0 ? hovered : cur; // hovering names the spot before it's picked
    c.Text(kSpots[shown].name, Rect(r.left, r.bottom - kCaption, Width(r), kCaption), Weight::Medium, 11, hovered >= 0 && hovered != cur ? t.text : t.textDim, Align::Center);
    if (picked < 0) return;
    const Spot& s = kSpots[picked];
    if (s.edge == 0 && s.align != cfg.align) SetPref(cfg.align, L"Align", s.align), rowW = 0; // jumps to its new width, no slow stretch across the screen
    SetEdge(s.edge); // straight there, in place: windows' taskbar never shows in between
}

void Bar::SetSong(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Bar");
    bool a = false, p = false;
    Toggle(ui, t, "set.song", Rect(in.left, in.top, Width(in), kLineH), L"Song in the bar", cfg.song != 0, a);
    Toggle(ui, t, "set.pills", Rect(in.left, in.top + kLineH, Width(in), kLineH), L"Alert pills", cfg.pills != 0, p); // off: windows' own banners
    Note(c, t, in, in.top + 2 * kLineH, L"Pills off: Windows' own banners");
    if (a) SetPref(cfg.song, L"Song", cfg.song ? 0 : 1);
    if (p) SetPref(cfg.pills, L"Pills", cfg.pills ? 0 : 1);
}

void Bar::SetClock(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"Clock");
    bool h24 = cfg.clock24 == prefs::Settings::kFollow ? SystemClock24() : cfg.clock24 == 1; // following windows: show what it shows
    bool a = false, b = false;
    Toggle(ui, t, "set.date", Rect(in.left, in.top, Width(in), kLineH), L"Show date", cfg.date != 0, a);
    Toggle(ui, t, "set.24h", Rect(in.left, in.top + kLineH, Width(in), kLineH), L"24-hour", h24, b);
    bool follow = cfg.clock24 == prefs::Settings::kFollow;
    Note(c, t, in, in.top + 2 * kLineH, follow ? L"Follows Windows until you flip it" : L"Set here, whatever Windows uses");
    if (a) SetPref(cfg.date, L"Date", cfg.date ? 0 : 1);
    if (b) SetPref(cfg.clock24, L"Clock24", h24 ? 0 : 1); // the first flip leaves "follow" for an explicit choice
}

void Bar::BeginHex(int which, int hex) {
    if (hexFor == which) return;
    if (hexFor >= 0) EndHex(true);
    HWND front = GetForegroundWindow();
    hexBack = front != hwnd ? front : nullptr;
    hexFor = which;
    hexEdit = HexText(hex);
    SetForegroundWindow(hwnd); // typing needs the keyboard; it goes back when the field closes
    SetFocus(hwnd);
    Redraw();
}

void Bar::EndHex(bool keep) {
    if (hexFor < 0) return;
    int which = hexFor, rgb = 0;
    hexFor = -1;
    if (keep && ParseHex(hexEdit, rgb)) StoreWheel(which, rgb, true);
    if (hexBack && IsWindow(hexBack) && GetForegroundWindow() == hwnd) SetForegroundWindow(hexBack);
    hexBack = nullptr;
    Redraw();
}

void Bar::HexChar(wchar_t ch) {
    constexpr wchar_t kPaste = 0x16; // ctrl+v, as a character
    constexpr size_t kHexLen = 6;
    int rgb = 0;
    if (ch == VK_RETURN) return EndHex(true);
    if (ch == VK_ESCAPE) return EndHex(false);
    if (ch == VK_BACK) {
        if (!hexEdit.empty()) hexEdit.pop_back();
    } else if (ch == kPaste) {
        if (ParseHex(ClipNow(), rgb)) hexEdit = HexText(rgb);
    } else if (iswxdigit(ch) && hexEdit.size() < kHexLen) {
        hexEdit += (wchar_t)towupper(ch);
    }
    Redraw();
}

void Bar::StoreWheel(int which, int hex, bool done) { // live while dragging, saved on release
    WheelColor(which) = hex & 0xFFFFFF;
    if (which == 0) cfg.accent = 1; // the wheel's accent is the bar's own
    SyncLook();
    Redraw();
    if (!done) return;
    if (which == 0) prefs::SaveNum(L"AccentHex", cfg.accentHex), prefs::SaveNum(L"Accent", 1);
    else prefs::SaveNum(L"CustomBg", cfg.customBg);
}

void Bar::SetWheel(const Theme& t, D2D1_POINT_2F o, float R) {
    Canvas& c = ui.C();
    Hsv sel = HandleOf(wheelPick, WheelColor(wheelPick));
    ID2D1RenderTarget* rt = c.Target();
    c.Glow(o, R * 1.4f, R * 1.4f, t.accent.Alpha(0.18f));
    if (ID2D1Bitmap* bmp = WheelBmp(rt)) rt->DrawBitmap(bmp, {o.x - R, o.y - R, o.x + R, o.y + R}, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (sel.v < 0.999f) c.Dot(o, R, Color{0, 0, 0, 1 - sel.v}); // the wheel at the chosen handle's brightness
    c.Ring(o, R, t.hairTop, 1);
    uint32_t id = Ui::Id("set.wheel");
    ui.Button(id, {o.x - R - 8, o.y - R - 8, o.x + R + 8, o.y + R + 8}); // a little past the rim, so handles at full saturation stay grabbable
    auto spot = [&](int which) {
        Hsv x = HandleOf(which, WheelColor(which));
        float a = (x.h - 90) * kPi / 180;
        return D2D1_POINT_2F{o.x + std::cos(a) * x.s * R, o.y + std::sin(a) * x.s * R};
    };
    D2D1_POINT_2F ms = input.mouse;
    if (ui.IsActive(id) && input.pressed) // grab the handle under the press, or move the selected one here
        for (int w = 0; w < 2; ++w) {
            D2D1_POINT_2F q = spot(w);
            if (std::hypot(ms.x - q.x, ms.y - q.y) < 12) wheelPick = w;
        }
    if (ui.IsActive(id) && (input.down || input.released)) {
        float dx = ms.x - o.x, dy = ms.y - o.y;
        Hsv x = HandleOf(wheelPick, WheelColor(wheelPick));
        x.h = std::fmod(std::atan2(dy, dx) * 180 / kPi + 90 + 360, 360.0f);
        x.s = (std::min)(1.0f, std::hypot(dx, dy) / R); // dragging past the rim pins it to full saturation
        int hex = ToHex(FromHsv(x));
        handles[wheelPick] = x, handleFor[wheelPick] = hex;
        StoreWheel(wheelPick, hex, input.released);
    }
    for (int k = 0; k < 2; ++k) { // the other handle first, so the one being edited sits on top
        int w = k == 0 ? 1 - wheelPick : wheelPick;
        D2D1_POINT_2F q = spot(w);
        float big = w == wheelPick ? 2.0f : 0.0f;
        c.Dot({q.x, q.y + 1.5f}, 9 + big, Color{0, 0, 0, 0.35f}); // lifts off the wheel
        c.Dot(q, 9 + big, Color{1, 1, 1, 1});
        c.Dot(q, 6.5f + big, Color::Rgb((uint32_t)(WheelColor(w) & 0xFFFFFF)));
    }
}

void Bar::SetShade(const Theme& t, const D2D1_RECT_F& r) { // brightness of the selected handle: black to its full color
    Hsv x = HandleOf(wheelPick, WheelColor(wheelPick));
    uint32_t id = Ui::Id("set.shade");
    float v = x.v;
    D2D1_RECT_F row = {r.left - 14, (r.top + r.bottom) / 2 - kSlideH / 2, r.right + 14, (r.top + r.bottom) / 2 + kSlideH / 2}; // the thin ramp, with a roomy grip
    w::SlideLook lk;
    lk.bubble = std::to_wstring((int)std::lround(x.v * 100)) + L"%";
    lk.ramp = true, lk.lo = Color{0, 0, 0, 1}, lk.hi = FromHsv({x.h, x.s, 1}); // the full range this handle's hue can take
    if (Inside(row, input.mouse)) slideHot = true;
    if (w::Slide(ui, t, id, row, v, lk)) {
        x.v = v;
        int hex = ToHex(FromHsv(x));
        handles[wheelPick] = x, handleFor[wheelPick] = hex;
        StoreWheel(wheelPick, hex, !ui.IsActive(id)); // a wheel notch is kept at once
    }
    if (ui.IsActive(id) && input.released) StoreWheel(wheelPick, WheelColor(wheelPick), true); // persist what the drag left
}

void Bar::SetChip(const Theme& t, const D2D1_RECT_F& r, int which, const wchar_t* name) { // a handle's color: tap to edit that one on the wheel
    Canvas& c = ui.C();
    uint32_t id = Ui::Id("set.chip", which);
    Ui::Hit h = ui.Button(id, r);
    bool on = wheelPick == which, typing = hexFor == which;
    float rad = Height(r) / 2;
    c.Fill(r, rad, on ? t.cardHi.Mix(t.track, 0.6f) : t.cardHi.Alpha(0.6f));
    c.Stroke(r, rad, on || typing ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, on ? 1.5f : 1);
    D2D1_POINT_2F dot = {r.left + rad, (r.top + r.bottom) / 2};
    c.Dot(dot, rad - 5, Color::Rgb((uint32_t)(WheelColor(which) & 0xFFFFFF)));
    c.Ring(dot, rad - 5, t.hairTop, 1);
    D2D1_RECT_F label = {dot.x + rad - 1, r.top, r.right - 6, r.bottom};
    std::wstring hex = L"#" + (typing ? hexEdit : HexText(WheelColor(which)));
    c.Text(typing || on ? hex : std::wstring(name), label, Weight::Medium, 11, typing || on ? t.text : t.textDim);
    if (typing) {
        if (Caret()) {
            float cx = label.left + TextW(hex, Weight::Medium, 11) + 1;
            c.Line({cx, dot.y - 7}, {cx, dot.y + 7}, t.text, 1.5f);
        }
        if (input.pressed && !Inside(r, input.mouse)) EndHex(true); // clicked away: kept if it's a color
    }
    if (h.clicked) {
        if (!on) wheelPick = which; // first tap picks the handle
        else if (!typing) BeginHex(which, WheelColor(which)); // second tap types its hex
    }
}

void setui::FreeWheel() { wheelBmp.Reset(), wheelOwner.Reset(); }
