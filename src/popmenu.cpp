#include "popmenu.h"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>

#include "glyphs.h"

namespace popmenu {
namespace {

constexpr float kMinW = 208, kMaxW = 380, kPad = 6, kRow = 32, kSep = 9, kSection = 26, kHead = 50; // dips: island's tray menu, tightened for longer lists
constexpr float kIconX = 18, kTextX = 38, kHintGap = 20, kSideR = 14, kChevronW = 20, kCheckW = 22, kRadius = 8;
constexpr float kLabelSize = 12.5f, kHintSize = 11, kSectionSize = 10.5f, kGlyphSize = 18;
constexpr float kOpenMs = 150, kSlide = 6; // the content rises into place as it fades in
constexpr float kSubGap = 4; // between the menu and its submenu, dips
constexpr UINT_PTR kAnimTimer = 1, kSubTimer = 2;
constexpr UINT kFrameMs = 10, kSubDelayMs = 220; // a submenu waits a moment, so a diagonal move toward it doesn't switch rows
const Color kDangerDark = Color::Rgb(0xFF8A80), kDangerLight = Color::Rgb(0xC62828); // island's exit tint, and one that reads on paper
const wchar_t kClass[] = L"HaloBarMenu";

struct Pane {
    HWND hwnd = nullptr;
    const std::vector<Item>* items = nullptr;
    const Head* head = nullptr;
    std::vector<float> tops, hintW; // each item's top, its hint's width: measured once, not per paint
    float w = 0, h = 0;
    int hover = -1;
    bool tracking = false;
    float slide = 0; // where the content starts: +down, -up, dips
    ULONGLONG born = 0;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Layer> layer;
    ComPtr<ID2D1Bitmap> pic;
    std::unique_ptr<Canvas> canvas;
};

struct State {
    HWND owner = nullptr;
    Theme t;
    float scale = 1;
    bool light = false, glass = false;
    Pane main, sub;
    int subOf = -1; // the main row whose submenu is open
    bool inSub = false; // the keyboard works in the submenu
    bool armed = false; // a button release picks only after a press or a move in the menu: the release that opened it never does
    POINT openedAt{};
    RECT work{};
    UINT pick = 0;
    bool done = false;
};
State* s = nullptr; // the open menu, only while Pick runs

float Ease(float x) { return 1 - (1 - x) * (1 - x) * (1 - x); }

float Measure(const std::wstring& text, Weight w, float size) {
    IDWriteTextFormat* f = gfx::TextFmt(w, size, Align::Left);
    ComPtr<IDWriteTextLayout> l;
    DWRITE_TEXT_METRICS m{};
    if (text.empty() || !f || FAILED(gfx::DWrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), f, 4096, 64, &l)) || FAILED(l->GetMetrics(&m))) return 0;
    return m.widthIncludingTrailingWhitespace;
}

float ItemH(const Item& it) { return it.kind == Item::kSep ? kSep : it.kind == Item::kSection ? kSection : kRow; }

bool Selectable(const Item& it) { return it.kind == Item::kRow && !it.disabled; }

void Lay(Pane& p) {
    const auto& items = *p.items;
    float y = p.head ? kHead + 2 : kPad, w = kMinW;
    p.tops.clear();
    p.hintW.clear();
    for (const Item& it : items) {
        p.tops.push_back(y);
        p.hintW.push_back(it.hint.empty() ? 0 : Measure(it.hint, Weight::Regular, kHintSize));
        y += ItemH(it);
        if (it.kind == Item::kSep) continue;
        float need = kPad + (it.kind == Item::kSection ? 12 : kTextX) + Measure(it.label, it.kind == Item::kSection ? Weight::Medium : Weight::Regular, it.kind == Item::kSection ? kSectionSize : kLabelSize);
        if (!it.hint.empty()) need += kHintGap + p.hintW.back();
        if (!it.sub.empty()) need += kChevronW;
        if (it.checked) need += kCheckW;
        w = (std::max)(w, need + kSideR + kPad);
    }
    if (p.head) w = (std::max)(w, kPad + 48 + (std::max)(Measure(p.head->title, Weight::Medium, 14), Measure(p.head->sub, Weight::Light, 11)) + 16);
    p.w = std::ceil((std::min)(w, kMaxW));
    p.h = y + kPad;
}

int HitAt(const Pane& p, float x, float y) {
    if (x < 0 || x >= p.w) return -1;
    for (size_t i = 0; i < p.tops.size(); ++i) {
        const Item& it = (*p.items)[i];
        if (it.kind == Item::kRow && y >= p.tops[i] && y < p.tops[i] + kRow) return (int)i;
    }
    return -1;
}

int Step(const Pane& p, int from, int dir) { // the next row the keyboard can land on, wrapping; from -1 starts at an end
    int n = (int)p.items->size(), i = from;
    if (i < 0) i = dir > 0 ? -1 : n;
    for (int k = 0; k < n; ++k) {
        i = ((i + dir) % n + n) % n;
        if (Selectable((*p.items)[i])) return i;
    }
    return from;
}

void Redraw(Pane& p) {
    if (p.hwnd) InvalidateRect(p.hwnd, nullptr, FALSE);
}

void Finish(UINT id) {
    if (s->done) return; // the first answer stands: tearing the windows down deactivates them too
    s->pick = id;
    s->done = true;
    if (s->main.hwnd) PostMessageW(s->main.hwnd, WM_NULL, 0, 0); // wakes the loop if this came from a hook callback
}

ComPtr<ID2D1Bitmap> Logo(ID2D1RenderTarget* rt, int px) { // halobar's own colors, as on the taskbar
    ComPtr<ID2D1Bitmap> out;
    HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
    ComPtr<IWICBitmap> wb;
    ComPtr<IWICFormatConverter> conv;
    if (ico && gfx::Wic() && SUCCEEDED(gfx::Wic()->CreateBitmapFromHICON(ico, &wb)) && SUCCEEDED(gfx::Wic()->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    if (ico) DestroyIcon(ico);
    return out;
}

ComPtr<ID2D1Bitmap> FromSource(ID2D1RenderTarget* rt, IWICBitmapSource* src) {
    ComPtr<ID2D1Bitmap> out;
    ComPtr<IWICFormatConverter> conv;
    if (src && gfx::Wic() && SUCCEEDED(gfx::Wic()->CreateFormatConverter(&conv)) &&
        SUCCEEDED(conv->Initialize(src, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
        rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &out);
    return out;
}

void DrawHead(Pane& p, Canvas& c) {
    const Theme& t = s->t;
    const Head& hd = *p.head;
    c.FillV({0, 0, p.w, kHead + 18}, 0, t.accent.Alpha(0.12f), t.accent.Alpha(0)); // the accent's wash behind the header, as the tray menu
    D2D1_POINT_2F badge = {kPad + 20, kHead / 2 + 1};
    D2D1_RECT_F at = {badge.x - 14, badge.y - 14, badge.x + 14, badge.y + 14};
    if (p.pic && hd.round) c.Image(p.pic.Get(), at, 14);
    else if (p.pic) c.Pixels(p.pic.Get(), hd.logo ? at : Inset(at, 2, 2));
    else {
        c.Dot(badge, 15, t.accent.Alpha(0.20f));
        c.Icon(hd.glyph ? hd.glyph : glyph::Sparkle, badge, 18, t.accentHi, true);
    }
    float x = kPad + 44, right = p.w - kPad - 8;
    if (hd.sub.empty()) {
        c.Text(hd.title, {x, 0, right, kHead + 2}, Weight::Medium, 14, t.text);
        return;
    }
    c.Text(hd.title, {x, 8, right, 28}, Weight::Medium, 14, t.text);
    c.Text(hd.sub, {x, 27, right, 42}, Weight::Light, 11, t.textLow);
}

void DrawRow(Pane& p, Canvas& c, const Item& it, float y, float hintW, bool hot) {
    const Theme& t = s->t;
    Color danger = s->light ? kDangerLight : kDangerDark;
    D2D1_RECT_F r = {kPad, y + 1, p.w - kPad, y + kRow - 1};
    float cy = (r.top + r.bottom) / 2;
    if (hot) c.Fill(r, 8, it.danger ? danger.Alpha(0.15f) : t.accent.Alpha(s->light ? 0.16f : 0.22f)); // the accent under the row, red under a destructive one
    Color ink = it.disabled ? t.textLow : hot && it.danger ? danger : t.text;
    Color icon = it.disabled ? t.textLow.Alpha(0.7f) : it.danger ? danger.Alpha(hot ? 1.0f : 0.8f) : hot ? t.accentHi : t.textDim;
    if (it.glyph) c.Icon(it.glyph, {r.left + kIconX, cy}, kGlyphSize, icon, hot);
    float right = r.right - kSideR + 4;
    if (!it.sub.empty()) {
        c.Icon(glyph::ChevronRight, {r.right - 12, cy}, 16, hot ? t.text : t.textDim);
        right -= kChevronW - 4;
    }
    if (it.checked) {
        c.Icon(glyph::Check, {right - 6, cy}, 16, it.disabled ? t.textLow : t.accentHi, true);
        right -= kCheckW;
    }
    if (!it.hint.empty()) {
        float hw = hintW + 2;
        c.Text(it.hint, {right - hw, r.top, right, r.bottom}, Weight::Regular, kHintSize, hot && !it.disabled ? t.textDim : t.textLow, Align::Right);
        right -= hw + kHintGap / 2;
    }
    c.Text(it.label, {r.left + kTextX, r.top, right, r.bottom}, Weight::Regular, kLabelSize, ink);
}

void DrawItems(Pane& p, Canvas& c) {
    const Theme& t = s->t;
    const auto& items = *p.items;
    bool isMain = &p == &s->main;
    for (size_t i = 0; i < items.size(); ++i) {
        const Item& it = items[i];
        float y = p.tops[i];
        if (it.kind == Item::kSep) {
            c.Line({kPad + 12, y + kSep / 2}, {p.w - kPad - 12, y + kSep / 2}, t.hairTop, 1, false);
            continue;
        }
        if (it.kind == Item::kSection) {
            c.Text(it.label, {kPad + 12, y + 4, p.w - kPad - 12, y + kSection}, Weight::Medium, kSectionSize, t.textLow);
            continue;
        }
        bool hot = (int)i == p.hover && !it.disabled;
        if (isMain && (int)i == s->subOf) hot = true; // the row whose submenu is open stays lit
        DrawRow(p, c, it, y, p.hintW[i], hot);
    }
}

bool MakeTarget(Pane& p) {
    RECT rc;
    GetClientRect(p.hwnd, &rc);
    D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, s->glass ? D2D1_ALPHA_MODE_PREMULTIPLIED : D2D1_ALPHA_MODE_IGNORE), 96 * s->scale, 96 * s->scale); // glass: see-through where dwm's blur sits
    if (FAILED(gfx::D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(p.hwnd, D2D1::SizeU(rc.right, rc.bottom)), &p.rt))) return false;
    p.rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    p.rt->CreateLayer(nullptr, &p.layer);
    p.canvas = std::make_unique<Canvas>(p.rt.Get());
    if (p.head && p.head->logo) p.pic = Logo(p.rt.Get(), (int)std::lround(28 * s->scale));
    else if (p.head && p.head->pic) p.pic = FromSource(p.rt.Get(), p.head->pic);
    return true;
}

void Paint(Pane& p) {
    if (!p.rt && !MakeTarget(p)) return;
    const Theme& t = s->t;
    Canvas& c = *p.canvas;
    float k = Ease((std::min)(1.0f, (float)(GetTickCount64() - p.born) / kOpenMs));
    p.rt->BeginDraw();
    p.rt->SetTransform(D2D1::IdentityMatrix());
    if (s->glass) {
        p.rt->Clear({0, 0, 0, 0}); // dwm's acrylic shows through
        c.Fill({0, 0, p.w, p.h}, 0, t.bg);
        c.FillV({0, 0, p.w, (std::min)(p.h, 90.0f)}, 0, t.sheen, t.sheen.Alpha(0)); // island's gloss from the top
    } else {
        p.rt->Clear(t.bg.D2D());
    }
    if (p.layer) p.rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), nullptr, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), k), p.layer.Get());
    p.rt->SetTransform(D2D1::Matrix3x2F::Translation(0, p.slide * (1 - k)));
    if (p.head) DrawHead(p, c);
    DrawItems(p, c);
    p.rt->SetTransform(D2D1::IdentityMatrix());
    if (p.layer) p.rt->PopLayer();
    c.StrokeV({0, 0, p.w, p.h}, kRadius, s->glass ? Color{1, 1, 1, 0.30f} : t.hairTop, t.hairBot, 1); // the rim, lit from above; dwm rounds the outside
    if (p.rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        p.canvas.reset();
        p.pic.Reset();
        p.layer.Reset();
        p.rt.Reset();
    }
}

COLORREF Ref(Color c) { return RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)); }

bool Open(Pane& p, RECT r, bool isSub) {
    p.hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | (isSub ? WS_EX_NOACTIVATE : 0), kClass, isSub ? L"" : L"HaloBar menu", WS_POPUP, r.left, r.top,
                             r.right - r.left, r.bottom - r.top, isSub ? s->main.hwnd : s->owner, nullptr, GetModuleHandleW(nullptr), &p);
    if (!p.hwnd) return false;
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND; // windows 11 rounds it and gives it its soft shadow
    DwmSetWindowAttribute(p.hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    BOOL dark = !s->light;
    DwmSetWindowAttribute(p.hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF border = Ref(s->glass ? Color{0.5f, 0.5f, 0.55f, 1} : s->t.track);
    DwmSetWindowAttribute(p.hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    if (s->glass) { // acrylic behind the card, as windows' own flyouts
        int backdrop = DWMSBT_TRANSIENTWINDOW;
        DwmSetWindowAttribute(p.hwnd, DWMWA_SYSTEMBACKDROP_TYPE, &backdrop, sizeof(backdrop));
        MARGINS m = {-1, -1, -1, -1};
        DwmExtendFrameIntoClientArea(p.hwnd, &m);
    }
    p.born = GetTickCount64();
    SetTimer(p.hwnd, kAnimTimer, kFrameMs, nullptr); // only while the content fades in
    ShowWindow(p.hwnd, isSub ? SW_SHOWNOACTIVATE : SW_SHOW);
    return true;
}

RECT Clamp(RECT r) { // inside the work area, as far as it fits
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = (std::max)((std::min)((int)r.left, (int)s->work.right - w), (int)s->work.left);
    int y = (std::max)((std::min)((int)r.top, (int)s->work.bottom - h), (int)s->work.top);
    return {x, y, x + w, y + h};
}

void CloseSub() {
    if (s->sub.hwnd) DestroyWindow(s->sub.hwnd);
    s->sub = Pane{};
    if (s->subOf >= 0) Redraw(s->main);
    s->subOf = -1;
    s->inSub = false;
}

void OpenSub(int i, bool keyboard) {
    if (s->subOf == i && s->sub.hwnd) {
        if (keyboard) s->inSub = true, s->sub.hover = Step(s->sub, -1, 1), Redraw(s->sub);
        return;
    }
    CloseSub();
    const Item& it = (*s->main.items)[i];
    if (it.sub.empty() || it.disabled) return;
    s->sub.items = &it.sub;
    Lay(s->sub);
    RECT mr;
    GetWindowRect(s->main.hwnd, &mr);
    int w = (int)std::ceil(s->sub.w * s->scale), h = (int)std::ceil(s->sub.h * s->scale), gap = (int)std::lround(kSubGap * s->scale);
    int x = mr.right + gap;
    if (x + w > s->work.right) x = mr.left - gap - w; // no room on the right: it opens to the left, as native menus do
    int y = mr.top + (int)std::lround((s->main.tops[i] - kPad) * s->scale); // its first row level with the parent's
    s->subOf = i;
    if (!Open(s->sub, Clamp({x, y, x + w, y + h}), true)) return CloseSub();
    if (keyboard) s->inSub = true, s->sub.hover = Step(s->sub, -1, 1);
    Redraw(s->main);
}

void Choose(Pane& p, int i) {
    if (i < 0 || i >= (int)p.items->size()) return;
    const Item& it = (*p.items)[i];
    if (!Selectable(it)) return;
    if (!it.sub.empty()) return OpenSub(i, false);
    Finish(it.id);
}

void Key(WPARAM vk) {
    Pane& p = s->inSub && s->sub.hwnd ? s->sub : s->main;
    switch (vk) {
    case VK_DOWN:
    case VK_UP:
        p.hover = Step(p, p.hover, vk == VK_DOWN ? 1 : -1);
        if (&p == &s->main) KillTimer(s->main.hwnd, kSubTimer);
        Redraw(p);
        break;
    case VK_HOME:
    case VK_END:
        p.hover = Step(p, -1, vk == VK_HOME ? 1 : -1);
        Redraw(p);
        break;
    case VK_RIGHT:
        if (&p == &s->main && p.hover >= 0 && !(*p.items)[p.hover].sub.empty()) OpenSub(p.hover, true);
        break;
    case VK_LEFT:
        if (&p == &s->sub) CloseSub();
        break;
    case VK_ESCAPE:
        if (&p == &s->sub) CloseSub(); // one level back, as native menus
        else Finish(0);
        break;
    case VK_RETURN:
    case VK_SPACE:
        if (p.hover >= 0 && &p == &s->main && !(*p.items)[p.hover].sub.empty()) OpenSub(p.hover, true);
        else Choose(p, p.hover);
        break;
    case VK_MENU:
    case VK_F10:
    case VK_LWIN:
    case VK_RWIN:
        Finish(0);
        break;
    }
}

void Hover(Pane& p, LPARAM lp) {
    if (!p.tracking) {
        TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, p.hwnd, 0};
        p.tracking = TrackMouseEvent(&tme) != FALSE;
    }
    POINT cur;
    GetCursorPos(&cur);
    if (cur.x != s->openedAt.x || cur.y != s->openedAt.y) s->armed = true; // the mouse really moved over the menu
    int i = HitAt(p, GET_X_LPARAM(lp) / s->scale, GET_Y_LPARAM(lp) / s->scale);
    if (&p == &s->sub) {
        KillTimer(s->main.hwnd, kSubTimer); // reached the submenu: it stays
        s->inSub = true;
        if (s->main.hover != s->subOf) s->main.hover = s->subOf, Redraw(s->main);
        if (i != p.hover) p.hover = i, Redraw(p);
        return;
    }
    s->inSub = false;
    if (i == p.hover) return;
    p.hover = i;
    Redraw(p);
    if (s->sub.hwnd && s->sub.hover >= 0) s->sub.hover = -1, Redraw(s->sub);
    bool wantsSub = i >= 0 && !(*p.items)[i].sub.empty() && !(*p.items)[i].disabled;
    if ((wantsSub && i != s->subOf) || (s->subOf >= 0 && i != s->subOf)) SetTimer(p.hwnd, kSubTimer, kSubDelayMs, nullptr);
    else KillTimer(p.hwnd, kSubTimer);
}

LRESULT CALLBACK Proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
    Pane* p = reinterpret_cast<Pane*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (!s || !p || p->hwnd != hwnd) return DefWindowProcW(hwnd, msg, wp, lp); // hwnd isn't set until CreateWindowEx returns
    bool isSub = p == &s->sub;
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint(*p);
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEACTIVATE:
        return isSub ? MA_NOACTIVATE : MA_ACTIVATE; // the keyboard stays with the main card
    case WM_SETCURSOR:
        SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        return TRUE;
    case WM_MOUSEMOVE:
        Hover(*p, lp);
        return 0;
    case WM_MOUSELEAVE:
        p->tracking = false;
        if (isSub) p->hover = -1;
        else p->hover = s->subOf >= 0 ? s->subOf : -1, KillTimer(hwnd, kSubTimer); // left toward the submenu or away: the open one stays
        Redraw(*p);
        return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
        s->armed = true;
        if (!isSub) { // a press on a row with a submenu opens it at once
            int i = HitAt(*p, GET_X_LPARAM(lp) / s->scale, GET_Y_LPARAM(lp) / s->scale);
            if (i >= 0 && !(*p->items)[i].sub.empty()) KillTimer(hwnd, kSubTimer), OpenSub(i, false);
        }
        return 0;
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: // either button picks, as TPM_RIGHTBUTTON menus
        if (s->armed) Choose(*p, HitAt(*p, GET_X_LPARAM(lp) / s->scale, GET_Y_LPARAM(lp) / s->scale));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        Key(wp);
        return 0;
    case WM_TIMER:
        if (wp == kAnimTimer) {
            if (GetTickCount64() - p->born >= (ULONGLONG)kOpenMs) KillTimer(hwnd, kAnimTimer);
            Redraw(*p);
        } else if (wp == kSubTimer) {
            KillTimer(hwnd, kSubTimer);
            int i = s->main.hover;
            if (i >= 0 && !(*s->main.items)[i].sub.empty()) OpenSub(i, false);
            else if (s->subOf >= 0 && !s->inSub) CloseSub();
        }
        return 0;
    case WM_ACTIVATE:
        if (!isSub && LOWORD(wp) == WA_INACTIVE && (HWND)lp != s->sub.hwnd) Finish(0); // clicked elsewhere
        return 0;
    case WM_ACTIVATEAPP:
        if (!wp) Finish(0);
        return 0;
    case WM_CANCELMODE:
    case WM_CLOSE:
        Finish(0);
        return 0;
    case WM_NCDESTROY:
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        break;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void CALLBACK OnForeground(HWINEVENTHOOK, DWORD, HWND h, LONG, LONG, DWORD, DWORD) { // another app came to the front: the menu goes, even if it never had the focus
    if (s && h != s->main.hwnd && h != s->sub.hwnd) Finish(0);
}

bool Ours(HWND h) { return h && (h == s->main.hwnd || h == s->sub.hwnd); }

bool OutsideClick(const MSG& m) {
    switch (m.message) {
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
    case WM_NCLBUTTONDOWN: case WM_NCRBUTTONDOWN: case WM_NCMBUTTONDOWN: case WM_NCXBUTTONDOWN:
        return !Ours(m.hwnd);
    }
    return false;
}

bool IsKey(UINT msg) { return msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_CHAR || msg == WM_SYSCHAR; }

ATOM Register() {
    static ATOM cls = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = Proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kClass;
        return RegisterClassExW(&wc);
    }();
    return cls;
}

RECT Where(const Place& at, int w, int h, const RECT& mon) {
    int x = at.h == Place::Left ? at.at.x : at.h == Place::Right ? at.at.x - w : at.at.x - w / 2;
    bool above = at.v == Place::Above || (at.v == Place::Auto && at.at.y > (mon.top + mon.bottom) / 2);
    int y = above ? at.at.y - h : at.at.y;
    if (above && y < s->work.top && at.at.y + h <= s->work.bottom) y = at.at.y; // no room above: below instead, as native menus flip
    else if (!above && y + h > s->work.bottom && at.at.y - h >= s->work.top) y = at.at.y - h;
    return Clamp({x, y, x + w, y + h});
}

}

UINT Pick(HWND owner, const std::vector<Item>& items, const Place& place, const Theme& t, float scale, const Head* head) {
    if (s || items.empty() || !Register()) return 0; // one at a time
    State state;
    s = &state;
    s->owner = owner;
    s->t = t;
    s->scale = scale > 0 ? scale : 1;
    s->glass = t.glass;
    s->light = !t.glass && theme::Luma(t.bg) > 0.4f;
    GetCursorPos(&s->openedAt);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(place.at, MONITOR_DEFAULTTONEAREST), &mi);
    s->work = mi.rcWork;
    s->main.items = &items;
    s->main.head = head;
    Lay(s->main);
    int w = (int)std::ceil(s->main.w * s->scale), h = (int)std::ceil(s->main.h * s->scale);
    RECT r = Where(place, w, h, mi.rcMonitor);
    s->main.slide = r.top < place.at.y ? kSlide : -kSlide; // rises when it opens upward, drops when it opens downward
    if (!Open(s->main, r, false)) {
        s = nullptr;
        return 0;
    }
    SetForegroundWindow(s->main.hwnd); // so the keys come here and a click elsewhere deactivates it
    HWINEVENTHOOK fg = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, OnForeground, 0, 0, WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
    MSG m;
    while (!s->done) {
        BOOL got = GetMessageW(&m, nullptr, 0, 0);
        if (got <= 0) { // quitting: the menu goes, and the main loop still hears it
            if (got == 0) PostQuitMessage((int)m.wParam);
            break;
        }
        if (OutsideClick(m)) { // a press anywhere else closes the menu and is spent on it, as native menus do
            Finish(0);
            continue;
        }
        if (IsKey(m.message) && !Ours(m.hwnd)) m.hwnd = s->main.hwnd; // keys typed while it's up are for it
        TranslateMessage(&m);
        DispatchMessageW(&m);
        if (!IsWindow(owner)) break;
    }
    if (fg) UnhookWinEvent(fg);
    UINT pick = s->done ? s->pick : 0;
    s->done = true; // what the teardown says no longer counts
    CloseSub();
    if (s->main.hwnd) DestroyWindow(s->main.hwnd);
    s->main = Pane{};
    s = nullptr;
    return pick;
}

}
