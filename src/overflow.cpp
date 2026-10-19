#include "overflow.h"

#include <objbase.h>
#include <shlobj.h>
#include <uiautomation.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwctype>
#include <string>
#include <vector>

#include "taskbar.h"

using Microsoft::WRL::ComPtr;

namespace overflow {
namespace {

constexpr wchar_t kFlyoutClass[] = L"TopLevelWindowForOverflowXamlIsland";
constexpr wchar_t kBridgeClass[] = L"Windows.UI.Composition.DesktopWindowContentBridge"; // the xaml content of explorer's taskbar
constexpr wchar_t kIconsKey[] = L"Control Panel\\NotifyIconSettings"; // one subkey per icon windows has ever seen

ComPtr<IUIAutomation> uia;
ComPtr<IUIAutomationElement> chevron; // found once, pressed many times: the search is the slow part
HWND flyout = nullptr;
POINT at{};
int edge = 0; // the flyout hangs from here, or sits on it
bool below = true;

struct Pin {
    std::wstring exe; // lowercase, known folders expanded
    DWORD uid = 0;
    bool anyUid = true; // guid icons have no uid: the app alone decides
};
std::vector<Pin> pins;
HKEY iconsKey = nullptr;
HANDLE iconsChanged = nullptr;

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

IUIAutomation* Uia() {
    if (!uia) CoCreateInstance(__uuidof(CUIAutomation), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&uia));
    return uia.Get();
}

bool Search() { // the chevron leads explorer's notification area; with its hidden icon menu off, icons come first and there is none
    chevron.Reset();
    HWND tray = taskbar::ExplorerTray();
    HWND bridge = tray ? FindWindowExW(tray, nullptr, kBridgeClass, nullptr) : nullptr;
    ComPtr<IUIAutomationElement> root;
    ComPtr<IUIAutomationCondition> all;
    ComPtr<IUIAutomationElementArray> found;
    if (!bridge || !Uia() || FAILED(uia->ElementFromHandle(bridge, &root)) || !root) return false;
    if (FAILED(uia->CreateTrueCondition(&all)) || FAILED(root->FindAll(TreeScope_Descendants, all.Get(), &found)) || !found) return false;
    int n = 0;
    found->get_Length(&n);
    for (int i = 0; i < n; ++i) {
        ComPtr<IUIAutomationElement> e;
        BSTR cls = nullptr;
        if (FAILED(found->GetElement(i, &e)) || !e || FAILED(e->get_CurrentClassName(&cls)) || !cls) continue;
        bool area = wcsncmp(cls, L"SystemTray.", 11) == 0, chev = wcscmp(cls, L"SystemTray.NormalButton") == 0;
        SysFreeString(cls);
        if (!area) continue; // start, task view, the apps
        if (chev) chevron = e;
        return chev;
    }
    return false;
}

bool Find() {
    if (chevron) return true;
    if (Search()) return true;
    HWND tray = taskbar::ExplorerTray(); // a taskbar hidden since sign-in may never have built its xaml: shown a moment, off screen and auto-hidden
    if (!tray) return false;
    ShowWindow(tray, SW_SHOWNA);
    bool ok = Search();
    ShowWindow(tray, SW_HIDE);
    return ok;
}

bool Press() {
    ComPtr<IUIAutomationInvokePattern> invoke;
    return chevron && SUCCEEDED(chevron->GetCurrentPatternAs(UIA_InvokePatternId, IID_PPV_ARGS(&invoke))) && invoke && SUCCEEDED(invoke->Invoke());
}

HWND Flyout() {
    if (flyout && IsWindow(flyout)) return flyout;
    flyout = nullptr;
    HWND tray = taskbar::ExplorerTray();
    DWORD want = 0, pid = 0;
    if (tray) GetWindowThreadProcessId(tray, &want);
    for (HWND h = nullptr; (h = FindWindowExW(nullptr, h, kFlyoutClass, nullptr));) // explorer's own, never a lookalike
        if (GetWindowThreadProcessId(h, &pid), pid == want) return flyout = h;
    return nullptr;
}

std::wstring Expand(const std::wstring& path) { // "{known folder guid}\rest", as windows stores app paths that move between machines
    if (path.size() < 39 || path[0] != L'{' || path[37] != L'}') return path;
    GUID id;
    PWSTR base = nullptr;
    std::wstring out = path;
    if (SUCCEEDED(CLSIDFromString(path.substr(0, 38).c_str(), &id)) && SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &base))) out = std::wstring(base) + path.substr(38);
    CoTaskMemFree(base);
    return out;
}

void Load() {
    pins.clear();
    wchar_t name[64];
    for (DWORD i = 0, len = 64; RegEnumKeyExW(iconsKey, i, name, &len, nullptr, nullptr, nullptr, nullptr) == ERROR_SUCCESS; ++i, len = 64) {
        DWORD promoted = 0, uid = 0, size = sizeof(DWORD);
        if (RegGetValueW(iconsKey, name, L"IsPromoted", RRF_RT_REG_DWORD, nullptr, &promoted, &size) != ERROR_SUCCESS || !promoted) continue;
        wchar_t exe[MAX_PATH * 2] = {};
        size = sizeof(exe);
        if (RegGetValueW(iconsKey, name, L"ExecutablePath", RRF_RT_REG_SZ, nullptr, exe, &size) != ERROR_SUCCESS) continue;
        Pin p{Lower(Expand(exe))};
        size = sizeof(DWORD);
        if (RegGetValueW(iconsKey, name, L"UID", RRF_RT_REG_DWORD, nullptr, &uid, &size) == ERROR_SUCCESS) p.uid = uid, p.anyUid = false;
        pins.push_back(std::move(p));
    }
}

void Watch() { RegNotifyChangeKeyValue(iconsKey, TRUE, REG_NOTIFY_CHANGE_NAME | REG_NOTIFY_CHANGE_LAST_SET, iconsChanged, TRUE); } // one shot: re-armed after each change

}

bool Open(POINT anchor, int edgeY, bool hang) {
    at = anchor, edge = edgeY, below = hang;
    if (Shown()) return true;
    if (!Find()) return false;
    if (!Press()) { // explorer restarted since: its elements are gone
        chevron.Reset();
        if (!Find() || !Press()) return false;
    }
    Flyout();
    return true;
}

void Close() {
    if (Shown()) Press(); // the chevron toggles: explorer closes it as if clicked, focus and all
}

bool Shown() { return flyout && IsWindow(flyout) && IsWindowVisible(flyout); }

bool Is(HWND h) { return h && h == flyout; }

void Place() {
    HWND f = Flyout();
    RECT r;
    if (!f || !GetWindowRect(f, &r)) return;
    int w = r.right - r.left, h = r.bottom - r.top;
    MONITORINFO mi = {sizeof(mi)};
    GetMonitorInfoW(MonitorFromPoint(at, MONITOR_DEFAULTTONEAREST), &mi);
    int x = (std::clamp)((int)at.x - w / 2, (int)mi.rcWork.left + 8, (std::max)((int)mi.rcWork.left + 8, (int)mi.rcWork.right - 8 - w)); // centered on the button, kept on screen
    int y = below ? edge : edge - h;
    if (x != r.left || y != r.top) SetWindowPos(f, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE); // only when it moved: this runs on its own location events
}

void Forget() {
    chevron.Reset();
    flyout = nullptr;
}

bool Promoted(HWND owner, UINT id) {
    if (pins.empty() || !owner) return false;
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!p) return false;
    wchar_t path[MAX_PATH * 2];
    DWORD len = MAX_PATH * 2;
    bool ok = QueryFullProcessImageNameW(p, 0, path, &len);
    CloseHandle(p);
    if (!ok) return false;
    std::wstring exe = Lower(path);
    return std::any_of(pins.begin(), pins.end(), [&](const Pin& pin) { return pin.exe == exe && (pin.anyUid || pin.uid == id); });
}

bool PromotedChanged() {
    if (!iconsChanged) { // first ask: open, read, start watching
        if (RegOpenKeyExW(HKEY_CURRENT_USER, kIconsKey, 0, KEY_READ | KEY_NOTIFY, &iconsKey) != ERROR_SUCCESS) return false;
        iconsChanged = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        Watch();
        Load();
        return true;
    }
    if (WaitForSingleObject(iconsChanged, 0) != WAIT_OBJECT_0) return false;
    Watch();
    Load();
    return true;
}

}
