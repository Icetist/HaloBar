// Settings, grown out of the island like the player: island's instruments, a color orb and small cards you touch
// instead of rows of options. Every choice can also just follow astraeus island.
#include "bar.h"

#include <shellapi.h>

#include <cmath>

#include "glyphs.h"
#include "version.h"

namespace {

constexpr float kPi = 3.14159265f;
constexpr float kHeroW = 210; // the orb's column
constexpr float kPad = 16, kCardGap = 10; // the sheet's margin, and the space between cards
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

ID2D1Bitmap* WheelBmp(ID2D1RenderTarget* rt) { // hue around, saturation outward, at full brightness; made once per device
    static ComPtr<ID2D1Bitmap> bmp;
    static ComPtr<ID2D1Device> owner; // held, so a new device can't reuse its address
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

std::wstring HexText(int hex) {
    wchar_t b[8];
    swprintf_s(b, L"%06X", hex & 0xFFFFFF);
    return b;
}

bool SystemClock24() { // what windows' own clock does, for "follow"
    wchar_t v[4] = {};
    return GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_ITIME, v, 4) > 0 && v[0] == L'1'; // "1" means 24-hour
}

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title) { // island's card shell, returns its inner area
    c.Fill(r, 16, t.cardHi.Alpha(0.55f));
    c.StrokeV(r, 16, t.hairTop, t.hairBot, 1);
    c.TextSp(title, Rect(r.left + 14, r.top + 11, Width(r) - 28, 12), Weight::Medium, 9.5f, t.textLow, 1.4f); // small spaced caps, island's card label
    return {r.left + 12, r.top + 30, r.right - 12, r.bottom - 10};
}

int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur) {
    Canvas& c = ui.C();
    float seg = Width(r) / n, rad = Height(r) / 2;
    c.Fill(r, rad, t.track.Alpha(0.7f));
    float x = ui.Smooth(Ui::Id(id, 99), r.left + seg * cur, 18); // the thumb slides between options
    c.FillV({x + 3, r.top + 3, x + seg - 3, r.bottom - 3}, rad - 3, t.accentHi, t.accent);
    int picked = -1;
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F cell = {r.left + seg * i, r.top, r.left + seg * (i + 1), r.bottom};
        Ui::Hit h = ui.Button(Ui::Id(id, i), cell);
        if (h.clicked && i != cur) picked = i; // the current one again changes nothing
        c.Text(labels[i], cell, Weight::Medium, 11, i == cur ? t.onAccent : t.textDim.Mix(t.text, h.hover ? 0.6f : 0), Align::Center);
    }
    return picked;
}

void Monitor(Canvas& c, const Theme& t, const D2D1_RECT_F& s) { // a little screen on a stand, as island's
    float mx = (s.left + s.right) / 2;
    c.Fill(Rect(mx - 6, s.bottom, 12, 5), 2, t.track);
    c.Fill(Rect(mx - 15, s.bottom + 5, 30, 3), 1.5f, t.track);
    c.FillV(s, 7, t.track.Alpha(0.9f), t.track.Alpha(0.5f));
    c.Stroke(s, 7, t.hairTop, 1);
}

void Toggle(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& row, const wchar_t* label, bool on, bool& flip) { // a label and its switch
    ui.C().Text(label, Rect(row.left + 2, row.top, Width(row) - 56, Height(row)), Weight::Medium, 12, t.textDim.Mix(t.text, on ? 0.7f : 0));
    if (w::Switch(ui, t, Ui::Id(id), {row.right - 25, (row.top + row.bottom) / 2}, on)) flip = true; // reported, so the caller saves it after drawing
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
    c.TextSp(L"SETTINGS", Rect(gx + 4, area.top + 4, 200, 14), Weight::Medium, 10, t.textLow, 1.4f);
    if (w::IconBtn(ui, t, Ui::Id("set.close"), {area.right - 12, area.top + 10}, 28, glyph::Close, 17)) SetSettingsOpen(false);

    D2D1_RECT_F grid = {gx, area.top + 30, area.right, area.bottom};
    float cw = (Width(grid) - 2 * kCardGap) / 3, ch = (Height(grid) - kCardGap) / 2;
    SetMaterial(t, Rect(grid.left, grid.top, 2 * cw + kCardGap, ch)); // materials need room for five swatches: two cells wide
    SetSize(t, Rect(grid.left + 2 * (cw + kCardGap), grid.top, cw, ch));
    float y1 = grid.top + ch + kCardGap;
    SetRoom(t, Rect(grid.left, y1, cw, ch));
    SetSong(t, Rect(grid.left + cw + kCardGap, y1, cw, ch));
    SetClock(t, Rect(grid.left + 2 * (cw + kCardGap), y1, cw, ch));
    if (ui.RClick(pill) && Inside({x0, y0, x0 + w, rowTop}, input.mouse)) SetSettingsOpen(false); // right click closes, as island's sheets
    rt->PopLayer();
}

void Bar::SetHero(const Theme& t, const D2D1_RECT_F& r) { // the accent: a hue ring around a glowing orb, where it comes from, island's presets
    Canvas& c = ui.C();
    float hx = (r.left + r.right) / 2;
    c.TextSp(std::wstring(L"HALOBAR  ·  ") + APP_VER_W, Rect(r.left, r.top + 2, Width(r), 12), Weight::Medium, 9.5f, t.textLow, 1.4f, Align::Center);

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
        if (ui.IsActive(id) && input.pressed && std::fabs(std::hypot(ps.x - o.x, ps.y - o.y) - R) < ringW + 8) ringDrag = true; // grabbed the ring
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
        c.Dot({o.x - R * 0.22f, o.y - R * 0.26f}, R * 0.30f, Color{1, 1, 1, 0.16f}); // specular highlight, a glassy orb
        D2D1_POINT_2F k = {o.x + R * std::cos(Rad(cur.hue - 90)), o.y + R * std::sin(Rad(cur.hue - 90))}; // the knob sits at the current hue
        c.Dot(k, ringW * 0.75f + 3, t.text); // a rim in the text color, so it reads on any hue
        c.Dot(k, ringW * 0.75f, theme::Hsl(cur.hue, 0.80f, 0.60f));
        static const wchar_t* const kNames[] = {L"Island", L"Wallpaper", L"Custom"};
        int skip = islandHere ? 0 : 1; // without island there's nothing to follow: wallpaper and custom only
        int src = (std::max)(cfg.accent + 1, skip); // -1 follow island, 0 wallpaper, 1 custom; following a missing island looks like wallpaper
        c.Text(kNames[src], {o.x - R, o.y - 9, o.x + R, o.y + 9}, Weight::Medium, 11.5f, t.onAccent, Align::Center);

        float segW = 64.0f * (3 - skip);
        int v = Segmented(ui, t, "set.accent", Rect(hx - segW / 2, r.top + 146, segW, 24), kNames + skip, 3 - skip, src - skip);
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

void Bar::SetMaterial(const Theme& t, const D2D1_RECT_F& r) { // swatches drawn in the actual materials
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"MATERIAL");
    bool ownGlass = cfg.look == 2; // only the bar's own glass gets a blur slider; island's glass is island's to tune
    Seed s = theme::FromCol(t.accent); // every swatch is tinted with the live accent
    int first = islandHere ? 0 : 1, n = 5 - first; // without island, no swatch to follow it
    float sw = (Width(in) - (n - 1) * 10) / n, sh = Height(in) - (ownGlass ? 42 : 20); // glass keeps its names: the blur slider goes under them
    int chosen = cfg.look < 0 && !islandHere ? look.look : cfg.look; // following a missing island: its defaults, shown on their own swatch
    static const wchar_t* const kNames[] = {L"Island", L"Dark", L"Light", L"Glass", L"Custom"};
    for (int i = first; i < 5; ++i) {
        int pick = i - 1; // -1 follow island
        D2D1_RECT_F q = Rect(in.left + (i - first) * (sw + 10), in.top + 2, sw, sh);
        uint32_t id = Ui::Id("set.mat", i);
        Ui::Hit h = ui.Button(id, q);
        float on = ui.Smooth(id + 10, chosen == pick ? 1.0f : 0, 14), hov = ui.Smooth(id + 20, h.hover ? 1.0f : 0, 16); // offset ids: separate animation state per swatch
        int shown = pick < 0 ? look.look : pick; // island's swatch wears whatever island wears
        Theme th = Theme::From(Seed{s.hue, s.chroma, shown, s.rgb, pick == 3 ? cfg.customBg : look.customBg});
        if (shown == 2) { // glass: color under frost, a light streak, a bright rim
            ComPtr<ID2D1RoundedRectangleGeometry> round;
            if (SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry(D2D1::RoundedRect(q, 10, 10), &round))) {
                c.Target()->PushLayer(D2D1::LayerParameters(q, round.Get()), nullptr); // clip the blobs and streak to the swatch's corners
                c.Dot({q.left + sw * 0.3f, q.top + sh * 0.7f}, sh * 0.6f, th.accent.Alpha(0.65f));
                c.Dot({q.right - sw * 0.25f, q.top + sh * 0.25f}, sh * 0.5f, theme::Hsl(s.hue + 60, 0.7f, 0.6f, 0.55f));
                c.FillV(q, 10, Color{1, 1, 1, 0.22f}, Color{1, 1, 1, 0.06f});
                c.Line({q.left + sw * 0.15f, q.bottom + 6}, {q.left + sw * 0.75f, q.top - 6}, Color{1, 1, 1, 0.22f}, 8);
                c.Target()->PopLayer();
            }
            c.Stroke(q, 10, Color{1, 1, 1, 0.40f}, 1.2f);
        } else {
            c.FillV(q, 10, th.cardHi, th.bg.Alpha(1)); // opaque: the swatch shows the material, not what's behind the bar
            float pw = sw * 0.62f, ph = (std::min)(14.0f, sh * 0.3f); // a tiny island inside, in that material
            D2D1_RECT_F mini = Rect(q.left + (sw - pw) / 2, q.bottom - ph - 6, pw, ph);
            c.Fill(mini, ph / 2, th.bg.Alpha(1).Mix(th.text, 0.06f));
            c.Dot({mini.left + ph / 2 + 2, (mini.top + mini.bottom) / 2}, ph * 0.22f, th.accent);
            c.StrokeV(q, 10, th.hairTop, th.hairBot, 1);
        }
        if (on > 0.01f || hov > 0.01f) c.Stroke(Inset(q, -3, -3), 12, t.accent.Alpha((std::max)(on, 0.5f * hov)), 2); // a halo outside the swatch, hover at half strength
        c.Text(kNames[i], Rect(q.left, q.bottom + 3, sw, 14), Weight::Medium, 11, t.textDim.Mix(t.text, on), Align::Center);
        if (h.clicked && pick == 3 && cfg.accent != 1) { // the wheel starts from the accent you're looking at
            cfg.accentHex = ToHex(t.accent), cfg.accent = 1;
            prefs::SaveNum(L"AccentHex", cfg.accentHex), prefs::SaveNum(L"Accent", 1);
        }
        if (h.clicked) SetPref(cfg.look, L"Look", pick);
    }
    if (!ownGlass) return; // the rest is the blur slider
    float v = cfg.blur / 100.0f; // how frosted: clear glass to windows' full backdrop blur
    uint32_t id = Ui::Id("set.blur");
    D2D1_RECT_F bar = {in.left + 44, in.bottom - 16, in.right - 50, in.bottom - 4};
    c.TextSp(L"BLUR", Rect(in.left, bar.top, 40, 12), Weight::Medium, 9.5f, t.textLow, 1.6f);
    c.Text(std::to_wstring(cfg.blur) + L"%", Rect(in.right - 46, bar.top - 3, 46, 16), Weight::Semi, 12, t.accentHi, Align::Right);
    D2D1_RECT_F knob = Inset(bar, 6, -6); // a taller hit area than the thin rail
    if (ui.Slider(id, knob, v, false)) cfg.blur = (int)(v * 100 + 0.5f), Redraw(); // live
    if (ui.IsActive(id) && input.released) prefs::SaveNum(L"GlassBlur", cfg.blur); // saved once, on release
    float rad = Height(bar) / 2, cy = (bar.top + bar.bottom) / 2, x = knob.left + Width(knob) * (cfg.blur / 100.0f);
    c.FillH(bar, rad, Color{1, 1, 1, 0.04f}, Color{1, 1, 1, 0.55f}); // clear to frosted, left to right
    c.Stroke(bar, rad, t.hairTop, 1);
    c.FillH({bar.left, bar.top, x + rad, bar.bottom}, rad, t.accent.Alpha(0.35f), t.accentHi.Alpha(0.55f));
    c.Dot({x, cy}, rad + 3, Color{1, 1, 1, 1});
    c.Ring({x, cy}, rad + 3, t.accent, 2);
}

void Bar::SetSize(const Theme& t, const D2D1_RECT_F& r) { // three little islands, small to large
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"SIZE");
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

void Bar::SetRoom(const Theme& t, const D2D1_RECT_F& r) { // a maximized window on a tiny screen: above the island, or under it
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"WINDOWS");
    Ui::Hit h = ui.Button(Ui::Id("set.room"), in);
    float under = ui.Smooth(Ui::Id("set.room", 1), cfg.room ? 1.0f : 0, 12);
    float mh = Height(in) - 26, mw = (std::min)(mh / 0.6f, Width(in) - 8); // room for the stand and the label; a 5:3 screen
    D2D1_RECT_F scr = Rect((in.left + in.right - mw) / 2, in.top, mw, mh);
    Monitor(c, t, scr);
    float strip = mh * 0.17f, ph = strip * 0.62f; // the bar's band at the bottom, and the island in it
    D2D1_RECT_F win = {scr.left + 3, scr.top + 3, scr.right - 3, scr.bottom - 3 - strip * (1 - under)}; // the window stops above, or runs under
    c.FillV(win, 4, t.cardHi.Mix(t.track, 0.4f), t.card);
    D2D1_RECT_F mini = Rect((scr.left + scr.right) / 2 - mw * 0.2f, scr.bottom - 3 - strip / 2 - ph / 2, mw * 0.4f, ph);
    c.Fill(mini, ph / 2, Color{0.02f, 0.02f, 0.03f, 0.95f});
    c.Dot({mini.left + ph / 2 + 1, (mini.top + mini.bottom) / 2}, ph * 0.22f, t.accent);
    if (h.hover) c.Stroke(Inset(in, -4, -4), 12, t.accent.Alpha(0.35f), 1.5f);
    c.Text(cfg.room ? L"Auto-hide" : L"Always show", Rect(in.left, in.bottom - 14, Width(in), 14), Weight::Medium, 11,
           cfg.room ? t.accentHi : t.textDim, Align::Center);
    if (h.clicked) SetPref(cfg.room, L"Room", cfg.room ? 0 : 1);
}

void Bar::SetSong(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"BAR");
    float rh = (Height(in) - 8) / 3;
    bool a = false, b = false;
    Toggle(ui, t, "set.song", Rect(in.left, in.top, Width(in), rh), L"Song in the bar", cfg.song != 0, a);
    Toggle(ui, t, "set.launch", Rect(in.left, in.top + rh + 4, Width(in), rh), L"Logo: launcher", cfg.launcher != 0, b); // off: the logo opens windows' own start
    if (a) SetPref(cfg.song, L"Song", cfg.song ? 0 : 1);
    bool p = false;
    Toggle(ui, t, "set.pills", Rect(in.left, in.top + 2 * (rh + 4), Width(in), rh), L"Alert pills", cfg.pills != 0, p); // off: windows' own banners
    if (p) SetPref(cfg.pills, L"Pills", cfg.pills ? 0 : 1);
    if (b) SetPref(cfg.launcher, L"Launcher", cfg.launcher ? 0 : 1);
}

void Bar::SetClock(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    D2D1_RECT_F in = Box(c, t, r, L"CLOCK");
    float rh = (Height(in) - 4) / 2;
    bool h24 = cfg.clock24 == prefs::Settings::kFollow ? SystemClock24() : cfg.clock24 == 1; // following windows: show what it shows
    bool a = false, b = false;
    Toggle(ui, t, "set.date", Rect(in.left, in.top, Width(in), rh), L"Show date", cfg.date != 0, a);
    Toggle(ui, t, "set.24h", Rect(in.left, in.top + rh + 4, Width(in), rh), L"24-hour", h24, b);
    if (a) SetPref(cfg.date, L"Date", cfg.date ? 0 : 1);
    if (b) SetPref(cfg.clock24, L"Clock24", h24 ? 0 : 1); // the first flip leaves "follow" for an explicit choice
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
    Canvas& c = ui.C();
    Hsv x = HandleOf(wheelPick, WheelColor(wheelPick));
    float rad = Height(r) / 2;
    c.FillH(r, rad, Color{0, 0, 0, 1}, FromHsv({x.h, x.s, 1})); // the full range this handle's hue can take
    c.Stroke(r, rad, t.hairTop, 1);
    uint32_t id = Ui::Id("set.shade");
    float v = x.v;
    D2D1_RECT_F rail = {r.left + rad, r.top - 4, r.right - rad, r.bottom + 4};
    if (ui.Slider(id, rail, v, false)) {
        x.v = v;
        int hex = ToHex(FromHsv(x));
        handles[wheelPick] = x, handleFor[wheelPick] = hex;
        StoreWheel(wheelPick, hex, false);
    }
    if (ui.IsActive(id) && input.released) StoreWheel(wheelPick, WheelColor(wheelPick), true); // persist what the drag left
    D2D1_POINT_2F k = {rail.left + Width(rail) * x.v, (r.top + r.bottom) / 2};
    c.Dot(k, rad + 2, Color{1, 1, 1, 1});
    c.Dot(k, rad - 1, FromHsv(x));
}

void Bar::SetChip(const Theme& t, const D2D1_RECT_F& r, int which, const wchar_t* name) { // a handle's color: tap to edit that one on the wheel
    Canvas& c = ui.C();
    uint32_t id = Ui::Id("set.chip", which);
    Ui::Hit h = ui.Button(id, r);
    bool on = wheelPick == which;
    float rad = Height(r) / 2;
    c.Fill(r, rad, on ? t.cardHi.Mix(t.track, 0.6f) : t.cardHi.Alpha(0.6f));
    c.Stroke(r, rad, on ? t.accent : h.hover ? t.text.Alpha(0.4f) : t.hairTop, on ? 1.5f : 1);
    D2D1_POINT_2F dot = {r.left + rad, (r.top + r.bottom) / 2};
    c.Dot(dot, rad - 5, Color::Rgb((uint32_t)(WheelColor(which) & 0xFFFFFF)));
    c.Ring(dot, rad - 5, t.hairTop, 1);
    c.Text(on ? L"#" + HexText(WheelColor(which)) : std::wstring(name), {dot.x + rad - 1, r.top, r.right - 6, r.bottom}, Weight::Medium, 11, on ? t.text : t.textDim);
    if (h.clicked) wheelPick = which;
}
