// The clipboard history's store: reading what was copied, keeping it on disk, and putting it back.
#include "clips.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <thread>
#include <unordered_set>

#include "prefs.h"
#include "sys.h"

namespace clip {

namespace {

constexpr uint32_t kMagic = 0x48434248, kVersion = 1; // "HBCH"
constexpr size_t kFilesMax = 512; // paths kept from one file copy
constexpr size_t kPngMax = 64u << 20; // a png bigger than this on the clipboard is not kept
constexpr UINT kThumbPx = 256; // the previews' longest side, in pixels
constexpr int kTitleLow = 0; // option floors and ceilings
constexpr wchar_t kNoApps[] = L"ClipNoApps", kNoSites[] = L"ClipNoSites";
const wchar_t* const kPasswordApps[] = {L"1password.exe", L"bitwarden.exe", L"lastpass.exe", L"keepass.exe", L"keepassxc.exe", L"dashlane.exe", L"nordpass.exe",
                                        L"keeperpasswordmanager.exe", L"keeper password manager.exe", L"enpass.exe", L"roboform.exe", L"proton pass.exe",
                                        L"protonpass.exe", L"pwsafe.exe", L"passwordsafe.exe", L"bitwarden desktop.exe", L"authy desktop.exe", L"credentialuibroker.exe"};

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

std::vector<std::wstring> Split(const std::wstring& s, wchar_t sep) {
    std::vector<std::wstring> out;
    for (size_t i = 0; i <= s.size();) {
        size_t e = s.find(sep, i);
        if (e == std::wstring::npos) e = s.size();
        if (e > i) out.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    return out;
}

std::wstring Join(const std::vector<std::wstring>& v, wchar_t sep) {
    std::wstring out;
    for (const auto& s : v) out += (out.empty() ? L"" : std::wstring(1, sep)) + s;
    return out;
}

uint64_t Fnv(const void* data, size_t n, uint64_t h = 1469598103934665603ull) {
    const auto* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

int64_t Now() { // unix seconds
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    return (int64_t)(u.QuadPart / 10000000ULL) - 11644473600LL;
}

std::wstring ExeOf(HWND w) { // the app behind a window: its exe's name, lowercased
    DWORD pid = 0;
    if (!w || !GetWindowThreadProcessId(w, &pid) || !pid) return {};
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return {};
    wchar_t path[MAX_PATH];
    DWORD n = MAX_PATH;
    std::wstring out;
    if (QueryFullProcessImageNameW(p, 0, path, &n)) {
        const wchar_t* slash = wcsrchr(path, L'\\');
        out = Lower(slash ? slash + 1 : path);
    }
    CloseHandle(p);
    return out;
}

bool WriteAll(const std::wstring& path, const void* data, size_t n) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = n == 0 || (WriteFile(f, data, (DWORD)n, &w, nullptr) && w == n);
    CloseHandle(f);
    return ok;
}

bool ReadAll(const std::wstring& path, std::string& out, size_t most) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && (size_t)size.QuadPart <= most;
    if (ok) {
        out.resize((size_t)size.QuadPart);
        DWORD got = 0;
        ok = out.empty() || (ReadFile(f, out.data(), (DWORD)out.size(), &got, nullptr) && got == out.size());
    }
    CloseHandle(f);
    return ok;
}

struct Out { // the index, written little endian as it sits in memory
    std::string b;
    template <class T> void Num(T v) { b.append(reinterpret_cast<const char*>(&v), sizeof(v)); }
    void Str(const std::wstring& s) {
        Num((uint32_t)s.size());
        b.append(reinterpret_cast<const char*>(s.data()), s.size() * sizeof(wchar_t));
    }
};

struct In {
    const char *p, *e;
    bool ok = true;
    template <class T> T Num() {
        T v{};
        if (!ok || (size_t)(e - p) < sizeof(T)) return ok = false, v;
        memcpy(&v, p, sizeof(T));
        p += sizeof(T);
        return v;
    }
    std::wstring Str(size_t most) {
        uint32_t n = Num<uint32_t>();
        if (!ok || n > most || (size_t)(e - p) < n * sizeof(wchar_t)) return ok = false, std::wstring();
        std::wstring s(reinterpret_cast<const wchar_t*>(p), n);
        p += n * sizeof(wchar_t);
        return s;
    }
};

bool Private() { // password managers mark what they copy: never kept
    static const UINT kExclude = RegisterClipboardFormatW(L"ExcludeClipboardContentFromMonitorProcessing");
    static const UINT kHistory = RegisterClipboardFormatW(L"CanIncludeInClipboardHistory");
    static const UINT kIgnore = RegisterClipboardFormatW(L"Clipboard Viewer Ignore");
    if (IsClipboardFormatAvailable(kExclude) || IsClipboardFormatAvailable(kIgnore)) return true;
    if (IsClipboardFormatAvailable(kHistory))
        if (HANDLE h = GetClipboardData(kHistory)) {
            const DWORD* v = static_cast<const DWORD*>(GlobalLock(h));
            bool no = v && *v == 0;
            GlobalUnlock(h);
            return no;
        }
    return false;
}

std::string Bytes(UINT fmt, size_t most) { // a format's raw bytes, while the clipboard is open; empty when missing or too big
    std::string out;
    if (!fmt || !IsClipboardFormatAvailable(fmt)) return out;
    HANDLE h = GetClipboardData(fmt);
    if (!h) return out;
    SIZE_T n = GlobalSize(h);
    if (n == 0 || n > most) return out;
    if (const void* p = GlobalLock(h)) out.assign(static_cast<const char*>(p), n), GlobalUnlock(h);
    return out;
}

std::wstring Widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::wstring SourceUrl(const std::string& html) { // chromium's own note of the page, else the html's header
    static const UINT kChromium = RegisterClipboardFormatW(L"Chromium internal source URL");
    std::string raw = Bytes(kChromium, 8192);
    std::wstring url;
    if (raw.size() >= 2 && raw[1] == 0) url.assign(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / 2); // utf-16
    else url = Widen(raw);
    if (url.empty() && !html.empty()) {
        size_t at = html.find("SourceURL:");
        if (at != std::string::npos && at < 1024) {
            size_t end = html.find_first_of("\r\n", at);
            url = Widen(html.substr(at + 10, end == std::string::npos ? std::string::npos : end - at - 10));
        }
    }
    url = url.substr(0, url.find(L'\0'));
    return url.size() > 2048 ? std::wstring() : url;
}

HGLOBAL Global(const void* data, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
    if (!g) return nullptr;
    if (void* p = GlobalLock(g)) memcpy(p, data, n), GlobalUnlock(g);
    return g;
}

void Set(UINT fmt, HGLOBAL g) {
    if (g && !SetClipboardData(fmt, g)) GlobalFree(g); // the system owns it once set
}

HGLOBAL Dib(const std::string& png, bool& ok) { // a png's pixels as a bottom-up dib every app reads
    ok = false;
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapDecoder> dec;
    ComPtr<IWICBitmapFrameDecode> fr;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    if (png.empty() || !gfx::Wic() || FAILED(gfx::Wic()->CreateStream(&st)) || FAILED(st->InitializeFromMemory((BYTE*)png.data(), (DWORD)png.size())) ||
        FAILED(gfx::Wic()->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) || FAILED(dec->GetFrame(0, &fr)) ||
        FAILED(gfx::Wic()->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(fr.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) || FAILED(conv->GetSize(&w, &h)))
        return nullptr;
    UINT stride = w * 4;
    size_t bytes = (size_t)stride * h;
    HGLOBAL dib = GlobalAlloc(GMEM_MOVEABLE, sizeof(BITMAPINFOHEADER) + bytes);
    if (!dib) return nullptr;
    auto* bi = static_cast<BITMAPINFOHEADER*>(GlobalLock(dib));
    *bi = {sizeof(BITMAPINFOHEADER), (LONG)w, (LONG)h, 1, 32, BI_RGB, (DWORD)bytes};
    BYTE* px = reinterpret_cast<BYTE*>(bi + 1);
    ok = true;
    for (UINT y = 0; y < h && ok; ++y) {
        WICRect row = {0, (INT)(h - 1 - y), (INT)w, 1}; // a dib runs bottom up
        ok = SUCCEEDED(conv->CopyPixels(&row, stride, stride, px + (size_t)y * stride));
    }
    GlobalUnlock(dib);
    if (!ok) GlobalFree(dib), dib = nullptr;
    return dib;
}

bool EncodePng(IWICBitmapSource* src, const std::wstring& path) {
    ComPtr<IWICStream> st;
    ComPtr<IWICBitmapEncoder> enc;
    ComPtr<IWICBitmapFrameEncode> fr;
    ComPtr<IWICFormatConverter> conv;
    UINT w = 0, h = 0;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat32bppBGRA;
    IWICImagingFactory* f = gfx::Wic();
    if (!f || FAILED(src->GetSize(&w, &h)) || FAILED(f->CreateFormatConverter(&conv)) ||
        FAILED(conv->Initialize(src, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) || FAILED(f->CreateStream(&st)) ||
        FAILED(st->InitializeFromFilename(path.c_str(), GENERIC_WRITE)) || FAILED(f->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) ||
        FAILED(enc->Initialize(st.Get(), WICBitmapEncoderNoCache)) || FAILED(enc->CreateNewFrame(&fr, nullptr)) || FAILED(fr->Initialize(nullptr)) || FAILED(fr->SetSize(w, h)) ||
        FAILED(fr->SetPixelFormat(&fmt)) || FAILED(fr->WriteSource(conv.Get(), nullptr)) || FAILED(fr->Commit()) || FAILED(enc->Commit()))
        return DeleteFileW(path.c_str()), false;
    return true;
}

ComPtr<IWICBitmapSource> Scaled(IWICBitmapSource* src, UINT most) { // fits most pixels on its longer side
    UINT w = 0, h = 0;
    ComPtr<IWICBitmapScaler> sc;
    if (!src || FAILED(src->GetSize(&w, &h)) || !w || !h) return nullptr;
    if (w <= most && h <= most) return src;
    float k = (float)most / (float)(std::max)(w, h);
    if (FAILED(gfx::Wic()->CreateBitmapScaler(&sc)) || FAILED(sc->Initialize(src, (std::max)(1u, (UINT)(w * k)), (std::max)(1u, (UINT)(h * k)), WICBitmapInterpolationModeFant)))
        return nullptr;
    return sc;
}

struct ImageJob { // a picture's files, written off the ui thread: a 4k screenshot takes a while to compress
    std::wstring full, thumb;
    std::string png; // the clipboard's own png, written as is
    ComPtr<IWICBitmap> bmp; // or its bitmap, compressed here
    HWND notify = nullptr;
    UINT msg = 0;
    uint64_t id = 0;
};

void WriteImage(ImageJob job) {
    std::thread([job = std::move(job)]() mutable {
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        ComPtr<IWICBitmapSource> src = job.bmp;
        if (!job.png.empty()) {
            WriteAll(job.full, job.png.data(), job.png.size());
            ComPtr<IWICStream> st;
            ComPtr<IWICBitmapDecoder> dec;
            ComPtr<IWICBitmapFrameDecode> fr;
            if (SUCCEEDED(gfx::Wic()->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)job.png.data(), (DWORD)job.png.size())) &&
                SUCCEEDED(gfx::Wic()->CreateDecoderFromStream(st.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)))
                src = fr;
            if (src) EncodePng(Scaled(src.Get(), kThumbPx).Get(), job.thumb);
        } else if (src) {
            EncodePng(src.Get(), job.full);
            EncodePng(Scaled(src.Get(), kThumbPx).Get(), job.thumb);
        }
        src.Reset(), job.bmp.Reset(); // the pixels go before the message: the ui may read the files at once
        PostMessageW(job.notify, job.msg, (WPARAM)job.id, 0);
        if (SUCCEEDED(co)) CoUninitialize();
    }).detach();
}

uint64_t PixelHash(IWICBitmap* bmp, UINT w, UINT h) { // a sample of rows: enough to tell two screenshots apart, cheap on a 4k one
    ComPtr<IWICBitmapLock> lock;
    WICRect all = {0, 0, (INT)w, (INT)h};
    uint64_t hash = Fnv(&w, sizeof(w), Fnv(&h, sizeof(h)));
    if (FAILED(bmp->Lock(&all, WICBitmapLockRead, &lock))) return hash;
    UINT stride = 0, size = 0;
    BYTE* p = nullptr;
    if (FAILED(lock->GetStride(&stride)) || FAILED(lock->GetDataPointer(&size, &p)) || !p) return hash;
    UINT step = (std::max)(1u, h / 64);
    for (UINT y = 0; y < h; y += step) hash = Fnv(p + (size_t)y * stride, (std::min)((size_t)stride, (size_t)size - (size_t)y * stride), hash);
    return hash;
}

bool PngSize(const std::string& png, uint32_t& w, uint32_t& h) { // from the header, no decode
    if (png.size() < 24 || memcmp(png.data() + 12, "IHDR", 4)) return false;
    auto be = [&](size_t at) { return (uint32_t)(uint8_t)png[at] << 24 | (uint32_t)(uint8_t)png[at + 1] << 16 | (uint32_t)(uint8_t)png[at + 2] << 8 | (uint8_t)png[at + 3]; };
    w = be(16), h = be(20);
    return w && h;
}

std::wstring Trimmed(const std::wstring& s) {
    size_t a = s.find_first_not_of(L" \t\r\n"), b = s.find_last_not_of(L" \t\r\n");
    return a == std::wstring::npos ? std::wstring() : s.substr(a, b - a + 1);
}

bool Listed(const std::vector<std::wstring>& list, const std::wstring& v) {
    return !v.empty() && std::find(list.begin(), list.end(), v) != list.end();
}

bool SiteListed(const std::vector<std::wstring>& list, const std::wstring& host) { // the site or any of its subdomains
    for (const auto& s : list)
        if (host == s || (host.size() > s.size() && host.compare(host.size() - s.size(), s.size(), s) == 0 && host[host.size() - s.size() - 1] == L'.')) return true;
    return false;
}

}

Store& History() {
    static Store s;
    return s;
}

std::wstring Store::File(uint64_t id, const wchar_t* ext) const {
    wchar_t name[40];
    swprintf_s(name, L"\\%016llx", (unsigned long long)id);
    return dir + name + ext;
}

uint64_t Store::NewId() {
    FILETIME ft;
    GetSystemTimeAsFileTime(&ft);
    uint64_t t = (uint64_t)ft.dwHighDateTime << 32 | ft.dwLowDateTime;
    lastId = (std::max)(lastId + 1, t); // unique even within one tick
    return lastId;
}

void Store::Load() {
    if (loaded) return;
    loaded = true;
    PWSTR base = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) dir = std::wstring(base) + L"\\HaloBar";
    CoTaskMemFree(base);
    if (!dir.empty()) CreateDirectoryW(dir.c_str(), nullptr), dir += L"\\clips", CreateDirectoryW(dir.c_str(), nullptr);
    opt.max = (std::clamp)(prefs::ReadNum(L"ClipMax", kMaxDef), kMaxLow, kMaxHigh);
    opt.paste = prefs::ReadNum(L"ClipPaste", 1) != 0;
    opt.plain = prefs::ReadNum(L"ClipPlain", 0) != 0;
    opt.clean = prefs::ReadNum(L"ClipClean", 1) != 0;
    opt.noPass = prefs::ReadNum(L"ClipNoPass", 1) != 0;
    opt.noAdult = prefs::ReadNum(L"ClipNoAdult", 1) != 0;
    opt.titles = prefs::ReadNum(L"ClipTitles", kTitleLow) != 0;
    opt.view = (std::clamp)(prefs::ReadNum(L"ClipView", 0), 0, 1);
    opt.size = (std::clamp)(prefs::ReadNum(L"ClipSize", 1), 0, 2);
    opt.key = (std::clamp)(prefs::ReadNum(L"ClipKey", 1), 0, kKeys - 1);
    opt.theme = (std::clamp)(prefs::ReadNum(L"ClipTheme", 0), 0, kThemes - 1);
    opt.paused = prefs::ReadNum(L"ClipPaused", 0) != 0;
    opt.noApps = Split(Lower(sys::ReadText(kNoApps)), L'|');
    opt.noSites = Split(Lower(sys::ReadText(kNoSites)), L'|');

    std::string raw;
    std::unordered_set<uint64_t> ids;
    if (!dir.empty() && ReadAll(dir + L"\\index.dat", raw, 64u << 20)) {
        In in{raw.data(), raw.data() + raw.size()};
        uint32_t magic = in.Num<uint32_t>(), format = in.Num<uint32_t>(), n = in.Num<uint32_t>();
        if (magic == kMagic && format == kVersion)
            for (uint32_t i = 0; i < n && i < (uint32_t)kMaxHigh * 2 && in.ok; ++i) {
                Item it;
                it.id = in.Num<uint64_t>();
                it.at = in.Num<int64_t>();
                uint8_t kind = in.Num<uint8_t>(), flags = in.Num<uint8_t>();
                it.kind = kind < kKinds ? (Kind)kind : kText;
                it.pinned = flags & 1, it.image = flags & 2, it.more = flags & 4, it.html = flags & 8, it.rtf = flags & 16;
                it.w = in.Num<uint32_t>(), it.h = in.Num<uint32_t>(), it.argb = in.Num<uint32_t>(), it.count = in.Num<uint32_t>();
                it.hash = in.Num<uint64_t>();
                it.head = in.Str(kHeadMax), it.app = in.Str(MAX_PATH), it.note = in.Str(512), it.src = in.Str(2048);
                if (!in.ok) break; // a torn tail: what came before stays
                lastId = (std::max)(lastId, it.id);
                ids.insert(it.id);
                items.push_back(std::move(it));
            }
    }
    WIN32_FIND_DATAW fd; // files no entry points at (a crash between writes): gone
    HANDLE f = dir.empty() ? INVALID_HANDLE_VALUE : FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (f != INVALID_HANDLE_VALUE) {
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY || !_wcsicmp(fd.cFileName, L"index.dat")) continue;
            uint64_t id = wcstoull(fd.cFileName, nullptr, 16);
            if (!ids.count(id)) DeleteFileW((dir + L"\\" + fd.cFileName).c_str());
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    Trim();
    ++ver;
}

void Store::SaveOptions() const {
    prefs::SaveNum(L"ClipMax", opt.max);
    prefs::SaveNum(L"ClipPaste", opt.paste);
    prefs::SaveNum(L"ClipPlain", opt.plain);
    prefs::SaveNum(L"ClipClean", opt.clean);
    prefs::SaveNum(L"ClipNoPass", opt.noPass);
    prefs::SaveNum(L"ClipNoAdult", opt.noAdult);
    prefs::SaveNum(L"ClipTitles", opt.titles);
    prefs::SaveNum(L"ClipView", opt.view);
    prefs::SaveNum(L"ClipSize", opt.size);
    prefs::SaveNum(L"ClipKey", opt.key);
    prefs::SaveNum(L"ClipTheme", opt.theme);
    prefs::SaveNum(L"ClipPaused", opt.paused);
    sys::SaveText(kNoApps, Join(opt.noApps, L'|'));
    sys::SaveText(kNoSites, Join(opt.noSites, L'|'));
}

void Store::Save() {
    dirty = false;
    if (dir.empty() || !loaded) return;
    Out o;
    o.b.reserve(items.size() * 160);
    o.Num(kMagic), o.Num(kVersion), o.Num((uint32_t)items.size());
    for (const Item& it : items) {
        o.Num(it.id), o.Num(it.at), o.Num((uint8_t)it.kind);
        o.Num((uint8_t)((it.pinned ? 1 : 0) | (it.image ? 2 : 0) | (it.more ? 4 : 0) | (it.html ? 8 : 0) | (it.rtf ? 16 : 0)));
        o.Num(it.w), o.Num(it.h), o.Num(it.argb), o.Num(it.count), o.Num(it.hash);
        o.Str(it.head), o.Str(it.app), o.Str(it.note), o.Str(it.src);
    }
    std::wstring tmp = dir + L"\\index.tmp";
    if (WriteAll(tmp, o.b.data(), o.b.size())) MoveFileExW(tmp.c_str(), (dir + L"\\index.dat").c_str(), MOVEFILE_REPLACE_EXISTING); // whole or not at all
}

void Store::Forget(const Item& it) {
    for (const wchar_t* ext : {L".txt", L".htm", L".rtf", L".png", L".t.png"}) DeleteFileW(File(it.id, ext).c_str()); // missing ones fail quietly
}

void Store::Remove(size_t i) {
    if (i >= items.size()) return;
    Forget(items[i]);
    items.erase(items.begin() + (ptrdiff_t)i);
    dirty = true, ++ver;
}

void Store::Clear() {
    for (size_t i = items.size(); i-- > 0;)
        if (!items[i].pinned) Remove(i);
}

void Store::Trim() {
    int unpinned = (int)std::count_if(items.begin(), items.end(), [](const Item& it) { return !it.pinned; });
    for (size_t i = items.size(); i-- > 0 && unpinned > opt.max;)
        if (!items[i].pinned) Remove(i), --unpinned;
}

void Store::Touch(size_t i) {
    if (i >= items.size()) return;
    items[i].at = Now();
    std::rotate(items.begin(), items.begin() + (ptrdiff_t)i, items.begin() + (ptrdiff_t)i + 1);
    dirty = true, ++ver;
}

void Store::Unload() {
    if (dirty) Save();
    items.clear();
    items.shrink_to_fit();
    loaded = false, ++ver;
}

void Store::DropPixels() {
    for (Item& it : items) it.px.Reset(), it.bmp.Reset(), it.pxTried = false;
}

std::wstring Store::Full(const Item& it) const {
    if (!it.more) return it.head;
    std::string raw;
    if (!ReadAll(File(it.id, L".txt"), raw, kTextMax * sizeof(wchar_t))) return it.head;
    return std::wstring(reinterpret_cast<const wchar_t*>(raw.data()), raw.size() / sizeof(wchar_t));
}

void Store::Insert(Item&& it) {
    items.insert(items.begin(), std::move(it));
    dirty = true, ++ver;
    Trim();
}

int Store::Capture(HWND hwnd, HWND notify, UINT doneMsg) {
    if (!loaded || opt.paused) return 0;
    HWND owner = GetClipboardOwner();
    if (owner == hwnd) return 0; // our own put: it's at the top already
    std::wstring app = ExeOf(owner);
    if (app.empty()) app = ExeOf(GetForegroundWindow()); // delayed rendering leaves no owner: the app in front copied it
    if (opt.noPass && std::any_of(std::begin(kPasswordApps), std::end(kPasswordApps), [&](const wchar_t* p) { return app == p; })) return 0;
    if (Listed(opt.noApps, app)) return 0;
    if (!OpenClipboard(hwnd)) return -1; // the copying app still holds it: the caller asks again shortly
    Item got;
    std::wstring text;
    std::string html, rtf, png;
    ComPtr<IWICBitmap> bmp;
    std::vector<std::wstring> paths;
    if (!Private()) {
        static const UINT kHtml = RegisterClipboardFormatW(L"HTML Format"), kRtf = RegisterClipboardFormatW(L"Rich Text Format"), kPng = RegisterClipboardFormatW(L"PNG");
        if (HANDLE h = IsClipboardFormatAvailable(CF_HDROP) ? GetClipboardData(CF_HDROP) : nullptr) { // files: one item, however many
            HDROP drop = static_cast<HDROP>(h);
            UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            for (UINT i = 0; i < n && i < kFilesMax; ++i) {
                wchar_t p[MAX_PATH * 2];
                if (DragQueryFileW(drop, i, p, MAX_PATH * 2)) paths.push_back(p);
            }
            got.count = n;
        } else if (HANDLE t = GetClipboardData(CF_UNICODETEXT)) { // text wins over a picture: a spreadsheet's cells come with one too
            if (const wchar_t* p = static_cast<const wchar_t*>(GlobalLock(t))) {
                text.assign(p, wcsnlen(p, (std::min)(kTextMax, GlobalSize(t) / sizeof(wchar_t))));
                GlobalUnlock(t);
            }
            html = Bytes(kHtml, kRichMax);
            rtf = Bytes(kRtf, kRichMax);
            got.src = SourceUrl(html);
        } else if (png = Bytes(kPng, kPngMax); !png.empty()) { // keeps transparency
        } else if (HBITMAP hb = static_cast<HBITMAP>(GetClipboardData(CF_BITMAP)); hb && gfx::Wic()) {
            gfx::Wic()->CreateBitmapFromHBITMAP(hb, nullptr, WICBitmapIgnoreAlpha, &bmp); // copied out while the clipboard is still open
        }
    }
    CloseClipboard();

    got.app = app;
    got.at = Now();
    std::wstring srcHost = Host(got.src);
    if ((opt.noAdult && Adult(srcHost)) || SiteListed(opt.noSites, srcHost)) return 0; // copied off a page that's never kept
    if (!paths.empty()) {
        got.kind = std::all_of(paths.begin(), paths.end(), MediaFile) ? kMedia : kFiles;
        text = Join(paths, L'\n');
        got.hash = Fnv(text.data(), text.size() * sizeof(wchar_t), got.kind);
    } else if (!png.empty() || bmp) {
        got.kind = kMedia, got.image = true;
        if (!png.empty()) PngSize(png, got.w, got.h), got.hash = Fnv(png.data(), png.size());
        else bmp->GetSize(&got.w, &got.h), got.hash = PixelHash(bmp.Get(), got.w, got.h);
    } else {
        std::wstring t = Trimmed(text);
        if (t.empty()) return 0;
        if (IsLink(t)) {
            std::wstring host = Host(t);
            if ((opt.noAdult && Adult(host)) || SiteListed(opt.noSites, host)) return 0;
            got.kind = kLink;
            bool changed = false;
            if (opt.clean) t = CleanLink(t, changed);
            if (changed && OpenClipboard(hwnd)) { // the clean one is what gets pasted from now on
                EmptyClipboard();
                Set(CF_UNICODETEXT, Global(t.c_str(), (t.size() + 1) * sizeof(wchar_t)));
                CloseClipboard();
            }
            text = t, html.clear(), rtf.clear();
        } else if (ParseColor(t, got.argb)) {
            got.kind = kColor, text = t;
        } else if (std::wstring lang = CodeLang(text); !lang.empty()) {
            got.kind = kCode, got.note = lang;
        }
        got.hash = Fnv(text.data(), text.size() * sizeof(wchar_t), got.kind);
    }
    for (size_t i = 0; i < items.size(); ++i) // copied again: the old one comes to the top, newer
        if (items[i].hash == got.hash && items[i].kind == got.kind) {
            items[i].app = got.app;
            if (!got.src.empty()) items[i].src = got.src;
            Touch(i);
            return 1;
        }
    got.id = NewId();
    got.head = text.substr(0, kHeadMax);
    if (text.size() > kHeadMax) got.more = WriteAll(File(got.id, L".txt"), text.data(), text.size() * sizeof(wchar_t));
    if (!html.empty()) got.html = WriteAll(File(got.id, L".htm"), html.data(), html.size());
    if (!rtf.empty()) got.rtf = WriteAll(File(got.id, L".rtf"), rtf.data(), rtf.size());
    if (got.image) {
        ImageJob job{File(got.id, L".png"), File(got.id, L".t.png"), std::move(png), std::move(bmp), notify, doneMsg, got.id};
        WriteImage(std::move(job));
    }
    Insert(std::move(got));
    return 1;
}

bool Store::Put(HWND hwnd, const Item& it, bool plain) const {
    std::string png;
    std::wstring text = it.image ? std::wstring() : Full(it);
    if (it.image && !ReadAll(File(it.id, L".png"), png, kPngMax)) return false; // still being written, or gone
    bool dibOk = false;
    HGLOBAL dib = it.image ? Dib(png, dibOk) : nullptr;
    if (it.image && !dib) return false;
    if (!OpenClipboard(hwnd)) {
        if (dib) GlobalFree(dib);
        return false;
    }
    EmptyClipboard();
    if (it.image) {
        static const UINT kPng = RegisterClipboardFormatW(L"PNG");
        Set(CF_DIB, dib);
        Set(kPng, Global(png.data(), png.size()));
    } else if (it.kind == kFiles || it.kind == kMedia) { // files go back as files, and their paths as text for an editor
        std::vector<std::wstring> paths = Split(text, L'\n');
        std::wstring list;
        for (const auto& p : paths) list += p + L'\0';
        list += L'\0';
        size_t bytes = sizeof(DROPFILES) + list.size() * sizeof(wchar_t);
        if (HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE | GMEM_ZEROINIT, bytes)) {
            auto* df = static_cast<DROPFILES*>(GlobalLock(g));
            df->pFiles = sizeof(DROPFILES);
            df->fWide = TRUE;
            memcpy(df + 1, list.data(), list.size() * sizeof(wchar_t));
            GlobalUnlock(g);
            Set(CF_HDROP, g);
        }
        static const UINT kEffect = RegisterClipboardFormatW(L"Preferred DropEffect");
        DWORD copy = DROPEFFECT_COPY;
        Set(kEffect, Global(&copy, sizeof(copy)));
        std::wstring lines = Join(paths, L'\n');
        Set(CF_UNICODETEXT, Global(lines.c_str(), (lines.size() + 1) * sizeof(wchar_t)));
    } else {
        Set(CF_UNICODETEXT, Global(text.c_str(), (text.size() + 1) * sizeof(wchar_t)));
        std::string rich;
        static const UINT kHtml = RegisterClipboardFormatW(L"HTML Format"), kRtf = RegisterClipboardFormatW(L"Rich Text Format");
        if (!plain && it.html && ReadAll(File(it.id, L".htm"), rich, kRichMax)) Set(kHtml, Global(rich.data(), rich.size()));
        if (!plain && it.rtf && ReadAll(File(it.id, L".rtf"), rich, kRichMax)) Set(kRtf, Global(rich.data(), rich.size()));
    }
    CloseClipboard();
    return true;
}

}
