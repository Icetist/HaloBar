// The apps in the dock, pinned ones first as on the windows taskbar: click, drag to reorder or pin, and right click
// for that app's sheet over it: its windows, a new one, pin or unpin, close them all.
#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cmath>
#include <cwctype>

#include "glyphs.h"

namespace {

constexpr float kRecentHead = 24, kRecentRow = 30; // the jump list's recent files under the windows
constexpr UINT kRecentMax = 5; // a short list: the sheet must still fit above the row

constexpr double kPulseSec = 6; // an attention call pulses this long, then holds still
constexpr float kDragStart = 8; // dips the mouse travels with the button down before an app is being dragged
constexpr int kSheetRows = 6; // windows listed in an app's sheet
constexpr float kSheetHead = 56, kSheetRow = 32, kSheetFoot = 54; // name and count, one window, the action buttons
constexpr Color kError = {1.0f, 0.42f, 0.42f, 1}, kPaused = {0.96f, 0.77f, 0.26f, 1}; // progress bar inks, red and amber as windows uses

uint32_t KeyId(const std::wstring& k) { // stable ui id per app, low bits left free for its parts
    uint32_t h = 2166136261u; // fnv-1a
    for (wchar_t c : k) h = (h ^ (uint32_t)c) * 16777619u;
    return h & ~0xFu; // id + 1..4 are the slot's smoothers
}

}

float Bar::SheetRise() const {
    const TaskApp* a = FindApp(sheetKey);
    int rows = a ? (std::min)((int)a->wins.size(), kSheetRows) : 0; // more windows than rows are cut, not scrolled
    float recent = sheetRecent.empty() ? 0 : kRecentHead + (float)sheetRecent.size() * kRecentRow;
    return kSheetHead + rows * kSheetRow + recent + kSheetFoot + 6;
}

void Bar::DrawProgress(const Badge& b, float cx, float y, float w, float grow, const Theme& t) {
    Canvas& c = ui.C();
    Color ink = b.state == 4 ? kError : b.state == 8 ? kPaused : t.accent; // TBPF_ERROR and TBPF_PAUSED from ITaskbarList3
    D2D1_RECT_F lane = Rect(cx - w / 2, y, w, 3); // the track sits where the running dash would be
    c.Fill(lane, 1.5f, t.track.Alpha(0.9f * grow));
    if (b.state == 1) { // indeterminate: a short segment sweeping across
        float k = (float)std::fmod(ui.Now() * 0.9, 1.0), seg = w * 0.35f;
        float x0 = lane.left - seg + (w + seg) * k;
        c.PushClip(lane);
        c.Fill(Rect(x0, y, seg, 3), 1.5f, ink.Alpha(grow));
        c.PopClip();
        ui.KeepAlive(); // the sweep needs frames for as long as the app reports it
        return;
    }
    c.Fill(Rect(lane.left, y, (std::max)(3.0f, w * b.value), 3), 1.5f, ink.Alpha(grow)); // never thinner than its rounded ends, so 0% still shows
}

void Bar::DrawDock(const Theme& t) { // the apps in the dock: wegX and wegS (DrawWeg) say where each sits and how magnified it is
    if (!tasks) return; // no task tracking yet: the dock is empty
    Canvas& c = ui.C();
    const auto& apps = tasks->Apps();
    int n = (std::min)({dockCount, (int)apps.size(), (int)wegX.size()}), pinned = tasks->PinCount();
    HWND fore = tasks->Fore();
    float tileTop = rowTop + kWegPad; // each app's spot, the same size as every other item's
    bool pulsing = ui.Now() - callSince < kPulseSec; // after that a calling app keeps its bright dot but stops costing frames

    if (dragFrom >= n) dragFrom = -1, dragging = false; // the app went while held
    int drop = -1; // where the dragged app would land
    if (dragging) {
        int last = dragFrom < pinned ? pinned - 1 : pinned; // pins move among pins; a running app can drop in among them to be pinned
        drop = (std::clamp)((int)std::lround((input.mouse.x - dockX - slotW / 2) / slotW), 0, (std::max)(0, last));
        if (dragFrom >= pinned && drop >= pinned) drop = dragFrom; // not dropped among the pins: stays where it was
    }

    int clicked = -1, launched = -1, sheet = -1, hovered = -1;
    slots.clear(); // rebuilt each frame, read by the preview card and the sheet
    for (int i = 0; i < n; ++i) {
        const TaskApp& a = apps[i];
        uint32_t id = KeyId(a.key);
        int slot = i;
        if (dragging && i != dragFrom) { // the others make room where it would land
            if (i > dragFrom) --slot;
            if (slot >= drop) ++slot;
        }
        float grow = ui.SmoothFrom(id + 1, 1, 0, 14); // a new app opens up its slot
        float w = wegW[(size_t)slot];
        float sx = ui.Smooth(id + 2, wegX[(size_t)slot], 22); // the others slide when apps come and go
        bool held = dragging && i == dragFrom;
        if (held) sx = (std::clamp)(input.mouse.x - w / 2, wegX[0], wegX[(size_t)n - 1]); // the held one follows the mouse, kept inside the dock
        D2D1_RECT_F r = Rect(sx, rowTop, w, kWegH);
        Ui::Hit h = ui.Button(id, r);
        if (h.hover && !dragging) hovered = i;
        if (ui.IsActive(id) && input.down && !dragging && std::fabs(input.mouse.x - input.pressPos.x) > kDragStart && (a.pinned || pinned > 0 || n > 1))
            dragging = true, dragFrom = i; // a lone unpinned app has nowhere to go, so it never drags
        bool active = false, calling = false;
        for (const auto& win : a.wins) {
            active = active || win.hwnd == fore;
            calling = calling || (win.flash && win.hwnd != fore); // a flash from the window in front is already seen
        }
        float act = ui.Smooth(id + 4, active ? 1.0f : 0, 14);
        float hov = ui.Smooth(id + 3, h.hover && !dragging ? 1.0f : 0, 18);
        float cx = sx + w / 2, fade = grow * (held ? 0.5f : 1.0f); // the dragged one goes half see-through
        D2D1_RECT_F tile = Rect(cx - kIcon / 2, tileTop, kIcon, kIcon);
        DrawTile(t, tile, hov, fade);
        float size = kAppIcon * grow * (h.down && !held ? 0.9f : 1.0f); // presses in when held down
        D2D1_RECT_F icon = Rect(cx - size / 2, tileTop + kIcon / 2 - size / 2, size, size);
        slots[a.key] = tile;
        DrawApp(&a, icon, fade, t);
        if (float notch = ui.Wheel(r); notch != 0 && !a.exe.empty()) { // the wheel over an app turns that app's own volume
            std::wstring key = a.exe;
            for (auto& ch : key) ch = (wchar_t)towlower(ch); // the mixer's key: the lowercased exe path
            if (!mixer.IsOpen()) mixer.Open(); // the sessions are opened lazily, only while someone scrolls
            else if (!mixer.HasApp(key)) mixer.Refresh(); // it may have started playing since the last look
            float v = mixer.VolumeOf(key);
            if (v >= 0) { // negative: the app has no audio session
                v = anim::Clamp01(v + notch * 0.04f); // 4% a notch, as the quick settings slider
                mixer.SetVolume(key, v);
                wheelApp = a.key, wheelAt = ui.Now(), wheelVol = v;
            }
            if (!(qsOpen && qsPage == 4)) ::SetTimer(hwnd, kMixCloseTimer, 3000, nullptr); // let the sessions go once the scrolling stops
        }
        if (wheelApp == a.key && ui.Now() - wheelAt < 1.4) { // its level as a ring around it, for a moment
            float k = anim::Clamp01((float)(1.4 - (ui.Now() - wheelAt)) / 0.3f); // fades over its last 0.3 s
            D2D1_POINT_2F o = {cx, (icon.top + icon.bottom) / 2};
            c.Ring(o, size / 2 + 3, t.track.Alpha(k), 2.5f);
            c.Arc(o, size / 2 + 3, -1.5708f, 6.2832f * wheelVol, t.accentHi.Alpha(k), 2.5f);
            ui.KeepAlive();
        }

        int unreadN = 0; // notifications it sent since it was last in front: a count on its corner
        if (auto u = unread.find(a.key); u != unread.end()) unreadN = u->second;
        else if (!a.aumid.empty()) { // toasts are keyed by the sender's app id, lowercased
            std::wstring k = a.aumid;
            for (auto& ch : k) ch = (wchar_t)towlower(ch);
            if (auto u2 = unread.find(k); u2 != unread.end()) unreadN = u2->second;
        }
        if (unreadN > 0) {
            D2D1_POINT_2F o = {tile.right - 3, tile.top + 3};
            std::wstring num = unreadN > 9 ? L"9+" : std::to_wstring(unreadN); // two characters at most, to fit the corner
            float bw = num.size() > 1 ? 20.0f : 16.0f; // a circle for one digit, a short pill for two
            c.Fill(Rect(o.x - bw / 2, o.y - 8, bw, 16), 8, t.accent.Alpha(grow)); // its notifications: an accent bead, top right
            c.Text(num, Rect(o.x - bw / 2, o.y - 8, bw, 16), Weight::Semi, 10, t.onAccent.Alpha(grow), Align::Center);
        }
        const Badge* b = tasks->BadgeOf(a);
        if (b && b->overlay) { // the app's own badge on its icon's corner, as windows draws it; remade when its ver moves
            if (ID2D1Bitmap* ov = IconBitmap(b->overlay, (uint64_t)id << 8 | 0xB, b->ver)) c.Image(ov, Rect(icon.right - 13, icon.bottom - 13, 15, 15), 0, grow);
        }
        float dotY = tile.bottom + 4; // 3 under the tile, in the dock's padding
        if (b && b->state) {
            DrawProgress(*b, cx, tile.bottom - 5, kIcon - 10, grow, t); // over the tile's foot
        }
        if (!a.wins.empty()) { // running: a dot per window up to three; the app in front, one glowing accent bar; calling, it pulses
            float pulse = calling && pulsing ? 0.5f + 0.5f * std::sin((float)ui.Now() * 5) : 0;
            if (calling && pulsing) ui.KeepAlive();
            int dots = (int)(std::min)((size_t)3, a.wins.size());
            Color ink = calling ? t.accentHi : t.text.Alpha(0.45f).Mix(t.accentHi, act);
            if (act > 0.01f) c.Glow({cx, dotY}, 12 * act, 5 * act, t.accent.Alpha(0.5f * act * grow));
            float bar = (10 + 6 * pulse) * act; // the dots draw together into one bar as it comes to the front
            float span = (dots - 1) * 5.0f * (1 - act);
            for (int k = 0; k < dots; ++k) {
                float dx = dots > 1 ? -span / 2 + span * k / (dots - 1) : 0;
                float iw = (3 + bar) * grow;
                c.Fill(Rect(cx + dx - iw / 2, dotY - 1.5f, iw, 3), 1.5f, ink.Alpha(grow));
            }
        }
        if (h.clicked && !dragged) clicked = i;
        if (midClick && Inside(r, input.mouse)) launched = i; // middle click
        if (input.rclick && Inside(r, input.mouse)) sheet = i;
    }
    if (hovered >= 0 && !dragging && !card.Shown()) { // the app's name on a small pill over it
        const D2D1_RECT_F& ic = slots[apps[hovered].key];
        float tw = (std::min)(TextW(apps[hovered].name, Weight::Medium, 12) + 20, 300.0f), cx = (ic.left + ic.right) / 2;
        D2D1_RECT_F tip = Rect(cx - tw / 2, wegR.top - 32, tw, 24);
        c.Fill(tip, 12, t.bg.Alpha(1).Mix(t.text, 0.08f));
        c.StrokeV(tip, 12, t.hairTop, t.hairBot, 1);
        c.Text(apps[hovered].name, Inset(tip, 10, 0), Weight::Medium, 12, t.text, Align::Center);
    }

    if (dragging && input.released) { // dropped
        if (drop != dragFrom) {
            if (dragFrom < pinned) tasks->MovePin(dragFrom, drop);
            else tasks->Pin(dragFrom, drop); // a running app dropped among the pins becomes one there
        }
        dragging = false;
        dragged = true; // the release that ends a drag isn't a click
        dragFrom = -1;
    }
    if (input.pressed) dragged = false;
    std::wstring want = hovered >= 0 && !AnyOpen() && !apps[hovered].wins.empty() ? apps[hovered].key : L""; // the card over an app at rest under the mouse
    if (want != previewMuted) previewMuted.clear(); // the mouse moved to another app: cards may show again
    if (!previewMuted.empty()) want.clear();
    if (clicked >= 0 || launched >= 0 || sheet >= 0 || dragging) { // the click does the job: no card until the mouse moves on
        if (hovered >= 0) previewMuted = apps[hovered].key;
        want.clear();
        card.Hide();
    }
    if (want != hoverApp) {
        hoverApp = want;
        if (!want.empty()) {
            hoverSlot = slots[want];
            KillTimer(hwnd, kPreviewHideTimer);
            ::SetTimer(hwnd, kPreviewTimer, card.Shown() ? 1 : kPreviewMs, nullptr); // one card already up: the next follows at once
        } else {
            KillTimer(hwnd, kPreviewTimer);
            if (card.Shown()) ::SetTimer(hwnd, kPreviewHideTimer, kPreviewHideMs, nullptr); // a grace period, so the mouse can reach the card itself
        }
    }
    bool shift = GetKeyState(VK_SHIFT) < 0, ctrl = GetKeyState(VK_CONTROL) < 0;
    if (clicked >= 0 && shift) tasks->Launch(clicked, ctrl); // as windows: shift+click a new window, ctrl+shift+click one as administrator
    else if (clicked >= 0) tasks->Click(clicked);
    if (launched >= 0 && !apps[launched].wins.empty()) { // middle click closes its window: the one used last
        const TaskWin* last = &apps[launched].wins[0];
        for (const auto& w : apps[launched].wins)
            if (w.used > last->used) last = &w;
        PostMessageW(last->hwnd, WM_SYSCOMMAND, SC_CLOSE, 0); // as its own close button: the app may still ask to save
    } else if (launched >= 0) {
        tasks->Launch(launched); // nothing open: middle click starts it
    }
    if (sheet >= 0) {
        menuApp = apps[sheet].key;
        PostMessageW(hwnd, WM_APP_MENU, 4, 0); // a menu runs its own loop: after the frame
    }
}

void Bar::DrawApps(const Theme& t, const D2D1_RECT_F& r, bool live) { // live: the dock's apps as ever; in edit mode only their faces, the block moves as one
    if (!tasks) return;
    Canvas& c = ui.C();
    const auto& apps = tasks->Apps();
    int n = (std::min)(dockCount, (int)apps.size());
    dockX = r.left;
    for (int i = 0; i < n; ++i) wegX.push_back(r.left + i * slotW + (slotW - kIcon - kWegGap) / 2), wegW.push_back(kIcon), wegS.push_back(1);
    if (live) DrawDock(t);
    else
        for (int i = 0; i < n; ++i) DrawApp(&apps[(size_t)i], Rect(wegX[(size_t)i] + kIcon / 2 - kAppIcon / 2, r.top + kIcon / 2 - kAppIcon / 2, kAppIcon, kAppIcon), 0.8f, t);
    if (moreN <= 0) return;
    D2D1_RECT_F m = Rect(r.right - kIcon, r.top, kIcon, kIcon); // the ones without room: how many, and a click away
    uint32_t id = Ui::Id("weg.more");
    Ui::Hit h = live ? ui.Button(id, m) : Ui::Hit{};
    bool open = wgOpen && wgKind == kWgApps;
    DrawTile(t, m, live ? ui.Smooth(id + 1, h.hover || open ? 1.0f : 0, 18) : 0, 1);
    c.Text(L"+" + std::to_wstring(moreN), m, Weight::Semi, 13, open ? t.accentHi : t.text, Align::Center);
    if (h.clicked) OpenWidget(kWgApps, (m.left + m.right) / 2);
}

void Bar::DrawSheet(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, sheetSpring.value);
    const TaskApp* a = FindApp(sheetKey);
    if (rise < 1 || !shape || !a) {
        if (sheetOpen && !a && tasks) SetSheet(L""); // its app is gone
        return;
    }
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float tall = SheetRise(), alpha = anim::Clamp01((rise - 30) / (tall - 30)); // contents fade in only once the island has room for them
    if (alpha <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), alpha), nullptr);
    int ai = (int)(a - tasks->Apps().data());
    float x0 = W / 2 - kSheetW / 2 + 18, x1 = W / 2 + kSheetW / 2 - 18, y = pill.top + CallH() + 16;

    DrawApp(a, Rect(x0, y, 26, 26), 1, t); // who it is
    size_t nw = a->wins.size();
    std::wstring sub = nw ? std::to_wstring(nw) + (nw == 1 ? L" window" : L" windows") : L"Not running";
    if (a->pinned) sub += L"  ·  pinned";
    c.Text(a->name, Rect(x0 + 36, y - 2, x1 - x0 - 36, 18), Weight::Medium, 14, t.text);
    c.Text(sub, Rect(x0 + 36, y + 16, x1 - x0 - 36, 14), Weight::Light, 11, t.textDim);
    y += kSheetHead - 16;

    HWND fore = tasks->Fore(), activate = nullptr, close = nullptr;
    for (size_t i = 0; i < nw && i < (size_t)kSheetRows; ++i) { // its windows: pick one, or close it
        const TaskWin& w = a->wins[i];
        D2D1_RECT_F row = Rect(x0 - 8, y, x1 - x0 + 16, kSheetRow - 4);
        uint32_t id = Ui::Id("sheet.win", (int)i);
        Ui::Hit h = ui.Button(id, row);
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(row, 10, t.text.Alpha(0.09f * hov));
        if (w.hwnd == fore) c.Dot({row.left + 12, (row.top + row.bottom) / 2}, 3, t.accent);
        c.Text(w.title, Rect(row.left + 24, row.top, Width(row) - 60, Height(row)), Weight::Regular, 12, t.text.Mix(t.textDim, w.hwnd == fore ? 0 : 0.3f));
        if (w::IconBtn(ui, t, Ui::Id("sheet.close", (int)i), {row.right - 16, (row.top + row.bottom) / 2}, 22, glyph::CloseSmall, 16)) close = w.hwnd;
        else if (h.clicked) activate = w.hwnd;
        y += kSheetRow;
    }

    int openRecent = -1;
    if (!sheetRecent.empty()) { // what it opened lately, as its jump list: click to open it again
        c.TextSp(L"RECENT", Rect(x0, y + 6, 120, 12), Weight::Medium, 9.5f, t.textLow, 1.2f);
        y += kRecentHead;
        for (size_t i = 0; i < sheetRecent.size(); ++i) {
            D2D1_RECT_F row = Rect(x0 - 8, y, x1 - x0 + 16, kRecentRow - 3);
            uint32_t id = Ui::Id("sheet.recent", (int)i);
            Ui::Hit h = ui.Button(id, row);
            float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
            if (hov > 0.01f) c.Fill(row, 10, t.text.Alpha(0.09f * hov));
            c.Icon(glyph::Doc, {row.left + 14, (row.top + row.bottom) / 2}, 15, t.textDim);
            c.Text(sheetRecent[i].name, Rect(row.left + 28, row.top, Width(row) - 34, Height(row)), Weight::Regular, 11.5f, t.text.Mix(t.textDim, 0.2f));
            if (h.clicked) openRecent = (int)i;
            y += kRecentRow;
        }
    }

    struct Act {
        int glyph;
        const wchar_t* label;
        bool enabled;
    };
    const Act acts[] = {{glyph::OpenNew, L"New window", true},
                        {a->pinned ? glyph::Unpin : glyph::Pin, a->pinned ? L"Unpin" : L"Pin", a->pinned || !a->exe.empty() || !a->aumid.empty()},
                        {glyph::Close, L"Close all", nw > 0}};
    float bw = (x1 - x0 - 2 * 8) / 3, by = y + 6;
    int picked = -1;
    for (int i = 0; i < 3; ++i) {
        D2D1_RECT_F r = Rect(x0 + i * (bw + 8), by, bw, 34);
        uint32_t id = Ui::Id("sheet.act", i);
        Ui::Hit h = acts[i].enabled ? ui.Button(id, r) : Ui::Hit{};
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16), k = acts[i].enabled ? 1.0f : 0.4f;
        c.Fill(r, 17, t.card.Mix(t.accent.Alpha(0.25f), hov).Alpha(k));
        c.Icon(acts[i].glyph, {r.left + 20, (r.top + r.bottom) / 2}, 16, t.accentHi.Alpha(k));
        c.Text(acts[i].label, Rect(r.left + 32, r.top, bw - 36, 34), Weight::Medium, 11.5f, t.text.Alpha(k));
        if (h.clicked) picked = i;
    }
    if (ui.RClick(pill)) SetSheet(L""); // right click in the sheet itself closes it
    rt->PopLayer();

    if (close) PostMessageW(close, WM_SYSCOMMAND, SC_CLOSE, 0); // as its own close button: the app may still ask to save
    if (openRecent >= 0) { // the file itself, with whatever opens it: as clicking it in a jump list
        SHELLEXECUTEINFOW ex = {sizeof(ex)};
        ex.fMask = SEE_MASK_IDLIST | SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK; // by pidl, so files with no path still open; async keeps the frame quick
        ex.lpIDList = (void*)sheetRecent[openRecent].pidl.data();
        ex.nShow = SW_SHOWNORMAL;
        ShellExecuteExW(&ex);
        SetSheet(L"");
        return;
    }
    if (activate) {
        Tasks::Activate(activate);
        SetSheet(L"");
    }
    if (picked == 0) tasks->Launch(ai), SetSheet(L"");
    else if (picked == 1) a->pinned ? tasks->Unpin(ai) : tasks->Pin(ai);
    else if (picked == 2) tasks->CloseAll(ai), SetSheet(L"");
}

void Bar::ReadRecent(const TaskApp& a) { // windows keeps each app's recent files by its app id: the same list its jump list shows
    sheetRecent.clear();
    std::wstring appId = !a.aumid.empty() ? a.aumid : a.key; // an explicit app id wins over the resolved one
    if (appId.empty()) return;
    ComPtr<IApplicationDocumentLists> lists;
    ComPtr<IObjectArray> items;
    if (FAILED(CoCreateInstance(CLSID_ApplicationDocumentLists, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&lists))) || FAILED(lists->SetAppID(appId.c_str())) ||
        FAILED(lists->GetList(ADLT_RECENT, kRecentMax, IID_PPV_ARGS(&items))))
        return;
    UINT n = 0;
    items->GetCount(&n);
    for (UINT i = 0; i < n && i < kRecentMax; ++i) {
        ComPtr<IShellItem> item;
        if (FAILED(items->GetAt(i, IID_PPV_ARGS(&item)))) continue;
        PWSTR name = nullptr;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) && SUCCEEDED(SHGetIDListFromObject(item.Get(), &pidl))) {
            UINT bytes = ILGetSize(pidl); // kept as raw bytes: the pidl is freed below, the copy lives with the sheet
            Recent r;
            r.name = name;
            r.pidl.assign(reinterpret_cast<BYTE*>(pidl), reinterpret_cast<BYTE*>(pidl) + bytes);
            sheetRecent.push_back(std::move(r));
        }
        CoTaskMemFree(name);
        CoTaskMemFree(pidl);
    }
}
