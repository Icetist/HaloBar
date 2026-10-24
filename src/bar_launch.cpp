// The launcher: click the halo and type. Every app the start menu knows, found as you type, Enter starts the best one;
// the dock's pins show as favourites while the field is empty, and all apps lists everything. The list and its icons are read once on a worker, then kept until the bar idles.
#include "bar.h"

#include <climits>
#include <cwctype>
#include <thread>
#include <unordered_map>

#include "glyphs.h"

namespace {

constexpr int kCols = 7, kRows = 3; // seven across, three rows in view
constexpr float kHead = 44, kFoot = 52; // the search bar and the foot
constexpr float kCellW = 110, kCellH = 112, kGridTile = 56, kGridIcon = 44; // one app: its tile, the icon on it, and two lines of name
constexpr float kListW = 540, kRowH = 40, kLetterH = 32, kListIcon = 26, kIndent = 22; // all apps as a list: its width, a row, a letter's heading, the icon, a folder's apps set in
constexpr int kFolderTag = 1 << 24; // in launchHits: a folder, by its place in launchFolders

std::wstring Folded(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

wchar_t Letter(const std::wstring& name) { // the heading a name sits under: its first letter, or # for numbers and symbols
    wchar_t ch = name.empty() ? L'#' : (wchar_t)std::towupper(name[0]);
    return std::iswalpha(ch) ? ch : L'#';
}

struct AppsLoad { // what the worker brings back
    std::vector<sys::AppEntry> appList;
    std::vector<ComPtr<IWICBitmapSource>> icons; // same order as appList, null where the shell had none
};

}

constexpr size_t kIconBatch = 24; // icons handed over this many at a time while they load

float Bar::LaunchRise() const { return kHead + kRows * kCellH + 24 + kFoot; } // fixed height: the panel doesn't jump as results come and go

void Bar::OpenLauncher() {
    query.clear(); // each open starts on an empty field, showing the favourites
    launchSel = 0, launchAll = false, launchScroll = 0;
    SetPanel(Panel::Launch);
    SetForegroundWindow(hwnd); // typing needs the keyboard; it goes back to the app when the launcher closes
    SetFocus(hwnd);
    bool stale = appList.empty() || GetTickCount64() / 1000.0 - appsAt > 600; // ten minutes: long enough to skip the rescan, short enough to see new installs
    if (stale || !appIconsDone) LoadApps(true); // icons may have been trimmed by an idle
}

void Bar::LoadApps(bool withIcons) { // two steps on a worker: the names first, so search works at once; the icons after, filling in
    if (appsLoading) return; // one load at a time; the running one will post its result
    appsLoading = true;
    HWND to = hwnd; // captured by value: the worker never touches the bar itself
    UINT px = (UINT)(std::max)(48L, std::lround(kGridIcon * scale)); // the grid's icon size in pixels: drawn 1:1, never stretched
    std::thread([to, px, withIcons] {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the shell's app folder needs com on this thread too
        auto* list = new AppsLoad{sys::ListApps(), {}};
        std::vector<sys::AppEntry> names = list->appList; // the worker's own copy, for the icons
        if (!PostMessageW(to, WM_APP_APPS, withIcons ? 1 : 0, (LPARAM)list)) delete list; // the window owns it once posted; a failed post would leak it
        if (withIcons) {
            auto* icons = new AppsLoad{{}, {}};
            icons->icons.resize(names.size());
            for (size_t i = 0; i < names.size(); ++i) {
                HBITMAP hb = sys::AppIcon(names[i], (int)px);
                if (hb) icons->icons[i] = gfx::FromShellBitmap(hb), DeleteObject(hb); // its own pixels, independent of the hbitmap freed here; null: the tile shows a letter
                if ((i + 1) % kIconBatch == 0 && i + 1 < names.size()) { // a batch in: shown at once, the rest still coming
                    auto* part = new AppsLoad{{}, icons->icons};
                    if (!PostMessageW(to, WM_APP_APPS, 3, (LPARAM)part)) delete part;
                }
            }
            if (!PostMessageW(to, WM_APP_APPS, 2, (LPARAM)icons)) delete icons;
        }
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

void Bar::OnApps(WPARAM step, LPARAM lp) { // step 0: names, all done; 1: names, icons to come; 2: the icons; 3: the icons so far
    auto* got = reinterpret_cast<AppsLoad*>(lp);
    if (!got) return;
    if (step == 3) {
        if (got->icons.size() == appList.size()) appIcons = std::move(got->icons); // still loading: appIconsDone stays false
        delete got;
        if (launchOpen) Redraw();
        return;
    }
    if (step == 2) {
        if (got->icons.size() == appList.size()) appIcons = std::move(got->icons), appIconsDone = true; // a size mismatch means the list was reloaded meanwhile
        appBmps.clear(); // device bitmaps are made again from the new icons on the next draw
        appsLoading = false;
    } else {
        appList = std::move(got->appList);
        appIcons.assign(appList.size(), nullptr); // old icons no longer line up with the new list
        appIconsDone = false;
        appBmps.clear();
        appsAt = GetTickCount64() / 1000.0;
        if (step == 0) appsLoading = false;
    }
    delete got;
    if (launchOpen) Redraw(); // closed: nothing to show, so no frame
}

void Bar::BuildLaunchRows() { // start's all apps: a heading per letter, apps and folders in name order, an open folder's apps under it
    launchRows.clear(), launchListHits.clear(), launchFolders.clear();
    struct Entry {
        std::wstring key, label;
        int app = -1, folder = -1;
    };
    std::vector<Entry> top;
    std::unordered_map<std::wstring, int> seen; // folder, by its folded name
    for (int i = 0; i < (int)appList.size(); ++i) {
        const sys::AppEntry& a = appList[i];
        if (a.folder.empty()) {
            top.push_back({a.lower, a.name, i, -1});
            continue;
        }
        std::wstring key = Folded(a.folder);
        if (seen.emplace(key, (int)launchFolders.size()).second) top.push_back({key, a.folder, -1, (int)launchFolders.size()}), launchFolders.push_back(a.folder);
    }
    std::stable_sort(top.begin(), top.end(), [](const Entry& x, const Entry& y) { return x.key < y.key; });
    float y = 0;
    wchar_t letter = 0;
    auto row = [&](int kind, int idx, int hit, float h) {
        LaunchRow r;
        r.kind = kind, r.idx = idx, r.y = y, r.h = h;
        if (hit != INT_MIN) r.sel = (int)launchListHits.size(), launchListHits.push_back(hit);
        launchRows.push_back(r);
        y += h;
    };
    for (const Entry& e : top) {
        if (wchar_t l = Letter(e.label); l != letter) letter = l, row(0, l, INT_MIN, kLetterH);
        if (e.app >= 0) {
            row(1, e.app, e.app, kRowH);
            continue;
        }
        row(2, e.folder, kFolderTag + e.folder, kRowH);
        if (std::find(launchOpenDirs.begin(), launchOpenDirs.end(), e.label) == launchOpenDirs.end()) continue;
        for (int i = 0; i < (int)appList.size(); ++i) // its apps, already in name order
            if (appList[i].folder == e.label) row(3, i, i, kRowH);
    }
    launchRowsAt = appsAt, launchRowsVer = launchDirsVer;
}

void Bar::LaunchKey(WPARAM vk) {
    int n = (int)launchHits.size(), cols = LaunchList() ? 1 : kCols; // the list moves a row at a time
    if (vk == VK_ESCAPE && !query.empty()) query.clear(), launchSel = 0; // esc clears the search first, then closes
    else if (vk == VK_ESCAPE) SetPanel(Panel::None);
    else if (vk == VK_RETURN) LaunchPick(launchSel);
    else if (n > 0 && vk == VK_RIGHT) launchSel = (launchSel + 1) % n; // left and right wrap; up and down stop at the edges
    else if (n > 0 && vk == VK_LEFT) launchSel = (launchSel + n - 1) % n;
    else if (n > 0 && vk == VK_DOWN) launchSel = (std::min)(n - 1, launchSel + cols);
    else if (n > 0 && vk == VK_UP) launchSel = (std::max)(0, launchSel - cols);
    Redraw();
}

void Bar::LaunchChar(wchar_t ch) {
    if (ch == VK_BACK) {
        if (!query.empty()) query.pop_back();
    } else if (ch >= 32 && query.size() < 64) { // printable only; the cap keeps the field on one line
        query.push_back(ch);
    } else {
        return;
    }
    launchSel = 0; // a new query puts the best match back under enter
    Redraw();
}

void Bar::LaunchPick(int i) {
    if (i < 0 || i >= (int)launchHits.size()) return; // enter with no results does nothing
    int h = launchHits[i];
    if (h >= kFolderTag) { // a folder opens or closes in place; the launcher stays
        const std::wstring& name = launchFolders[(size_t)(h - kFolderTag)];
        auto it = std::find(launchOpenDirs.begin(), launchOpenDirs.end(), name);
        if (it == launchOpenDirs.end()) launchOpenDirs.push_back(name);
        else launchOpenDirs.erase(it);
        ++launchDirsVer;
        Redraw();
        return;
    }
    bool admin = GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0; // ctrl+shift+enter, as start: as administrator
    SetPanel(Panel::None); // closed first, so the started app can take the foreground
    if (h >= 0 && h < (int)appList.size()) sys::LaunchApp(appList[h], admin);
    else if (h < 0 && tasks) tasks->Click(-h - 1); // a pin: as clicking it in the row
}

void Bar::DrawLaunchList(const Theme& t, const D2D1_RECT_F& body, int& pick) { // all apps as start lists it: letters, apps, folders that open in place
    Canvas& c = ui.C();
    float totalH = launchRows.empty() ? 0 : launchRows.back().y + launchRows.back().h;
    float maxScroll = (std::max)(0.0f, totalH + 24 - Height(body));
    if (float n = ui.Wheel(body); n != 0) launchScroll = (std::clamp)(launchScroll - n * kRowH * 3, 0.0f, maxScroll);
    for (const LaunchRow& rw : launchRows) // the keys keep the picked row in view
        if (rw.sel == launchSel) {
            if (rw.y < launchScroll) launchScroll = rw.y;
            else if (rw.y + rw.h > launchScroll + Height(body) - 24) launchScroll = (std::min)(maxScroll, rw.y + rw.h - Height(body) + 24);
        }
    launchScroll = (std::min)(launchScroll, maxScroll);
    float scroll = ui.Smooth(Ui::Id("ln.lscroll"), launchScroll, 18);
    if (launchRows.empty()) c.Text(appsLoading ? L"Finding your apps\u2026" : L"No apps found", Rect(body.left, body.top + 60, Width(body), 18), Weight::Regular, 13, t.textLow, Align::Center);
    float lx = (body.left + body.right) / 2 - kListW / 2, y0 = body.top + 10;
    c.PushClip(body);
    for (const LaunchRow& rw : launchRows) {
        float y = y0 + rw.y - scroll;
        if (y + rw.h < body.top || y > body.bottom) continue; // out of view: not drawn, not hit
        if (rw.kind == 0) { // a letter's heading
            wchar_t s[2] = {(wchar_t)rw.idx, 0};
            c.Text(s, Rect(lx + 12, y + 6, 40, rw.h - 6), Weight::Semi, 13, t.accentHi);
            continue;
        }
        float in = rw.kind == 3 ? kIndent : 0;
        D2D1_RECT_F cell = Rect(lx + in, y + 2, kListW - in, rw.h - 4);
        Ui::Hit hit = ui.Button(Ui::Id("ln.row", rw.kind == 2 ? kFolderTag + rw.idx : rw.idx), cell);
        if (hit.hover && (input.mouse.x != lastLaunchMouse.x || input.mouse.y != lastLaunchMouse.y)) launchSel = rw.sel;
        bool sel = rw.sel == launchSel;
        if (sel) c.Fill(cell, 10, t.accent.Alpha(0.20f));
        else if (hit.hover) c.Fill(cell, 10, t.text.Alpha(0.06f));
        D2D1_RECT_F ic = Rect(cell.left + 10, (cell.top + cell.bottom) / 2 - kListIcon / 2, kListIcon, kListIcon);
        std::wstring name;
        if (rw.kind == 2) { // a folder: its own tile, and which way it's turned
            name = launchFolders[(size_t)rw.idx];
            bool open = std::find(launchOpenDirs.begin(), launchOpenDirs.end(), name) != launchOpenDirs.end();
            c.Fill(ic, 7, t.card);
            c.Icon(glyph::Folder, {(ic.left + ic.right) / 2, (ic.top + ic.bottom) / 2}, 17, t.accentHi, true);
            c.Icon(open ? glyph::ExpandLess : glyph::ExpandMore, {cell.right - 18, (cell.top + cell.bottom) / 2}, 18, t.textDim);
        } else {
            name = appList[(size_t)rw.idx].name;
            ComPtr<ID2D1Bitmap>& bmp = appBmps[rw.idx];
            if (!bmp && rw.idx < (int)appIcons.size() && appIcons[(size_t)rw.idx] && ctx) ctx->CreateBitmapFromWicBitmap(appIcons[(size_t)rw.idx].Get(), nullptr, &bmp);
            if (bmp) c.Pixels(bmp.Get(), ic);
            else c.Fill(ic, 7, t.card), c.Text(name.substr(0, 1), ic, Weight::Semi, 13, t.textDim, Align::Center); // its icon still loading
        }
        c.Text(name, Rect(ic.right + 12, cell.top, Width(cell) - (ic.right + 12 - cell.left) - 36, Height(cell)), Weight::Medium, 13, t.text);
        if (hit.clicked) pick = rw.sel;
    }
    c.PopClip();
}

void Bar::DrawLauncher(const Theme& t, ID2D1Geometry* shape) { // a search bar, a grid of favourites or every app, the user and the session at its foot
    float rise = (std::max)(0.0f, launchSpring.value);
    if (rise < 1 || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 40) / (LaunchRise() - 40));
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    D2D1_RECT_F r = {W / 2 - kLaunchW / 2, pill.top + CallH(), W / 2 + kLaunchW / 2, pill.top + CallH() + LaunchRise()};

    // the top bar: a borderless search field, and all apps or back on the right
    D2D1_RECT_F f = Rect(r.left + 16, r.top, Width(r) - 32 - 96, kHead);
    float tw = query.empty() ? 0 : TextW(query, Weight::Regular, 13);
    if (query.empty()) c.Text(L"Applications", f, Weight::Regular, 13, t.textLow);
    else c.Text(query, f, Weight::Regular, 13, t.text);
    if (Caret()) c.Fill(Rect(f.left + tw + 1, f.top + 14, 1.5f, kHead - 28), 0.75f, t.accent); // the caret blinks while it's open
    bool back = launchAll || !query.empty();
    D2D1_RECT_F ab = Rect(r.right - 16 - 88, r.top + 10, 88, kHead - 20);
    {
        Ui::Hit h = ui.Button(Ui::Id("ln.all"), ab);
        c.Fill(ab, 6, t.card);
        if (h.hover) c.Fill(ab, 6, t.cardHi);
        c.Text(back ? L"Back" : L"All Apps", ab, Weight::Regular, 12.5f, t.text, Align::Center);
        if (h.clicked) {
            if (!query.empty()) query.clear();
            else launchAll = !launchAll;
            launchSel = 0, launchScroll = 0;
        }
    }
    c.Line({r.left, r.top + kHead}, {r.right, r.top + kHead}, t.line, 1);

    // the grid: matches while typing, every app in all apps, the dock's pins as favourites
    launchHits.clear();
    if (!query.empty()) {
        for (int i : sys::Search(appList, query, kCols * kRows)) launchHits.push_back(i);
    } else if (launchAll) {
        if (launchRowsAt != appsAt || launchRowsVer != launchDirsVer) BuildLaunchRows(); // remade only when the list or an open folder changes
        launchHits = launchListHits;
    } else if (tasks) {
        const auto& all = tasks->Apps();
        for (int i = 0; i < (int)all.size() && (int)launchHits.size() < kCols * kRows; ++i)
            if (all[i].pinned) launchHits.push_back(-i - 1); // negative: a pin, by its place in the dock
    }
    if (launchSel >= (int)launchHits.size()) launchSel = (std::max)(0, (int)launchHits.size() - 1);
    D2D1_RECT_F body = {r.left, r.top + kHead + 1, r.right, r.bottom - kFoot - 1};
    int pick = -1;
    if (LaunchList()) {
        DrawLaunchList(t, body, pick);
    } else {
    float gx = (r.left + r.right) / 2 - kCols * kCellW / 2, gy = body.top + 12;
    int rowsAll = ((int)launchHits.size() + kCols - 1) / kCols;
    float maxScroll = (std::max)(0.0f, rowsAll * kCellH + 24 - Height(body));
    if (float n = ui.Wheel(body); n != 0) launchScroll = (std::clamp)(launchScroll - n * kCellH, 0.0f, maxScroll);
    float selTop = (launchSel / kCols) * kCellH; // the keys keep the picked one in view
    if (selTop < launchScroll) launchScroll = selTop;
    else if (selTop + kCellH > launchScroll + Height(body) - 24) launchScroll = (std::min)(maxScroll, selTop + kCellH - Height(body) + 24);
    float scroll = ui.Smooth(Ui::Id("ln.scroll"), launchScroll, 18);
    if (!query.empty() && launchHits.empty())
        c.Text(appsLoading ? L"Finding your apps…" : L"No apps match", Rect(body.left, body.top + 60, Width(body), 18), Weight::Regular, 13, t.textLow, Align::Center);
    else if (query.empty() && !launchAll && launchHits.empty())
        c.Text(L"Pin apps to the dock and they show here", Rect(body.left, body.top + 60, Width(body), 18), Weight::Regular, 13, t.textLow, Align::Center);
    c.PushClip(body);
    UINT px = (UINT)(std::max)(48L, std::lround(kGridIcon * scale));
    ComPtr<ID2D1SolidColorBrush> ink; // one for every name this frame
    rt->CreateSolidColorBrush(t.text.D2D(), &ink);
    for (int k = 0; k < (int)launchHits.size(); ++k) {
        float y = gy + (k / kCols) * kCellH - scroll;
        if (y + kCellH < body.top || y > body.bottom) continue; // out of view: not drawn, not hit
        int h = launchHits[k];
        D2D1_RECT_F cell = Rect(gx + (k % kCols) * kCellW + 4, y, kCellW - 8, kCellH - 8);
        Ui::Hit hit = ui.Button(Ui::Id("ln.tile", k), cell);
        if (hit.hover && (input.mouse.x != lastLaunchMouse.x || input.mouse.y != lastLaunchMouse.y)) launchSel = k; // the mouse moving picks, a still mouse leaves the keys in charge
        bool sel = k == launchSel;
        if (sel) c.Fill(cell, 18, t.accent.Alpha(0.22f)), c.Stroke(cell, 18, t.accent.Alpha(0.7f), 1.2f); // the one enter starts: an accent wash and ring
        D2D1_RECT_F tile = Rect((cell.left + cell.right) / 2 - kGridTile / 2, cell.top + 8, kGridTile, kGridTile);
        c.Fill(tile, 10, t.card);
        D2D1_RECT_F ic = Inset(tile, (kGridTile - kGridIcon) / 2, (kGridTile - kGridIcon) / 2);
        std::wstring name;
        if (h >= 0) {
            name = appList[h].name;
            ComPtr<ID2D1Bitmap>& bmp = appBmps[h];
            if (!bmp && h < (int)appIcons.size() && appIcons[h] && ctx) ctx->CreateBitmapFromWicBitmap(appIcons[h].Get(), nullptr, &bmp);
            if (bmp) c.Pixels(bmp.Get(), ic);
            else c.Text(name.substr(0, 1), ic, Weight::Semi, 20, t.textDim, Align::Center); // its icon is still loading
        } else if (tasks) {
            const TaskApp& ta = tasks->Apps()[-h - 1];
            name = ta.name;
            DrawApp(&ta, ic, 1, t, px);
        }
        D2D1_RECT_F label = Rect(cell.left + 4, tile.bottom + 6, Width(cell) - 8, 34);
        ComPtr<IDWriteTextLayout>& lay = launchLabels[name]; // two lines at most, centered; the caret's blink redraws the grid many times a second
        if (!lay && SUCCEEDED(gfx::DWrite()->CreateTextLayout(name.c_str(), (UINT32)name.size(), gfx::TextFmt(Weight::Medium, 12.5f, Align::Center), Width(label), Height(label), &lay))) {
            lay->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
            lay->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        }
        if (lay && ink) rt->DrawTextLayout({label.left, label.top}, lay.Get(), ink.Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
        if (hit.clicked) pick = k;
    }
    c.PopClip();
    }
    lastLaunchMouse = input.mouse;

    // the foot: who's signed in, then settings and power
    float fy = r.bottom - kFoot / 2;
    c.Line({r.left, r.bottom - kFoot}, {r.right, r.bottom - kFoot}, t.line, 1);
    {
        std::wstring name = userName.empty() ? L"User" : userName;
        D2D1_RECT_F u = Rect(r.left + 12, fy - 16, TextW(name, Weight::Medium, 12.5f) + 50, 32);
        Ui::Hit h = ui.Button(Ui::Id("ln.user"), u);
        if (h.hover) c.Fill(u, 6, t.cardHi);
        D2D1_RECT_F av = Rect(u.left + 8, fy - 12, 24, 24);
        if (!avatarBmp && avatarSrc && ctx) ctx->CreateBitmapFromWicBitmap(avatarSrc.Get(), nullptr, &avatarBmp);
        if (avatarBmp) c.Image(avatarBmp.Get(), av, 12, 1);
        else c.Fill(av, 12, t.accent);
        c.Text(name, Rect(av.right + 8, u.top, Width(u) - 40, 32), Weight::Medium, 12.5f, t.text);
        if (h.clicked) SetPanel(Panel::None), PostMessageW(hwnd, WM_APP_MENU, 3, 0);
    }
    if (w::IconBtn(ui, t, Ui::Id("ln.set"), {r.right - 56, fy}, 32, glyph::Settings, 18)) SetSettingsOpen(true);
    if (w::IconBtn(ui, t, Ui::Id("ln.pw"), {r.right - 22, fy}, 32, glyph::Power, 18)) SetPanel(Panel::None), PostMessageW(hwnd, WM_APP_MENU, 1, 0);
    rt->PopLayer();
    if (pick >= 0) LaunchPick(pick);
}
