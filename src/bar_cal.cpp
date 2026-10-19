// The calendar panel's events: the day's agenda under the month, and connecting a calendar by copying its private link.
#include "bar.h"

#include <shellapi.h>

#include "glyphs.h"

namespace {

constexpr int kAgendaRows = 3; // what fits under the month; more shows as a count
constexpr float kAgendaHead = 26, kAgendaRow = 26, kTimeW = 66; // dips; the time column fits "12:45 PM"

SYSTEMTIME LocalOf(int64_t unix) {
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)(unix + 11644473600LL) * 10000000ULL; // unix seconds to filetime
    FILETIME ft{u.LowPart, u.HighPart};
    SYSTEMTIME utc{}, loc{};
    FileTimeToSystemTime(&ft, &utc);
    SystemTimeToTzSpecificLocalTimeEx(nullptr, &utc, &loc); // the ex form applies the dst rule of that date, not today's
    return loc;
}

LONGLONG DayNumber(const SYSTEMTIME& st) { // whole days, for "today" and "tomorrow"
    SYSTEMTIME d = st;
    d.wHour = d.wMinute = d.wSecond = d.wMilliseconds = 0; // midnight, so two times on one day give one number
    FILETIME ft;
    SystemTimeToFileTime(&d, &ft);
    return (LONGLONG)(((ULONGLONG)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / (86400ULL * 10000000ULL));
}

std::wstring DayName(int y, int m, int d) { // "Today", "Tomorrow", or the date in the user's own words
    SYSTEMTIME now;
    GetLocalTime(&now);
    SYSTEMTIME st{};
    st.wYear = (WORD)y, st.wMonth = (WORD)m, st.wDay = (WORD)d;
    LONGLONG diff = DayNumber(st) - DayNumber(now);
    if (diff == 0) return L"Today";
    if (diff == 1) return L"Tomorrow";
    if (diff == -1) return L"Yesterday";
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    FileTimeToSystemTime(&ft, &st); // fills in the weekday
    wchar_t buf[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", buf, 64, nullptr); // short day and month names in the user's language
    return buf;
}

std::wstring Host(const std::wstring& url) { // which calendar it is, without showing the private part of its link
    size_t b = url.find(L"://"), cal = url.find(L"/ical/");
    if (cal != std::wstring::npos) { // google: the calendar's own address, e.g. someone@gmail.com
        size_t s = cal + 6, e = url.find(L'/', s);
        std::wstring id = url.substr(s, e == std::wstring::npos ? std::wstring::npos : e - s);
        for (size_t at; (at = id.find(L"%40")) != std::wstring::npos;) id.replace(at, 3, L"@"); // the link carries the @ url-encoded
        return id.size() > 40 ? id.substr(0, 37) + L"..." : id; // shared calendars have long random ids
    }
    b = b == std::wstring::npos ? 0 : b + 3;
    return url.substr(b, url.find(L'/', b) - b); // anything else: just its host, the path is the secret
}

std::wstring ClipText(HWND owner) {
    std::wstring text;
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT) || !OpenClipboard(owner)) return text;
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
        if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(h))) {
            text.assign(p, wcsnlen(p, 4096)); // a link is short; a huge copy is not one
            GlobalUnlock(h);
        }
    }
    CloseClipboard(); // promptly: other apps can't touch the clipboard while it's open
    return text;
}

}

void Bar::StartCalWait(bool openGoogle) {
    if (!calWaiting && !clipListening) AddClipboardFormatListener(hwnd); // the link the user copies next, from any app, is the one
    calWaiting = true;
    SetTimer(hwnd, kCalWaitTimer, kCalWaitMs, nullptr); // gives up after a while, so the listener doesn't linger
    if (openGoogle) ShellExecuteW(nullptr, L"open", sys::cal::Calendars::GoogleSettings(), nullptr, nullptr, SW_SHOWNORMAL); // the settings page that shows the secret link
    OnClipboard(); // it may already be copied
    Redraw();
}

void Bar::StopCalWait() {
    if (!calWaiting) return;
    if (!clipListening) RemoveClipboardFormatListener(hwnd); // stop hearing every copy once there's nothing to wait for, unless the clipboard widget listens
    KillTimer(hwnd, kCalWaitTimer);
    calWaiting = false;
}

void Bar::OnClipboard() {
    if (!calWaiting) return;
    std::wstring text = ClipText(hwnd);
    if (!sys::cal::Calendars::LooksLikeCalendar(text) || !cals.Add(text, hwnd, WM_APP_CAL)) return; // anything else copied meanwhile is left alone
    StopCalWait();
    SetPanel(Panel::Calendar); // connected: the calendar rises to show it, its events arriving a moment later
}

void Bar::CalMenu() { // what's connected, to add another or let one go
    auto links = cals.Links();
    constexpr UINT kAddGoogle = 1, kAddAny = 2, kRemove = 100; // remove ids are kRemove + index, 0 means dismissed
    HMENU m = CreatePopupMenu();
    if (!m) return;
    if (!links.empty()) AppendMenuW(m, MF_STRING | MF_GRAYED, 0, L"Remove a calendar");
    for (size_t i = 0; i < links.size() && i < 50; ++i) AppendMenuW(m, MF_STRING, kRemove + (UINT)i, (L"    Remove " + links[i].name + L"  (" + Host(links[i].url) + L")").c_str());
    if (!links.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kAddGoogle, L"Connect Google Calendar");
    AppendMenuW(m, MF_STRING, kAddAny, L"Connect from a copied link (Outlook, iCloud...)");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_NONOTIFY, p.x, p.y, hwnd, nullptr); // grows up from the bar
    DestroyMenu(m);
    HWND back = tasks ? tasks->Fore() : nullptr; // the bar never keeps the keyboard
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick == kAddGoogle) StartCalWait(true);
    else if (pick == kAddAny) StartCalWait(false);
    else if (pick >= kRemove && pick - kRemove < links.size()) cals.Remove(pick - kRemove, hwnd, WM_APP_CAL);
    Redraw();
}

void Bar::DrawAgenda(const Theme& t, const D2D1_RECT_F& r, int y, int m, int d) { // the picked day's events, or how to connect a calendar
    Canvas& c = ui.C();
    if (!cals.Linked()) { // nothing connected yet: the empty state teaches how
        float cy = r.top + Height(r) / 2;
        if (calWaiting) {
            c.Text(L"Copy the “Secret address in iCal format”", Rect(r.left + 4, cy - 26, Width(r) - 8, 18), Weight::Medium, 12, t.text, Align::Center);
            c.Text(L"It connects on its own. Any calendar link works.", Rect(r.left + 4, cy - 6, Width(r) - 8, 16), Weight::Regular, 11, t.textDim, Align::Center);
            D2D1_RECT_F cancel = Rect(r.left + Width(r) / 2 - 40, cy + 14, 80, 22);
            if (ui.Button(Ui::Id("cal.wait.x"), cancel).clicked) StopCalWait(), Redraw();
            c.Text(L"Cancel", cancel, Weight::Medium, 11, t.accentHi, Align::Center);
            return;
        }
        D2D1_RECT_F btn = Rect(r.left + Width(r) / 2 - 110, cy - 22, 220, 34);
        uint32_t id = Ui::Id("cal.connect");
        Ui::Hit h = ui.Button(id, btn);
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        c.Fill(btn, 17, t.accent.Alpha(0.22f + 0.14f * hov));
        c.Icon(glyph::Calendar, {btn.left + 24, (btn.top + btn.bottom) / 2}, 17, t.accentHi);
        c.Text(L"Connect Google Calendar", Rect(btn.left + 40, btn.top, Width(btn) - 46, Height(btn)), Weight::Medium, 12, t.text);
        c.Text(L"Outlook, iCloud or any calendar link: right-click", Rect(r.left + 4, btn.bottom + 6, Width(r) - 8, 16), Weight::Regular, 10.5f, t.textLow, Align::Center);
        if (h.clicked) StartCalWait(true); // opens google's settings and listens for the copy
        if (ui.RClick(btn)) PostMessageW(hwnd, WM_APP_CALMENU, 0, 0); // menus run their own loop, so not from inside the frame
        return;
    }
    auto events = cals.Day(y, m, d);
    D2D1_RECT_F headR = Rect(r.left + 4, r.top, Width(r) - 66, kAgendaHead - 6);
    c.Text(DayName(y, m, d), headR, Weight::Semi, 12, t.text);
    if (events.size() > (size_t)kAgendaRows) c.Text(L"+" + std::to_wstring(events.size() - kAgendaRows) + L" more", headR, Weight::Medium, 11, t.textDim, Align::Right);
    if (w::IconBtn(ui, t, Ui::Id("cal.new"), {r.right - 40, r.top + 9}, 24, glyph::Add, 17)) // google's own new-event page: saved there, it's in the account and shows here on the next sync
        ShellExecuteW(nullptr, L"open", sys::cal::Calendars::NewEventUrl(y, m, d).c_str(), nullptr, nullptr, SW_SHOWNORMAL), SetPanel(Panel::None);
    if (w::IconBtn(ui, t, Ui::Id("cal.links"), {r.right - 12, r.top + 9}, 24, glyph::Link, 15)) PostMessageW(hwnd, WM_APP_CALMENU, 0, 0); // connect another, or remove one
    float ry = r.top + kAgendaHead;
    if (events.empty()) {
        c.Text(cals.Busy() ? L"Syncing…" : L"Nothing on", Rect(r.left + 4, ry, Width(r) - 8, kAgendaRow), Weight::Regular, 12, t.textDim);
        return;
    }
    const wchar_t* fmt = cfg.clock24 == 1 ? L"HH:mm" : cfg.clock24 == 0 ? L"h:mm tt" : nullptr; // as the bar's own clock
    for (size_t i = 0; i < events.size() && i < (size_t)kAgendaRows; ++i, ry += kAgendaRow) {
        const auto& e = events[i];
        std::wstring when = L"All day";
        if (!e.allDay) {
            SYSTEMTIME st = LocalOf(e.start);
            wchar_t buf[32] = {};
            GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, TIME_NOSECONDS, &st, fmt, buf, 32); // a null format uses the region's own
            when = buf;
        }
        c.Fill(Rect(r.left + 2, ry + 6, 3, kAgendaRow - 12), 1.5f, t.accent);
        c.Text(when, Rect(r.left + 12, ry, kTimeW, kAgendaRow), Weight::Medium, 11.5f, t.accentHi);
        c.Text(e.title.empty() ? std::wstring(L"(No title)") : e.title, Rect(r.left + 12 + kTimeW, ry, Width(r) - 16 - kTimeW, kAgendaRow), Weight::Medium, 12, t.text);
    }
}
