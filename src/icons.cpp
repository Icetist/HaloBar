#include "icons.h"

#include <shobjidl.h>

#include <unordered_set>

namespace {

ComPtr<IWICBitmapSource> Pbgra(IWICBitmapSource* src) { // d2d takes premultiplied bgra only
    ComPtr<IWICBitmapSource> out;
    if (src) WICConvertBitmapSource(GUID_WICPixelFormat32bppPBGRA, src, &out); // a no-op wrapper when it already is
    return out;
}

ComPtr<IWICBitmapSource> Fit(IWICBitmapSource* src, UINT px) { // exactly px square, scaled down once with a good filter, kept in memory
    if (!src) return nullptr;
    UINT w = 0, h = 0;
    src->GetSize(&w, &h);
    ComPtr<IWICBitmapSource> from = src;
    if (w != px || h != px) {
        ComPtr<IWICBitmapScaler> sc;
        if (FAILED(gfx::Wic()->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src, px, px, WICBitmapInterpolationModeHighQualityCubic))) return src;
        from = sc;
    }
    ComPtr<IWICBitmap> baked; // the scaler is lazy: bake it so it runs once, not on every upload
    if (FAILED(gfx::Wic()->CreateBitmapFromSource(from.Get(), WICBitmapCacheOnLoad, &baked))) return src;
    return baked;
}

ComPtr<IWICBitmapSource> FromShell(const std::wstring& path, UINT px) {
    ComPtr<IShellItemImageFactory> f;
    HBITMAP hb = nullptr;
    if (path.empty() || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&f))) ||
        FAILED(f->GetImage({(LONG)px, (LONG)px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb) // icon only: never a file's thumbnail
        return nullptr;
    ComPtr<IWICBitmap> bmp;
    gfx::Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &bmp); // the shell hands back premultiplied pixels
    DeleteObject(hb); // wic copied the pixels, the hbitmap is ours to free
    return Pbgra(bmp.Get());
}

ComPtr<IWICBitmapSource> FromIdList(const std::vector<BYTE>& pidl, UINT px) { // a pin: the same icon the windows taskbar shows
    ComPtr<IShellItemImageFactory> f;
    HBITMAP hb = nullptr;
    if (pidl.size() < 2 || FAILED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.data()), IID_PPV_ARGS(&f))) ||
        FAILED(f->GetImage({(LONG)px, (LONG)px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb)
        return nullptr;
    ComPtr<IWICBitmap> bmp;
    gfx::Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapUsePremultipliedAlpha, &bmp);
    DeleteObject(hb);
    return Pbgra(bmp.Get());
}

ComPtr<IWICBitmapSource> FromWindow(HWND h) { // last resort: whatever icon the window itself shows
    DWORD_PTR r = 0;
    HICON ic = nullptr;
    if (SendMessageTimeoutW(h, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &r) && r) ic = reinterpret_cast<HICON>(r); // a hung app can't stall the bar
    if (!ic) ic = reinterpret_cast<HICON>(GetClassLongPtrW(h, GCLP_HICON)); // the class icon, when the window never set one
    if (!ic) return nullptr;
    ComPtr<IWICBitmap> bmp;
    if (FAILED(gfx::Wic()->CreateBitmapFromHICON(ic, &bmp))) return nullptr; // borrowed icon, not ours to destroy
    return Pbgra(bmp.Get());
}

}

ID2D1Bitmap* Icons::Get(ID2D1DeviceContext* ctx, const TaskApp& app, UINT px) {
    Ent& e = map[app.key]; // one entry per app, shared by all its windows
    if (e.size != px) { // first time, or the screen's scale changed
        e = {};
        e.size = px;
    }
    if (!e.px && !e.tried) {
        e.tried = true; // a miss isn't retried every frame
        const UINT ask = px * 2; // a bigger designed size, scaled down well, beats the shell stretching a small one
        if (!app.pidl.empty()) e.px = FromIdList(app.pidl, ask);
        if (!e.px && !app.aumid.empty()) e.px = FromShell(L"shell:AppsFolder\\" + app.aumid, ask); // store apps: their tile icon, not the exe's
        if (!e.px) e.px = FromShell(app.exe, ask);
        if (!e.px && !app.wins.empty()) e.px = FromWindow(app.wins.front().hwnd);
        e.px = Fit(e.px.Get(), px);
    }
    if (!e.px) return nullptr;
    if (!e.bmp && ctx) ctx->CreateBitmapFromWicBitmap(e.px.Get(), nullptr, &e.bmp); // the gpu copy, rebuilt cheaply from the kept pixels
    return e.bmp.Get();
}

void Icons::Drop() {
    for (auto& [key, e] : map) e.bmp.Reset(); // device lost or rebuilt: keep the pixels, drop what lived on it
}

void Icons::Keep(const std::vector<TaskApp>& apps) {
    std::unordered_set<std::wstring> keys;
    for (const auto& a : apps) keys.insert(a.key);
    for (auto it = map.begin(); it != map.end();) it = keys.count(it->first) ? std::next(it) : map.erase(it); // closed apps let go of their icons
    for (auto& [key, e] : map)
        if (!e.px) e.tried = false; // a window's icon often arrives after the window itself
}
