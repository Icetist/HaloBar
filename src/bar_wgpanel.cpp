// What rises over the dock from a widget: the forecast, the load, the clipboard, a folder, a note, the day's events, lists,
// clocks, and edit mode's gallery, where widgets are picked by category, previewed live, sized and set up.
#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

#include "glyphs.h"
#include "widgets.h"

int WgToday();
int WgDayShift(int ymd, int days);
std::wstring WgDeg(double c, bool f);
int WgWxIcon(int code, bool day);
const wchar_t* WgWxText(int code);
double WgFrac(int kind);

namespace {

constexpr float kHead = 30; // a panel's title row
constexpr float kRow = 30; // a list's row
constexpr int kListRows = 8;
constexpr int kFileCols = 4;
constexpr float kFileCell = 92, kFileRowH = 84;
constexpr int kGalCols = 3;
constexpr float kCardH = 106, kGalGap = 9, kTabsH = 30, kSelH = 52;
constexpr float kGalleryRise = 470;

}

float Bar::WidgetPanelW() const { return wgKind == kWgGallery ? kGalleryW : kWgW; }

float Bar::WidgetRise() const {
    auto list = [&](size_t n) { return kHead + 14 + (float)(std::max)((size_t)1, (std::min)(n, (size_t)kListRows)) * kRow + 46; };
    switch (wgKind) {
    case kWgGallery: return kGalleryRise;
    case kWgWeather: return 214;
    case kWgStats: return 176;
    case kWgClip: return list(clips.size());
    case kWgFiles:
    case kWgFolder: return kHead + 14 + (float)(std::max)(1, ((int)files.size() + kFileCols - 1) / kFileCols) * kFileRowH + 20;
    case kWgNotes: return 240;
    case kWgTodo: return list(todos.size() + 1);
    case kWgHabit: return list(habits.size() + 1);
    case kWgCalendar: {
        SYSTEMTIME st;
        GetLocalTime(&st);
        return list(cals.Day(st.wYear, st.wMonth, st.wDay).size()) + 10;
    }
    case kWgClock: return 196;
    case kWgProgress: return 196;
    case kWgApps: return kHead + 14 + (float)(std::max)(1, (moreN + kFileCols - 1) / kFileCols) * kFileRowH + 20;
    default: return 120;
    }
}

void Bar::DrawWidgetPanel(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, wgSpring.value);
    if (rise < 1 || !shape || wgKind < 0) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 30) / (WidgetRise() - 30));
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    float pw = WidgetPanelW();
    D2D1_RECT_F r = {W / 2 - pw / 2 + 18, pill.top + CallH() + 14, W / 2 + pw / 2 - 18, rowTop - 12};
    auto head = [&](const wchar_t* title) { c.TextSp(title, Rect(r.left, r.top, Width(r), 14), Weight::Medium, 10, t.textLow, 1.4f); };
    auto row = [&](uint32_t id, float y) { // a hoverable row the panel's width
        D2D1_RECT_F rr = Rect(r.left - 6, y, Width(r) + 12, kRow - 2);
        Ui::Hit h = ui.Button(id, rr);
        if (h.hover) c.Fill(rr, 10, t.text.Alpha(0.07f));
        return std::pair{rr, h};
    };
    auto field = [&](float y, const wchar_t* hint) { // the line being typed, with its caret
        D2D1_RECT_F f = Rect(r.left - 6, y, Width(r) + 12, kRow);
        c.Fill(f, 10, t.text.Alpha(0.06f));
        bool blink = std::fmod(ui.Now(), 1.0) < 0.55;
        ui.KeepAlive();
        if (draft.empty()) c.Text(hint, Inset(f, 12, 0), Weight::Regular, 12.5f, t.textLow);
        c.Text(draft + (blink ? L"│" : L""), Inset(f, 12, 0), Weight::Regular, 12.5f, t.text);
    };
    bool closeOnRight = true;
    switch (wgKind) {
    case kWgGallery:
        DrawGallery(t, r);
        closeOnRight = false;
        break;
    case kWgWeather: {
        head(L"WEATHER");
        if (!wx.valid) {
            c.Text(L"Finding the forecast…", Rect(r.left, r.top + 60, Width(r), 18), Weight::Regular, 13, t.textDim, Align::Center);
            break;
        }
        float y = r.top + 24;
        c.Icon(WgWxIcon(wx.code, wx.isDay), {r.left + 26, y + 26}, 44, t.accentHi, true);
        c.Text(WgDeg(wx.tempC, fahrenheit), Rect(r.left + 60, y, 120, 44), Weight::Light, 34, t.text);
        c.Text(WgWxText(wx.code), Rect(r.left + 170, y + 4, Width(r) - 170, 18), Weight::Medium, 13, t.text);
        c.Text(wx.place + L"  ·  H " + WgDeg(wx.highC, fahrenheit) + L"  L " + WgDeg(wx.lowC, fahrenheit), Rect(r.left + 170, y + 24, Width(r) - 170, 16), Weight::Regular, 11.5f, t.textDim);
        y += 64;
        int n = (std::min)(wx.nHours, 6);
        float cw = Width(r) / (float)(std::max)(1, n);
        for (int i = 0; i < n; ++i) { // the next hours
            const sys::WxHour& hr = wx.hours[i];
            float hx = r.left + cw * i + cw / 2;
            wchar_t lab[8];
            swprintf_s(lab, L"%02d:00", hr.hour);
            c.Text(lab, Rect(hx - 30, y, 60, 14), Weight::Regular, 10.5f, t.textDim, Align::Center);
            c.Icon(WgWxIcon(hr.code, hr.day), {hx, y + 28}, 20, t.text, true);
            c.Text(WgDeg(hr.tempC, fahrenheit), Rect(hx - 30, y + 42, 60, 16), Weight::Medium, 12, t.text, Align::Center);
        }
        y += 66;
        c.Line({r.left, y}, {r.right, y}, t.text.Alpha(0.06f), 1);
        int d = (std::min)(wx.nDays, 5);
        float dw = Width(r) / (float)(std::max)(1, d);
        for (int i = 0; i < d; ++i) { // the days after today
            const sys::WxDay& dy = wx.days[i];
            SYSTEMTIME st{(WORD)dy.y, (WORD)dy.m, 0, (WORD)dy.d};
            wchar_t day[16] = {};
            GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd", day, 16, nullptr);
            float dx = r.left + dw * i + dw / 2;
            c.Icon(WgWxIcon(dy.code, true), {dx - 20, y + 18}, 16, t.textDim, true);
            c.Text(day, Rect(dx - 8, y + 10, 40, 16), Weight::Medium, 11.5f, t.text);
            c.Text(WgDeg(dy.hiC, fahrenheit) + L" " + WgDeg(dy.loC, fahrenheit), Rect(dx - 30, y + 28, 60, 14), Weight::Regular, 10.5f, t.textDim, Align::Center);
        }
        break;
    }
    case kWgStats: {
        head(L"THIS PC");
        float y = r.top + 30;
        auto bar = [&](const wchar_t* label, const std::wstring& value, double v) {
            c.Text(label, Rect(r.left, y, 120, 16), Weight::Medium, 12.5f, t.text);
            c.Text(value, Rect(r.left, y, Width(r), 16), Weight::Regular, 12, t.textDim, Align::Right);
            D2D1_RECT_F lane = Rect(r.left, y + 22, Width(r), 6);
            c.Fill(lane, 3, t.text.Alpha(0.1f));
            c.Fill(Rect(lane.left, lane.top, (std::max)(6.0f, Width(lane) * anim::Clamp01((float)v)), 6), 3, v > 0.85 ? Color::Rgb(0xFF6B6B) : t.accentHi);
            y += 42;
        };
        wchar_t buf[64];
        swprintf_s(buf, L"%.0f%%  ·  %d threads", stats.cpu * 100, stats.threads);
        bar(L"Processor", buf, stats.cpu);
        swprintf_s(buf, L"%.1f of %.1f GB", stats.memUsedGb, stats.memTotalGb);
        bar(L"Memory", buf, stats.mem);
        if (stats.gpu >= 0) swprintf_s(buf, L"%.0f%%", stats.gpu * 100), bar(L"Graphics", buf, stats.gpu);
        break;
    }
    case kWgClip: {
        head(L"CLIPBOARD");
        if (LabelButton(t, Ui::Id("wg.clip.clear"), Rect(r.right - 86, r.top - 6, 86, 26), glyph::Delete, L"Clear")) clips.clear(), Redraw();
        float y = r.top + kHead;
        if (clips.empty()) c.Text(L"Copy some text and it shows here", Rect(r.left, y + 6, Width(r), 18), Weight::Regular, 12.5f, t.textDim, Align::Center);
        int pick = -1, drop = -1;
        for (size_t i = 0; i < clips.size() && i < (size_t)kListRows; ++i, y += kRow) {
            auto [rr, h] = row(Ui::Id("wg.clip", (int)i), y);
            c.Text(clips[i].substr(0, clips[i].find_first_of(L"\r\n")), Inset(rr, 10, 0), Weight::Regular, 12.5f, t.text);
            if (h.clicked) pick = (int)i;
            if (input.rclick && Inside(rr, input.mouse)) drop = (int)i;
        }
        c.Text(L"Click to copy again  ·  right click to forget", Rect(r.left, r.bottom - 18, Width(r), 14), Weight::Regular, 10.5f, t.textLow, Align::Center);
        if (pick >= 0 && OpenClipboard(hwnd)) { // back onto the clipboard, ready to paste
            const std::wstring s = clips[pick];
            if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, (s.size() + 1) * sizeof(wchar_t))) {
                memcpy(GlobalLock(g), s.c_str(), (s.size() + 1) * sizeof(wchar_t));
                GlobalUnlock(g);
                EmptyClipboard();
                if (!SetClipboardData(CF_UNICODETEXT, g)) GlobalFree(g);
            }
            CloseClipboard();
            SetPanel(Panel::None);
        } else if (drop >= 0) {
            clips.erase(clips.begin() + drop), Redraw(); // right click: forgotten
        }
        closeOnRight = false;
        break;
    }
    case kWgFiles:
    case kWgFolder: {
        static const wchar_t* const kNames[] = {L"DOCUMENTS", L"DESKTOP", L"PICTURES", L"MUSIC", L"VIDEOS"};
        head(wgKind == kWgFiles ? L"DOWNLOADS" : kNames[folderPick]);
        if (LabelButton(t, Ui::Id("wg.files.open"), Rect(r.right - 96, r.top - 6, 96, 26), glyph::OpenNew, L"Open")) {
            std::wstring dir = filesDir;
            SetPanel(Panel::None);
            ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            break;
        }
        float y = r.top + kHead, gx = (r.left + r.right) / 2 - kFileCols * kFileCell / 2;
        if (files.empty()) c.Text(L"Nothing in here yet", Rect(r.left, y + 20, Width(r), 18), Weight::Regular, 12.5f, t.textDim, Align::Center);
        int open = -1, show = -1;
        for (size_t i = 0; i < files.size(); ++i) {
            FileRow& f = files[i];
            D2D1_RECT_F cell = Rect(gx + (i % kFileCols) * kFileCell + 3, y + (i / kFileCols) * kFileRowH, kFileCell - 6, kFileRowH - 6);
            Ui::Hit h = ui.Button(Ui::Id("wg.file", (int)i), cell);
            if (h.hover) c.Fill(cell, 12, t.text.Alpha(0.07f));
            if (!f.icon && f.pixels && ctx) ctx->CreateBitmapFromWicBitmap(f.pixels.Get(), nullptr, &f.icon);
            D2D1_RECT_F ic = Rect((cell.left + cell.right) / 2 - 18, cell.top + 8, 36, 36);
            if (f.icon) c.Pixels(f.icon.Get(), ic, 1);
            else c.Icon(glyph::SysFolder, {(ic.left + ic.right) / 2, (ic.top + ic.bottom) / 2}, 26, t.textDim);
            c.Text(f.name, Rect(cell.left + 4, ic.bottom + 6, Width(cell) - 8, 14), Weight::Regular, 10.5f, t.text, Align::Center);
            if (h.clicked) open = (int)i;
            if (input.rclick && Inside(cell, input.mouse)) show = (int)i;
        }
        if (open >= 0) {
            std::wstring path = files[open].path;
            SetPanel(Panel::None);
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        } else if (show >= 0) { // right click: shown in explorer, selected
            if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(files[show].path.c_str())) SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0), ILFree(pidl);
            SetPanel(Panel::None);
        }
        closeOnRight = false;
        break;
    }
    case kWgApps: { // the apps the dock had no room for
        head(L"MORE APPS");
        if (!tasks) break;
        const auto& apps = tasks->Apps();
        float y = r.top + kHead, gx = (r.left + r.right) / 2 - kFileCols * kFileCell / 2;
        int pick = -1, menu = -1;
        for (int i = dockCount; i < (int)apps.size(); ++i) {
            int k = i - dockCount;
            const TaskApp& a = apps[(size_t)i];
            D2D1_RECT_F cell = Rect(gx + (k % kFileCols) * kFileCell + 3, y + (k / kFileCols) * kFileRowH, kFileCell - 6, kFileRowH - 6);
            Ui::Hit h = ui.Button(Ui::Id("wg.more", k), cell);
            if (h.hover) c.Fill(cell, 12, t.text.Alpha(0.07f));
            float mx = (cell.left + cell.right) / 2;
            DrawApp(&a, Rect(mx - 18, cell.top + 8, 36, 36), 1, t, (UINT)std::lround(36 * scale));
            if (!a.wins.empty()) c.Fill(Rect(mx - 4, cell.top + 48, 8, 3), 1.5f, t.text.Alpha(0.45f)); // running, as the dock's dot
            c.Text(a.name, Rect(cell.left + 4, cell.top + 56, Width(cell) - 8, 14), Weight::Regular, 10.5f, t.text, Align::Center);
            if (h.clicked) pick = i;
            if (input.rclick && Inside(cell, input.mouse)) menu = i;
        }
        if (pick >= 0) {
            SetPanel(Panel::None);
            tasks->Click(pick);
        } else if (menu >= 0) { // its menu, as in the dock
            menuApp = apps[(size_t)menu].key;
            PostMessageW(hwnd, WM_APP_MENU, 4, 0);
        }
        closeOnRight = false;
        break;
    }
    case kWgNotes: {
        head(L"NOTE");
        D2D1_RECT_F box = Rect(r.left - 4, r.top + 22, Width(r) + 8, Height(r) - 24);
        c.Fill(box, 12, t.text.Alpha(0.05f));
        std::wstring shown = note + ((std::fmod(ui.Now(), 1.0) < 0.55) ? L"│" : L" "); // a blinking caret at the end
        ui.KeepAlive();
        ComPtr<IDWriteTextLayout> lay;
        if (SUCCEEDED(gfx::DWrite()->CreateTextLayout(shown.c_str(), (UINT32)shown.size(), gfx::TextFmt(Weight::Regular, 13, Align::Left), Width(box) - 20, Height(box) - 16, &lay))) {
            lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            ComPtr<ID2D1SolidColorBrush> ink;
            rt->CreateSolidColorBrush(D2D1::ColorF(t.text.r, t.text.g, t.text.b, t.text.a), &ink);
            if (ink) rt->DrawTextLayout({box.left + 10, box.top + 8}, lay.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        }
        if (note.empty()) c.Text(L"Type a note… it's kept between restarts", Rect(box.left + 22, box.top + 8, Width(box) - 30, 18), Weight::Regular, 13, t.textLow);
        break;
    }
    case kWgTodo: {
        head(L"TO-DO");
        float y = r.top + kHead;
        int flip = -1, drop = -1;
        for (size_t i = 0; i < todos.size() && i < (size_t)kListRows - 1; ++i, y += kRow) {
            auto [rr, h] = row(Ui::Id("wg.todo", (int)i), y);
            D2D1_POINT_2F o = {rr.left + 16, (rr.top + rr.bottom) / 2};
            c.Ring(o, 7, todos[i].done ? t.accent : t.textDim, 1.5f);
            if (todos[i].done) c.Dot(o, 4.5f, t.accent);
            c.Text(todos[i].text, Rect(rr.left + 32, rr.top, Width(rr) - 40, Height(rr)), Weight::Regular, 12.5f, todos[i].done ? t.textLow : t.text);
            if (todos[i].done) c.Line({rr.left + 32, o.y}, {(std::min)(rr.right - 8, rr.left + 32 + TextW(todos[i].text, Weight::Regular, 12.5f)), o.y}, t.textLow, 1);
            if (h.clicked) flip = (int)i;
            if (input.rclick && Inside(rr, input.mouse)) drop = (int)i;
        }
        field(y + 4, L"Type a to-do and press Enter");
        c.Text(L"Click to tick  ·  right click to delete", Rect(r.left, r.bottom - 16, Width(r), 14), Weight::Regular, 10.5f, t.textLow, Align::Center);
        if (flip >= 0) todos[flip].done = !todos[flip].done, SaveLists(), Redraw();
        if (drop >= 0) todos.erase(todos.begin() + drop), SaveLists(), Redraw();
        closeOnRight = false;
        break;
    }
    case kWgHabit: {
        head(L"HABITS");
        int today = WgToday(), yesterday = WgDayShift(today, -1);
        float y = r.top + kHead;
        int flip = -1, drop = -1;
        for (size_t i = 0; i < habits.size() && i < (size_t)kListRows - 1; ++i, y += kRow) {
            auto [rr, h] = row(Ui::Id("wg.habit", (int)i), y);
            bool done = habits[i].last == today;
            D2D1_POINT_2F o = {rr.left + 16, (rr.top + rr.bottom) / 2};
            c.Fill(Rect(o.x - 8, o.y - 8, 16, 16), 5, done ? t.accent : t.text.Alpha(0.1f));
            if (done) c.Icon(glyph::Check, o, 13, t.onAccent);
            c.Text(habits[i].name, Rect(rr.left + 32, rr.top, Width(rr) - 120, Height(rr)), Weight::Regular, 12.5f, t.text);
            int streak = habits[i].last >= yesterday ? habits[i].streak : 0; // a missed day breaks it
            c.Text(std::to_wstring(streak) + (streak == 1 ? L" day" : L" days"), Rect(rr.left, rr.top, Width(rr) - 12, Height(rr)), Weight::Semi, 11.5f, streak ? t.accentHi : t.textLow, Align::Right);
            if (h.clicked) flip = (int)i;
            if (input.rclick && Inside(rr, input.mouse)) drop = (int)i;
        }
        field(y + 4, L"Name a new habit and press Enter");
        if (flip >= 0) { // done today, or undone
            Habit& hb = habits[flip];
            if (hb.last == today) hb.streak = (std::max)(0, hb.streak - 1), hb.last = hb.streak ? yesterday : 0;
            else hb.streak = (hb.last == yesterday ? hb.streak : 0) + 1, hb.last = today;
            SaveLists(), Redraw();
        }
        if (drop >= 0) habits.erase(habits.begin() + drop), SaveLists(), Redraw();
        closeOnRight = false;
        break;
    }
    case kWgCalendar: {
        head(L"TODAY");
        SYSTEMTIME st;
        GetLocalTime(&st);
        std::vector<sys::cal::Event> today = cals.Day(st.wYear, st.wMonth, st.wDay);
        float y = r.top + kHead;
        if (today.empty()) c.Text(L"Nothing on today", Rect(r.left, y + 6, Width(r), 18), Weight::Regular, 12.5f, t.textDim, Align::Center);
        for (size_t i = 0; i < today.size() && i < (size_t)kListRows; ++i, y += kRow) {
            const auto& e = today[i];
            std::wstring when = L"All day";
            if (!e.allDay) {
                ULONGLONG u = ((ULONGLONG)e.start + 11644473600ULL) * 10000000ULL;
                FILETIME f = {(DWORD)u, (DWORD)(u >> 32)};
                SYSTEMTIME utc, loc;
                FileTimeToSystemTime(&f, &utc);
                SystemTimeToTzSpecificLocalTime(nullptr, &utc, &loc);
                wchar_t tm[16] = {};
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &loc, nullptr, tm, 16);
                when = tm;
            }
            c.Fill(Rect(r.left, y + 7, 3, kRow - 14), 1.5f, t.accent);
            c.Text(when, Rect(r.left + 12, y, 74, kRow), Weight::Semi, 12, t.textDim);
            c.Text(e.title, Rect(r.left + 90, y, Width(r) - 90, kRow), Weight::Regular, 12.5f, t.text);
        }
        if (LabelButton(t, Ui::Id("wg.cal.all"), Rect(r.left + Width(r) / 2 - 60, r.bottom - 30, 120, 26), glyph::Calendar, L"Calendar")) Anchor(anchorX, false), SetPanel(Panel::Calendar);
        break;
    }
    case kWgClock: { // the world's time: a few places
        head(L"WORLD CLOCK");
        struct Zone {
            const wchar_t* key;
            const wchar_t* name;
        };
        static const Zone kZones[] = {{L"UTC", L"UTC"}, {L"GMT Standard Time", L"London"}, {L"Eastern Standard Time", L"New York"}, {L"Tokyo Standard Time", L"Tokyo"}};
        SYSTEMTIME utc;
        GetSystemTime(&utc);
        DYNAMIC_TIME_ZONE_INFORMATION tz{};
        for (DWORD i = 0; EnumDynamicTimeZoneInformation(i, &tz) == ERROR_SUCCESS; ++i)
            for (int z = 0; z < 4; ++z) {
                if (wcscmp(tz.TimeZoneKeyName, kZones[z].key)) continue;
                SYSTEMTIME loc;
                if (!SystemTimeToTzSpecificLocalTimeEx(&tz, &utc, &loc)) continue;
                wchar_t tm[16] = {};
                GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &loc, nullptr, tm, 16);
                float ry = r.top + kHead + z * 36;
                c.Text(kZones[z].name, Rect(r.left, ry, 160, 30), Weight::Medium, 13, t.text);
                c.Text(tm, Rect(r.left, ry, Width(r), 30), Weight::Semi, 15, t.accentHi, Align::Right);
            }
        break;
    }
    case kWgProgress: {
        head(L"PROGRESS");
        static const wchar_t* const kNames[] = {L"Today", L"This week", L"This month", L"This year"};
        float y = r.top + kHead + 4;
        for (int i = 0; i < 4; ++i, y += 36) {
            double v = WgFrac(i);
            c.Text(kNames[i], Rect(r.left, y, 140, 16), Weight::Medium, 12.5f, t.text);
            c.Text(std::to_wstring((int)(v * 100)) + L"%", Rect(r.left, y, Width(r), 16), Weight::Semi, 12, t.accentHi, Align::Right);
            D2D1_RECT_F lane = Rect(r.left, y + 20, Width(r), 6);
            c.Fill(lane, 3, t.text.Alpha(0.1f));
            c.Fill(Rect(lane.left, lane.top, (std::max)(6.0f, Width(lane) * (float)v), 6), 3, t.accentHi);
        }
        break;
    }
    }
    if (closeOnRight && ui.RClick(pill) && input.mouse.y < rowTop) SetPanel(Panel::None); // right click closes, as the other panels
    rt->PopLayer();
}

void Bar::DrawGallery(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    auto chip = [&](uint32_t id, const D2D1_RECT_F& b, const std::wstring& label, bool on, bool can = true) { // a pill choice
        Ui::Hit h = can ? ui.Button(id, b) : Ui::Hit{};
        c.Fill(b, Height(b) / 2, on ? t.accent : t.text.Alpha(h.hover ? 0.12f : 0.07f));
        c.Text(label, b, Weight::Medium, 11.5f, on ? t.onAccent : can ? t.textDim.Mix(t.text, h.hover ? 0.6f : 0) : t.textLow, Align::Center);
        return h.clicked;
    };
    c.TextSp(L"CUSTOMIZE DOCK", Rect(r.left, r.top, 240, 14), Weight::Medium, 10, t.textLow, 1.4f);
    if (chip(Ui::Id("gal.done"), Rect(r.right - 70, r.top - 6, 70, 26), L"Done", true)) {
        SetPanel(Panel::None);
        return;
    }

    // the picked widget: its size and its options, or how edit mode works
    float y = r.top + 26;
    D2D1_RECT_F sel = Rect(r.left - 6, y, Width(r) + 12, kSelH - 8);
    c.Fill(sel, 14, t.text.Alpha(0.05f));
    if (wgSel >= 0 && wgSel < (int)wgSlots.size()) {
        WgSlot& s = wgSlots[wgSel];
        const wg::Meta& m = wg::Info(s.kind);
        float x = sel.left + 12, cy = (sel.top + sel.bottom) / 2;
        c.Icon(m.glyph, {x + 10, cy}, 18, t.accentHi, true);
        c.Text(m.name, Rect(x + 26, sel.top, 120, Height(sel)), Weight::Semi, 12.5f, t.text);
        x += 150;
        static const wchar_t* const kSizes[] = {L"S", L"M", L"L"};
        for (int z = 0; z < 3; ++z) {
            bool can = (m.sizes >> z) & 1;
            if (chip(Ui::Id("gal.size", z), Rect(x + z * 34, cy - 12, 30, 24), kSizes[z], s.size == z && can, can) && s.size != z) s.size = z, SaveSlots();
        }
        x += 3 * 34 + 14;
        auto opt = [&](const char* id, const std::vector<std::wstring>& labels, int cur, auto set) { // one option: a row of choices
            float ox = x;
            for (size_t i = 0; i < labels.size(); ++i) {
                float w = TextW(labels[i], Weight::Medium, 11.5f) + 18;
                if (chip(Ui::Id(id, (int)i), Rect(ox, cy - 12, w, 24), labels[i], (int)i == cur)) set((int)i);
                ox += w + 5;
            }
        };
        static const int kMins[] = {15, 25, 45, 60}, kGoals[] = {6, 8, 10, 12};
        if (s.kind == kWgWeather) {
            opt("gal.wx", {L"°C", L"°F"}, fahrenheit ? 1 : 0, [&](int i) { fahrenheit = i == 1, prefs::SaveNum(L"WgFahrenheit", i), Redraw(); });
        } else if (s.kind == kWgFocus) {
            int cur = 1;
            for (int i = 0; i < 4; ++i) cur = kMins[i] == focusMin ? i : cur;
            opt("gal.focus", {L"15m", L"25m", L"45m", L"60m"}, cur, [&](int i) { focusMin = kMins[i], prefs::SaveNum(L"WgFocusMin", focusMin), Redraw(); });
        } else if (s.kind == kWgWater) {
            int cur = 1;
            for (int i = 0; i < 4; ++i) cur = kGoals[i] == waterGoal ? i : cur;
            opt("gal.water", {L"6", L"8", L"10", L"12"}, cur, [&](int i) { waterGoal = kGoals[i], prefs::SaveNum(L"WgWaterGoal", waterGoal), Redraw(); });
        } else if (s.kind == kWgFolder) {
            opt("gal.folder", {L"Docs", L"Desktop", L"Pics", L"Music", L"Videos"}, folderPick, [&](int i) { folderPick = i, prefs::SaveNum(L"WgFolder", i), Redraw(); });
        }
        if (s.kind != kWgApps && chip(Ui::Id("gal.remove"), Rect(sel.right - 92, cy - 12, 80, 24), L"Remove", false)) {
            wgSlots.erase(wgSlots.begin() + wgSel);
            wgSel = -1;
            SaveSlots();
        }
    } else {
        c.Text(L"Click a widget in the dock to size it or set it up  ·  drag to reorder  ·  × to remove", Inset(sel, 14, 0), Weight::Regular, 11.5f, t.textDim, Align::Center);
    }
    y += kSelH;

    // the categories, then every widget in the chosen one as a card with a live preview
    float tx = r.left;
    for (int k = 0; k < wg::kCatCount; ++k) {
        float w = TextW(wg::CatName(k), Weight::Medium, 11.5f) + 22;
        if (chip(Ui::Id("gal.cat", k), Rect(tx, y, w, kTabsH - 6), wg::CatName(k), galleryCat == k)) galleryCat = k, galleryScroll = 0;
        tx += w + 6;
    }
    y += kTabsH + 2;
    D2D1_RECT_F grid = {r.left - 6, y, r.right + 6, r.bottom};
    std::vector<int> kinds;
    for (int k = 0; k < kWgCount; ++k)
        if (k != kWgRetired && k != kWgApps && (galleryCat == wg::kCatAll || wg::Info(k).cat == galleryCat)) kinds.push_back(k);
    int rows = ((int)kinds.size() + kGalCols - 1) / kGalCols;
    float maxScroll = (std::max)(0.0f, rows * (kCardH + kGalGap) - Height(grid));
    if (float n = ui.Wheel(grid); n != 0) galleryScroll = (std::clamp)(galleryScroll - n * (kCardH + kGalGap) * 0.6f, 0.0f, maxScroll);
    float scroll = ui.Smooth(Ui::Id("gal.scroll"), galleryScroll, 18);
    float cw = (Width(grid) - (kGalCols - 1) * kGalGap) / kGalCols;
    c.PushClip(grid);
    int toggle = -1;
    for (size_t i = 0; i < kinds.size(); ++i) {
        int k = kinds[i];
        float cy = grid.top + (i / kGalCols) * (kCardH + kGalGap) - scroll;
        if (cy + kCardH < grid.top || cy > grid.bottom) continue; // out of view: not drawn, not hit
        D2D1_RECT_F card = Rect(grid.left + (i % kGalCols) * (cw + kGalGap), cy, cw, kCardH);
        const wg::Meta& m = wg::Info(k);
        bool in = HasWg(k) && k != kWgDivider; // dividers are never "in": each click adds another
        Ui::Hit h = ui.Button(Ui::Id("gal.card", k), card);
        c.Fill(card, 16, t.text.Alpha(h.hover ? 0.09f : 0.05f));
        if (in) c.Stroke(card, 16, t.accent.Alpha(0.8f), 1.5f);
        else c.StrokeV(card, 16, t.hairTop, t.hairBot, 1);
        int size = (m.sizes & 2) ? 1 : 0; // previewed at medium when it comes in it
        float pw = (std::min)(WidgetW(k, size), Width(card) - 20);
        D2D1_RECT_F pv = Rect((card.left + card.right) / 2 - pw / 2, card.top + 10, pw, kIcon);
        c.Fill(pv, TileRadius(), t.bg.Alpha(1).Mix(t.text, 0.04f)); // a sliver of dock under it
        c.PushClip(pv);
        DrawWg(t, k, size, pv, false); // its real face, live data and all
        c.PopClip();
        c.Text(m.name, Rect(card.left + 12, pv.bottom + 8, Width(card) - 40, 16), Weight::Semi, 12, t.text);
        c.Text(m.about, Rect(card.left + 12, pv.bottom + 25, Width(card) - 20, 14), Weight::Regular, 10, t.textDim);
        D2D1_POINT_2F o = {card.right - 16, pv.bottom + 16}; // in the dock or not
        c.Dot(o, 8, in ? t.accent : t.text.Alpha(0.1f));
        c.Icon(in ? glyph::Check : glyph::Add, o, 12, in ? t.onAccent : t.textDim);
        if (h.clicked) toggle = k;
    }
    c.PopClip();
    if (toggle >= 0) { // in or out of the dock, at the end of the widgets, at its usual size
        auto it = std::find_if(wgSlots.begin(), wgSlots.end(), [&](const WgSlot& s) { return s.kind == toggle; });
        if (it != wgSlots.end() && toggle != kWgDivider) wgSlots.erase(it), wgSel = -1;
        else wgSlots.push_back({toggle, wg::Info(toggle).defSize}), wgSel = (int)wgSlots.size() - 1;
        SaveSlots();
    }
}
