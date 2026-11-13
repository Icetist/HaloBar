// the settings hints: a small see-through pill by the mouse, a sketch of what the hovered switch does. the same idea as astraeus' settings previews
#include "hints.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "glyphs.h"

namespace hints {
namespace {

constexpr float kDelay = 0.35f; // seconds hovered before it shows: passing over doesn't flash it
constexpr float kW = 236, kH = 112, kPad = 12, kOff = 18; // the pill, its inner margin, how far it sits from the mouse
constexpr float kFade = 14; // how quickly it comes and goes
constexpr float kSketchAt = 0.5f; // the sketch shows once the pill is this far in: never a faint card with a full drawing on it

int cur = kNone, shown = kNone; // under the mouse now; the last one drawn, kept while it fades
double since = 0;
bool seen = false; // some control claimed the mouse this frame

const Color kInk{0.95f, 0.95f, 0.97f, 1}, kNight{0.06f, 0.06f, 0.08f, 0.92f}, kPaper{0.94f, 0.94f, 0.96f, 1};
const Color kCoverA{1.0f, 0.42f, 0.62f, 1}, kCoverB{0.42f, 0.32f, 0.86f, 1};

struct Sketch { // the drawing tools, inside the pill
    Canvas& c;
    const Theme& t;
    D2D1_RECT_F in;
    float cx() const { return (in.left + in.right) / 2; }
    float cy() const { return (in.top + in.bottom) / 2; }
    Color Dim() const { return t.text.Alpha(0.35f); }
    void Line(float x, float y, float w, Color col, float h = 6) { c.Fill({x, y, x + w, y + h}, h / 2, col); } // a line of words, as a bar
    void Cover(const D2D1_RECT_F& r) { c.FillV(r, (std::min)(r.right - r.left, r.bottom - r.top) * 0.2f, kCoverA, kCoverB); }
    void Words(const std::wstring& s, const D2D1_RECT_F& r, float size, Color col, Align a = Align::Left, Weight w = Weight::Medium) { c.Text(s, r, w, size, col, a); }
    D2D1_RECT_F Dock(float w, float y, Color fill, int apps = 4, int lit = -1) { // the dock along the bottom, with a few apps
        D2D1_RECT_F d = {cx() - w / 2, y, cx() + w / 2, y + 26};
        c.Fill(d, 13, fill);
        for (int i = 0; i < apps; ++i) {
            D2D1_POINT_2F o = {d.left + 18 + i * 22.0f, (d.top + d.bottom) / 2};
            c.Fill({o.x - 7, o.y - 7, o.x + 7, o.y + 7}, 4, i == lit ? t.accent : kInk.Alpha(0.55f));
        }
        return d;
    }
    void Key(const wchar_t* k, float x, float y, float w) { // a key on the keyboard
        D2D1_RECT_F r = {x, y, x + w, y + 26};
        c.Fill({r.left, r.top + 3, r.right, r.bottom + 3}, 7, Color{0, 0, 0, 0.25f});
        c.Fill(r, 7, t.cardHi);
        c.Stroke(r, 7, t.hairTop, 1);
        Words(k, r, 11, t.text, Align::Center);
    }
    void Cards(bool fadeRest) { // a few clips, the first one pinned
        for (int i = 0; i < 3; ++i) {
            D2D1_RECT_F card = {in.left + 14 + i * 64.0f, in.top + 18, in.left + 72 + i * 64.0f, in.bottom - 18};
            float a = fadeRest && i > 0 ? 0.35f : 1.0f;
            c.Fill(card, 10, t.cardHi.Alpha(a));
            Line(card.left + 8, card.top + 12, 34, t.text.Alpha(0.6f * a));
            Line(card.left + 8, card.top + 22, 22, t.text.Alpha(0.3f * a));
        }
    }
};

void PaintDock(Sketch& s, int kind) {
    Canvas& c = s.c;
    const Theme& t = s.t;
    D2D1_RECT_F in = s.in;
    float cx = s.cx(), base = in.bottom - 30;
    switch (kind) {
    case kAppsTheme: { // one look on the desktop, another over your apps
        D2D1_RECT_F a = {in.left + 6, in.top + 26, cx - 6, in.top + 54}, b = {cx + 6, in.top + 26, in.right - 6, in.top + 54};
        c.Fill(a, 14, kPaper);
        c.Fill(b, 14, kNight);
        s.Words(L"Desktop", {a.left, a.bottom + 6, a.right, a.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        s.Words(L"Over apps", {b.left, b.bottom + 6, b.right, b.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        break;
    }
    case kWinKeys: { // win and a number opens that app
        s.Key(L"Win", cx - 52, in.top + 4, 46);
        s.Key(L"2", cx + 2, in.top + 4, 30);
        s.Dock(110, base, kNight, 4, 1);
        break;
    }
    case kAdaptive: { // a maximized window above: the dock spans the screen under it
        c.Fill({in.left + 4, in.top + 2, in.right - 4, base - 6}, 6, t.cardHi);
        c.Fill({in.left + 4, base, in.right - 4, base + 26}, 4, kNight);
        for (int i = 0; i < 4; ++i) c.Fill({in.left + 18 + i * 22.0f, base + 6, in.left + 32 + i * 22.0f, base + 20}, 4, kInk.Alpha(0.55f));
        break;
    }
    case kTrayApps: { // the tray's apps sit in the dock, after the others
        D2D1_RECT_F d = s.Dock(196, base, kNight, 4);
        for (int i = 0; i < 3; ++i) c.Dot({d.right - 46 + i * 14.0f, (d.top + d.bottom) / 2}, 4, t.accentHi);
        c.Line({d.right - 58, d.top + 6}, {d.right - 58, d.bottom - 6}, kInk.Alpha(0.25f), 1);
        break;
    }
    case kHoverPlay: { // the mouse over the song opens the player
        D2D1_RECT_F d = s.Dock(170, base, kNight, 0);
        s.Cover({d.left + 8, d.top + 5, d.left + 24, d.top + 21});
        s.Words(L"Afterglow", {d.left + 30, d.top, d.right, d.bottom}, 10.5f, kInk);
        D2D1_RECT_F p = {cx - 70, in.top + 2, cx + 70, base - 8};
        c.Fill(p, 12, t.cardHi);
        s.Cover({p.left + 8, p.top + 8, p.left + 40, p.top + 40});
        s.Line(p.left + 48, p.top + 14, 60, t.text.Alpha(0.7f));
        break;
    }
    case kArtBg: { // the player sits on its cover's colours
        D2D1_RECT_F card = {in.left + 4, in.top + 4, in.right - 4, in.bottom - 4};
        c.FillH(card, 12, kCoverA.Alpha(0.75f), kCoverB.Alpha(0.85f));
        c.Fill(card, 12, Color{0, 0, 0, 0.25f});
        s.Cover({card.left + 12, card.top + 14, card.left + 66, card.top + 68});
        s.Line(card.left + 78, card.top + 26, 80, kInk.Alpha(0.85f));
        s.Line(card.left + 78, card.top + 38, 56, kInk.Alpha(0.5f));
        break;
    }
    case kLauncher: { // the start button opens the launcher
        D2D1_RECT_F p = {cx - 80, in.top + 2, cx + 80, base - 8};
        c.Fill(p, 12, t.cardHi);
        c.Fill({p.left + 10, p.top + 10, p.right - 10, p.top + 30}, 10, t.track);
        c.Icon(glyph::Search, {p.left + 22, p.top + 20}, 12, s.Dim());
        D2D1_RECT_F d = s.Dock(110, base, kNight, 3);
        c.Icon(glyph::Star, {d.left - 16, (d.top + d.bottom) / 2}, 15, t.accentHi, true);
        break;
    }
    case kPills: { // a pill rises from the dock with the news
        D2D1_RECT_F p = {cx - 80, in.top + 10, cx + 80, in.top + 40};
        c.Fill(p, 15, kNight);
        c.Icon(glyph::Bolt, {p.left + 18, (p.top + p.bottom) / 2}, 14, Color{0.45f, 0.9f, 0.55f, 1}, true);
        s.Words(L"Charging  ·  64%", {p.left + 32, p.top, p.right - 8, p.bottom}, 11, kInk);
        s.Dock(110, base, kNight, 3);
        break;
    }
    case kDate: case k24h: { // the dock's clock
        D2D1_RECT_F d = {cx - 58, in.top + 18, cx + 58, in.bottom - 18};
        c.Fill(d, 14, kNight);
        bool date = kind == kDate;
        s.Words(date ? L"10:42" : L"22:42", {d.left, d.top + (date ? 8.0f : 0), d.right, date ? d.top + 30 : d.bottom}, 15, kInk, Align::Center, Weight::Semi);
        if (date) s.Words(L"Thu 12 Nov", {d.left, d.top + 30, d.right, d.bottom - 6}, 10.5f, kInk.Alpha(0.6f), Align::Center);
        break;
    }
    case kSplit: { // a different set of widgets over your apps
        D2D1_RECT_F a = {in.left + 6, in.top + 24, cx - 6, in.top + 50}, b = {cx + 6, in.top + 24, in.right - 6, in.top + 50};
        c.Fill(a, 13, kNight);
        c.Fill(b, 13, kNight);
        c.Icon(glyph::ClearDay, {a.left + 18, (a.top + a.bottom) / 2}, 13, kInk);
        c.Icon(glyph::Calendar, {a.left + 40, (a.top + a.bottom) / 2}, 13, kInk);
        c.Icon(glyph::Music, {b.left + 18, (b.top + b.bottom) / 2}, 13, kInk);
        s.Words(L"Desktop", {a.left, a.bottom + 6, a.right, a.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        s.Words(L"Over apps", {b.left, b.bottom + 6, b.right, b.bottom + 22}, 10.5f, t.text.Alpha(0.55f), Align::Center);
        break;
    }
    case kSong: { // what's playing, at the start of the dock
        D2D1_RECT_F d = s.Dock(190, s.cy() - 13, kNight, 0);
        s.Cover({d.left + 8, d.top + 5, d.left + 24, d.top + 21});
        s.Words(L"Afterglow", {d.left + 30, d.top, d.left + 110, d.bottom}, 10.5f, kInk);
        for (int i = 0; i < 3; ++i) c.Fill({d.left + 120 + i * 20.0f, d.top + 6, d.left + 134 + i * 20.0f, d.top + 20}, 4, kInk.Alpha(0.55f));
        break;
    }
    case kDockLyrics: { // the sung line takes the song's place
        D2D1_RECT_F d = s.Dock(190, s.cy() - 13, kNight, 0);
        s.Cover({d.left + 8, d.top + 5, d.left + 24, d.top + 21});
        s.Words(L"every word, in time", {d.left + 30, d.top, d.right - 8, d.bottom}, 10.5f, kInk);
        s.Words(L"Afterglow", {d.left, d.bottom + 6, d.right, d.bottom + 22}, 10, t.text.Alpha(0.4f), Align::Center);
        break;
    }
    default:
        break;
    }
}

void PaintClip(Sketch& s, int kind) {
    Canvas& c = s.c;
    const Theme& t = s.t;
    D2D1_RECT_F in = s.in;
    float cy = s.cy();
    D2D1_RECT_F row = {in.left + 6, cy - 20, in.right - 6, cy + 20}; // one clip, as a row
    switch (kind) {
    case kClipClean: { // a link keeps what you meant, not who sent you
        c.Fill(row, 10, t.cardHi);
        s.Words(L"shop.com/item", {row.left + 10, row.top, row.right, row.bottom}, 11, t.text);
        s.Words(L"?utm_source=…", {row.left + 98, row.top, row.right, row.bottom}, 11, t.text.Alpha(0.3f));
        c.Line({row.left + 98, cy}, {row.right - 12, cy}, t.accentHi, 1.5f);
        break;
    }
    case kClipWipe: // a restart clears what isn't pinned
        s.Cards(true);
        s.Words(L"Pinned stays", {in.left, in.bottom - 16, in.right, in.bottom}, 10.5f, t.accentHi, Align::Center);
        break;
    case kClipTitles: { // a link shows its page's title
        c.Fill(row, 10, t.cardHi);
        s.Words(L"How light bends", {row.left + 10, row.top + 3, row.right, row.top + 21}, 11.5f, t.text, Align::Left, Weight::Semi);
        s.Words(L"example.com/optics", {row.left + 10, row.top + 20, row.right, row.bottom - 2}, 10, s.Dim());
        break;
    }
    case kClipPause: // nothing new is kept
        s.Cards(false);
        c.Dot({s.cx(), cy}, 18, kNight);
        c.Icon(glyph::Pause, {s.cx(), cy}, 16, kInk, true);
        break;
    case kClipPass: case kClipAdult: { // copied from a password manager or a site it skips: not kept
        c.Fill(row, 10, t.cardHi.Alpha(0.5f));
        s.Words(kind == kClipPass ? L"• • • • • • • •" : L"a site it skips", {row.left + 12, row.top, row.right, row.bottom}, 12, t.text.Alpha(0.5f));
        c.Icon(glyph::Lock, {row.right - 18, cy}, 14, t.accentHi);
        s.Words(L"Not kept", {in.left, row.bottom + 4, in.right, row.bottom + 20}, 10.5f, t.accentHi, Align::Center);
        break;
    }
    case kClipPaste: { // a pick goes straight into the app you came from
        D2D1_RECT_F card = {in.left + 6, cy - 22, in.left + 80, cy + 22}, field = {in.right - 110, cy - 14, in.right - 6, cy + 14};
        c.Fill(card, 10, t.cardHi);
        s.Line(card.left + 8, card.top + 14, 44, t.text.Alpha(0.6f));
        c.Line({card.right + 10, cy}, {field.left - 10, cy}, t.accentHi, 2);
        c.Line({field.left - 16, cy - 5}, {field.left - 10, cy}, t.accentHi, 2);
        c.Line({field.left - 16, cy + 5}, {field.left - 10, cy}, t.accentHi, 2);
        c.Stroke(field, 6, t.text.Alpha(0.4f), 1);
        s.Words(L"Hello", {field.left + 8, field.top, field.right, field.bottom}, 11, t.text);
        break;
    }
    case kClipPlain: { // pasted as plain words, without their styling
        s.Words(L"Big, bold, red", {in.left + 6, in.top + 10, in.right, in.top + 34}, 15, Color{0.95f, 0.35f, 0.35f, 1}, Align::Left, Weight::Semi);
        s.Words(L"↓", {in.left + 6, in.top + 34, in.left + 30, in.top + 54}, 13, s.Dim());
        s.Words(L"Big, bold, red", {in.left + 6, in.top + 54, in.right, in.top + 76}, 12, t.text, Align::Left, Weight::Regular);
        break;
    }
    default:
        break;
    }
}

void PaintOther(Sketch& s, int kind) {
    Canvas& c = s.c;
    const Theme& t = s.t;
    D2D1_RECT_F in = s.in;
    float cx = s.cx(), cy = s.cy();
    switch (kind) {
    case kA11yFollow: // the text grows with windows' own setting
        s.Words(L"Aa", {in.left + 24, cy - 14, cx - 10, cy + 14}, 14, s.Dim(), Align::Center);
        s.Words(L"→", {cx - 12, cy - 12, cx + 12, cy + 12}, 14, s.Dim(), Align::Center);
        s.Words(L"Aa", {cx + 10, cy - 22, in.right - 24, cy + 22}, 26, t.text, Align::Center);
        break;
    case kA11yKeys: // a focus ring walks the controls
        for (int i = 0; i < 4; ++i) {
            D2D1_RECT_F k = {in.left + 14 + i * 52.0f, cy - 18, in.left + 56 + i * 52.0f, cy + 18};
            c.Fill(k, 10, t.cardHi);
            if (i == 1) c.Stroke({k.left - 3, k.top - 3, k.right + 3, k.bottom + 3}, 12, t.accent, 2);
        }
        s.Words(L"Tab", {in.left, in.bottom - 18, in.right, in.bottom}, 10.5f, s.Dim(), Align::Center);
        break;
    case kA11yReader: // a screen reader names what's there
        c.Icon(glyph::VolumeUp, {in.left + 26, cy}, 20, t.accentHi, true);
        s.Words(L"“Start, button”", {in.left + 46, cy - 12, in.right, cy + 12}, 13, t.text);
        break;
    case kTourNext: { // the first-run tour
        D2D1_RECT_F card = {in.left + 20, in.top + 10, in.right - 20, in.bottom - 10};
        c.Fill(card, 12, t.cardHi);
        s.Words(L"Welcome to HaloBar", {card.left + 12, card.top + 8, card.right, card.top + 26}, 12, t.text, Align::Left, Weight::Semi);
        s.Line(card.left + 12, card.top + 34, 120, s.Dim());
        for (int i = 0; i < 5; ++i) c.Dot({card.left + 16 + i * 10.0f, card.bottom - 14}, 2.5f, i == 0 ? t.accent : t.text.Alpha(0.18f));
        s.Words(L"Next", {card.right - 50, card.bottom - 24, card.right - 10, card.bottom - 4}, 11, t.accentHi, Align::Right);
        break;
    }
    default:
        break;
    }
}

void Paint(Sketch& s, int kind) {
    if (kind < kClipClean) PaintDock(s, kind);
    else if (kind < kA11yFollow) PaintClip(s, kind);
    else PaintOther(s, kind);
}

}

void Want(Ui& ui, int kind, bool hover) {
    if (!hover || kind == kNone) return;
    seen = true;
    if (kind != cur) cur = kind, since = ui.Now();
}

int ForId(const char* id) {
    static const struct { const char* id; int kind; } kMap[] = { // the settings card and the clipboard's own options share a picture
        {"set.appstheme", kAppsTheme}, {"set.winkeys", kWinKeys}, {"set.adaptive", kAdaptive}, {"set.trayapps", kTrayApps}, {"set.hoverplay", kHoverPlay},
        {"set.artbg", kArtBg}, {"set.launch", kLauncher}, {"set.pills", kPills}, {"set.date", kDate}, {"set.24h", k24h}, {"gal.split", kSplit},
        {"gal.song", kSong}, {"set.docklyrics", kDockLyrics}, {"set.clipclean", kClipClean}, {"clip.o.clean", kClipClean}, {"set.clipwipe", kClipWipe}, {"clip.o.wipe", kClipWipe},
        {"set.cliptitles", kClipTitles}, {"clip.o.titles", kClipTitles}, {"clip.o.pause", kClipPause}, {"clip.o.pass", kClipPass},
        {"clip.o.adult", kClipAdult}, {"clip.o.paste", kClipPaste}, {"clip.o.plain", kClipPlain}, {"set.a11y.follow", kA11yFollow},
        {"set.a11y.keys", kA11yKeys}, {"set.a11y.reader", kA11yReader}, {"set.tour.next", kTourNext},
    };
    for (const auto& e : kMap)
        if (!std::strcmp(e.id, id)) return e.kind;
    return kNone;
}

void Draw(Ui& ui, const Theme& t, const D2D1_RECT_F& bounds) {
    if (!seen) cur = kNone; // nothing under the mouse said so: it goes
    seen = false;
    bool want = cur != kNone && ui.Now() - since >= kDelay;
    if (want) shown = cur;
    if (cur != kNone && !want) ui.KeepAlive(); // waiting out the delay: frames keep coming so it can appear
    float a = ui.Smooth(Ui::Id("hints.fade"), want ? 1.0f : 0, kFade);
    if (a < 0.01f || shown == kNone) return;
    Canvas& c = ui.C();
    D2D1_POINT_2F m = ui.In().mouse;
    float x = m.x + kOff, y = m.y - kOff - kH; // above and right of the pointer: the dock is below; turned over where it would leave the panel
    if (x + kW > bounds.right - 6) x = m.x - kOff - kW;
    if (y < bounds.top + 6) y = m.y + kOff;
    x = (std::max)(x, bounds.left + 6), y = (std::clamp)(y, bounds.top + 6, (std::max)(bounds.top + 6, bounds.bottom - kH - 6));
    float lift = (1 - a) * 6;
    D2D1_RECT_F r = {x, y + lift, x + kW, y + lift + kH};
    float rad = kH / 2.4f;
    c.Fill({r.left, r.top + 8, r.right, r.bottom + 10}, rad, Color{0, 0, 0, 0.22f * a});
    c.Fill(r, rad, t.card.Alpha(0.88f * a)); // see-through, so the settings under it still show
    c.Stroke(r, rad, t.hairTop.Alpha(t.hairTop.a * a), 1);
    if (a >= kSketchAt) {
        c.PushClip(r);
        Sketch s{c, t, {r.left + kPad + 6, r.top + kPad, r.right - kPad - 6, r.bottom - kPad}};
        Paint(s, shown);
        c.PopClip();
    }
    if (a < 0.995f) ui.KeepAlive(); // it fades and lifts in; once there it holds still, so idle frames stop
}

}
