// Each widget's face in the dock, small, medium or large, and what a click on it does.
// Small is an icon with at most a number; medium adds a line of words; large is a card with the most it can say in the dock's height.
#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>

#include <cmath>
#include <cstdio>

#include "glyphs.h"
#include "picker.h"
#include "toggles.h"
#include "widgets.h"

int WgToday();

namespace {

constexpr float kPad = 9; // inside a widget's spot

int WxIcon(int code, bool day) { // wmo weather codes, as island's
    if (code == 0) return day ? glyph::ClearDay : glyph::ClearNight;
    if (code <= 2) return day ? glyph::PartlyDay : glyph::PartlyNight;
    if (code == 3) return glyph::Cloud;
    if (code == 45 || code == 48) return glyph::Fog;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return glyph::Snow;
    if (code >= 95) return glyph::Storm;
    return glyph::Rain;
}

const wchar_t* WxText(int code) {
    if (code == 0) return L"Clear";
    if (code <= 2) return L"Partly cloudy";
    if (code == 3) return L"Overcast";
    if (code == 45 || code == 48) return L"Fog";
    if (code >= 51 && code <= 57) return L"Drizzle";
    if ((code >= 61 && code <= 67) || (code >= 80 && code <= 82)) return L"Rain";
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return L"Snow";
    if (code >= 95) return L"Thunderstorm";
    return L"Showers";
}

std::wstring Rate(double bps) { // bytes a second, as people read speeds
    wchar_t b[24];
    if (bps >= 1024 * 1024) swprintf_s(b, L"%.1f MB/s", bps / (1024 * 1024));
    else swprintf_s(b, L"%.0f KB/s", bps / 1024);
    return b;
}

double Frac(int kind) { // how far through the day, week, month or year
    SYSTEMTIME st;
    GetLocalTime(&st);
    double day = (st.wHour * 60 + st.wMinute) / 1440.0;
    if (kind == 0) return day;
    if (kind == 1) return ((st.wDayOfWeek + 6) % 7 + day) / 7; // weeks start on monday
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (st.wYear % 4 == 0 && st.wYear % 100) || st.wYear % 400 == 0;
    int inMonth = kDays[st.wMonth - 1] + (st.wMonth == 2 && leap);
    if (kind == 2) return (st.wDay - 1 + day) / inMonth;
    int doy = st.wDay - 1;
    for (int m = 1; m < st.wMonth; ++m) doy += kDays[m - 1] + (m == 2 && leap);
    return (doy + day) / (leap ? 366 : 365);
}

}

std::wstring WgDeg(double c, bool f) { return std::to_wstring((int)std::lround(f ? c * 9 / 5 + 32 : c)) + L"°"; }
int WgWxIcon(int code, bool day) { return WxIcon(code, day); }
const wchar_t* WgWxText(int code) { return WxText(code); }
double WgFrac(int kind) { return Frac(kind); }
std::wstring WgRate(double bps) { return Rate(bps); }

static std::wstring Widest(std::wstring s) { // every digit as an 8: the widest the same shape of number gets, and few strings to measure
    for (wchar_t& ch : s)
        if (ch >= L'0' && ch <= L'9') ch = L'8';
    return s;
}

constexpr float kFitStep = 4, kFitSlack = 6; // widths move in small steps; a few dips spare isn't worth a reflow
constexpr float kFitMaxW = 260; // the widest a fitted widget gets
constexpr double kFitShrinkSec = 6; // a shorter number this long before the widget gives the room back

void Bar::SyncFit() {
    double now = (double)GetTickCount64() / 1000;
    bool moved = false;
    auto fit = [&](int kind, float want) {
        want = std::ceil(want / kFitStep) * kFitStep;
        float& w = fitW[kind];
        if (w <= 0 || want > w) moved = moved || w != want, w = want, fitLow[kind] = 0; // longer: room at once, nothing clipped
        else if (want >= w - kFitSlack) fitLow[kind] = 0;
        else if (fitLow[kind] <= 0) fitLow[kind] = now;
        else if (now - fitLow[kind] > kFitShrinkSec) w = want, fitLow[kind] = 0, moved = true; // a quieter spell: give the room back
    };
    if (HasWg(kWgNet)) fit(kWgNet, kPad * 2 + 16 + (std::max)(TextW(Widest(Rate(netDown)), Weight::Semi, 11.5f), TextW(Widest(Rate(netUp)), Weight::Regular, 11)) + 2);
    for (const WgSlot& s : wgSlots)
        if (s.kind == kWgWeather && s.size > 0) {
            std::wstring deg = wx.valid ? WgDeg(wx.tempC, fahrenheit) : L"--";
            if (s.size == 1) {
                fit(kWgWeather, kPad * 2 + 26 + TextW(Widest(deg), Weight::Semi, 15) + 2);
                continue;
            }
            std::wstring top = deg + L"  " + (wx.valid ? WxText(wx.code) : L"");
            std::wstring sub = wx.valid ? L"H " + WgDeg(wx.highC, fahrenheit) + L"  L " + WgDeg(wx.lowC, fahrenheit) + (wx.place.empty() ? L"" : L"  ·  " + wx.place) : L"Finding the forecast";
            float tw = (std::max)(TextW(Widest(top), Weight::Semi, 12.5f), TextW(Widest(sub), Weight::Regular, 10.5f));
            fit(kWgWeather, (std::min)(kFitMaxW, kPad * 2 + 26 + tw + 2)); // a long place name is cut, not the dock stretched
        } else if (s.kind == kWgBattery && s.size > 0) {
            std::wstring pct = status.battery && status.percent >= 0 ? std::to_wstring(status.percent) + L"%" : L"AC";
            float tw = TextW(Widest(pct), Weight::Semi, 12.5f);
            if (s.size == 2) tw = (std::max)(tw, TextW(status.charging ? L"Charging" : status.battery ? L"On battery" : L"Plugged in", Weight::Regular, 10.5f));
            fit(kWgBattery, kPad * 2 + 26 + tw + 2);
        }
    if (moved) Redraw();
}

float Bar::WidgetW(int kind, int size) const {
    if (kind == kWgApps) return appsW;
    if (kind == kWgMedia && !cfg.song && !editing) return 0; // "song in the bar" off: its spot goes, though edit mode still shows it to move or remove
    if (kind == kWgSpace) return (std::min)(spaceCap, (float)(size >= kSpaceMin ? size : kSpaceDef)); // its own width, along the dock either way, less what the apps need
    if (kind == kWgDivider) return kWegSep - 2 * kWegGap; // with the gaps either side, as wide as the dock's own separators
    if (kind == kWgMedia && Vert()) return size > 0 ? 168.0f : 76.0f; // a column: the song runs along it as astraeus' side pill, its title turned when medium or large
    if (Vert()) return kIcon; // a column: every widget at its small face
    if (size <= 0 || size > 2 || !(wg::Info(kind).sizes & (1 << size))) return kIcon; // small, or a widget that only comes small
    if ((kind == kWgNet || kind == kWgBattery || kind == kWgWeather) && fitW[kind] > 0) return (std::max)(kIcon, fitW[kind]); // sized to its words
    static const float kMedium[kWgCount] = {176, 40, 82, 76, 96, 76, 136, 92, 40, 40, 136, 136, 156, 136, 92, 112, 136, 136, 136, 40, 136, 104, 84, 124, 116, 80};
    static const float kLarge[kWgCount] = {236, 40, 184, 176, 176, 176, 196, 156, 40, 40, 196, 196, 216, 196, 160, 184, 196, 196, 196, 40, 196, 160, 140, 196, 184, 132};
    return size == 1 ? kMedium[kind] : kLarge[kind];
}

float Bar::WidgetsW() const {
    float w = 0;
    for (const WgSlot& s : wgSlots)
        if (float sw = WidgetW(s.kind, s.size); sw > 0 || s.kind != kWgMedia) w += sw + kWegGap; // a song switched off takes no gap either
    return w > 0 ? w - kWegGap : 0;
}

std::wstring Bar::WgTip(int kind) const {
    switch (kind) {
    case kWgWeather: return wx.valid ? std::wstring(WxText(wx.code)) + L", " + WgDeg(wx.tempC, fahrenheit) + (wx.place.empty() ? L"" : L" in " + wx.place) : L"Weather";
    case kWgFocus: return focusEnd > 0 ? L"Focus: click to pause, right click to reset" : focusLeft > 0 ? L"Focus: paused, click to carry on" : L"Focus for " + std::to_wstring(focusMin) + L" minutes";
    case kWgWater: return L"Water: " + std::to_wstring(water) + L" of " + std::to_wstring(waterGoal) + L" (right click: one less)";
    case kWgBin: return binFull ? L"Recycle Bin (full)" : L"Recycle Bin";
    case kWgPicker: return pickHex >= 0 ? L"Pick a color (the last one is copied)" : L"Pick a color";
    case kWgStats: return L"System stats (right click: Task Manager)";
    case kWgPhoto: return L"Open this photo (right click: another one)";
    default: return wg::Info(kind).name;
    }
}

void Bar::DrawWg(const Theme& t, int kind, int size, const D2D1_RECT_F& r, bool live) {
    Canvas& c = ui.C();
    if (size < 0 || size > 2 || !(wg::Info(kind).sizes & (1 << size))) size = 0; // a space carries its width here, not a face size
    float cx = (r.left + r.right) / 2, mid = (r.top + r.bottom) / 2;
    bool tiny = size == 0, large = size == 2;
    double now = (double)GetTickCount64() / 1000;
    auto lines = [&](int g, Color ink, const std::wstring& a, const std::wstring& b) { // the medium and large face: an icon, then a line or two
        c.Icon(g, {r.left + kPad + 10, mid}, 19, ink, true);
        float tx = r.left + kPad + 26, tw = r.right - kPad - tx;
        if (b.empty()) {
            c.Text(a, Rect(tx, r.top, tw, Height(r)), Weight::Semi, 12.5f, t.text);
        } else {
            c.Text(a, Rect(tx, mid - 15, tw, 16), Weight::Semi, 12.5f, t.text);
            c.Text(b, Rect(tx, mid + 1, tw, 14), Weight::Regular, 10.5f, t.textDim);
        }
    };
    auto badge = [&](const std::wstring& s, Color ink) { c.Text(s, Rect(r.left, mid + 5, Width(r), 12), Weight::Semi, 9, ink, Align::Center); }; // a number under a small icon
    switch (kind) {
    case kWgMedia:
        mediaR = r, mediaSize = size;
        DrawMediaTile(t, live);
        break;
    case kWgDrives: { // a disk and how many there are
        bool usb = std::any_of(drives.begin(), drives.end(), [](const DriveRow& d) { return d.removable; });
        c.Icon(usb ? glyph::SysUsb : glyph::SysDrive, {cx, mid - 4}, 18, usb ? t.accentHi : t.textDim);
        badge(std::to_wstring(drives.size()), t.text);
        break;
    }
    case kWgSpace: // empty room: nothing at all in the dock, a faint outline while arranging
        if (!live) {
            c.Fill(Inset(r, 2, 4), TileRadius() - 2, t.text.Alpha(0.04f));
            c.Icon(glyph::More, {cx, mid}, 16, t.textLow);
        }
        break;
    case kWgDivider: { // the dock's own separator, wherever it's put
        if (Vert()) { // across the column
            float ly = std::round(mid * scale) / scale;
            c.Line({r.left + Width(r) * 0.25f, ly}, {r.left + Width(r) * 0.75f, ly}, t.text.Alpha(0.14f), 1, false);
            break;
        }
        float lx = std::round(cx * scale) / scale; // on a whole pixel, so the hairline stays crisp
        c.Line({lx, r.top + Height(r) * 0.25f}, {lx, r.top + Height(r) * 0.75f}, t.text.Alpha(0.14f), 1, false);
        break;
    }
    case kWgBin: { // an outline can, filled once there's something in it, with an accent bead
        c.Icon(glyph::Delete, {cx, mid}, 21, binFull ? t.text : t.textDim, binFull);
        if (binFull) c.Dot({cx + 8, mid - 9}, 3, t.accent);
        break;
    }
    case kWgWeather: {
        int g = wx.valid ? WxIcon(wx.code, wx.isDay) : glyph::Cloud;
        std::wstring deg = wx.valid ? WgDeg(wx.tempC, fahrenheit) : L"--";
        if (tiny) {
            c.Icon(g, {cx, mid - 4}, 18, t.accentHi, true);
            badge(deg, t.text);
        } else if (!large) {
            c.Icon(g, {r.left + kPad + 11, mid}, 21, t.accentHi, true);
            c.Text(deg, Rect(r.left + kPad + 26, r.top, Width(r) - kPad - 28, Height(r)), Weight::Semi, 15, t.text);
        } else {
            lines(g, t.accentHi, deg + L"  " + (wx.valid ? WxText(wx.code) : L""), wx.valid ? L"H " + WgDeg(wx.highC, fahrenheit) + L"  L " + WgDeg(wx.lowC, fahrenheit) + (wx.place.empty() ? L"" : L"  ·  " + wx.place) : L"Finding the forecast");
        }
        break;
    }
    case kWgStats: {
        Color hot = Color::Rgb(0xFF6B6B);
        if (tiny) { // the processor as a ring, its number inside
            c.Ring({cx, mid}, 13, t.text.Alpha(0.14f), 2.5f);
            c.Arc({cx, mid}, 13, -1.5708f, 6.2832f * (float)stats.cpu, stats.cpu > 0.85 ? hot : t.accentHi, 2.5f);
            c.Text(std::to_wstring((int)std::lround(stats.cpu * 100)), Rect(cx - 12, mid - 7, 24, 14), Weight::Semi, 9.5f, t.text, Align::Center);
            break;
        }
        float lx = r.left + kPad, bx = lx + 28, bw = (large ? r.left + Width(r) * 0.52f : r.right - kPad) - bx;
        auto row = [&](const wchar_t* label, double v, float y) {
            c.Text(label, Rect(lx, y - 7, 28, 14), Weight::Medium, 9.5f, t.textDim);
            c.Fill(Rect(bx, y - 2, bw, 4), 2, t.text.Alpha(0.12f));
            c.Fill(Rect(bx, y - 2, (std::max)(4.0f, bw * anim::Clamp01((float)v)), 4), 2, v > 0.85 ? hot : t.accentHi);
        };
        row(L"CPU", stats.cpu, mid - 7);
        row(L"RAM", stats.mem, mid + 8);
        if (large) { // the last minute of load, as a line
            D2D1_RECT_F g = {bx + bw + 10, r.top + 7, r.right - kPad, r.bottom - 7};
            ID2D1RenderTarget* rt = c.Target();
            ComPtr<ID2D1PathGeometry> path;
            ComPtr<ID2D1GeometrySink> sink;
            if (SUCCEEDED(gfx::D2D()->CreatePathGeometry(&path)) && SUCCEEDED(path->Open(&sink))) {
                for (int i = 0; i < 30; ++i) {
                    float v = cpuHist[(cpuAt + i) % 30];
                    D2D1_POINT_2F p = {g.left + Width(g) * i / 29, g.bottom - Height(g) * anim::Clamp01(v)};
                    if (i == 0) sink->BeginFigure(p, D2D1_FIGURE_BEGIN_HOLLOW);
                    else sink->AddLine(p);
                }
                sink->EndFigure(D2D1_FIGURE_END_OPEN);
                sink->Close();
                ComPtr<ID2D1SolidColorBrush> ink;
                rt->CreateSolidColorBrush(D2D1::ColorF(t.accentHi.r, t.accentHi.g, t.accentHi.b, 0.9f), &ink);
                if (ink) rt->DrawGeometry(path.Get(), ink.Get(), 1.4f);
            }
        }
        break;
    }
    case kWgFocus: { // a ring that empties as the session runs, the minutes left inside it
        double total = focusMin * 60.0;
        bool running = focusEnd > 0, paused = !running && focusLeft > 0;
        double left = running ? (std::max)(0.0, focusEnd - now) : paused ? focusLeft : total;
        float k = (float)(left / total);
        D2D1_POINT_2F o = tiny ? D2D1_POINT_2F{cx, mid} : D2D1_POINT_2F{r.left + kPad + 13, mid};
        c.Ring(o, 13, t.text.Alpha(0.14f), 2.5f);
        if (running || paused) c.Arc(o, 13, -1.5708f, 6.2832f * k, paused ? t.textDim : t.accentHi, 2.5f);
        std::wstring mins = std::to_wstring((int)std::ceil(left / 60));
        if (running || paused) c.Text(mins, Rect(o.x - 12, mid - 8, 24, 16), Weight::Semi, 11, t.text, Align::Center);
        else c.Icon(glyph::SysStopwatch, o, 15, t.textDim);
        if (!tiny) {
            float tx = o.x + 20;
            c.Text(running ? mins + L" min" : paused ? L"Paused" : L"Focus", Rect(tx, large ? mid - 15 : r.top, r.right - kPad - tx, large ? 16 : Height(r)), Weight::Semi, 12.5f, t.text);
            if (large) c.Text(running ? L"Click to pause" : paused ? L"Click to carry on" : std::to_wstring(focusMin) + L" minutes, then a break", Rect(tx, mid + 1, r.right - kPad - tx, 14), Weight::Regular, 10.5f, t.textDim);
        }
        break;
    }
    case kWgWater: {
        bool done = water >= waterGoal;
        Color ink = done ? t.accentHi : t.textDim;
        if (tiny) {
            c.Icon(glyph::SysCup, {cx, mid - 4}, 18, ink);
            badge(std::to_wstring(water) + L"/" + std::to_wstring(waterGoal), ink);
        } else if (!large) {
            lines(glyph::SysCup, ink, std::to_wstring(water) + L" / " + std::to_wstring(waterGoal), L"");
        } else { // a bead per glass
            c.Icon(glyph::SysCup, {r.left + kPad + 10, mid}, 19, ink);
            float bx = r.left + kPad + 28, step = (r.right - kPad - bx) / (float)waterGoal;
            for (int i = 0; i < waterGoal; ++i) c.Dot({bx + step * (i + 0.5f), mid}, (std::min)(5.0f, step * 0.32f), i < water ? t.accentHi : t.text.Alpha(0.14f));
        }
        break;
    }
    case kWgClip: {
        if (live) clipAnchor = Mid(r); // the quick key opens the panel over it
        const clip::Store& st = clip::History();
        std::wstring last = st.opt.paused ? L"Paused" : st.items.empty() ? L"Nothing copied yet" : clip::Line(st.items.front());
        if (tiny) c.Icon(glyph::SysPaste, {cx, mid}, 19, t.textDim);
        else lines(glyph::SysPaste, t.textDim, large ? L"Clipboard" : last, large ? last : L"");
        break;
    }
    case kWgPicker: {
        wchar_t hex[10] = L"#------";
        if (pickHex >= 0) swprintf_s(hex, L"#%06X", pickHex);
        if (tiny) {
            c.Icon(glyph::SysPicker, {cx, mid}, 18, t.textDim);
            if (pickHex >= 0) c.Dot({r.right - 9, r.top + 9}, 4.5f, Color::Rgb((uint32_t)pickHex)), c.Ring({r.right - 9, r.top + 9}, 4.5f, t.hairTop, 1);
        } else {
            D2D1_RECT_F sw = Rect(r.left + kPad, mid - 11, 22, 22);
            c.Fill(sw, 7, pickHex >= 0 ? Color::Rgb((uint32_t)pickHex) : t.text.Alpha(0.1f));
            c.Stroke(sw, 7, t.hairTop, 1);
            if (pickHex < 0) c.Icon(glyph::SysPicker, {sw.left + 11, mid}, 13, t.textDim);
            float tx = sw.right + 8;
            c.Text(hex, Rect(tx, large ? mid - 15 : r.top, r.right - kPad - tx, large ? 16 : Height(r)), Weight::Semi, 12.5f, t.text);
            if (large) c.Text(L"Click to pick from the screen", Rect(tx, mid + 1, r.right - kPad - tx, 14), Weight::Regular, 10.5f, t.textDim);
        }
        break;
    }
    case kWgShot:
        c.Icon(glyph::Snip, {cx, mid}, 19, t.textDim);
        break;
    case kWgEmoji:
        c.Icon(glyph::SysEmoji, {cx, mid}, 19, t.textDim);
        break;
    case kWgFiles: {
        if (tiny) {
            c.Icon(glyph::SysDownload, {cx, mid}, 18, t.textDim);
        } else if (!large || files.empty()) {
            lines(glyph::SysDownload, t.textDim, files.empty() ? L"Downloads" : files.front().name, large ? L"Downloads" : L"");
        } else { // the three newest, as their icons
            c.Icon(glyph::SysDownload, {r.left + kPad + 10, mid}, 18, t.textDim);
            float x = r.left + kPad + 26;
            for (size_t i = 0; i < files.size() && i < 3; ++i) {
                FileRow& f = files[i];
                if (!f.icon && f.pixels && ctx) ctx->CreateBitmapFromWicBitmap(f.pixels.Get(), nullptr, &f.icon);
                D2D1_RECT_F ic = Rect(x + i * 34, mid - 14, 28, 28);
                if (f.icon) c.Pixels(f.icon.Get(), ic, 1);
            }
            c.Text(files.front().name, Rect(x + 3 * 34, r.top, r.right - kPad - x - 102, Height(r)), Weight::Regular, 10.5f, t.textDim);
        }
        break;
    }
    case kWgNotes: {
        std::wstring first = note.substr(0, note.find(L'\n'));
        if (tiny) c.Icon(glyph::SysNote, {cx, mid}, 18, t.textDim);
        else lines(glyph::SysNote, t.textDim, first.empty() ? L"Notes" : first, large ? (first.empty() ? L"Click to write" : L"Note") : L"");
        break;
    }
    case kWgCalendar: { // the next event today
        SYSTEMTIME st;
        GetLocalTime(&st);
        FILETIME ft;
        GetSystemTimeAsFileTime(&ft);
        int64_t unixNow = (int64_t)((((ULONGLONG)ft.dwHighDateTime << 32) | ft.dwLowDateTime) / 10000000ULL - 11644473600ULL);
        std::vector<sys::cal::Event> today = cals.Day(st.wYear, st.wMonth, st.wDay);
        const sys::cal::Event* next = nullptr;
        for (const auto& e : today)
            if (!e.allDay && e.end > unixNow) {
                next = &e;
                break;
            }
        std::wstring when, what = next ? next->title : cals.Linked() ? L"Nothing else today" : L"Connect a calendar";
        if (next) {
            ULONGLONG u = ((ULONGLONG)next->start + 11644473600ULL) * 10000000ULL;
            FILETIME f = {(DWORD)u, (DWORD)(u >> 32)};
            SYSTEMTIME utc, loc;
            FileTimeToSystemTime(&f, &utc);
            SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
            wchar_t tm[16] = {};
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &loc, nullptr, tm, 16);
            when = next->start <= unixNow ? L"Now" : tm;
        }
        if (tiny) {
            c.Icon(glyph::Calendar, {cx, mid - 4}, 17, next ? t.accentHi : t.textDim);
            badge(std::to_wstring(st.wDay), t.text);
        } else {
            lines(glyph::Calendar, next ? t.accentHi : t.textDim, what, next ? when + (large && !next->where.empty() ? L"  ·  " + next->where : L"") : L"");
        }
        break;
    }
    case kWgTodo: {
        int open = 0;
        const Todo* first = nullptr;
        for (const auto& d : todos)
            if (!d.done) {
                ++open;
                if (!first) first = &d;
            }
        if (tiny) {
            c.Icon(glyph::TaskAlt, {cx, mid - 4}, 17, open ? t.accentHi : t.textDim);
            badge(std::to_wstring(open), t.text);
        } else {
            lines(glyph::TaskAlt, open ? t.accentHi : t.textDim, first ? first->text : L"All done", large ? std::to_wstring(open) + (open == 1 ? L" thing to do" : L" things to do") : L"");
        }
        break;
    }
    case kWgClock: { // a flip clock: the hours and the minutes on their own plates
        SYSTEMTIME st;
        GetLocalTime(&st);
        bool h24 = cfg.clock24 == 1;
        int hh = h24 ? st.wHour : (st.wHour % 12 == 0 ? 12 : st.wHour % 12);
        wchar_t a[4], b[4];
        swprintf_s(a, L"%02d", hh), swprintf_s(b, L"%02d", st.wMinute);
        if (tiny) {
            c.Text(a, Rect(r.left, mid - 15, Width(r), 15), Weight::Semi, 12.5f, t.text, Align::Center);
            c.Text(b, Rect(r.left, mid, Width(r), 15), Weight::Semi, 12.5f, t.accentHi, Align::Center);
            break;
        }
        float pw = large ? 34 : 30, ph = large ? 30 : 28, x0 = r.left + kPad;
        for (int i = 0; i < 2; ++i) {
            D2D1_RECT_F p = Rect(x0 + i * (pw + 4), mid - ph / 2, pw, ph);
            c.FillV(p, 7, t.text.Alpha(0.12f), t.text.Alpha(0.06f));
            c.Line({p.left + 2, mid}, {p.right - 2, mid}, t.bg.Alpha(0.6f), 1, false); // the flip's seam
            c.Text(i ? b : a, p, Weight::Semi, large ? 17 : 15, t.text, Align::Center);
        }
        if (large) {
            wchar_t day[32] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", day, 32, nullptr);
            float tx = x0 + 2 * pw + 14;
            c.Text(day, Rect(tx, mid - 15, r.right - kPad - tx, 16), Weight::Semi, 12, t.text);
            c.Text(h24 ? L"24-hour" : st.wHour < 12 ? L"AM" : L"PM", Rect(tx, mid + 1, r.right - kPad - tx, 14), Weight::Regular, 10.5f, t.textDim);
        }
        break;
    }
    case kWgProgress: { // the day, and in the large card the week and the year too
        double d = Frac(0);
        if (tiny) {
            c.Ring({cx, mid}, 13, t.text.Alpha(0.14f), 2.5f);
            c.Arc({cx, mid}, 13, -1.5708f, 6.2832f * (float)d, t.accentHi, 2.5f);
            c.Text(std::to_wstring((int)(d * 100)), Rect(cx - 12, mid - 7, 24, 14), Weight::Semi, 9.5f, t.text, Align::Center);
            break;
        }
        static const wchar_t* const kNames[] = {L"Day", L"Week", L"Month", L"Year"};
        int rows = large ? 3 : 1;
        const int kinds[3] = {0, 1, 3};
        float rh = Height(r) / (rows + 0.6f);
        for (int i = 0; i < rows; ++i) {
            double v = Frac(kinds[i]);
            float y = rows == 1 ? mid : r.top + rh * (i + 0.8f);
            c.Text(kNames[kinds[i]], Rect(r.left + kPad, y - 7, 40, 14), Weight::Medium, 9.5f, t.textDim);
            D2D1_RECT_F lane = Rect(r.left + kPad + 36, y - 2, Width(r) - 2 * kPad - 70, 4);
            c.Fill(lane, 2, t.text.Alpha(0.12f));
            c.Fill(Rect(lane.left, lane.top, (std::max)(4.0f, Width(lane) * (float)v), 4), 2, t.accentHi);
            c.Text(std::to_wstring((int)(v * 100)) + L"%", Rect(lane.right + 2, y - 7, 32, 14), Weight::Semi, 9.5f, t.text, Align::Right);
        }
        break;
    }
    case kWgToggles: { // wi-fi, bluetooth, do not disturb, night light; the large card adds energy saver and the microphone
        struct Tg {
            int glyph;
            bool on, can;
        };
        bool micOff = mic && mic->Ready() && mic->Muted();
        const Tg tg[6] = {{radios.WifiOn() ? glyph::Wifi : glyph::WifiOff, radios.WifiOn(), radios.HasWifi()}, {radios.BtOn() ? glyph::Bluetooth : glyph::BluetoothOff, radios.BtOn(), radios.HasBt()},
                          {glyph::Dnd, dnd, true}, {glyph::NightLight, sys::NightLightOn(), sys::NightLightAvailable()},
                          {glyph::Eco, sys::SaverOn(), true}, {micOff ? glyph::MicOff : glyph::Mic, !micOff, mic && mic->Ready()}};
        int n = large ? 6 : 4;
        float bw = (Width(r) - 2 * 6) / n;
        for (int i = 0; i < n; ++i) {
            D2D1_RECT_F b = Rect(r.left + 6 + bw * i + 2, mid - 13, bw - 4, 26);
            Ui::Hit h = live && tg[i].can ? ui.Button(Ui::Id("wg.tg", i), b) : Ui::Hit{};
            c.Fill(b, 13, tg[i].on ? t.accent.Alpha(h.hover ? 1.0f : 0.85f) : t.text.Alpha(h.hover ? 0.14f : 0.08f));
            c.Icon(tg[i].glyph, {(b.left + b.right) / 2, mid}, 15, tg[i].on ? t.onAccent : t.textDim.Alpha(tg[i].can ? 1 : 0.4f), tg[i].on);
            if (!h.clicked) continue;
            if (i == 0) radios.SetWifi(!tg[i].on);
            else if (i == 1) radios.SetBt(!tg[i].on);
            else if (i == 2) SetQuiet(!dnd);
            else if (i == 3) sys::SetNightLight(!tg[i].on);
            else if (i == 4) sys::SetSaver(!tg[i].on);
            else if (mic) mic->SetMuted(!micOff);
            Redraw();
        }
        break;
    }
    case kWgAudio: {
        int g = VolumeGlyph(volShown, muteShown);
        if (tiny) {
            c.Icon(g, {cx, mid - 4}, 17, t.text);
            badge(std::to_wstring((int)std::lround(volShown * 100)), t.textDim);
            if (live)
                if (float n = ui.Wheel(r); n != 0 && spk) volShown = anim::Clamp01(spk->Level() + n * 0.02f), spk->SetLevel(volShown);
            break;
        }
        D2D1_RECT_F ib = Rect(r.left + 4, mid - 14, 28, 28); // the speaker mutes
        Ui::Hit mh = live ? ui.Button(Ui::Id("wg.vol.mute"), ib) : Ui::Hit{};
        if (mh.hover) c.Fill(ib, 14, t.text.Alpha(0.08f));
        c.Icon(g, {ib.left + 14, mid}, 17, t.text);
        if (mh.clicked && spk) spk->SetMuted(!muteShown), muteShown = !muteShown;
        D2D1_RECT_F rail = Rect(ib.right + 4, large ? mid - 8 : mid - 2, r.right - kPad - ib.right - 4, 4);
        float v = volShown;
        if (live && ui.Slider(Ui::Id("wg.vol"), Inset(rail, -2, -8), v, false) && spk) spk->SetLevel(v), volShown = v;
        if (live)
            if (float n = ui.Wheel(r); n != 0 && spk) volShown = anim::Clamp01(spk->Level() + n * 0.02f), spk->SetLevel(volShown);
        c.Fill(rail, 2, t.text.Alpha(0.14f));
        c.Fill(Rect(rail.left, rail.top, (std::max)(4.0f, Width(rail) * volShown), 4), 2, muteShown ? t.textDim : t.accentHi);
        c.Dot({rail.left + Width(rail) * volShown, rail.top + 2}, 5, t.text);
        if (large) {
            std::wstring out;
            for (const auto& o : outs)
                if (o.id == outId) out = o.name;
            c.Text(out.empty() ? L"Sound output" : out, Rect(rail.left, mid + 2, Width(rail), 14), Weight::Regular, 10.5f, t.textDim);
        }
        break;
    }
    case kWgSearch:
        if (tiny) {
            c.Icon(glyph::Search, {cx, mid}, 19, t.textDim);
        } else { // a field that opens the launcher
            D2D1_RECT_F f = Inset(r, 5, 6);
            c.Fill(f, Height(f) / 2, t.text.Alpha(0.07f));
            c.Icon(glyph::Search, {f.left + 15, mid}, 15, t.textDim);
            c.Text(L"Search apps", Rect(f.left + 28, f.top, Width(f) - 32, Height(f)), Weight::Regular, 12, t.textLow);
        }
        break;
    case kWgSwitch:
        c.Icon(glyph::TaskView, {cx, mid}, 19, t.textDim);
        break;
    case kWgFolder: {
        static const wchar_t* const kNames[] = {L"Documents", L"Desktop", L"Pictures", L"Music", L"Videos"};
        if (tiny) c.Icon(glyph::SysFolder, {cx, mid}, 19, t.accentHi);
        else lines(glyph::SysFolder, t.accentHi, kNames[folderPick], large ? L"Click to look inside" : L"");
        break;
    }
    case kWgNet: {
        if (tiny) {
            c.Icon(glyph::Down, {cx, mid - 5}, 15, t.accentHi);
            c.Text(Rate(netDown), Rect(r.left - 4, mid + 4, Width(r) + 8, 12), Weight::Semi, 8, t.text, Align::Center);
            break;
        }
        float tx = r.left + kPad;
        c.Icon(glyph::Down, {tx + 7, mid - 8}, 14, t.accentHi);
        c.Text(Rate(netDown), Rect(tx + 16, mid - 15, Width(r) - kPad * 2 - 16, 15), Weight::Semi, 11.5f, t.text);
        c.Icon(glyph::Up, {tx + 7, mid + 8}, 14, t.textDim);
        c.Text(Rate(netUp), Rect(tx + 16, mid + 1, Width(r) - kPad * 2 - 16, 15), Weight::Regular, 11, t.textDim);
        break;
    }
    case kWgBattery: {
        std::wstring pct = status.battery && status.percent >= 0 ? std::to_wstring(status.percent) + L"%" : L"AC";
        Color ink = status.charging || status.saver ? t.accentHi : t.text; // charging: the glyph's own bolt, lit
        if (tiny) {
            c.Icon(BatteryGlyph(status), {cx, mid - 4}, 17, ink);
            badge(pct, t.textDim);
        } else {
            lines(BatteryGlyph(status), ink, pct, large ? (status.charging ? L"Charging" : status.battery ? L"On battery" : L"Plugged in") : L"");
        }
        break;
    }
    case kWgHabit: {
        int today = WgToday(), done = 0, best = 0;
        for (const auto& h : habits) done += h.last == today, best = (std::max)(best, h.streak);
        bool all = done && done == (int)habits.size();
        if (tiny) {
            c.Icon(glyph::Check, {cx, mid - 4}, 17, all ? t.accentHi : t.textDim);
            badge(std::to_wstring(done) + L"/" + std::to_wstring(habits.size()), t.text);
        } else {
            lines(glyph::Check, all ? t.accentHi : t.textDim, std::to_wstring(done) + L" of " + std::to_wstring(habits.size()) + L" today",
                  large ? L"Best streak " + std::to_wstring(best) + (best == 1 ? L" day" : L" days") : L"");
        }
        break;
    }
    case kWgPhoto: {
        if (!photoBmp && photoSrc && ctx) ctx->CreateBitmapFromWicBitmap(photoSrc.Get(), nullptr, &photoBmp);
        D2D1_RECT_F p = Inset(r, 3, 3);
        if (photoBmp) c.Image(photoBmp.Get(), p, TileRadius() - 2, 1);
        else c.Fill(p, TileRadius() - 2, t.text.Alpha(0.06f)), c.Icon(glyph::SysPhoto, {cx, mid}, 18, t.textDim);
        break;
    }
    }
}

void Bar::WgClick(int kind, bool right, float cx) {
    double now = (double)GetTickCount64() / 1000;
    switch (kind) {
    case kWgBin:
        if (right) PostMessageW(hwnd, WM_APP_MENU, 2, 0); // its menu, after the frame
        else ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kWgStats:
        if (right) ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        else OpenWidget(kind, cx);
        break;
    case kWgFocus: {
        bool running = focusEnd > 0, paused = !running && focusLeft > 0;
        if (right) focusEnd = focusLeft = 0, KillTimer(hwnd, kFocusTimer), Redraw(); // reset
        else if (running) focusLeft = (std::max)(0.0, focusEnd - now), focusEnd = 0, KillTimer(hwnd, kFocusTimer), Redraw(); // pause
        else focusEnd = now + (paused ? focusLeft : focusMin * 60.0), focusLeft = 0, StepFocus(); // start, or carry on
        break;
    }
    case kWgWater:
        water = (std::clamp)(water + (right ? -1 : 1), 0, 99); // click: one more; right click: one less
        prefs::SaveNum(L"WaterCount", water), prefs::SaveNum(L"WaterDay", WgToday());
        Redraw();
        break;
    case kWgPicker:
        if (!right) sys::picker::Start(hwnd, WM_APP_PICK);
        break;
    case kWgShot:
        Keys({VK_LWIN, VK_LSHIFT, 'S'}); // windows' own snipping bar
        break;
    case kWgEmoji:
        Keys({VK_LWIN, VK_OEM_PERIOD}); // windows' own panel, into whatever has the caret
        break;
    case kWgFiles:
        if (right) ShellExecuteW(nullptr, L"open", L"shell:Downloads", nullptr, nullptr, SW_SHOWNORMAL);
        else OpenWidget(kind, cx);
        break;
    case kWgCalendar:
        if (cals.Linked() && !right) OpenWidget(kind, cx);
        else Anchor(cx, false), SetPanel(calOpen ? Panel::None : Panel::Calendar); // the full calendar, which also connects one
        break;
    case kWgToggles:
        if (right) OpenQuick(0, cx);
        break;
    case kWgAudio:
        if (right) OpenQuick(3, cx); // where the sound goes, and every app's level
        else if (spk) spk->SetMuted(!muteShown), muteShown = !muteShown, Redraw();
        break;
    case kWgSearch:
        Anchor(W / 2, false);
        launchOpen ? SetPanel(Panel::None) : OpenLauncher();
        break;
    case kWgSwitch:
        Keys({VK_LWIN, VK_TAB}); // task view
        break;
    case kWgFolder:
        if (right) {
            static const wchar_t* const kShell[] = {L"shell:Personal", L"shell:Desktop", L"shell:My Pictures", L"shell:My Music", L"shell:My Video"};
            ShellExecuteW(nullptr, L"open", kShell[folderPick], nullptr, nullptr, SW_SHOWNORMAL);
        } else {
            OpenWidget(kind, cx);
        }
        break;
    case kWgNet:
        ShellExecuteW(nullptr, L"open", L"taskmgr.exe", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kWgBattery:
        ShellExecuteW(nullptr, L"open", L"ms-settings:batterysaver", nullptr, nullptr, SW_SHOWNORMAL);
        break;
    case kWgPhoto:
        if (right) PickPhoto();
        else if (!photoPath.empty()) ShellExecuteW(nullptr, L"open", photoPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        break;
    default: // the rest open their panel
        if (!right) OpenWidget(kind, cx);
        break;
    }
}
