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
    ComPtr<IWICBitmapSource> pixels = gfx::FromShellBitmap(hb);
    DeleteObject(hb); // copied, the hbitmap is ours to free
    return pixels;
}

ComPtr<IWICBitmapSource> FromIdList(const std::vector<BYTE>& pidl, UINT px) { // a pin: the same icon the windows taskbar shows
    ComPtr<IShellItemImageFactory> f;
    HBITMAP hb = nullptr;
    if (pidl.size() < 2 || FAILED(SHCreateItemFromIDList(reinterpret_cast<PCIDLIST_ABSOLUTE>(pidl.data()), IID_PPV_ARGS(&f))) ||
        FAILED(f->GetImage({(LONG)px, (LONG)px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &hb)) || !hb)
        return nullptr;
    ComPtr<IWICBitmapSource> pixels = gfx::FromShellBitmap(hb);
    DeleteObject(hb);
    return pixels;
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

static ComPtr<IWICBitmapSource> Read(const Icons::Result& r, const std::vector<BYTE>& pidl, const std::wstring& aumid, const std::wstring& exe, HWND win) {
    const UINT ask = r.px * 2; // a bigger designed size, scaled down well, beats the shell stretching a small one
    ComPtr<IWICBitmapSource> px;
    if (!pidl.empty()) px = FromIdList(pidl, ask);
    if (!px && !aumid.empty()) px = FromShell(L"shell:AppsFolder\\" + aumid, ask); // store apps: their tile icon, not the exe's
    if (!px) px = FromShell(exe, ask);
    if (!px && win) px = FromWindow(win);
    return Fit(px.Get(), r.px);
}

void Icons::Start(HWND to, UINT m) {
    notify = to, msg = m;
    reader = std::thread([this] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // the shell's image factories want an sta
        for (;;) {
            Job j;
            {
                std::unique_lock<std::mutex> g(lock);
                wake.wait(g, [this] { return stopping || !jobs.empty(); });
                if (stopping) break;
                j = std::move(jobs.front());
                jobs.pop_front();
            }
            auto* r = new Result{j.key, j.px, nullptr};
            r->pixels = Read(*r, j.pidl, j.aumid, j.exe, j.win);
            if (!PostMessageW(notify, msg, 0, (LPARAM)r)) delete r; // the bar went: nobody to tell
        }
        CoUninitialize();
    });
}

void Icons::Stop() {
    if (!reader.joinable()) return;
    {
        std::lock_guard<std::mutex> g(lock);
        stopping = true;
        jobs.clear();
    }
    wake.notify_one();
    reader.join();
}

bool Icons::Deliver(LPARAM lp) {
    std::unique_ptr<Result> r(reinterpret_cast<Result*>(lp));
    auto it = map.find(r->key);
    if (it == map.end() || it->second.size != r->px) return false; // closed meanwhile, or wanted at another size now
    it->second.pending = false;
    it->second.px = r->pixels;
    it->second.bmp.Reset();
    return true;
}

bool Icons::Pending(const std::wstring& key) const {
    auto it = map.find(key);
    return it != map.end() && it->second.pending;
}

ID2D1Bitmap* Icons::Get(ID2D1DeviceContext* ctx, const TaskApp& app, UINT px) {
    Ent& e = map[app.key]; // one entry per app, shared by all its windows
    if (px > e.size) { // first time, or asked bigger than kept: the dock and the launcher share one copy, the largest either needs
        e = {};
        e.size = px;
    }
    px = e.size;
    if (!e.px && !e.tried) {
        e.tried = true; // a miss isn't retried every frame
        if (reader.joinable()) { // read on the reader thread: the frame draws a placeholder meanwhile
            e.pending = true;
            {
                std::lock_guard<std::mutex> g(lock);
                jobs.push_back({app.key, app.aumid, app.exe, app.pidl, app.wins.empty() ? nullptr : app.wins.front().hwnd, px});
            }
            wake.notify_one();
        } else {
            Result r{app.key, px, nullptr};
            e.px = Read(r, app.pidl, app.aumid, app.exe, app.wins.empty() ? nullptr : app.wins.front().hwnd);
        }
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
        if (!e.px && !e.pending) e.tried = false; // a window's icon often arrives after the window itself
}
