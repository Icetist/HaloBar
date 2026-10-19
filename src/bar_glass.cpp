// Glass look, as island's: the island sits on a live blur of whatever is behind it, cut to its exact shape.
// Only built while glass is the look; dark and light keep the plain composition path and never load any of this.
// windows headers first: they declare their own Color, which must not bind to ours from gfx.h
#include <windows.h>
#include <dwmapi.h>
#include <DispatcherQueue.h>
#include <windows.graphics.interop.h>
#include <windows.ui.composition.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Numerics.h>
#include <winrt/Windows.Graphics.h>
#include <winrt/Windows.Graphics.DirectX.h>
#include <winrt/Windows.System.h>
#include <winrt/Windows.UI.Composition.Desktop.h>
#include <winrt/Windows.UI.Composition.h>

#include "bar.h"

namespace WUC = winrt::Windows::UI::Composition;
namespace abi = ABI::Windows::UI::Composition;

namespace {

constexpr DWORD kUseHostBackdrop = 17; // DWMWA_USE_HOSTBACKDROPBRUSH, older sdks don't name it

bool EnsureQueue() { // composition needs a dispatcher queue on the ui thread, made once on first use
    static const bool ok = [] {
        if (winrt::Windows::System::DispatcherQueue::GetForCurrentThread()) return true;
        using MakeQueue = HRESULT(WINAPI*)(DispatcherQueueOptions, ABI::Windows::System::IDispatcherQueueController**);
        HMODULE lib = LoadLibraryExW(L"CoreMessaging.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32); // loaded only once glass is used
        auto make = lib ? reinterpret_cast<MakeQueue>(GetProcAddress(lib, "CreateDispatcherQueueController")) : nullptr;
        if (!make) return false;
        DispatcherQueueOptions o = {sizeof(o), DQTYPE_THREAD_CURRENT, DQTAT_COM_NONE}; // com is already set up on this thread
        ABI::Windows::System::IDispatcherQueueController* q = nullptr;
        return SUCCEEDED(make(o, &q)); // kept on purpose: it lives as long as the ui thread
    }();
    return ok;
}

struct ShapeSource // hands our d2d outline to composition as a clip path
    : winrt::implements<ShapeSource, winrt::Windows::Graphics::IGeometrySource2D, ABI::Windows::Graphics::IGeometrySource2DInterop> {
    ComPtr<ID2D1Geometry> geo;
    explicit ShapeSource(ID2D1Geometry* g) : geo(g) {}
    HRESULT __stdcall GetGeometry(ID2D1Geometry** value) override { return geo.CopyTo(value); }
    HRESULT __stdcall TryGetGeometryUsingFactory(ID2D1Factory*, ID2D1Geometry** value) override {
        *value = nullptr;
        return E_NOTIMPL;
    }
};

}

struct Bar::Glass {
    WUC::Compositor comp{nullptr};
    WUC::Desktop::DesktopWindowTarget target{nullptr};
    WUC::SpriteVisual frost{nullptr}; // the blur layer, its opacity is the blur setting
    WUC::CompositionPathGeometry outline{nullptr}; // clips the blur to the island
    WUC::CompositionGraphicsDevice gfx{nullptr};
    WUC::CompositionVirtualDrawingSurface surface{nullptr}; // our frames, kept like the plain path's virtual surface
    winrt::com_ptr<abi::ICompositionDrawingSurfaceInterop> draw;
    float blurShown = -1;
};

bool Bar::OpenGlass() {
    try {
        if (!EnsureQueue()) return false;
        auto g = std::make_unique<Glass>();
        BOOL hostBackdrop = TRUE; // windows 11: without this the backdrop brush stays transparent, no blur
        DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &hostBackdrop, sizeof(hostBackdrop));
        g->comp = WUC::Compositor();
        auto desk = g->comp.as<abi::Desktop::ICompositorDesktopInterop>();
        winrt::check_hresult(desk->CreateDesktopWindowTarget(hwnd, TRUE, reinterpret_cast<abi::Desktop::IDesktopWindowTarget**>(winrt::put_abi(g->target))));
        ComPtr<ID2D1Device> dev;
        ctx->GetDevice(&dev);
        winrt::check_hresult(g->comp.as<abi::ICompositorInterop>()->CreateGraphicsDevice(
            dev.Get(), reinterpret_cast<abi::ICompositionGraphicsDevice**>(winrt::put_abi(g->gfx))));
        g->surface = g->gfx.CreateVirtualDrawingSurface({surfPxW, surfPxH}, winrt::Windows::Graphics::DirectX::DirectXPixelFormat::B8G8R8A8UIntNormalized,
                                                        winrt::Windows::Graphics::DirectX::DirectXAlphaMode::Premultiplied);
        g->draw = g->surface.as<abi::ICompositionDrawingSurfaceInterop>();

        winrt::Windows::Foundation::Numerics::float2 size{(float)surfPxW, (float)surfPxH};
        auto frost = g->comp.CreateSpriteVisual(); // what's behind the window, blurred by windows itself, clipped to the island
        frost.Size(size);
        frost.Brush(g->comp.CreateHostBackdropBrush());
        g->outline = g->comp.CreatePathGeometry();
        frost.Clip(g->comp.CreateGeometricClip(g->outline));
        frost.IsVisible(false); // until the first frame hands us a shape
        auto brush = g->comp.CreateSurfaceBrush(g->surface); // 1:1 pixels, top-left
        brush.Stretch(WUC::CompositionStretch::None);
        brush.HorizontalAlignmentRatio(0);
        brush.VerticalAlignmentRatio(0);
        auto content = g->comp.CreateSpriteVisual(); // everything we draw, on top of the blur
        content.Size(size);
        content.Brush(brush);
        auto root = g->comp.CreateContainerVisual();
        root.Size(size);
        root.Children().InsertAtBottom(frost);
        root.Children().InsertAtTop(content);
        g->target.Root(root);
        g->frost = frost;
        glass = g.release();
        return true;
    } catch (...) {
        BOOL off = FALSE;
        DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &off, sizeof(off));
        return false; // no composition: the caller falls back to the plain path
    }
}

void Bar::FreeGlass() {
    if (!glass) return;
    BOOL hostBackdrop = FALSE; // dark and light don't need dwm sampling what's behind us
    DwmSetWindowAttribute(hwnd, kUseHostBackdrop, &hostBackdrop, sizeof(hostBackdrop));
    try { // closed outright: only dropping them leaves windows to clean up later, and meanwhile the empty glass layer covers what's drawn next
        glass->draw = nullptr;
        glass->target.Root(nullptr);
        glass->target.Close();
        glass->surface.Close();
        glass->gfx.Close();
        glass->comp.Close();
    } catch (...) {
    }
    delete glass;
    glass = nullptr;
}

void Bar::GlassMask(ID2D1Geometry* shape) {
    if (!glass) return;
    ComPtr<ID2D1TransformedGeometry> placed; // in pixels, like the visual
    bool ok = shape && SUCCEEDED(gfx::D2D()->CreateTransformedGeometry(shape, D2D1::Matrix3x2F::Scale(scale, scale), &placed));
    D2D1_RECT_F box{};
    ok = ok && SUCCEEDED(placed->GetBounds(nullptr, &box)) && box.right - box.left >= 4 && box.bottom - box.top >= 4; // a sliver makes composition fail fast
    try {
        if (ok) glass->outline.Path(WUC::CompositionPath(winrt::make<ShapeSource>(placed.Get())));
        glass->frost.IsVisible(ok);
    } catch (...) {
        glass->frost.IsVisible(false); // no clip, no blur: never frost the whole window
    }
}

HRESULT Bar::BeginSurface(const RECT& px, IDXGISurface** out, POINT* off) {
    if (glass) return glass->draw ? glass->draw->BeginDraw(&px, __uuidof(IDXGISurface), (void**)out, off) : E_FAIL;
    return surface ? surface->BeginDraw(&px, IID_PPV_ARGS(out), off) : E_FAIL;
}

void Bar::EndSurface() {
    if (glass) {
        if (glass->draw) glass->draw->EndDraw();
    } else if (surface) {
        surface->EndDraw();
    }
}

void Bar::TrimSurface(const RECT& keep) {
    if (glass) {
        try {
            winrt::Windows::Graphics::RectInt32 r{keep.left, keep.top, keep.right - keep.left, keep.bottom - keep.top};
            if (IsRectEmpty(&keep)) glass->surface.Trim({}); // nothing kept: every tile goes
            else glass->surface.Trim({&r, 1});
        } catch (...) {
        }
    } else if (surface) {
        surface->Trim(IsRectEmpty(&keep) ? nullptr : &keep, IsRectEmpty(&keep) ? 0 : 1);
    }
}

void Bar::Present() {
    if (!glass) {
        if (dcomp) dcomp->Commit();
        return;
    }
    float amount = Blur(); // composition commits its own batch on this thread's queue
    if (amount != glass->blurShown) {
        try {
            glass->frost.Opacity(amount); // 0 = clear glass, 1 = full backdrop blur
            glass->blurShown = amount;
        } catch (...) {
        }
    }
}

namespace {

float Brightness(const RECT& r) { // average luminance 0..1 of what's on screen there, from one tiny downscaled copy, as island's
    constexpr int kW = 24, kH = 12; // a thumbnail is plenty for "is this a white page"
    if (r.right <= r.left || r.bottom <= r.top) return 0;
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    BITMAPINFO bi = {};
    bi.bmiHeader = {sizeof(BITMAPINFOHEADER), kW, -kH, 1, 32, BI_RGB};
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    float lum = 0;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE); // averages instead of picking single pixels
        StretchBlt(mem, 0, 0, kW, kH, screen, r.left, r.top, r.right - r.left, r.bottom - r.top, SRCCOPY);
        GdiFlush();
        const auto* px = static_cast<const BYTE*>(bits);
        double sum = 0;
        for (int i = 0; i < kW * kH; ++i) sum += 0.0722 * px[i * 4] + 0.7152 * px[i * 4 + 1] + 0.2126 * px[i * 4 + 2]; // bgr
        lum = (float)(sum / (kW * kH) / 255.0);
        SelectObject(mem, old);
        DeleteObject(dib);
    }
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    return lum;
}

}

void Bar::SampleBehind() { // read once as a panel opens, before the island covers that spot; never per frame
    if (LookNow() != 2) {
        glassShade = 0;
        return;
    }
    LONG half = (LONG)(kSetW * scale / 2), cx = (monitor.left + monitor.right) / 2;
    RECT r = {cx - half, monitor.bottom - winPxH, cx + half, monitor.bottom - barPx - 2}; // where panels grow, above the row
    glassShade = anim::Clamp01((Brightness(r) - 0.42f) / 0.38f); // dark and colourful stay clear, bright pages get a deeper tint
}
