// The sound page's mixer: microphone, groups with their apps folded under them, apps on their own, the chat mix.
// Groups, ducking and profiles are one setup shared with astraeus; the main dock applies it unless astraeus already does.
#include "bar.h"

#include "glyphs.h"

namespace {

constexpr Color kNoTint{0, 0, 0, 0}; // no group color: the look's own accent (a plain Color is opaque black)

constexpr float kMixHead = 26, kMixRow = 50, kProfRow = 32, kChatRow = 40, kEditH = 292, kOptsH = 214; // the mixer's parts, top to bottom
constexpr float kChipH = 18, kMeterH = 2;
constexpr float kListHead = 52, kListRow = 40, kListFoot = 44; // as the sound page around it
constexpr int kListRows = 6;
constexpr UINT kFastMs = 100, kSlowMs = 1000; // meters and ducking want ten looks a second; groups alone, one
constexpr int kSlowEvery = kSlowMs / kFastMs;
constexpr int kMixKeyId = 0x4D00, kMixKeys = 12; // ctrl+alt: mic, next profile, chat mix both ways, eight groups
constexpr UINT kMixVk[kMixKeys] = {'M', 'P', VK_OEM_4, VK_OEM_6, '1', '2', '3', '4', '5', '6', '7', '8'};
constexpr int kGroupIcons[sys::Desk::kIcons] = {glyph::Pad, glyph::Chat, glyph::Music, glyph::Movie, glyph::Web, glyph::Bell, glyph::Work, glyph::Speaker};
constexpr size_t kNameMax = 24;
constexpr float kChatStep = 0.1f, kWheelStep = 0.05f;

sys::Desk& TheDesk() { // one per process: every screen's dock edits the same
    static sys::Desk d = sys::LoadDesk();
    return d;
}

sys::DeskEngine& TheEngine() {
    static sys::DeskEngine e;
    return e;
}

std::wstring Short(const std::wstring& s, size_t n) { return s.size() <= n ? s : s.substr(0, n - 1) + L"…"; }

std::wstring OutName(const std::vector<sys::OutDev>& outs, const std::wstring& id) {
    if (id.empty()) return L"Default";
    for (const auto& d : outs)
        if (d.id == id) return d.name;
    return L"Other";
}

std::wstring NextOut(const std::vector<sys::OutDev>& outs, const std::wstring& id) { // default, then each output, then default again
    if (outs.empty()) return {};
    if (id.empty()) return outs.front().id;
    for (size_t k = 0; k + 1 < outs.size(); ++k)
        if (outs[k].id == id) return outs[k + 1].id;
    return {};
}

bool Tiny(Ui& ui, const Theme& t, uint32_t id, const D2D1_RECT_F& r, const std::wstring& label, Color dot) { // a small pill on a row's name line
    Canvas& c = ui.C();
    Ui::Hit h = ui.Button(id, r);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
    c.Fill(r, kChipH / 2, t.text.Alpha(0.07f + 0.07f * hov));
    float x = r.left + 7;
    if (dot.a > 0) c.Dot({x + 3, (r.top + r.bottom) / 2}, 3.5f, dot), x += 9;
    c.Text(label, {x, r.top, r.right - 5, r.bottom}, Weight::Medium, 9.5f, t.textDim.Mix(t.text, hov), Align::Center);
    return h.clicked;
}

float ChipW(const std::wstring& s, bool dot) { return 16 + 5.6f * (float)s.size() + (dot ? 9 : 0); }

}

void Bar::DeskTick(float dt, bool fast) {
    sys::Desk& d = TheDesk();
    sys::DeskEngine& e = TheEngine();
    if (moon) {
        if (mixer.IsOpen()) fast ? mixer.Meter() : mixer.Refresh(); // a moon's page only shows; the main dock applies
        return;
    }
    if (!fast && sys::DeskChanged()) d = sys::LoadDesk(), e.Forget(); // astraeus edited it
    bool engine = !d.groups.empty() && e.Claim();
    bool want = (qsOpen && qsPage == 3) || engine;
    if (want && !mixer.IsOpen()) mixer.Open();
    else if (!want && mixer.IsOpen()) mixer.Close(), mixIcons.clear();
    if (!mixer.IsOpen()) return;
    if (!fast) mixer.Refresh();
    if (fast || e.NeedsMeter(d)) mixer.Meter();
    if (engine) e.Tick(mixer, d, dt);
    if (!fast && engine) sys::SaveDesk(d); // the engine learns of levels moved elsewhere; written only when something changed
}

void Bar::DeskSave() {
    sys::SaveDesk(TheDesk());
    Bar* m = moon && lead ? lead : this;
    if (TheEngine().Owner() && m->mixer.IsOpen()) TheEngine().Tick(m->mixer, TheDesk(), 0); // astraeus applies it otherwise, within a second
}

bool TheDeskIdle() { return TheDesk().groups.empty() || !TheEngine().Owner(); }

void Bar::SyncDesk() {
    sys::Desk& d = TheDesk();
    if (!moon && !mixTimerMs && sys::DeskChanged()) d = sys::LoadDesk(), TheEngine().Forget(); // no timer to notice astraeus' edits: the slow status tick does
    bool sound = qsOpen && qsPage == 3;
    bool engine = !moon && !d.groups.empty() && TheEngine().Claim();
    UINT ms = sound || (engine && TheEngine().NeedsMeter(d)) ? kFastMs : engine ? kSlowMs : 0;
    if (ms != mixTimerMs) {
        mixTimerMs = ms;
        if (ms) SetTimer(hwnd, kMixTimer, ms, nullptr);
        else KillTimer(hwnd, kMixTimer);
    }
    bool keys = engine && d.keys; // one app takes the keys: the one applying the mix
    if (keys == mixKeys) return;
    mixKeys = keys;
    for (int k = 0; k < kMixKeys; ++k)
        if (keys) RegisterHotKey(hwnd, kMixKeyId + k, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, kMixVk[k]); // a key another app holds is skipped
        else UnregisterHotKey(hwnd, kMixKeyId + k);
}

void Bar::DeskKey(int k) {
    sys::Desk& d = TheDesk();
    std::wstring say;
    int groups = (int)d.groups.size();
    if (k == 0 && mic && mic->Ready()) {
        mic->SetMuted(!mic->Muted());
        say = mic->Muted() ? L"Microphone muted" : L"Microphone on";
    } else if (k == 1 && !d.profiles.empty()) {
        d.Recall((d.profile + 1) % (int)d.profiles.size());
        say = d.profiles[d.profile].name;
    } else if ((k == 2 || k == 3) && d.Chatting()) {
        d.chatMix = (std::clamp)(d.chatMix + (k == 3 ? kChatStep : -kChatStep), -1.0f, 1.0f);
        int pct = (int)std::lround(std::fabs(d.chatMix) * 100);
        say = pct == 0 ? L"Chat mix even" : L"Chat mix " + std::to_wstring(pct) + L"% " + d.groups[d.chatMix > 0 ? d.game : d.chat].name;
    } else if (k >= 4 && k - 4 < groups) {
        auto& g = d.groups[k - 4];
        g.muted = !g.muted;
        say = g.name + (g.muted ? L" muted" : L" on");
    } else {
        return;
    }
    if (k != 0) DeskSave();
    if (!d.osd) return;
    pillNote = sys::Notif{};
    pillNote.app = L"Mixer";
    pillNote.title = say;
    pillUp = true;
    SetTimer(hwnd, kPillTimer, kPillMs, nullptr);
    Redraw();
}

void Bar::CommitMixName() {
    std::wstring s = mixNameEdit;
    while (!s.empty() && s.back() == L' ') s.pop_back();
    sys::Desk& d = TheDesk();
    if (mixName >= 0 && mixName < (int)d.groups.size() && !s.empty()) d.groups[mixName].name = s, DeskSave();
    mixName = -1;
}

float Bar::MixHeight() const {
    const sys::Desk& d = TheDesk();
    float want = kMixHead + (d.profiles.empty() || mixEdit >= 0 || mixOpts ? 0 : kProfRow);
    if (mixEdit >= 0) want += kEditH;
    else if (mixOpts) want += kOptsH;
    else {
        int rows = 1 + (int)d.groups.size(); // the microphone, each group
        for (const auto& a : mixer.Apps()) {
            int g = d.GroupOf(a.key);
            rows += g < 0 || (g < (int)mixUnfold.size() && mixUnfold[g]) ? 1 : 0;
        }
        want += rows * kMixRow + (d.Chatting() ? kChatRow : 0);
    }
    float room = kRoom - kRowH - 40 - (kListHead + (float)(std::min)(outs.size(), (size_t)kListRows) * kListRow + kListFoot + 8);
    return (std::max)(kMixHead + kMixRow, (std::min)(want, room)); // past the room, the rows scroll
}

bool Bar::MixLine(const Theme& t, const D2D1_RECT_F& row, uint32_t id, const sys::MixApp* app, int glyphId, Color tint, const std::wstring& name, float& v, bool muted, float peak,
                  bool& tap) {
    Canvas& c = ui.C();
    float cy = row.top + kMixRow / 2;
    D2D1_RECT_F ic = Rect(row.left + 2, cy - 12, 24, 24);
    ComPtr<ID2D1Bitmap>* bmp = nullptr;
    if (app) {
        bmp = &mixIcons[app->key]; // made once per app, kept while the page shows
        if (!*bmp && app->icon && ctx) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ctx->CreateBitmap({32, 32}, app->icon->data(), 32 * 4, bp, bmp->GetAddressOf()); // the mixer reads every icon at 32 px, premultiplied
        }
    }
    if (bmp && *bmp) c.Image(bmp->Get(), ic, 0, muted ? 0.4f : 1);
    else {
        if (tint.a > 0) c.Dot({ic.left + 12, cy}, 13, tint.Alpha(0.22f));
        c.Icon(glyphId, {ic.left + 12, cy}, 18, tint.a > 0 ? tint : t.textDim, tint.a > 0);
    }
    c.Text(name, Rect(row.left + 36, row.top + 2, Width(row) - 36 - 150, 15), Weight::Medium, 11.5f, t.text);
    D2D1_RECT_F pill = Rect(row.left + 36, row.top + 19, Width(row) - 36, 26);
    bool moved = w::Pill(ui, t, id, pill, glyphId == glyph::Mic ? (muted ? glyph::MicOff : glyph::Mic) : VolumeGlyph(v, muted), v, &tap, muted);
    float lit = ui.Env(id + 40, muted ? 0 : peak); // the live meter, a hairline under the slider
    if (lit > 0.01f) c.Fill({pill.left + 14, pill.bottom + 2, pill.left + 14 + (Width(pill) - 28) * lit, pill.bottom + 2 + kMeterH}, 1, (tint.a > 0 ? tint : t.accent).Alpha(0.85f));
    return moved;
}

float Bar::MixList(const Theme& t, const D2D1_RECT_F& r, float y) {
    Canvas& c = ui.C();
    sys::Desk& d = TheDesk();
    const auto& apps = mixer.Apps();
    { // the microphone; its name switches the input
        std::wstring name = L"Microphone";
        for (const auto& m : mics)
            if (m.id == micId) name = m.name;
        float v = mic && mic->Ready() ? mic->Level() : 0;
        bool muted = mic && mic->Ready() && mic->Muted(), tap = false;
        D2D1_RECT_F row = Rect(r.left, y, Width(r), kMixRow);
        if (MixLine(t, row, Ui::Id("mx.mic"), nullptr, glyph::Mic, kNoTint, name, v, muted, 0, tap) && mic) mic->SetLevel(v);
        if (tap && mic) mic->SetMuted(!muted);
        if (mics.size() > 1 && Tiny(ui, t, Ui::Id("mx.mic.next"), Rect(row.right - 82, row.top + 1, 82, kChipH), L"Switch input", kNoTint)) {
            size_t cur = 0;
            for (size_t i = 0; i < mics.size(); ++i)
                if (mics[i].id == micId) cur = i;
            const auto& next = mics[(cur + 1) % mics.size()];
            if (sys::SetIn(next.id)) micId = next.id, mic->Refresh();
        }
        y += kMixRow;
    }
    auto appRow = [&](const sys::MixApp& a, int g, bool member) {
        D2D1_RECT_F row = Rect(r.left + (member ? 14 : 0), y, Width(r) - (member ? 14 : 0), kMixRow);
        uint32_t id = Ui::Id("mx.app", (int)std::hash<std::wstring>{}(a.key));
        auto b = d.base.find(a.key);
        float v = member && b != d.base.end() ? b->second : a.volume; // in a group the slider is the app's own share
        bool tap = false;
        if (MixLine(t, row, id, &a, a.key == L"system" ? glyph::Bell : glyph::Speaker, kNoTint, a.name, v, a.muted, a.peak, tap)) {
            if (member) d.base[a.key] = v, DeskSave();
            else mixer.SetVolume(a.key, v);
            if (a.muted) mixer.SetMuted(a.key, false); // moving a muted slider brings it back
        }
        if (tap) mixer.SetMuted(a.key, !a.muted);
        float x = row.right;
        bool routable = a.key != L"system" && sys::Mixer::CanRoute() && !outs.empty() && (g < 0 || d.groups[g].device.empty());
        if (routable) {
            std::wstring lab = Short(OutName(outs, a.route), 10);
            float cw = ChipW(lab, false);
            x -= cw;
            if (Tiny(ui, t, id + 50, Rect(x, row.top + 1, cw, kChipH), lab, kNoTint)) mixer.Route(a.key, NextOut(outs, a.route));
            x -= 4;
        }
        if (a.key != L"system" && !d.groups.empty()) { // nothing to join until a group exists
            std::wstring lab = g >= 0 ? Short(d.groups[g].name, 9) : std::wstring(L"Add to group");
            float cw = ChipW(lab, g >= 0);
            if (Tiny(ui, t, id + 52, Rect(x - cw, row.top + 1, cw, kChipH), lab, g >= 0 ? Color::Rgb(d.groups[g].color) : kNoTint)) {
                int next = g + 1 < (int)d.groups.size() ? g + 1 : -1; // each group in turn, then none
                d.Assign(a.key, next);
                if (next >= 0) d.base[a.key] = a.volume;
                DeskSave();
            }
        }
        y += kMixRow;
    };
    for (int g = 0; g < (int)d.groups.size(); ++g) {
        auto& grp = d.groups[g];
        float peak = 0;
        int n = 0;
        for (const auto& a : apps)
            if (d.GroupOf(a.key) == g) peak = (std::max)(peak, a.peak), ++n;
        D2D1_RECT_F row = Rect(r.left, y, Width(r), kMixRow);
        uint32_t id = Ui::Id("mx.group", g);
        float v = grp.volume;
        bool tap = false;
        if (MixLine(t, row, id, nullptr, kGroupIcons[grp.icon % sys::Desk::kIcons], Color::Rgb(grp.color), grp.name, v, grp.muted, peak, tap)) grp.volume = v, d.profile = -1, DeskSave();
        if (tap) grp.muted = !grp.muted, d.profile = -1, DeskSave();
        bool open = mixUnfold[g] != 0;
        std::wstring count = std::to_wstring(n) + (n == 1 ? L" app " : L" apps ") + (open ? L"‹" : L"›");
        if (Tiny(ui, t, id + 50, Rect(row.right - 40 - ChipW(count, false), row.top + 1, ChipW(count, false), kChipH), count, kNoTint)) mixUnfold[g] = !open;
        if (Tiny(ui, t, id + 52, Rect(row.right - 36, row.top + 1, 36, kChipH), L"Edit", kNoTint)) mixEdit = g, mixOpts = false;
        y += kMixRow;
        if (open)
            for (const auto& a : apps)
                if (d.GroupOf(a.key) == g) appRow(a, g, true);
    }
    for (const auto& a : apps)
        if (d.GroupOf(a.key) < 0) appRow(a, -1, false);
    if (apps.empty()) c.Text(L"Apps show up here once they make a sound", Rect(r.left, y + 10, Width(r), 18), Weight::Light, 11.5f, t.textLow, Align::Center), y += kMixRow;
    return y;
}

void Bar::MixEditor(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    sys::Desk& d = TheDesk();
    int g = mixEdit;
    sys::DeskGroup& grp = d.groups[g];
    float x = r.left, y = r.top + 2, w = Width(r);
    D2D1_RECT_F field = Rect(x, y, w, 30); // the name: click to type
    bool typing = mixName == g;
    c.Fill(field, 15, t.text.Alpha(typing ? 0.10f : 0.06f));
    c.Stroke(field, 15, typing ? t.accent.Alpha(0.7f) : t.text.Alpha(0.08f), 1.2f);
    std::wstring shown = typing ? mixNameEdit : grp.name;
    c.Text(shown, Rect(field.left + 14, field.top, w - 28, 30), Weight::Medium, 12.5f, t.text);
    if (typing && Caret()) c.Fill(Rect(field.left + 14 + TextW(shown.empty() ? L"" : shown, Weight::Medium, 12.5f) + 1, field.top + 8, 1.5f, 14), 0.75f, t.accentHi);
    if (!typing && ui.Button(Ui::Id("mx.ed.name"), field).clicked) {
        mixName = g, mixNameEdit = grp.name;
        SetForegroundWindow(hwnd); // typing needs the keyboard; it goes back when the field closes
        SetFocus(hwnd);
    }
    y += 40;
    float step = (std::min)(26.0f, w / sys::Desk::kColors);
    for (int k = 0; k < sys::Desk::kColors; ++k) { // color
        D2D1_POINT_2F o = {x + step / 2 + k * step, y + 11};
        bool on = (grp.color & 0xFFFFFF) == sys::Desk::Swatch(k);
        if (ui.Button(Ui::Id("mx.ed.col", k), {o.x - 11, o.y - 11, o.x + 11, o.y + 11}).clicked) grp.color = sys::Desk::Swatch(k), DeskSave();
        c.Dot(o, on ? 9.0f : 7.0f, Color::Rgb(sys::Desk::Swatch(k)));
        if (on) c.Stroke({o.x - 12, o.y - 12, o.x + 12, o.y + 12}, 12, t.text.Alpha(0.7f), 1.5f);
    }
    y += 30;
    for (int k = 0; k < sys::Desk::kIcons; ++k) { // icon
        D2D1_POINT_2F o = {x + step / 2 + k * step, y + 12};
        bool on = grp.icon == k;
        if (ui.Button(Ui::Id("mx.ed.icon", k), {o.x - 12, o.y - 12, o.x + 12, o.y + 12}).clicked) grp.icon = k, DeskSave();
        if (on) c.Dot(o, 12, Color::Rgb(grp.color).Alpha(0.25f));
        c.Icon(kGroupIcons[k], o, 16, on ? Color::Rgb(grp.color) : t.textDim, on);
    }
    y += 32;
    float cx = x;
    for (int k = -1; k < (int)outs.size(); ++k) { // its output: default, then each
        std::wstring id = k < 0 ? L"" : outs[k].id, label = k < 0 ? L"Default" : Short(outs[k].name, 14);
        float cw = 22 + 6.5f * (float)label.size();
        if (cx + cw > r.right) cx = x, y += 28;
        if (w::Chip(ui, t, Ui::Id("mx.ed.out", k + 1), Rect(cx, y, cw, 24), label, grp.device == id)) grp.device = id, d.profile = -1, DeskSave();
        cx += cw + 6;
    }
    y += 32;
    static const wchar_t* const kRoles[] = {L"No chat mix", L"Game side", L"Chat side"};
    int role = d.game == g ? 1 : d.chat == g ? 2 : 0;
    cx = x;
    for (int k = 0; k < 3; ++k) {
        float cw = (w - 12) / 3;
        if (w::Chip(ui, t, Ui::Id("mx.ed.role", k), Rect(cx, y, cw, 24), kRoles[k], role == k) && k != role) {
            if (d.game == g) d.game = -1;
            if (d.chat == g) d.chat = -1;
            if (k == 1) d.game = g;
            if (k == 2) d.chat = g;
            DeskSave();
        }
        cx += cw + 6;
    }
    y += 34;
    auto toggle = [&](const char* id, const wchar_t* label, bool& on) {
        c.Text(label, Rect(x, y, w - 56, 28), Weight::Regular, 11.5f, t.text);
        if (w::Switch(ui, t, Ui::Id(id), {r.right - 24, y + 14}, on)) on = !on, DeskSave();
        y += 32;
    };
    toggle("mx.ed.ducks", L"Lower while others play", grp.ducks);
    toggle("mx.ed.lowers", L"Lowers the others", grp.lowers);
    y += 4;
    D2D1_RECT_F del = Rect(x, y, 110, 28), done = Rect(r.right - 90, y, 90, 28);
    Ui::Hit dh = ui.Button(Ui::Id("mx.ed.del"), del), oh = ui.Button(Ui::Id("mx.ed.done"), done);
    c.Fill(del, 14, t.text.Alpha(dh.hover ? 0.12f : 0.07f));
    c.Text(L"Delete group", del, Weight::Medium, 11.5f, t.text, Align::Center);
    c.Fill(done, 14, t.accent);
    c.Text(L"Done", done, Weight::Medium, 11.5f, t.onAccent, Align::Center);
    if (dh.clicked) {
        for (const auto& a : grp.apps) mixer.SetMuted(a, false); // nothing stays muted by a group that's gone
        d.RemoveGroup(g);
        std::fill(mixUnfold.begin(), mixUnfold.end(), 0);
        mixEdit = mixName = -1;
        DeskSave();
    } else if (oh.clicked) {
        if (mixName >= 0) CommitMixName();
        mixEdit = -1;
    }
}

void Bar::MixOptions(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    sys::Desk& d = TheDesk();
    float x = r.left, y = r.top + 2, w = Width(r);
    c.Text(L"Lower by " + std::to_wstring(d.duckBy) + L"%", Rect(x, y, w, 15), Weight::Medium, 11.5f, t.text);
    float v = (d.duckBy - sys::Desk::kDuckMin) / (float)(sys::Desk::kDuckMax - sys::Desk::kDuckMin);
    bool tap = false;
    if (w::Pill(ui, t, Ui::Id("mx.duck"), Rect(x, y + 18, w, 26), glyph::VolumeDown, v, &tap))
        d.duckBy = sys::Desk::kDuckMin + (int)std::lround(v * (sys::Desk::kDuckMax - sys::Desk::kDuckMin) / 5) * 5, DeskSave();
    y += 54;
    auto toggle = [&](const char* id, const wchar_t* label, bool& on) {
        c.Text(label, Rect(x, y, w - 56, 28), Weight::Regular, 11.5f, t.text);
        if (w::Switch(ui, t, Ui::Id(id), {r.right - 24, y + 14}, on)) on = !on, DeskSave();
        y += 30;
    };
    toggle("mx.keys", L"Shortcuts", d.keys);
    toggle("mx.osd", L"Show what a shortcut did", d.osd);
    c.Text(L"Ctrl+Alt: M mic · P profile · [ ] chat mix · 1-8 groups", Rect(x, y, w, 14), Weight::Light, 10, t.textLow);
    y += 24;
    c.Text(L"PROFILES", Rect(x, y, w, 12), Weight::Medium, 9.5f, t.textLow);
    y += 18;
    float cx = x;
    for (int k = 0; k < (int)d.profiles.size(); ++k) { // recall, right-click forgets
        std::wstring name = Short(d.profiles[k].name, 12);
        float cw = 22 + 6.5f * (float)name.size();
        if (cx + cw > r.right) break;
        D2D1_RECT_F pr = Rect(cx, y, cw, 24);
        if (w::Chip(ui, t, Ui::Id("mx.prof", k), pr, name, d.profile == k)) d.Recall(k), DeskSave();
        if (ui.RClick(pr)) {
            d.profiles.erase(d.profiles.begin() + k);
            d.profile = -1;
            DeskSave();
            break;
        }
        cx += cw + 6;
    }
    if ((int)d.profiles.size() < sys::Desk::kMaxProfiles && cx + 120 <= r.right && w::Chip(ui, t, Ui::Id("mx.prof.add"), Rect(cx, y, 120, 24), L"Save current mix", false))
        d.SaveProfile((int)d.profiles.size(), L""), DeskSave();
    y += 30;
    c.Text(L"A profile keeps each group's level, mute and output. Right-click one to forget it.", Rect(x, y, w, 28), Weight::Light, 10, t.textLow);
}

float Bar::MixRows(const Theme& t, const D2D1_RECT_F& r, float y) {
    Canvas& c = ui.C();
    sys::Desk& d = TheDesk();
    if (mixEdit >= (int)d.groups.size()) mixEdit = mixName = -1; // astraeus deleted it meanwhile
    float h = MixHeight(), top = y;
    c.Text(mixEdit >= 0 ? L"GROUP" : mixOpts ? L"MIXER OPTIONS" : L"MIXER", Rect(r.left + 2, y + 6, 160, 12), Weight::Medium, 9.5f, t.textLow);
    if (mixEdit < 0 && (int)d.groups.size() < sys::Desk::kMaxGroups && !mixOpts && w::IconBtn(ui, t, Ui::Id("mx.add"), {r.right - 44, y + 11}, 22, glyph::Add, 15)) {
        int g = d.AddGroup();
        if (g >= 0) DeskSave(), mixEdit = g;
    }
    if (w::IconBtn(ui, t, Ui::Id("mx.opts"), {r.right - 12, y + 11}, 22, mixOpts || mixEdit >= 0 ? glyph::Check : glyph::Tune, 15)) {
        if (mixEdit >= 0) {
            if (mixName >= 0) CommitMixName();
            mixEdit = -1;
        } else {
            mixOpts = !mixOpts;
        }
    }
    y += kMixHead;
    if (mixEdit >= 0) {
        MixEditor(t, Rect(r.left, y, Width(r), kEditH));
        return top + h;
    }
    if (mixOpts) {
        MixOptions(t, Rect(r.left, y, Width(r), kOptsH));
        return top + h;
    }
    if (!d.profiles.empty()) { // the profiles, one tap away
        float cx = r.left;
        for (int k = 0; k < (int)d.profiles.size(); ++k) {
            std::wstring name = Short(d.profiles[k].name, 12);
            float cw = 22 + 6.5f * (float)name.size();
            if (cx + cw > r.right) break;
            if (w::Chip(ui, t, Ui::Id("mx.head.prof", k), Rect(cx, y + 2, cw, 24), name, d.profile == k)) d.Recall(k), DeskSave();
            cx += cw + 6;
        }
        y += kProfRow;
    }
    float chatH = d.Chatting() ? kChatRow : 0;
    D2D1_RECT_F view = {r.left - 6, y, r.right + 6, top + h - chatH};
    float content = 0;
    {
        int rows = 1 + (int)d.groups.size();
        for (const auto& a : mixer.Apps()) {
            int g = d.GroupOf(a.key);
            rows += g < 0 || mixUnfold[g] ? 1 : 0;
        }
        content = rows * kMixRow + (mixer.Apps().empty() ? kMixRow : 0);
    }
    float maxS = (std::max)(0.0f, content - Height(view));
    if (maxS > 0 && !ui.Captured()) mixScroll = (std::clamp)(mixScroll - ui.Wheel(view) * 50, 0.0f, maxS); // past the room the rows scroll
    mixScroll = (std::clamp)(mixScroll, 0.0f, maxS);
    c.PushClip(view);
    MixList(t, r, y - mixScroll);
    c.PopClip();
    if (chatH > 0) { // game on the left, chat on the right: one slider between them
        const auto& game = d.groups[d.game];
        const auto& chat = d.groups[d.chat];
        float cy = top + h - chatH / 2;
        c.Icon(kGroupIcons[game.icon % sys::Desk::kIcons], {r.left + 10, cy}, 16, Color::Rgb(game.color), true);
        c.Icon(kGroupIcons[chat.icon % sys::Desk::kIcons], {r.right - 10, cy}, 16, Color::Rgb(chat.color), true);
        D2D1_RECT_F track = {r.left + 30, cy - 3, r.right - 30, cy + 3};
        float v = (1 - d.chatMix) / 2; // left is all game
        uint32_t id = Ui::Id("mx.chat");
        bool moved = ui.Slider(id, {track.left, cy - 14, track.right, cy + 14}, v, false);
        if (float n = ui.Wheel({r.left, cy - 18, r.right, cy + 18})) v = (std::clamp)(v - n * kWheelStep, 0.0f, 1.0f), moved = true;
        if (moved) d.chatMix = std::fabs(v - 0.5f) < 0.03f ? 0.0f : 1 - 2 * v, d.profile = -1, DeskSave(); // snaps to even near the middle
        float shown = ui.Smooth(id + 1, (1 - d.chatMix) / 2, 18), mid = (track.left + track.right) / 2, x = track.left + Width(track) * shown;
        c.Fill(track, 3, t.text.Alpha(0.10f));
        c.Fill({(std::min)(mid, x), track.top, (std::max)(mid, x), track.bottom}, 3, Color::Rgb(d.chatMix > 0 ? game.color : chat.color));
        c.Fill(Rect(mid - 1, track.top - 4, 2, 14), 1, t.textLow);
        c.Dot({x, cy}, 7, t.text);
    }
    return top + h;
}
