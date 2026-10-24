#include "bar.h"

#include <shellapi.h>

#include "tasks.h"

namespace {

constexpr ULONGLONG kRaiseMs = 600; // a file held over an app this long brings its window up to drop into, as windows' taskbar does

HWND Latest(const TaskApp& a) { // the window used last: the one a click would bring back
    const TaskWin* last = &a.wins[0];
    for (const auto& w : a.wins)
        if (w.used > last->used) last = &w;
    return last->hwnd;
}

std::vector<std::wstring> FilesOf(IDataObject* d) { // the paths a drag carries, empty when it isn't files
    std::vector<std::wstring> out;
    FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    STGMEDIUM sm{};
    if (!d || FAILED(d->GetData(&fe, &sm))) return out;
    if (auto drop = (HDROP)GlobalLock(sm.hGlobal)) {
        UINT n = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        for (UINT i = 0; i < n; ++i) {
            std::wstring p(DragQueryFileW(drop, i, nullptr, 0), L'\0');
            DragQueryFileW(drop, i, p.data(), (UINT)p.size() + 1);
            out.push_back(std::move(p));
        }
        GlobalUnlock(sm.hGlobal);
    }
    ReleaseStgMedium(&sm);
    return out;
}

class Dropper final : public IDropTarget { // ole calls in on the bar's own thread, between its messages
public:
    explicit Dropper(Bar* b) : bar(b) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        *out = id == IID_IUnknown || id == IID_IDropTarget ? static_cast<IDropTarget*>(this) : nullptr;
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
    HRESULT STDMETHODCALLTYPE DragEnter(IDataObject* d, DWORD, POINTL pt, DWORD* effect) override {
        FORMATETC fe = {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
        files = d && d->QueryGetData(&fe) == S_OK; // other drags still raise windows, they just can't be dropped
        *effect = bar->DragOver({pt.x, pt.y}, files);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragOver(DWORD, POINTL pt, DWORD* effect) override {
        *effect = bar->DragOver({pt.x, pt.y}, files);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DragLeave() override {
        bar->DragGone();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Drop(IDataObject* d, DWORD, POINTL pt, DWORD* effect) override {
        *effect = bar->Drop({pt.x, pt.y}, FilesOf(d));
        return S_OK;
    }

private:
    ULONG refs = 1;
    Bar* bar;
    bool files = false;
};

}

void Bar::SyncDrop() {
    static const bool ole = SUCCEEDED(OleInitialize(nullptr)); // drag and drop wants ole on top of the sta main already has
    if (!ole || dropper || !hwnd) return;
    ComPtr<IDropTarget> d;
    d.Attach(new Dropper(this));
    if (SUCCEEDED(RegisterDragDrop(hwnd, d.Get()))) dropper = d;
}

bool Bar::ScreenRect(const D2D1_RECT_F& r, RECT& out) const { // where it shows with the dock out: tucked away it's past the screen's edge
    if (r.right <= r.left || r.bottom <= r.top) return false;
    float lift = sink * (kWegH + kWegMargin + 4);
    D2D1_POINT_2F d = !Vert() ? D2D1_POINT_2F{0, -lift} : D2D1_POINT_2F{cfg.edge == 1 ? lift : -lift, 0};
    POINT a = ToScreen({r.left + d.x, r.top + d.y}), b = ToScreen({r.right + d.x, r.bottom + d.y});
    out = {a.x, a.y, b.x, b.y};
    return true;
}

int Bar::AppAt(POINT screen) const { // the dock's app under a point on screen, -1 none
    if (!tasks) return -1;
    POINT p = screen;
    ScreenToClient(hwnd, &p);
    D2D1_POINT_2F at = {p.x / scale, p.y / scale};
    const auto& apps = tasks->Apps();
    int n = (std::min)(dockCount, (int)apps.size());
    for (int i = 0; i < n; ++i)
        if (auto it = slots.find(apps[(size_t)i].key); it != slots.end() && Inside(it->second, at)) return i;
    return -1;
}

DWORD Bar::DragOver(POINT screen, bool files) {
    if (AutoHide() && sink > 0.5f && !peek) peek = true, Redraw(); // dragged to the edge: the dock comes out to take it
    KillTimer(hwnd, kPeekTimer);
    int i = AppAt(screen);
    ULONGLONG now = GetTickCount64();
    if (i != dropApp) dropApp = i, dropSince = now, dropRaised = false, Redraw();
    if (i < 0) return DROPEFFECT_NONE;
    const TaskApp& a = tasks->Apps()[(size_t)i];
    if (!dropRaised && !a.wins.empty() && now - dropSince >= kRaiseMs) dropRaised = true, Tasks::Activate(Latest(a)); // ole asks again and again while the mouse rests
    return files && tasks->CanOpen(i) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

void Bar::DragGone() {
    if (dropApp >= 0) dropApp = -1, Redraw();
    dropRaised = false;
    if (peek) SetTimer(hwnd, kPeekTimer, kPeekMs, nullptr); // brought out for the drag: tucks away again as after the mouse
}

DWORD Bar::Drop(POINT screen, const std::vector<std::wstring>& files) {
    int i = AppAt(screen);
    DragGone();
    return i >= 0 && tasks->OpenWith(i, files) ? DROPEFFECT_COPY : DROPEFFECT_NONE;
}

void Bar::FocusDock(int step) { // win + t, as windows' taskbar: the keyboard walks the dock's apps
    int n = tasks && !moon ? (std::min)(dockCount, (int)tasks->Apps().size()) : 0;
    if (n <= 0) return;
    if (dockKey < 0) {
        HWND f = GetForegroundWindow();
        keyBack = f != hwnd ? f : nullptr;
        dockKey = step > 0 ? 0 : n - 1;
    } else {
        dockKey = (dockKey + step + n) % n;
    }
    if (AutoHide() && sink > 0.5f) peek = true; // tucked away: out it comes while the keyboard is on it
    KillTimer(hwnd, kPeekTimer);
    SetForegroundWindow(hwnd); // the hook's own input came last, so windows lets us
    Redraw();
}

void Bar::LeaveDock(bool back) {
    dockKey = -1;
    if (back && keyBack && IsWindow(keyBack)) SetForegroundWindow(keyBack);
    keyBack = nullptr;
    if (peek) SetTimer(hwnd, kPeekTimer, kPeekMs, nullptr);
    Redraw();
}

void Bar::DockKey(WPARAM vk) {
    int n = tasks ? (std::min)(dockCount, (int)tasks->Apps().size()) : 0;
    if (n <= 0 || vk == VK_ESCAPE) return LeaveDock(true);
    if (vk == VK_LEFT || vk == VK_UP) dockKey = (dockKey - 1 + n) % n;
    else if (vk == VK_RIGHT || vk == VK_DOWN || vk == VK_TAB) dockKey = (dockKey + 1) % n;
    else if (vk == VK_HOME) dockKey = 0;
    else if (vk == VK_END) dockKey = n - 1;
    else if (vk == VK_RETURN || vk == VK_SPACE) { // its window comes up, or it starts: never minimized, the dock had the keyboard
        int i = (std::min)(dockKey, n - 1);
        LeaveDock(false);
        const TaskApp& a = tasks->Apps()[(size_t)i];
        if (a.wins.empty()) tasks->Launch(i);
        else Tasks::Activate(Latest(a));
        return;
    }
    Redraw();
}
