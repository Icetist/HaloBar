#include "preview.h"

#include <windowsx.h>

#include <algorithm>
#include <cmath>

#include "glyphs.h"
#include "tasks.h"

namespace {

constexpr float kShotW = 196, kShotH = 112; // one window's thumbnail box, dips
constexpr float kTitleH = 28, kPad = 10, kGap = 8, kRise = 10; // title line over it, card padding, between tiles, above the island
constexpr float kRadius = 16; // island's card radius
constexpr float kMediaH = 44; // the transport row under the thumbnails
constexpr int kMaxTiles = 6; // more windows than this: the sheet lists them all
constexpr wchar_t kClass[] = L"HaloBarPreview";

}

Preview::~Preview() { Hide(); } // thumbnails are dwm resources: always unregister

void Preview::Show(HWND own, const std::wstring& k, const std::vector<Win>& wins, POINT at, float sc, const Theme& theme, int med) {
    if (wins.empty()) {
        Hide();
        return;
    }
    bool same = hwnd && k == key && wins.size() == tiles.size(); // hover fires every move: don't rebuild for nothing
    for (size_t i = 0; same && i < wins.size(); ++i) same = tiles[i].win.hwnd == wins[i].hwnd && tiles[i].win.title == wins[i].title;
    if (same && at.x == anchor.x && at.y == anchor.y && med == media) return; // already showing exactly this
    media = med;
    owner = own;
    key = k;
    scale = sc;
    t = theme;
    anchor = at; // screen point over the dock icon, in pixels
    Unthumb(); // old thumbnails point at the old tiles
    tiles.clear();
    for (size_t i = 0; i < wins.size() && (int)i < kMaxTiles; ++i) tiles.push_back({wins[i]}); // the rest stay reachable from the app sheet
    if (!hwnd) {
        static bool registered = false;
        HINSTANCE inst = GetModuleHandleW(nullptr);
        if (!registered) {
            WNDCLASSEXW wc = {sizeof(wc)};
            wc.lpfnWndProc = Proc;
            wc.hInstance = inst;
            wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
            wc.lpszClassName = kClass;
            registered = RegisterClassExW(&wc) != 0;
        }
        hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kClass, L"", WS_POPUP, 0, 0, 1, 1, owner, nullptr, inst, this); // no alt-tab
        if (!hwnd) return;
        DWM_WINDOW_CORNER_PREFERENCE round = DWMWCP_ROUND; // windows 11 rounds it and gives it a soft shadow
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &round, sizeof(round));
        BOOL dark = TRUE;
        DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark)); // dark border, to sit with island's black
    }
    Layout(); // size and place first: thumbnails are positioned in client pixels
    Thumbs();
    ShowWindow(hwnd, SW_SHOWNOACTIVATE); // never steal focus from the app under the mouse
    InvalidateRect(hwnd, nullptr, FALSE);
}

void Preview::Hide() {
    Unthumb();
    tiles.clear();
    canvas.reset(); // the canvas holds brushes made on rt, so it goes first
    rt.Reset();
    if (hwnd) DestroyWindow(hwnd); // destroyed, not hidden: nothing kept while idle
    hwnd = nullptr;
    hover = -2; // -2: nothing hovered, unlike -1 which means the media row
    inside = tracking = false;
    key.clear(); // so the next Show never mistakes itself for the same card
}

void Preview::Layout() {
    int n = (int)tiles.size();
    W = kPad * 2 + n * kShotW + (n - 1) * kGap; // tiles side by side, one row
    H = kPad + kTitleH + kShotH + kPad + ((media & kHasMedia) ? kMediaH : 0); // the transport row only when the app plays something
    if ((media & kHasMedia) && W < 200) W = 200; // room for the three buttons
    for (int i = 0; i < n; ++i) {
        float x = kPad + i * (kShotW + kGap);
        tiles[i].box = {x, kPad, x + kShotW, kPad + kTitleH + kShotH}; // title and shot together: the hover target
        tiles[i].shot = {x, kPad + kTitleH, x + kShotW, kPad + kTitleH + kShotH}; // where dwm draws the live window
    }
    int pw = (int)std::lround(W * scale), ph = (int)std::lround(H * scale);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(anchor, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (std::clamp)((int)(anchor.x - pw / 2), (int)mi.rcMonitor.left + 8, (int)(std::max)(mi.rcMonitor.left + 8, mi.rcMonitor.right - 8 - pw));
    int y = anchor.y - (int)std::lround(kRise * scale) - ph; // floats above the island
    SetWindowPos(hwnd, HWND_TOPMOST, x, y, pw, ph, SWP_NOACTIVATE);
    if (rt) rt->Resize({(UINT32)pw, (UINT32)ph});
}

void Preview::Thumbs() {
    for (auto& tl : tiles) {
        if (FAILED(DwmRegisterThumbnail(hwnd, tl.win.hwnd, &tl.thumb))) continue; // the window died meanwhile: its tile stays blank
        SIZE src{};
        DwmQueryThumbnailSourceSize(tl.thumb, &src);
        float bw = kShotW * scale, bh = kShotH * scale;
        float k = src.cx > 0 && src.cy > 0 ? (std::min)(bw / src.cx, bh / src.cy) : 1; // whole window, never cropped or stretched
        float w = src.cx * k, h = src.cy * k;
        float cx = (tl.shot.left + tl.shot.right) / 2 * scale, cy = (tl.shot.top + tl.shot.bottom) / 2 * scale;
        DWM_THUMBNAIL_PROPERTIES p = {};
        p.dwFlags = DWM_TNP_RECTDESTINATION | DWM_TNP_VISIBLE | DWM_TNP_SOURCECLIENTAREAONLY | DWM_TNP_OPACITY;
        p.rcDestination = {(LONG)std::lround(cx - w / 2), (LONG)std::lround(cy - h / 2), (LONG)std::lround(cx + w / 2), (LONG)std::lround(cy + h / 2)};
        p.fVisible = TRUE;
        p.fSourceClientAreaOnly = FALSE; // with its title bar, as windows' own previews show it
        p.opacity = 255;
        DwmUpdateThumbnailProperties(tl.thumb, &p); // dwm composes it live over the card, no copying
    }
}

void Preview::Unthumb() {
    for (auto& tl : tiles)
        if (tl.thumb) DwmUnregisterThumbnail(tl.thumb), tl.thumb = nullptr;
}

D2D1_POINT_2F Preview::MediaAt(int i) const { return {W / 2 + (i - 1) * 52.0f, H - kPad - kMediaH / 2 + 4}; } // 0 prev, 1 play, 2 next, centred

int Preview::HitMedia(float x, float y) const {
    if (!(media & kHasMedia)) return -1;
    for (int i = 0; i < 3; ++i) {
        D2D1_POINT_2F o = MediaAt(i);
        if (std::hypot(x - o.x, y - o.y) < 20) return i; // round hit, a bit bigger than the drawn dot
    }
    return -1;
}

int Preview::HitTile(float x, float y, bool* onClose) const {
    for (size_t i = 0; i < tiles.size(); ++i) {
        const auto& b = tiles[i].box;
        if (x < b.left - kGap / 2 || x > b.right + kGap / 2 || y < b.top - kPad || y > b.bottom + kPad) continue; // gaps count too, so hover never flickers between tiles
        if (onClose) *onClose = x > b.right - 26 && y < b.top + kTitleH; // the x in the title line's corner
        return (int)i;
    }
    return -1;
}

void Preview::Paint() {
    PAINTSTRUCT ps;
    BeginPaint(hwnd, &ps);
    EndPaint(hwnd, &ps); // validates the window; d2d draws outside the paint pair
    if (!rt) { // made lazily, and again after a device loss
        RECT rc;
        GetClientRect(hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                                                           D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * scale, 96 * scale);
        if (FAILED(gfx::D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(hwnd, D2D1::SizeU(rc.right, rc.bottom)), &rt))) return;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        canvas = std::make_unique<Canvas>(rt.Get());
    }
    Canvas& c = *canvas;
    bool light = theme::Luma(t.text) < 0.5f;
    Color base = light ? t.bg.Alpha(1 / (std::max)(0.01f, t.bg.a)) : Color::Rgb(0x0E0F12); // an opaque card: island's black, or the light look's paper
    rt->BeginDraw();
    rt->Clear(base.D2D());
    c.NewFrame();
    c.Stroke({0.5f, 0.5f, W - 0.5f, H - 0.5f}, kRadius, t.line, 1); // half-dip inset keeps the hairline crisp
    for (size_t i = 0; i < tiles.size(); ++i) {
        const Tile& tl = tiles[i];
        bool hot = hover == (int)i;
        if (hot) c.Fill({tl.box.left - 4, tl.box.top - 4, tl.box.right + 4, tl.box.bottom + 4}, 12, t.text.Alpha(0.09f)); // island's hover ink
        float right = hot ? tl.box.right - 28 : tl.box.right - 4; // hovered titles make room for the close x
        c.Text(tl.win.title.empty() ? L"Untitled" : tl.win.title, {tl.box.left + 4, tl.box.top + 4, right, tl.box.top + kTitleH - 4}, Weight::Medium, 12,
               hot ? t.text : t.textDim);
        if (hot) {
            D2D1_POINT_2F x = {tl.box.right - 13, tl.box.top + kTitleH / 2 - 2};
            if (hoverClose) c.Dot(x, 11, Color::Rgb(0xE5484D)); // close red, like a title bar's
            c.Line({x.x - 4, x.y - 4}, {x.x + 4, x.y + 4}, hoverClose ? Color{1, 1, 1, 1} : t.textDim, 1.6f);
            c.Line({x.x + 4, x.y - 4}, {x.x - 4, x.y + 4}, hoverClose ? Color{1, 1, 1, 1} : t.textDim, 1.6f);
        }
    }
    if (media & kHasMedia) { // what's playing: previous, play or pause, next
        const int glyphs[3] = {glyph::Prev, (media & kPlaying) ? glyph::Pause : glyph::Play, glyph::Next};
        const bool enabled[3] = {(media & kCanPrev) != 0, true, (media & kCanNext) != 0};
        for (int i = 0; i < 3; ++i) {
            D2D1_POINT_2F o = MediaAt(i);
            if (i == 1) c.Dot(o, 19, t.accent.Alpha(hoverMedia == 1 ? 1.0f : 0.85f)); // the main button sits on an accent disc
            else if (hoverMedia == i && enabled[i]) c.Dot(o, 17, t.text.Alpha(0.09f));
            Color ink = i == 1 ? t.onAccent : enabled[i] ? t.text : t.textLow;
            c.Icon(glyphs[i], o, i == 1 ? 24.0f : 22.0f, ink, true);
        }
    }
    if (rt->EndDraw() == D2DERR_RECREATE_TARGET) { // gpu reset: rebuild on the next paint
        canvas.reset();
        rt.Reset();
    }
}

LRESULT CALLBACK Preview::Proc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams); // the this passed to CreateWindowEx
    auto* self = (Preview*)GetWindowLongPtrW(h, GWLP_USERDATA);
    return self && self->hwnd == h ? self->Handle(msg, wp, lp) : DefWindowProcW(h, msg, wp, lp); // hwnd isn't set until CreateWindowEx returns
}

LRESULT Preview::Handle(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_MOUSEACTIVATE:
        return MA_NOACTIVATE; // the app keeps the keyboard
    case WM_PAINT:
        Paint();
        return 0;
    case WM_MOUSEMOVE: {
        if (!tracking) { // leave tracking is one-shot: re-arm after each leave
            TRACKMOUSEEVENT tm = {sizeof(tm), TME_LEAVE, hwnd, 0};
            tracking = TrackMouseEvent(&tm) != FALSE;
        }
        inside = true; // the bar keeps the card open while this is set
        bool onClose = false;
        float mx = GET_X_LPARAM(lp) / scale, my = GET_Y_LPARAM(lp) / scale; // signed: GET_X_LPARAM, not LOWORD
        int hm = HitMedia(mx, my);
        int h = hm >= 0 ? -1 : HitTile(mx, my, &onClose); // the media row wins over a tile under it
        if (h != hover || onClose != hoverClose || hm != hoverMedia) {
            hover = h;
            hoverClose = onClose;
            hoverMedia = hm;
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        return 0;
    }
    case WM_MOUSELEAVE:
        tracking = inside = false;
        hover = -2;
        InvalidateRect(hwnd, nullptr, FALSE);
        PostMessageW(owner, kLeft, 0, 0); // the bar decides whether to close, the mouse may be back on the dock
        return 0;
    case WM_LBUTTONUP:
    case WM_MBUTTONUP: {
        if (int m = HitMedia(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale); m >= 0 && msg == WM_LBUTTONUP) {
            PostMessageW(owner, kMediaCmd, (WPARAM)m, 0); // the card stays: the song changes under it
            return 0;
        }
        bool onClose = false;
        int h = HitTile(GET_X_LPARAM(lp) / scale, GET_Y_LPARAM(lp) / scale, &onClose);
        if (h < 0) return 0;
        HWND target = tiles[h].win.hwnd;
        if (msg == WM_MBUTTONUP || onClose) { // close that window, as its own close button would; the card stays for the rest
            PostMessageW(target, WM_SYSCOMMAND, SC_CLOSE, 0); // posted, so an app asking to save can't hang the bar
            DwmUnregisterThumbnail(tiles[h].thumb);
            tiles.erase(tiles.begin() + h);
            if (tiles.empty()) {
                PostMessageW(owner, kDone, 0, 0); // posted: the bar hides this card, can't destroy it from inside its own proc
                return 0;
            }
            Unthumb(); // the rest shift left: re-register at their new spots
            hover = -2;
            Layout();
            Thumbs();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        Tasks::Activate(target); // restores it if minimized, then brings it in front
        PostMessageW(owner, kDone, 0, 0);
        return 0;
    }
    case WM_DESTROY:
        Unthumb(); // dwm thumbnails must go before their destination window does
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}
