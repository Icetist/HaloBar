// The two bars: the top bar across the screen, its modules left, center and right, in island's material,
// and the dock as an island-like pill over the bottom edge. With the top bar off, its status rides at the dock's end.
#include "bar.h"

#include <shellapi.h>
#include <sddl.h>
#include <shlobj.h>

#include <cmath>
#include <cwctype>

#include "glyphs.h"
#include "overflow.h"

namespace {

constexpr float kDot = 7, kDotOn = 20, kDotGap = 7; // a workspace: a dot, the one in front a wider accent pill
constexpr float kPlus = 18; // the add-a-desktop button
constexpr float kIconGap = 6; // between a module's icons, and before its text
constexpr wchar_t kDesksKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\VirtualDesktops";
constexpr UINT kPowerLock = 1, kPowerSleep = 2, kPowerOut = 3, kPowerRestart = 4, kPowerOff = 5;

}

// ---- shared ----

void Bar::PaintPlate(const D2D1_RECT_F& r, float radius, const Theme& t, float shade) { // island's material on a capsule or the dock
    Canvas& c = *canvas;
    c.Fill(r, radius, t.bg);
    if (t.glass) {
        if (shade > 0.01f) c.Fill(r, radius, Color{0.02f, 0.02f, 0.04f, 0.45f * shade}); // smoked over a white page, so the ink still reads
        c.FillV(r, radius, t.sheen, t.sheen.Alpha(0));
        c.Stroke(r, radius, Color{1, 1, 1, 0.24f}, 1.1f);
    } else {
        c.StrokeV(r, radius, t.hairTop, t.hairBot, 1); // island's hairline: lit from above
    }
}

ComPtr<ID2D1Geometry> Bar::Chrome() {
    ID2D1Geometry* parts[4] = {};
    UINT n = 0;
    ComPtr<ID2D1RoundedRectangleGeometry> geo[4];
    auto add = [&](const D2D1_RECT_F& r, float radius) {
        if (r.right > r.left && SUCCEEDED(gfx::D2D()->CreateRoundedRectangleGeometry({r, radius, radius}, &geo[n]))) parts[n] = geo[n].Get(), ++n;
    };
    add(capL, 0); // the top bar's strip, square to the screen's edges
    add(wegDrawn, WegRadius());
    D2D1_RECT_F c = CardOnScreen();
    add(c, (std::min)(CardRadius(), Height(c) / 2));
    ComPtr<ID2D1GeometryGroup> g;
    if (n == 0 || FAILED(gfx::D2D()->CreateGeometryGroup(D2D1_FILL_MODE_WINDING, parts, n, &g))) return nullptr;
    return g;
}

bool Bar::Module(uint32_t id, const D2D1_RECT_F& r, bool lit) {
    Ui::Hit h = ui.Button(id, r);
    float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 18), on = ui.Smooth(id + 2, lit ? 1.0f : 0, 16);
    Canvas& c = ui.C();
    float rad = Height(r) / 2; // pills, as island's buttons
    if (on > 0.01f) c.Fill(r, rad, shellNow.accent.Alpha(0.26f * on)); // its panel is open: an accent wash
    if (hov > 0.01f) c.Fill(r, rad, shellNow.text.Alpha(0.09f * hov));
    return h.clicked;
}

float Bar::ModuleW(const std::wstring& text, int glyphs, Weight weight) {
    float w = 2 * kItemPad + glyphs * kBarIcon + (std::max)(0, glyphs - 1) * kIconGap;
    if (text.empty()) return w;
    std::wstring key = text + L'\x1' + std::to_wstring((int)weight); // the same words in another weight measure differently
    auto it = textWs.find(key);
    if (it == textWs.end()) { // measured once per string: the clock changes once a minute, titles now and then
        if (textWs.size() > 64) textWs.clear();
        it = textWs.emplace(key, TextW(text, weight, kBarText)).first;
    }
    return w + it->second + (glyphs > 0 ? kIconGap : 0);
}

void Bar::SyncClockText() { // formatted once a minute, never per frame
    SYSTEMTIME st;
    GetLocalTime(&st);
    int key = ((st.wDay * 24 + st.wHour) * 60 + st.wMinute) * 8 + (cfg.clock24 + 1) * 2 + cfg.date; // a format change formats again too
    if (key == clockMin) return;
    clockMin = key;
    wchar_t tm[32] = {}, day[48] = {};
    const wchar_t* fmt = cfg.clock24 == 1 ? L"HH:mm" : cfg.clock24 == 0 ? L"h:mm tt" : nullptr; // null: windows' own format
    GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, fmt ? 0 : TIME_NOSECONDS, &st, fmt, tm, 32);
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &st, L"ddd d MMM", day, 48, nullptr);
    clockTime = tm, clockDate = day;
    clockWBar = ModuleW(clockTime, 0, Weight::Semi) + (cfg.date ? kIconGap + ModuleW(clockDate, 0) - 2 * kItemPad : 0);
    clockWDock = 2 * kItemPad + (std::max)(TextW(clockTime, Weight::Semi, 12.5f), cfg.date ? TextW(clockDate, Weight::Regular, 10.5f) : 0.0f);
}

// ---- the top bar ----

void Bar::SyncDesktops() { // windows lists its virtual desktops by id; the one in front is named on its own
    BYTE ids[16 * 32];
    BYTE cur[16];
    DWORD size = sizeof(ids), csize = sizeof(cur);
    int n = 1, at = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kDesksKey, L"VirtualDesktopIDs", RRF_RT_REG_BINARY, nullptr, ids, &size) == ERROR_SUCCESS && size >= 16) {
        n = (int)(size / 16);
        if (RegGetValueW(HKEY_CURRENT_USER, kDesksKey, L"CurrentVirtualDesktop", RRF_RT_REG_BINARY, nullptr, cur, &csize) == ERROR_SUCCESS && csize == 16)
            for (int i = 0; i < n; ++i)
                if (!memcmp(ids + i * 16, cur, 16)) at = i;
    }
    if (n != desks || at != desk) desks = n, desk = at, Redraw();
}

void Bar::SyncUser() { // the signed-in user's name and account picture, for the launcher's foot
    wchar_t name[256];
    DWORD n = 256;
    if (GetUserNameW(name, &n)) userName = name;
    HANDLE tok = nullptr;
    BYTE buf[256];
    DWORD got = 0;
    LPWSTR sid = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok) && GetTokenInformation(tok, TokenUser, buf, sizeof(buf), &got) &&
        ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(buf)->User.Sid, &sid)) {
        std::wstring key = std::wstring(L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\AccountPicture\\Users\\") + sid; // windows keeps each size's file here
        wchar_t path[MAX_PATH] = {};
        DWORD ps = sizeof(path);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, key.c_str(), L"Image96", RRF_RT_REG_SZ, nullptr, path, &ps) == ERROR_SUCCESS) avatarSrc = gfx::DecodeFile(path, 96);
        LocalFree(sid);
    }
    if (tok) CloseHandle(tok);
    avatarBmp.Reset();
}

void Bar::ShowUserMenu() { // the halo menu: the usual folders, halobar itself, then the session
    struct Folder {
        const wchar_t* label;
        const KNOWNFOLDERID* id;
    };
    static const Folder kFolders[] = {{L"Desktop", &FOLDERID_Desktop}, {L"Downloads", &FOLDERID_Downloads}, {L"Documents", &FOLDERID_Documents},
                                      {L"Pictures", &FOLDERID_Pictures}, {L"Music", &FOLDERID_Music}, {L"Videos", &FOLDERID_Videos}};
    constexpr UINT kAccount = 50, kLock = 51, kOut = 52, kOwn = 53;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING | MF_GRAYED, 0, userName.c_str());
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    for (UINT i = 0; i < ARRAYSIZE(kFolders); ++i) AppendMenuW(m, MF_STRING, 1 + i, kFolders[i].label);
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kOwn, L"HaloBar settings");
    AppendMenuW(m, MF_STRING, kAccount, L"Account settings");
    AppendMenuW(m, MF_STRING, kLock, L"Lock");
    AppendMenuW(m, MF_STRING, kOut, L"Sign out");
    POINT p{};
    if (TopOn()) p = ToScreen({capL.left, capL.bottom + 4});
    else GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_LEFTALIGN | (TopOn() ? TPM_TOPALIGN : TPM_BOTTOMALIGN) | TPM_NONOTIFY, p.x, p.y, hwnd, nullptr);
    DestroyMenu(m);
    HWND back = tasks ? tasks->Fore() : nullptr; // the bar never keeps the keyboard
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick >= 1 && pick <= ARRAYSIZE(kFolders)) {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*kFolders[pick - 1].id, 0, nullptr, &path))) ShellExecuteW(nullptr, L"open", path, nullptr, nullptr, SW_SHOWNORMAL);
        CoTaskMemFree(path);
    } else if (pick == kAccount) ShellExecuteW(nullptr, L"open", L"ms-settings:yourinfo", nullptr, nullptr, SW_SHOWNORMAL);
    else if (pick == kOwn) Anchor(W / 2, TopOn()), SetSettingsOpen(true);
    else if (pick == kLock) sys::power::Lock();
    else if (pick == kOut) sys::power::SignOut();
}

void Bar::ShowPowerMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, kPowerLock, L"Lock");
    AppendMenuW(m, MF_STRING, kPowerSleep, L"Sleep");
    AppendMenuW(m, MF_STRING, kPowerOut, L"Sign out");
    AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(m, MF_STRING, kPowerRestart, L"Restart");
    AppendMenuW(m, MF_STRING, kPowerOff, L"Shut down");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY, p.x, p.y, hwnd, nullptr);
    DestroyMenu(m);
    HWND back = tasks ? tasks->Fore() : nullptr; // the bar never keeps the keyboard
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick == kPowerLock) sys::power::Lock();
    else if (pick == kPowerSleep) sys::power::Sleep();
    else if (pick == kPowerOut) sys::power::SignOut();
    else if (pick == kPowerRestart) sys::power::Restart();
    else if (pick == kPowerOff) sys::power::Shutdown();
}

void Bar::OpenQuick(int page, float x) { // quick settings on one of its pages, from the module that asked; asked again, it closes
    if (qsOpen && qsPage == page && std::fabs(anchorX - x) < 1) return SetPanel(Panel::None);
    Anchor(x, TopOn()); // under the top bar, or rising over the dock when the status lives there
    if (!qsOpen) SetPanel(Panel::Quick);
    if (page != qsPage) OpenQuickPage(page);
}

float Bar::DrawStatus(const Theme& t, float at, float cy, float h, bool dock) {
    // what's shown, in order, and how wide: measured first, so the capsule can be painted under them
    struct Item {
        int mod;
        float w;
    };
    Item items[kModCount];
    int n = 0;
    auto add = [&](int m, float w) {
        if (Shows(m) && w > 0) items[n++] = {m, w};
    };
    bool hidden = tray && HiddenCount() > 0;
    std::wstring keys = langSub.empty() ? lang : lang + L" " + langSub;
    float keysW = 2 * kItemPad + (std::max)(TextW(lang, Weight::Semi, 11.5f), TextW(langSub, Weight::Regular, 10)); // stacked in the dock, as windows
    std::wstring pct = status.battery && status.percent >= 0 ? std::to_wstring(status.percent) + L"%" : L"";
    SyncClockText();
    add(kModTray, tray && TrayCount() > 0 ? (hidden ? ModuleW(L"", 1) : 0) + PromotedW() : 0);
    add(kModLang, lang.empty() ? 0 : dock ? keysW : ModuleW(keys, 0));
    add(kModBt, radios.HasBt() ? ModuleW(L"", 1) : 0);
    add(kModNet, ModuleW(L"", 1));
    add(kModVol, ModuleW(L"", 1));
    add(kModBat, status.battery ? ModuleW(pct, status.charging ? 2 : 1) : 0);
    add(kModBell, ModuleW(L"", 1));
    add(kModQs, ModuleW(L"", 1));
    add(kModClock, dock ? clockWDock : clockWBar);
    add(kModDesktop, dock ? 30.0f : 0); // show desktop, last: the far right, where windows keeps it
    float total = 0;
    for (int i = 0; i < n; ++i) total += items[i].w + (i ? kItemGap : 0);
    for (int m = kModTray; m < kModCount; ++m) modAt[m] = -1;
    trayR = {};
    if (n == 0) return 0;

    Canvas& c = ui.C();
    float x = dock ? at : at - total; // in the top bar, laid against the right edge
    float top = cy - h / 2, edge = dock ? wegR.top : barR.bottom; // panels and flyouts hang from here
    int unreadN = 0;
    for (const auto& [k, v] : unread) unreadN += v;
    bool news = unreadN > 0 || notifUnread;
    auto icon = [&](int g, const D2D1_RECT_F& r, float dx, const Color& ink) { c.Icon(g, {r.left + kItemPad + kBarIcon / 2 + dx, cy}, kBarIcon, ink); };
    for (int i = 0; i < n; ++i) {
        D2D1_RECT_F r = Rect(x, top, items[i].w, h);
        float mid = (r.left + r.right) / 2;
        int m = items[i].mod;
        modAt[m] = mid;
        x = r.right + kItemGap;
        switch (m) {
        case kModTray: { // the chevron to every icon, then the ones kept out in windows' settings
            if (hidden) {
                trayR = Rect(r.left, top, ModuleW(L"", 1), h);
                bool open = trayOpen || overflowUp;
                if (Module(Ui::Id("tb.tray"), trayR, open)) trayOpen ? SetTrayOpen(false) : OpenOverflow();
                bool up = dock != open; // points where the icons are, or back once they're out
                c.Icon(up ? glyph::ExpandLess : glyph::ExpandMore, {(trayR.left + trayR.right) / 2, cy}, 17, t.textDim);
                if (input.rclick && Inside(trayR, input.mouse)) menuWanted = true;
            }
            if (promoted.empty()) break;
            const auto& its = tray->Items();
            float px = hidden ? trayR.right : r.left;
            int pick = -1;
            bool right = false;
            D2D1_POINT_2F p{};
            for (uint64_t key : promoted) {
                auto it = std::find_if(its.begin(), its.end(), [&](const TrayItem& ti) { return ti.key == key; });
                if (it == its.end()) continue;
                D2D1_RECT_F cell = Rect(px, top, kPinCell, h);
                px += kPinCell;
                uint32_t id = (uint32_t)(key ^ (key >> 32)) & ~0xFu;
                if (Module(id, cell, false)) pick = (int)(it - its.begin()), p = {(cell.left + cell.right) / 2, edge};
                if (input.rclick && Inside(cell, input.mouse)) pick = (int)(it - its.begin()), right = true, p = {(cell.left + cell.right) / 2, edge};
                if (ID2D1Bitmap* b = TrayBitmap(*it)) c.Image(b, Rect((cell.left + cell.right) / 2 - 8, cy - 8, 16, 16), 0, 1);
            }
            if (pick >= 0) tray->Click((size_t)pick, right, ToScreen(p)); // the app may open a menu
            break;
        }
        case kModLang: {
            if (Module(Ui::Id("tb.lang"), r, false)) { // a click steps to the next layout, in the app being typed into
                HKL list[16];
                int n = GetKeyboardLayoutList(16, list);
                HWND f = tasks ? tasks->Fore() : GetForegroundWindow();
                HKL cur = GetKeyboardLayout(GetWindowThreadProcessId(f, nullptr));
                int at = 0;
                for (int k = 0; k < n; ++k)
                    if (list[k] == cur) at = k;
                if (n > 1 && f) PostMessageW(f, WM_INPUTLANGCHANGEREQUEST, 0, (LPARAM)list[(at + 1) % n]);
                SetTimer(hwnd, kLangTimer, 150, nullptr); // its label follows once the app has switched
            }
            if (input.rclick && Inside(r, input.mouse)) Keys({VK_LWIN, VK_SPACE}); // right click: windows' own switcher, with every layout named
            if (dock && !langSub.empty()) {
                c.Text(lang, Rect(r.left, cy - 15, Width(r), 16), Weight::Semi, 11.5f, t.text, Align::Center);
                c.Text(langSub, Rect(r.left, cy + 1, Width(r), 14), Weight::Regular, 10, t.textDim, Align::Center);
            } else {
                c.Text(keys, r, Weight::Medium, 12, t.textDim, Align::Center);
            }
            break;
        }
        case kModDesktop:
            if (Module(Ui::Id("tb.desktop"), r, false)) ShowDesktop();
            c.Line({r.left + 3, cy - h * 0.3f}, {r.left + 3, cy + h * 0.3f}, t.text.Alpha(0.14f), 1, false); // set off from the clock, as windows' corner
            c.Icon(glyph::Desktop, {mid + 2, cy}, 17, t.textDim);
            break;
        case kModBt:
            if (Module(Ui::Id("tb.bt"), r, qsOpen && qsPage == 2 && std::fabs(anchorX - mid) < 1)) OpenQuick(2, mid);
            icon(radios.BtOn() ? glyph::Bluetooth : glyph::BluetoothOff, r, 0, radios.BtOn() ? t.text : t.textDim);
            break;
        case kModNet:
            if (Module(Ui::Id("tb.net"), r, qsOpen && qsPage == 1 && std::fabs(anchorX - mid) < 1)) OpenQuick(1, mid);
            icon(WifiGlyph(status), r, 0, t.text);
            break;
        case kModVol:
            if (Module(Ui::Id("tb.vol"), r, qsOpen && qsPage == 3 && std::fabs(anchorX - mid) < 1)) OpenQuick(3, mid);
            icon(VolumeGlyph(volShown, muteShown), r, 0, t.text);
            if (float notch = ui.Wheel(r); notch != 0 && spk) { // 2% a notch, as windows
                float v = anim::Clamp01(spk->Level() + notch * 0.02f);
                spk->SetLevel(v);
                volShown = v; // shown at once, not at the next poll
            }
            break;
        case kModBat: {
            if (Module(Ui::Id("tb.power"), r, false)) ShellExecuteW(nullptr, L"open", L"ms-settings:powersleep", nullptr, nullptr, SW_SHOWNORMAL);
            float dx = 0;
            if (status.charging) icon(glyph::Bolt, r, 0, t.accentHi), dx = kBarIcon + kIconGap;
            icon(BatteryGlyph(status), r, dx, status.saver ? t.accentHi : t.text);
            if (!pct.empty()) c.Text(pct, Rect(r.left + kItemPad + dx + kBarIcon + kIconGap, top, 40, h), Weight::Medium, 12, t.text);
            break;
        }
        case kModBell:
            if (Module(Ui::Id("tb.bell"), r, calOpen && std::fabs(anchorX - mid) < 1)) Anchor(mid, !dock), SetPanel(calOpen ? Panel::None : Panel::Calendar);
            icon(dnd ? glyph::BellOff : glyph::Bell, r, 0, t.text);
            if (news) { // how many, on an accent bead
                std::wstring num = unreadN > 9 ? L"9+" : unreadN > 0 ? std::to_wstring(unreadN) : L"";
                D2D1_RECT_F b = Rect(r.right - 12, top, num.size() > 1 ? 15.0f : 12.0f, 12);
                c.Fill(b, 6, t.accent);
                if (!num.empty()) c.Text(num, b, Weight::Semi, 8.5f, t.onAccent, Align::Center);
            }
            break;
        case kModQs:
            if (Module(Ui::Id("tb.qs"), r, qsOpen && qsPage == 0 && std::fabs(anchorX - mid) < 1)) OpenQuick(0, mid);
            icon(glyph::Tune, r, 0, t.text);
            break;
        case kModClock: {
            if (Module(Ui::Id("tb.clock"), r, calOpen && std::fabs(anchorX - mid) < 1)) Anchor(mid, !dock), SetPanel(calOpen ? Panel::None : Panel::Calendar);
            if (dock) { // stacked: the time over the date
                bool two = cfg.date != 0;
                c.Text(clockTime, Rect(r.left, two ? cy - 15 : top, Width(r), two ? 17 : h), Weight::Semi, 12.5f, t.text, Align::Center);
                if (two) c.Text(clockDate, Rect(r.left, cy + 1, Width(r), 14), Weight::Regular, 10.5f, t.textDim, Align::Center);
            } else {
                float tw = ModuleW(clockTime, 0, Weight::Semi) - 2 * kItemPad;
                c.Text(clockTime, Rect(r.left + kItemPad, top, tw + 2, h), Weight::Semi, kBarText, t.text);
                if (cfg.date) c.Text(clockDate, Rect(r.left + kItemPad + tw + kIconGap, top, Width(r), h), Weight::Regular, kBarText, t.textDim);
            }
            if (news && !Shows(kModBell)) c.Dot({r.right - 5, top + 5}, 3, t.accent); // no bell: the clock carries the dot
            break;
        }
        }
    }
    return total;
}

void Bar::DrawToolbar(const Theme& t) {
    capL = capR = {};
    if (!TopOn() || topSink >= 0.999f) return;
    Canvas& c = ui.C();
    capL = barR;
    c.Fill(barR, 0, t.bg); // edge to edge, in island's material
    if (t.glass) {
        if (shadeTopNow > 0.01f) c.Fill(barR, 0, Color{0.02f, 0.02f, 0.04f, 0.45f * shadeTopNow}); // smoked over a white page
        c.FillV(barR, 0, t.sheen, t.sheen.Alpha(0));
    }
    c.Line({0, barR.bottom - 0.5f}, {W, barR.bottom - 0.5f}, t.hairBot, 1); // a hairline under it
    float cy = barR.top + kBarH / 2, top = cy - kItemH / 2;

    // ---- left: who's signed in, a bar, the app in front ----
    float x = 12;
    {
        std::wstring name = userName.empty() ? L"User" : userName;
        D2D1_RECT_F r = Rect(x, top, ModuleW(name, 1, Weight::Medium), kItemH);
        if (Module(Ui::Id("tb.user"), r, false)) PostMessageW(hwnd, WM_APP_MENU, 3, 0); // the user menu, after the frame
        if (input.rclick && Inside(r, input.mouse)) menuWanted = true; // right click: windows' power user menu
        D2D1_RECT_F av = Rect(r.left + kItemPad, cy - 8, 16, 16);
        if (!avatarBmp && avatarSrc && ctx) ctx->CreateBitmapFromWicBitmap(avatarSrc.Get(), nullptr, &avatarBmp);
        if (avatarBmp) c.Image(avatarBmp.Get(), av, 8, 1);
        else c.Fill(av, 8, t.accent), c.Icon(glyph::Pc, {av.left + 8, cy}, 11, t.onAccent);
        c.Text(name, Rect(av.right + kIconGap, top, Width(r), kItemH), Weight::Medium, kBarText, t.text);
        x = r.right + kItemGap;
    }
    if (tasks && Shows(kModTitle)) { // the app in front: its icon, its name, a dash, its window's title
        HWND f = tasks->Fore();
        int ai = tasks->AppOf(f);
        std::wstring app, title;
        if (ai >= 0) {
            const TaskApp& a = tasks->Apps()[ai];
            app = a.name;
            for (const auto& w : a.wins)
                if (w.hwnd == f) title = w.title;
        } else if (f) {
            wchar_t cls[32] = {};
            GetClassNameW(f, cls, 32);
            if (!wcscmp(cls, L"Progman") || !wcscmp(cls, L"WorkerW") || !wcscmp(cls, L"Shell_TrayWnd")) app = L"Desktop"; // the bare desktop, or the taskbar we hid
            else {
                wchar_t buf[256] = {};
                GetWindowTextW(f, buf, 256);
                title = buf;
                app = L"Windows Explorer";
            }
        }
        if (!app.empty()) {
            c.Text(L"|", Rect(x, top, 10, kItemH), Weight::Regular, kBarText, t.textLow, Align::Center);
            x += 10 + kItemGap;
            float aw = ModuleW(app, 0, Weight::Medium) - 2 * kItemPad, dash = ModuleW(L" - ", 0) - 2 * kItemPad;
            float tw = title.empty() ? 0 : dash + ModuleW(title, 0) - 2 * kItemPad;
            D2D1_RECT_F r = Rect(x, top, (std::min)(2 * kItemPad + 16 + kIconGap + aw + tw, W / 2 - 80 - x), kItemH);
            if (Width(r) > 40) {
                Module(Ui::Id("tb.focus"), r, false);
                D2D1_RECT_F ic = Rect(r.left + kItemPad, cy - 8, 16, 16);
                if (ai >= 0) DrawApp(&tasks->Apps()[ai], ic, 1, t);
                else if (app == L"Desktop") c.Icon(glyph::Desktop, {ic.left + 8, cy}, 16, t.accentHi, true);
                c.PushClip(r);
                float tx = ic.right + kIconGap, end = r.right - kItemPad;
                c.Text(app, Rect(tx, top, end - tx, kItemH), Weight::Medium, kBarText, t.text);
                tx += aw;
                if (!title.empty() && tx < end - 12) {
                    c.Text(L" - ", Rect(tx, top, dash + 2, kItemH), Weight::Regular, kBarText, t.textLow);
                    tx += dash;
                    c.Text(title, Rect(tx, top, end - tx, kItemH), Weight::Regular, kBarText, t.textDim);
                }
                c.PopClip();
            }
        }
    }

    // ---- center: the workspaces, a dot each, the one in front a wider accent pill, and a plus to add one ----
    if (Shows(kModDesks)) {
        float dotsW = (desks - 1) * (kDot + kDotGap) + kDotOn;
        float w = 2 * kItemPad + dotsW + 8 + kPlus;
        D2D1_RECT_F r = Rect(W / 2 - w / 2, top, w, kItemH);
        D2D1_RECT_F dots = Rect(r.left, top, 2 * kItemPad + dotsW + 4, kItemH);
        Ui::Hit h = ui.Button(Ui::Id("tb.desks"), dots);
        float dx = r.left + kItemPad;
        int pick = -1, close = -1;
        for (int i = 0; i < desks; ++i) {
            float on = ui.Smooth(Ui::Id("tb.desk", i), i == desk ? 1.0f : 0, 14);
            float dw = kDot + (kDotOn - kDot) * on;
            bool under = input.mouse.y >= top && input.mouse.y < top + kItemH && input.mouse.x >= dx - 4 && input.mouse.x < dx + dw + 4;
            c.Fill(Rect(dx, cy - kDot / 2, dw, kDot), kDot / 2, t.text.Alpha(under ? 0.7f : 0.4f).Mix(t.accent, on));
            if (h.clicked && under) pick = i;
            if (input.rclick && under && desks > 1) close = i;
            dx += dw + kDotGap;
        }
        if (float notch = ui.Wheel(r); notch != 0) pick = (std::clamp)(desk + (notch < 0 ? 1 : -1), 0, desks - 1); // the wheel steps through them
        D2D1_RECT_F plus = Rect(r.right - kItemPad - kPlus, cy - kPlus / 2, kPlus, kPlus);
        if (Module(Ui::Id("tb.desk.add"), Inset(plus, -2, -2), false)) Keys({VK_LCONTROL, VK_LWIN, 'D'}); // a new desktop, and straight onto it
        c.Icon(glyph::Add, {(plus.left + plus.right) / 2, cy}, 15, t.textDim);
        if (close >= 0) { // windows closes only the desktop in front: go there first, then close it; its windows move next door
            for (int k = 0; k < std::abs(close - desk); ++k) Keys({VK_LCONTROL, VK_LWIN, close > desk ? (WORD)VK_RIGHT : (WORD)VK_LEFT});
            Keys({VK_LCONTROL, VK_LWIN, VK_F4});
        } else if (pick >= 0 && pick != desk) { // windows' own shortcut, once per desktop to cross
            WORD dir = pick > desk ? VK_RIGHT : VK_LEFT;
            for (int k = 0; k < std::abs(pick - desk); ++k) Keys({VK_LCONTROL, VK_LWIN, dir});
        }
    }

    // ---- right: the status, laid against the right edge ----
    DrawStatus(t, W - 12, cy, kItemH, false);
}

// ---- the dock ----

void Bar::DrawTile(const Theme& t, const D2D1_RECT_F& tile, float hover, float alpha) { // bare icons: only the mouse lights a soft plate under one
    if (hover > 0.01f) ui.C().Fill(tile, TileRadius(), t.text.Alpha(0.09f * hover * alpha));
}

void Bar::DrawWeg(const Theme& t) {
    wegDrawn = {}, mediaR = {}, barsRect = {};
    wegX.clear(), wegW.clear(), wegS.clear();
    if (sink >= 0.999f || full) return; // tucked away: nothing of it shows
    Canvas& c = ui.C();
    wegDrawn = wegR;
    c.Glow({(wegR.left + wegR.right) / 2, wegR.bottom - 4}, Width(wegR) / 2 + 12, 20, Color{0, 0, 0, 0.22f}); // it floats: a soft shadow under it
    PaintPlate(wegR, WegRadius(), t, shadeBotNow);

    float x = wegR.left + kWegPad, top = rowTop + kWegPad, mid = top + kIcon / 2;
    auto tip = [&](const D2D1_RECT_F& over, const std::wstring& name) { // the item's name, on a small pill above it
        float tw = (std::min)(TextW(name, Weight::Medium, 12) + 20, 300.0f), cx = (over.left + over.right) / 2;
        D2D1_RECT_F r = Rect(cx - tw / 2, wegR.top - 32, tw, 24);
        c.Fill(r, 12, t.bg.Alpha(1).Mix(t.text, 0.08f));
        c.StrokeV(r, 12, t.hairTop, t.hairBot, 1);
        c.Text(name, Inset(r, 10, 0), Weight::Medium, 12, t.text, Align::Center);
    };
    auto sep = [&] { // a faint hairline, half a spot tall
        float lx = std::round((x + kWegSep / 2 - kWegGap / 2) * scale) / scale;
        c.Line({lx, top + kIcon * 0.25f}, {lx, top + kIcon * 0.75f}, t.text.Alpha(0.14f), 1, false);
        x += kWegSep;
    };

    { // start: the windows logo, in the accent
        uint32_t id = Ui::Id("weg.start");
        startR = Rect(x, top, kIcon, kIcon);
        Ui::Hit h = ui.Button(id, startR);
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 18);
        DrawTile(t, startR, hov, 1);
        if (hov > 0.01f) c.Glow({x + kIcon / 2, mid}, 22, 22, t.accent.Alpha(0.22f * hov));
        WinLogo({x + kIcon / 2, mid}, h.down ? 18.0f : 20.0f, t.accentHi, t.accent);
        if (h.hover) tip(startR, L"Start");
        if (h.clicked) {
            Anchor(W / 2, false); // the launcher rises centered over the dock
            launchOpen ? SetPanel(Panel::None) : cfg.launcher ? OpenLauncher() : StartMenu();
        }
        if (input.rclick && Inside(startR, input.mouse)) menuWanted = true; // the power user menu, after the frame
        x += kSlot;
    }
    sep();
    float ww = WidgetsW(); // the apps, the song and the bin among the widgets, in the order picked in edit mode
    DrawWidgets(t, x, top);
    x += (ww > 0 ? ww + kWegGap : 0) + (editing ? kIcon + kWegGap : 0);
    if (!TopOn()) { // the status, and show desktop at the far right
        sep();
        float w = DrawStatus(t, x, mid, kIcon - 6, true);
        if (std::fabs(w - statusW) > 0.5f) statusW = w, ui.Wake(); // the dock grows or shrinks to it on the next frames
    } else if (statusW != 0) {
        statusW = 0;
    }
    if (input.rclick && Inside(wegR, input.mouse) && !ui.HitAt(input.mouse)) StartEdit(); // right click on the dock itself: edit it
}

void Bar::DrawMediaTile(const Theme& t, bool live) { // the song as astraeus' pill: a small cover, title over artist, the little bars; hovered, play or pause
    Canvas& c = *canvas;
    D2D1_RECT_F r = mediaR;
    if (r.right <= r.left) return;
    const Theme& mt = mediaNow; // the album's own colors
    bool song = hasTrack && !track.title.empty();
    uint32_t id = Ui::Id("weg.media");
    float hov = live ? ui.Smooth(id + 1, Inside(r, input.mouse) ? 1.0f : 0, 18) : 0; // the visualizer's own frames run only while the mouse is elsewhere
    float rad = Height(r) / 2, cy = (r.top + r.bottom) / 2;
    bool play = live && song && mediaSize != 0 && hov > 0.5f; // medium and large: play or pause where the bars were
    D2D1_POINT_2F po = {r.right - 14 - 11, cy};
    D2D1_RECT_F pb = Rect(po.x - 13, cy - 13, 26, 26);
    Ui::Hit bh = play ? ui.Button(Ui::Id("weg.media.play"), pb) : Ui::Hit{}; // before the pill's own: the first button under a press takes it
    Ui::Hit h = live ? ui.Button(id, r) : Ui::Hit{};
    if (mediaSize == 0) { // small: just the cover, the little bars over its foot; hovered, play or pause
        D2D1_RECT_F art = Inset(r, 4, 4);
        barsRect = {};
        if (song && cover) c.Art(cover.Get(), coverBlur.Get(), art, TileRadius() - 2, 1);
        else c.Fill(art, TileRadius() - 2, t.text.Alpha(0.06f)), c.Icon(glyph::Music, {(r.left + r.right) / 2, cy}, 16, t.textDim);
        if (song && hov > 0.5f) {
            c.Fill(art, TileRadius() - 2, Color{0, 0, 0, 0.45f});
            c.Icon(track.playing ? glyph::Pause : glyph::Play, {(r.left + r.right) / 2, cy}, 18, Color{1, 1, 1, 1}, true);
            if (h.clicked) media.TogglePlay(), h.clicked = false;
        }
        if (live && h.hover && song) {
            std::wstring line = track.artist.empty() ? track.title : track.title + L" - " + track.artist;
            float tw = (std::min)(TextW(line, Weight::Medium, 12) + 20, 300.0f), x = (r.left + r.right) / 2;
            D2D1_RECT_F tr = Rect(x - tw / 2, wegR.top - 32, tw, 24);
            c.Fill(tr, 12, t.bg.Alpha(1).Mix(t.text, 0.08f));
            c.StrokeV(tr, 12, t.hairTop, t.hairBot, 1);
            c.Text(line, Inset(tr, 10, 0), Weight::Medium, 12, t.text, Align::Center);
        }
        if (live && input.rclick && Inside(r, input.mouse) && song) Anchor((r.left + r.right) / 2, false), SetMediaOpen(!mediaOpen); // right click: the player
        return;
    }
    c.Fill(r, rad, t.text.Alpha(0.05f + 0.05f * hov)); // a pill inside the dock, a shade off it
    c.StrokeV(r, rad, t.hairTop, t.hairBot, 1);
    if (mediaSize == 2 && song && track.duration > 0) { // large: where the song is, a hairline along its foot
        float k = (float)track.Progress();
        D2D1_RECT_F lane = {r.left + rad, r.bottom - 4, r.right - rad, r.bottom - 2.5f};
        c.Fill(lane, 1, t.text.Alpha(0.1f));
        c.Fill({lane.left, lane.top, lane.left + Width(lane) * k, lane.bottom}, 1, mt.accentHi);
    }
    if (!song) {
        c.Icon(glyph::Music, {r.left + 22, cy}, 15, t.textDim);
        c.Text(L"Nothing playing", Rect(r.left + 36, r.top, Width(r) - 46, Height(r)), Weight::Medium, 11.5f, t.textDim);
        barsRect = {};
    } else {
        D2D1_RECT_F art = Rect(r.left + 7, cy - 13, 26, 26); // rounded like an app icon
        if (cover) c.Art(cover.Get(), coverBlur.Get(), art, 7, 1);
        else c.Fill(art, 7, mt.cardHi), c.Icon(glyph::Music, {art.left + 13, cy}, 14, mt.accentHi, true);
        barsRect = Rect(r.right - 14 - 22, cy - 7, 22, 14);
        float tx = art.right + 8, tw = barsRect.left - 6 - tx;
        bool two = !track.artist.empty();
        c.Text(track.title, Rect(tx, two ? cy - 14 : cy - 8, tw, 16), Weight::Medium, 12, t.text);
        if (two) c.Text(track.artist, Rect(tx, cy + 1, tw, 14), Weight::Light, 10.5f, t.textDim);
        if (play) {
            c.Fill(pb, 13, mt.accent.Alpha(bh.hover ? 0.9f : 0.7f));
            c.Icon(track.playing ? glyph::Pause : glyph::Play, po, 16, mt.onAccent, true);
            if (bh.clicked) media.TogglePlay();
        } else {
            DrawBars(0, 0, false); // stepped once per frame, before the passes
        }
    }
    if (!live) return;
    if (h.clicked && song) Anchor((r.left + r.right) / 2, false), SetMediaOpen(!mediaOpen); // the player, with the rest of the controls
}

void Bar::SyncBin() { // asked of the poller: walking every drive's bin takes too long for the ui thread
    binAsk = true;
    if (pollWake) SetEvent(pollWake);
}

void Bar::BinMenu() {
    HMENU m = CreatePopupMenu();
    if (!m) return;
    AppendMenuW(m, MF_STRING, 1, L"Open");
    AppendMenuW(m, MF_STRING | (binFull ? 0 : MF_GRAYED), 2, L"Empty Recycle Bin");
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_NONOTIFY, p.x, p.y, hwnd, nullptr);
    DestroyMenu(m);
    HWND back = tasks ? tasks->Fore() : nullptr;
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    if (pick == 1) ShellExecuteW(nullptr, L"open", L"shell:RecycleBinFolder", nullptr, nullptr, SW_SHOWNORMAL);
    else if (pick == 2) SHEmptyRecycleBinW(hwnd, nullptr, 0), SyncBin(); // windows asks first, as its own bin does
}

void Bar::AppMenu() { // an app's menu in the dock: its windows, a new one, as administrator, where it lives, pin, close
    const TaskApp* a = FindApp(menuApp);
    if (!a || !tasks) return;
    int ai = (int)(a - tasks->Apps().data());
    constexpr UINT kPin = 1, kNew = 2, kWhere = 3, kAdmin = 4, kClose = 5, kWin = 100;
    HMENU m = CreatePopupMenu();
    if (!m) return;
    bool canPin = a->pinned || !a->exe.empty() || !a->aumid.empty();
    if (!a->wins.empty()) {
        for (size_t i = 0; i < a->wins.size() && i < 12; ++i) { // one row per window: click to go to it
            std::wstring title = a->wins[i].title.empty() ? a->name : a->wins[i].title;
            if (title.size() > 60) title = title.substr(0, 57) + L"...";
            AppendMenuW(m, MF_STRING, kWin + (UINT)i, title.c_str());
        }
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(m, MF_STRING, kNew, L"New window");
    AppendMenuW(m, MF_STRING, kAdmin, L"Run as administrator");
    if (!a->exe.empty()) AppendMenuW(m, MF_STRING, kWhere, L"Open file location");
    AppendMenuW(m, MF_STRING | (canPin ? 0 : MF_GRAYED), kPin, a->pinned ? L"Unpin from dock" : L"Pin to dock");
    if (!a->wins.empty()) {
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, kClose, a->wins.size() > 1 ? L"Close all windows" : L"Close window");
    }
    POINT p;
    GetCursorPos(&p);
    SetForegroundWindow(hwnd); // a menu needs its owner in front, or it never closes
    UINT pick = (UINT)TrackPopupMenuEx(m, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_CENTERALIGN | TPM_NONOTIFY, p.x, ToScreen({0, wegR.top - 6}).y, hwnd, nullptr);
    DestroyMenu(m);
    HWND back = tasks->Fore();
    if (back && back != hwnd && IsWindow(back)) SetForegroundWindow(back);
    a = FindApp(menuApp); // the menu ran its own loop: the list may have changed meanwhile
    if (!a) return;
    ai = (int)(a - tasks->Apps().data());
    if (pick == kPin) a->pinned ? tasks->Unpin(ai) : tasks->Pin(ai);
    else if (pick == kNew) tasks->Launch(ai);
    else if (pick == kAdmin) tasks->Launch(ai, true);
    else if (pick == kClose) tasks->CloseAll(ai);
    else if (pick == kWhere) { // explorer, with the exe selected
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(a->exe.c_str())) SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0), ILFree(pidl);
    } else if (pick >= kWin && pick < kWin + a->wins.size()) Tasks::Activate(a->wins[pick - kWin].hwnd);
}
