// Dragging an item out of the clipboard panel: files as themselves, a picture as a png file made on drop, words and links as text.
#include "clips.h"

#include <shellapi.h>
#include <shlobj.h>

#include <string>
#include <vector>

namespace clip {

namespace {

constexpr size_t kDragPngMax = 64u << 20; // as big as the history keeps
constexpr int64_t kUnixToFile = 11644473600LL; // seconds from 1601 to 1970
constexpr int64_t kTicksPerSec = 10000000LL;

HGLOBAL Copy(const void* data, size_t n) {
    HGLOBAL g = GlobalAlloc(GMEM_MOVEABLE, n ? n : 1);
    if (!g) return nullptr;
    if (void* p = GlobalLock(g)) memcpy(p, data, n), GlobalUnlock(g);
    return g;
}

HGLOBAL Dup(HGLOBAL g) {
    size_t n = g ? GlobalSize(g) : 0;
    const void* p = n ? GlobalLock(g) : nullptr;
    if (!p) return nullptr;
    HGLOBAL out = Copy(p, n);
    GlobalUnlock(g);
    return out;
}

struct Entry {
    FORMATETC fe;
    STGMEDIUM sm;
};

class Data final : public IDataObject { // the few formats one item offers, each handed out as a fresh copy
public:
    ~Data() {
        for (auto& e : list) ReleaseStgMedium(&e.sm);
    }
    void Add(UINT cf, HGLOBAL g, LONG index = -1) {
        if (!g) return;
        Entry e{{(CLIPFORMAT)cf, nullptr, DVASPECT_CONTENT, index, TYMED_HGLOBAL}, {}};
        e.sm.tymed = TYMED_HGLOBAL, e.sm.hGlobal = g;
        list.push_back(e);
    }
    bool Empty() const { return list.empty(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = id == IID_IUnknown || id == IID_IDataObject ? static_cast<IDataObject*>(this) : nullptr;
        if (!*out) return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetData(FORMATETC* fe, STGMEDIUM* sm) override {
        if (!fe || !sm) return E_INVALIDARG;
        const Entry* e = Find(fe);
        if (!e) return DV_E_FORMATETC;
        if (!(fe->tymed & TYMED_HGLOBAL)) return DV_E_TYMED;
        *sm = {};
        sm->tymed = TYMED_HGLOBAL;
        sm->hGlobal = Dup(e->sm.hGlobal); // the taker owns its copy
        return sm->hGlobal ? S_OK : E_OUTOFMEMORY;
    }
    HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC*, STGMEDIUM*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC* fe) override { return fe && Find(fe) && (fe->tymed & TYMED_HGLOBAL) ? S_OK : DV_E_FORMATETC; }
    HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC*, FORMATETC* out) override {
        if (out) out->ptd = nullptr;
        return E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetData(FORMATETC* fe, STGMEDIUM* sm, BOOL release) override { // the shell keeps its drag notes here
        if (!fe || !sm) return E_INVALIDARG;
        if (sm->tymed != TYMED_HGLOBAL) return E_NOTIMPL;
        HGLOBAL g = release ? sm->hGlobal : Dup(sm->hGlobal);
        if (!g) return E_OUTOFMEMORY;
        if (release && sm->pUnkForRelease) sm->pUnkForRelease->Release(); // the bytes are ours now
        for (size_t i = 0; i < list.size(); ++i)
            if (list[i].fe.cfFormat == fe->cfFormat && list[i].fe.lindex == fe->lindex) ReleaseStgMedium(&list[i].sm), list.erase(list.begin() + (ptrdiff_t)i);
        Add(fe->cfFormat, g, fe->lindex);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD dir, IEnumFORMATETC** out) override {
        if (dir != DATADIR_GET) return E_NOTIMPL;
        std::vector<FORMATETC> fes;
        for (const auto& e : list) fes.push_back(e.fe);
        return SHCreateStdEnumFmtEtc((UINT)fes.size(), fes.data(), out);
    }
    HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC*, DWORD, IAdviseSink*, DWORD*) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override { return OLE_E_ADVISENOTSUPPORTED; }
    HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA**) override { return OLE_E_ADVISENOTSUPPORTED; }

private:
    const Entry* Find(const FORMATETC* fe) const {
        for (const auto& e : list)
            if (e.fe.cfFormat == fe->cfFormat && (e.fe.lindex == fe->lindex || e.fe.lindex == -1) && (fe->dwAspect & DVASPECT_CONTENT)) return &e;
        return nullptr;
    }
    ULONG refs = 1;
    std::vector<Entry> list;
};

std::vector<std::wstring> Lines(const std::wstring& s) {
    std::vector<std::wstring> out;
    for (size_t a = 0; a < s.size();) {
        size_t e = s.find(L'\n', a);
        if (e == std::wstring::npos) e = s.size();
        if (e > a) out.push_back(s.substr(a, e - a));
        a = e + 1;
    }
    return out;
}

std::wstring PictureName(int64_t at) { // when it was copied, as the file's name
    ULARGE_INTEGER u;
    u.QuadPart = (ULONGLONG)((at + kUnixToFile) * kTicksPerSec);
    FILETIME utc{u.LowPart, u.HighPart}, local{};
    SYSTEMTIME t{};
    FileTimeToLocalFileTime(&utc, &local);
    FileTimeToSystemTime(&local, &t);
    wchar_t name[64];
    swprintf_s(name, L"Clip %04u-%02u-%02u %02u.%02u.%02u.png", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    return name;
}

void AddPicture(Data& d, const Store& st, const Item& it) { // a virtual file: the png is opened only into memory, never left on disk in the clear
    std::string png = st.Opened(it, L".png", kDragPngMax);
    if (png.empty()) return;
    FILEGROUPDESCRIPTORW fgd{};
    fgd.cItems = 1;
    fgd.fgd[0].dwFlags = FD_FILESIZE | FD_PROGRESSUI;
    fgd.fgd[0].nFileSizeLow = (DWORD)png.size();
    wcsncpy_s(fgd.fgd[0].cFileName, PictureName(it.at).c_str(), _TRUNCATE);
    d.Add(RegisterClipboardFormatW(CFSTR_FILEDESCRIPTORW), Copy(&fgd, sizeof(fgd)));
    d.Add(RegisterClipboardFormatW(CFSTR_FILECONTENTS), Copy(png.data(), png.size()), 0);
    d.Add(RegisterClipboardFormatW(L"PNG"), Copy(png.data(), png.size())); // apps that take a picture itself
}

void AddFiles(Data& d, const std::vector<std::wstring>& paths) {
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
        d.Add(CF_HDROP, g);
    }
    DWORD copy = DROPEFFECT_COPY; // a copy, never a move of the user's own files
    d.Add(RegisterClipboardFormatW(CFSTR_PREFERREDDROPEFFECT), Copy(&copy, sizeof(copy)));
}

}

bool DragOut(HWND hwnd, const Store& st, const Item& it) {
    static const bool ole = SUCCEEDED(OleInitialize(nullptr)); // drag and drop wants ole; already on when the dock takes drops
    if (!ole) return false;
    Microsoft::WRL::ComPtr<Data> d;
    d.Attach(new Data);
    DWORD allow = DROPEFFECT_COPY;
    if (it.image) {
        AddPicture(*d.Get(), st, it);
    } else if (it.kind == kFiles || it.kind == kMedia) {
        AddFiles(*d.Get(), Lines(st.Full(it))); // the full list: the head may be cut
        allow |= DROPEFFECT_LINK; // a shortcut is fine too
    } else {
        std::wstring text = st.Full(it);
        d->Add(CF_UNICODETEXT, Copy(text.c_str(), (text.size() + 1) * sizeof(wchar_t)));
        if (it.kind == kLink) {
            std::wstring url = it.head.rfind(L"www.", 0) == 0 ? L"https://" + it.head : it.head;
            d->Add(RegisterClipboardFormatW(CFSTR_INETURLW), Copy(url.c_str(), (url.size() + 1) * sizeof(wchar_t))); // a browser opens it, a folder makes a shortcut
        }
    }
    if (d->Empty()) return false;
    DWORD effect = DROPEFFECT_NONE;
    return SHDoDragDrop(hwnd, d.Get(), nullptr, allow, &effect) == DRAGDROP_S_DROP;
}

}
