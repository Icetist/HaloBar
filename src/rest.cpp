// Windows' own taskbar is back and halobar steps aside: a tray icon, and on a click astraeus' little dashboard to bring the dock back,
// check for updates, restart or exit. No dock, no surface, no pollers: just a hidden window waiting on its messages.
#include "rest.h"

#include <dwmapi.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <memory>
#include <string>

#include "gfx.h"
#include "glyphs.h"
#include "prefs.h"
#include "sys.h"
#include "theme.h"
#include "version.h"

namespace rest {
namespace {

constexpr UINT WM_APP_TRAY = WM_APP + 1, WM_APP_UPD = WM_APP + 2; // the icon's clicks, the updater's answer
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kSpinTimer = 1, kUpdateTimer = 2;
constexpr UINT kUpdateFirstMs = 60 * 1000, kUpdateEveryMs = 6 * 60 * 60 * 1000; // as the dock: a minute in, then a few times a day
enum Cmd : UINT { kSepCmd = 0, kBack = 100, kUpdate, kRestart, kExit };
enum class Upd { Idle, Checking, Downloading, Current, Failed, Installed };

constexpr float kW = 252, kPad = 6, kHead = 56, kRow = 38, kSep = 9; // layout in dips, as island's
const Color kDanger = Color::Rgb(0xFF8A80); // exit's hover tint

struct Item {
    UINT cmd;
    int glyph;
    const wchar_t* label;
};
const Item kItems[] = {{kBack, glyph::Sparkle, L"Bring back HaloBar"},
                       {kUpdate, 0, L"Check for updates"},
                       {kSepCmd, 0, nullptr},
                       {kRestart, glyph::Restart, L"Restart"},
                       {kExit, glyph::Power, L"Exit"}};
constexpr int kCount = sizeof(kItems) / sizeof(kItems[0]);

struct Menu {
    HWND hwnd = nullptr, owner = nullptr;
    Theme t;
    int hover = -1;
    bool tracking = false;
    float scale = 1;
    ComPtr<ID2D1HwndRenderTarget> rt;
    ComPtr<ID2D1Bitmap> logo;
    std::unique_ptr<Canvas> canvas;
};
Menu* g = nullptr; // the open dashboard, null while closed
HWND host = nullptr;
UINT tbMsg = 0; // explorer restarted: the icon has to be added again
Upd upd = Upd::Idle; // outlives the menu: reopened mid-check, it still shows the check
std::wstring updTag;
sys::Updater* updater = nullptr;

Theme Look() { // the dock's materials, read fresh at each open: dark, light or custom; glass is dark here, a menu has no blur under it
    prefs::Settings cfg = prefs::Load();
    prefs::Look island = prefs::IslandLook();
    Seed s;
    bool follow = cfg.accent == prefs::Settings::kFollow;
    if (follow ? island.accentFixed : cfg.accent == 1) s = theme::FromCol(Color::Rgb((uint32_t)(follow ? island.accentHex : cfg.accentHex)));
    int look = cfg.look == prefs::Settings::kFollow ? island.look : cfg.look;
    s.look = look == 2 ? 0 : look;
    s.bg = cfg.look == 3 ? cfg.customBg : island.customBg;
    return Theme::From(s);
}

bool UpdBusy() { return upd == Upd::Checking || upd == Upd::Downloading || upd == Upd::Installed; }

std::wstring UpdLabel() {
    switch (upd) {
    case Upd::Checking: return L"Checking…";
    case Upd::Downloading: return L"Downloading " + updTag + L"…";
    case Upd::Installed: return L"Updating to " + updTag + L"…";
    case Upd::Current: return L"Up to date";
    case Upd::Failed: return L"Couldn't reach GitHub";
    default: return L"Check for updates";
    }
}

void UpdRow(Canvas& c, const Theme& t, const D2D1_RECT_F& r, float cy) { // the version, a spinner while checking, a tick once current
    if (UpdBusy()) {
        float a = (float)(GetTickCount64() % 900) / 900.0f * 6.2832f;
        c.Ring({r.right - 24, cy}, 7, t.track, 2);
        c.Arc({r.right - 24, cy}, 7, a, 1.75f, t.accentHi, 2);
    } else if (upd == Upd::Current) {
        c.Icon(glyph::Check, {r.right - 20, cy}, 16, t.accentHi, true);
        c.Text(L"v" APP_VER_W, {r.right - 96, r.top, r.right - 32, r.bottom}, Weight::Medium, 11.5f, t.accentHi, Align::Right);
    } else {
        c.Text(L"v" APP_VER_W, {r.right - 80, r.top, r.right - 12, r.bottom}, Weight::Regular, 11, t.textLow, Align::Right);
    }
}

void UpdIcon(Canvas& c, D2D1_POINT_2F o, Color ink) { // an open circle round a down arrow; the icon font has no update glyph
    c.Arc(o, 7.5f, -1.05f, 5.24f, ink, 1.8f);
    c.Icon(glyph::Down, {o.x, o.y + 0.5f}, 13, ink, true);
}

float Top(int i) {
    float y = kHead;
    for (int k = 0; k < i; ++k) y += kItems[k].cmd == kSepCmd ? kSep : kRow;
    return y;
}

float Height() { return Top(kCount) + kPad; }

int HitAt(float y) {
    for (int i = 0; i < kCount; ++i) {
        float top = Top(i), h = kItems[i].cmd == kSepCmd ? kSep : kRow;
        if (y >= top && y < top + h) return kItems[i].cmd == kSepCmd ? -1 : i;
    }
    return -1;
}

void Step(int dir) { // keyboard focus, skipping the divider
    int i = g->hover;
    for (int n = 0; n < kCount; ++n) {
        i = (i + dir + kCount) % kCount;
        if (kItems[i].cmd != kSepCmd) break;
    }
    g->hover = i;
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void Paint() {
    if (!g->rt) { // made on the first paint, dropped with the window
        RECT rc;
        GetClientRect(g->hwnd, &rc);
        D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
            D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96 * g->scale, 96 * g->scale);
        if (FAILED(gfx::D2D()->CreateHwndRenderTarget(props, D2D1::HwndRenderTargetProperties(g->hwnd, D2D1::SizeU(rc.right, rc.bottom)), &g->rt))) return;
        g->rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        g->canvas = std::make_unique<Canvas>(g->rt.Get());
        int px = (int)std::lround(28 * g->scale); // the ico's own art at this size
        HICON ico = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
        ComPtr<IWICBitmap> wb;
        ComPtr<IWICFormatConverter> conv;
        if (ico && gfx::Wic() && SUCCEEDED(gfx::Wic()->CreateBitmapFromHICON(ico, &wb)) && SUCCEEDED(gfx::Wic()->CreateFormatConverter(&conv)) &&
            SUCCEEDED(conv->Initialize(wb.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeMedianCut)))
            g->rt->CreateBitmapFromWicBitmap(conv.Get(), nullptr, &g->logo);
        if (ico) DestroyIcon(ico);
    }
    const Theme& t = g->t;
    Canvas& c = *g->canvas;
    float h = Height();
    g->rt->BeginDraw();
    g->rt->Clear(t.card.D2D());
    c.FillV({0, 0, kW, kHead + 20}, 0, t.accent.Alpha(0.12f), t.accent.Alpha(0)); // the accent's wash behind the header
    D2D1_POINT_2F badge = {kPad + 22, kHead / 2};
    if (g->logo) {
        D2D1_RECT_F at = {badge.x - 14, badge.y - 14, badge.x + 14, badge.y + 14};
        g->rt->DrawBitmap(g->logo.Get(), at); // halobar's own colors, as on the taskbar
    } else {
        c.Dot(badge, 16, t.accent.Alpha(0.20f));
        c.Icon(glyph::Sparkle, badge, 18, t.accentHi, true);
    }
    c.Text(L"HaloBar", Rect(kPad + 48, 13, 160, 18), Weight::Medium, 14, t.text);
    c.Text(L"Resting · Windows has the taskbar", Rect(kPad + 48, 30, 180, 14), Weight::Light, 11, t.textLow);

    for (int i = 0; i < kCount; ++i) {
        const Item& it = kItems[i];
        float y = Top(i);
        if (it.cmd == kSepCmd) {
            c.Line({kPad + 12, y + kSep / 2}, {kW - kPad - 12, y + kSep / 2}, t.hairTop, 1, false);
            continue;
        }
        D2D1_RECT_F r = {kPad, y + 1, kW - kPad, y + kRow - 1};
        float cy = (r.top + r.bottom) / 2;
        bool hot = i == g->hover, danger = it.cmd == kExit, back = it.cmd == kBack;
        if (back) c.Fill(r, 10, t.accent.Alpha(hot ? 0.28f : 0.16f)); // the one the dashboard is for
        else if (hot) c.Fill(r, 10, danger ? kDanger.Alpha(0.14f) : t.cardHi.Mix(t.track, 0.6f));
        Color ink = hot && danger ? kDanger : t.text;
        Color icon = back || hot ? (danger ? kDanger : t.accentHi) : t.textDim;
        if (it.cmd == kUpdate) UpdIcon(c, {r.left + 22, cy}, icon);
        else c.Icon(it.glyph, {r.left + 22, cy}, 19, icon, hot || back);
        bool upRow = it.cmd == kUpdate;
        if (upRow && upd == Upd::Failed) ink = hot ? kDanger : t.textDim;
        c.Text(upRow ? UpdLabel() : it.label, {r.left + 44, r.top, r.right - 56, r.bottom}, back ? Weight::Medium : Weight::Regular, 13, ink);
        if (upRow) UpdRow(c, t, r, cy);
    }
    c.StrokeV({0, 0, kW, h}, 8, t.hairTop, t.hairBot, 1); // the inner rim; dwm rounds the outside
    if (g->rt->EndDraw() == D2DERR_RECREATE_TARGET) {
        g->canvas.reset();
        g->logo.Reset();
        g->rt.Reset();
    }
}

void SetUpd(Upd state) {
    upd = state;
    if (!g) return;
    if (UpdBusy()) SetTimer(g->hwnd, kSpinTimer, 16, nullptr); // the spinner turns while waiting
    InvalidateRect(g->hwnd, nullptr, FALSE);
}

void Pick(int i) {
    if (i < 0) return;
    UINT cmd = kItems[i].cmd;
    if (cmd == kUpdate) { // checked in place: the row tells how it went
        if (!UpdBusy()) SetUpd(Upd::Checking), updater->Check(host, WM_APP_UPD, true);
        return;
    }
    PostMessageW(host, WM_COMMAND, cmd, 0);
    DestroyWindow(g->hwnd);
}

LRESULT CALLBACK MenuProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (!g) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(hwnd, &ps);
        Paint();
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_ERASEBKGND:
        return 1;
    case WM_MOUSEMOVE: {
        if (!g->tracking) {
            TRACKMOUSEEVENT tme = {sizeof(tme), TME_LEAVE, hwnd, 0};
            g->tracking = TrackMouseEvent(&tme) != FALSE;
        }
        int i = HitAt(GET_Y_LPARAM(lp) / g->scale);
        if (i != g->hover) g->hover = i, InvalidateRect(hwnd, nullptr, FALSE); // repaints only on a change
        return 0;
    }
    case WM_MOUSELEAVE:
        g->tracking = false;
        g->hover = -1;
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONUP:
        Pick(HitAt(GET_Y_LPARAM(lp) / g->scale)); // on release, as native menus
        return 0;
    case WM_KEYDOWN:
        if (wp == VK_ESCAPE) DestroyWindow(hwnd);
        else if (wp == VK_DOWN) Step(1);
        else if (wp == VK_UP) Step(-1);
        else if (wp == VK_RETURN || wp == VK_SPACE) Pick(g->hover);
        return 0;
    case WM_TIMER:
        if (!UpdBusy()) KillTimer(hwnd, kSpinTimer);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) PostMessageW(hwnd, WM_CLOSE, 0, 0); // a click elsewhere closes it
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_NCDESTROY:
        delete g; // the render target with it
        g = nullptr;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

COLORREF Ref(Color c) { return RGB((int)(c.r * 255), (int)(c.g * 255), (int)(c.b * 255)); }

void Show() { // at the cursor, above it unless there's no room
    if (g) {
        DestroyWindow(g->hwnd);
        return; // a second click on the icon closes it
    }
    static ATOM cls = [] {
        WNDCLASSEXW wc = {sizeof(wc)};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = MenuProc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = L"HaloBarRestMenu";
        return RegisterClassExW(&wc);
    }();
    if (!cls) return;
    POINT p;
    GetCursorPos(&p);
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(p, MONITOR_DEFAULTTONEAREST), &mi);
    if (upd == Upd::Current || upd == Upd::Failed) upd = Upd::Idle; // the last answer is old news
    g = new Menu();
    g->owner = host;
    g->t = Look();
    g->hwnd = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"HaloBarRestMenu", L"HaloBar", WS_POPUP, p.x, p.y, 1, 1, host, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!g->hwnd) {
        delete g;
        g = nullptr;
        return;
    }
    g->scale = GetDpiForWindow(g->hwnd) / 96.0f;
    int w = (int)(kW * g->scale + 0.5f), h = (int)(Height() * g->scale + 0.5f);
    const RECT& wa = mi.rcWork;
    int x = (std::max)((std::min)((int)p.x, (int)wa.right - w), (int)wa.left);
    int y = p.y - h >= wa.top ? (int)p.y - h : (int)p.y;
    DWM_WINDOW_CORNER_PREFERENCE corner = DWMWCP_ROUND;
    DwmSetWindowAttribute(g->hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
    COLORREF border = Ref(g->t.track);
    DwmSetWindowAttribute(g->hwnd, DWMWA_BORDER_COLOR, &border, sizeof(border));
    SetWindowPos(g->hwnd, HWND_TOPMOST, x, y, w, h, SWP_SHOWWINDOW);
    SetForegroundWindow(g->hwnd); // so a click elsewhere deactivates and closes it
    if (UpdBusy()) SetTimer(g->hwnd, kSpinTimer, 16, nullptr);
}

void AddIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = host;
    nid.uID = kTrayId;
    nid.uFlags = NIF_MESSAGE | NIF_TIP | NIF_ICON;
    nid.uCallbackMessage = WM_APP_TRAY;
    int px = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForSystem()); // the tray's own size, the ico's art for it
    nid.hIcon = (HICON)LoadImageW(GetModuleHandleW(nullptr), MAKEINTRESOURCEW(101), IMAGE_ICON, px, px, LR_DEFAULTCOLOR);
    wcscpy_s(nid.szTip, L"HaloBar");
    Shell_NotifyIconW(NIM_ADD, &nid);
    if (nid.hIcon) DestroyIcon(nid.hIcon); // the shell keeps its own copy
}

void RemoveIcon() {
    NOTIFYICONDATAW nid = {sizeof(nid)};
    nid.hWnd = host;
    nid.uID = kTrayId;
    Shell_NotifyIconW(NIM_DELETE, &nid);
}

LRESULT CALLBACK HostProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (tbMsg && msg == tbMsg) { // explorer started again
        AddIcon();
        return 0;
    }
    switch (msg) {
    case WM_APP_TRAY:
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_CONTEXTMENU) Show();
        return 0;
    case WM_APP_UPD:
        switch (wp) {
        case sys::Updater::kInstalled:
            updTag = updater->Applied(), SetUpd(Upd::Installed);
            if (!g) PostQuitMessage(2); // the new one starts in its place, still resting
            break;
        case sys::Updater::kDownloading: updTag = updater->Applied(), SetUpd(Upd::Downloading); break;
        case sys::Updater::kCurrent: SetUpd(Upd::Current); break;
        default: SetUpd(Upd::Failed); break;
        }
        return 0;
    case WM_TIMER:
        if (wp == kUpdateTimer) {
            updater->Check(hwnd, WM_APP_UPD);
            SetCoalescableTimer(hwnd, kUpdateTimer, kUpdateEveryMs, nullptr, 60 * 1000);
        }
        return 0;
    case WM_COMMAND:
        switch (LOWORD(wp)) {
        case kBack: prefs::SaveNum(L"WindowsBar", 0), PostQuitMessage(2); break; // the dock comes back in a fresh start
        case kRestart: PostQuitMessage(2); break;
        case kExit: PostQuitMessage(0); break;
        }
        return 0;
    case WM_ENDSESSION:
    case WM_CLOSE:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

}

int Run(HINSTANCE instance) {
    WNDCLASSEXW wc = {sizeof(wc)};
    wc.lpfnWndProc = HostProc;
    wc.hInstance = instance;
    wc.lpszClassName = L"HaloBarRest";
    if (!RegisterClassExW(&wc)) return 1;
    host = CreateWindowExW(WS_EX_TOOLWINDOW, L"HaloBarRest", L"HaloBar", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, nullptr); // top level, never shown: it hears explorer's restart broadcast
    if (!host) return 1;
    tbMsg = RegisterWindowMessageW(L"TaskbarCreated");
    ChangeWindowMessageFilterEx(host, tbMsg, MSGFLT_ALLOW, nullptr);
    sys::Updater up;
    updater = &up;
    AddIcon();
    SetTimer(host, kUpdateTimer, kUpdateFirstMs, nullptr);
    sys::SetEco(true); // nothing here is ever in a hurry
    gfx::GiveBack();
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    int code = msg.message == WM_QUIT ? (int)msg.wParam : 0;
    if (g) DestroyWindow(g->hwnd);
    RemoveIcon();
    updater = nullptr;
    DestroyWindow(host);
    host = nullptr;
    return code;
}

}
