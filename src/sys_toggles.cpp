#include "toggles.h"

#include <objbase.h>
#include <powrprof.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <thread>
#include <vector>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Networking.Connectivity.h>
#include <winrt/Windows.Networking.NetworkOperators.h>

namespace sys {
namespace {

namespace WNC = winrt::Windows::Networking::Connectivity;
namespace WNO = winrt::Windows::Networking::NetworkOperators;

// ---- shared helpers ----

void ShellAsync(const wchar_t* target) { // shellexecute can stall on a cold settings app, keep it off the ui thread
    std::wstring t = target; // copied: the caller's string may not outlive the thread
    std::thread([t] {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE); // shell wants sta
        SHELLEXECUTEINFOW sei{sizeof(sei)};
        sei.fMask = SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI; // thread ends right after, so finish the launch here
        sei.lpVerb = L"open";
        sei.lpFile = t.c_str();
        sei.nShow = SW_SHOWNORMAL;
        if (!ShellExecuteExW(&sei)) OutputDebugStringW(L"[toggles] ShellExecuteExW failed\n");
        if (SUCCEEDED(hr)) CoUninitialize(); // only balance a com init that took
    }).detach(); // fire and forget: nothing waits for the settings app
}

bool SendChord(std::initializer_list<WORD> keys) { // press all in order, release in reverse
    std::vector<INPUT> in;
    in.reserve(keys.size() * 2);
    for (WORD vk : keys) {
        INPUT i{INPUT_KEYBOARD};
        i.ki.wVk = vk;
        in.push_back(i);
    }
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) {
        INPUT i{INPUT_KEYBOARD};
        i.ki.wVk = *it;
        i.ki.dwFlags = KEYEVENTF_KEYUP;
        in.push_back(i);
    }
    const UINT sent = SendInput((UINT)in.size(), in.data(), sizeof(INPUT)); // one call, so no real keystroke can slip between ours
    if (sent != in.size()) OutputDebugStringW(L"[toggles] SendInput blocked\n"); // uipi or a secure desktop
    return sent == in.size();
}

// ---- energy saver ----
// no public setter exists. settings' own "always use energy saver" switch publishes a dword to this wnf
// state (1 = always on, 2 = automatic by battery threshold); the power service persists it to
// hklm\...\Control\Power\EnergySaverState. publishing needs no admin. the GUID_ENERGY_SAVER_BATTERY_THRESHOLD
// route was tried and does nothing on ac, so it isn't used.
constexpr ULONGLONG kWnfEnergySaver = 0x41c6013da3bc3075ull; // from SettingsHandlers_OneCore_BatterySaver.dll
constexpr DWORD kSaverAlways = 1, kSaverAuto = 2; // the two values settings itself writes

using WnfQuery = LONG(NTAPI*)(const ULONGLONG*, const void*, const void*, ULONG*, void*, ULONG*); // undocumented: state name, type id, scope, stamp, buffer, size
using WnfPublish = LONG(NTAPI*)(ULONGLONG, const void*, const void*, ULONG, const void*); // name goes by value

struct Wnf {
    WnfQuery query = nullptr;
    WnfPublish publish = nullptr;
    Wnf() {
        if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
            query = reinterpret_cast<WnfQuery>(GetProcAddress(nt, "NtQueryWnfStateData")); // looked up at runtime, so a build without them still starts
            publish = reinterpret_cast<WnfPublish>(GetProcAddress(nt, "RtlPublishWnfStateData"));
        }
    }
};

const Wnf& Ntdll() {
    static const Wnf w;
    return w;
}

bool SaverMode(DWORD& mode) { // false on builds without the state (pre 24h2) or a shape we don't know
    const Wnf& w = Ntdll();
    if (!w.query) return false;
    DWORD v = 0;
    ULONG stamp = 0, size = sizeof(v);
    if (w.query(&kWnfEnergySaver, nullptr, nullptr, &stamp, &v, &size) < 0 || size != sizeof(v)) return false; // negative ntstatus: no such state here
    if (v != kSaverAlways && v != kSaverAuto) return false; // an unknown value: don't pretend to understand it
    mode = v;
    return true;
}

std::atomic<int> gSaverStatus{-1}; // ENERGY_SAVER_STATUS from the power notification, -1 until it lands

ULONG CALLBACK OnSaverStatus(PVOID, ULONG type, PVOID setting) { // runs on a system thread, hence the atomic
    if (type != PBT_POWERSETTINGCHANGE || !setting) return ERROR_SUCCESS;
    const auto* s = static_cast<const POWERBROADCAST_SETTING*>(setting);
    if (s->PowerSetting == GUID_ENERGY_SAVER_STATUS && s->DataLength >= sizeof(DWORD))
        gSaverStatus.store((int)*reinterpret_cast<const DWORD*>(s->Data));
    return ERROR_SUCCESS;
}

void WatchSaver() { // once per process; the registration posts the current value right away
    static std::once_flag once;
    std::call_once(once, [] {
        static DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS params{OnSaverStatus, nullptr}; // static: must outlive the registration
        HPOWERNOTIFY h = nullptr; // kept for the process lifetime on purpose
        if (PowerSettingRegisterNotification(&GUID_ENERGY_SAVER_STATUS, DEVICE_NOTIFY_CALLBACK, &params, &h) != ERROR_SUCCESS)
            OutputDebugStringW(L"[toggles] energy saver status unavailable\n"); // pre 24h2
    });
}

// ---- night light ----
// HKCU cloudstore blob, written by settings and watched live by the shell. layouts, verified on 26200:
//   off (41): 43 42 01 00 0a 02 01 00 2a 06 <unix secs, 5-byte leb128> 2a 2b 0e 13 43 42 01 00
//             d0 0a 02 c6 14 <filetime, 9-byte leb128> 00 00 00 00
//   on  (43): same, but byte 18 = 15 and 10 00 sits after the inner 43 42 01 00
//   never used (13): 43 42 01 00 0a 02 01 00 2a 2a 00 00 00, reads as off
constexpr wchar_t kNightKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current\\"
    L"default$windows.data.bluelightreduction.bluelightreductionstate\\"
    L"windows.data.bluelightreduction.bluelightreductionstate";
constexpr uint8_t kNightHead[] = {0x43, 0x42, 0x01, 0x00, 0x0a, 0x02, 0x01, 0x00, 0x2a, 0x06}; // "CB" magic, version, then the stamp's field tag
constexpr uint8_t kNightMid[] = {0x2a, 0x2b, 0x0e};
constexpr uint8_t kNightInner[] = {0x43, 0x42, 0x01, 0x00};
constexpr uint8_t kNightOnFlag[] = {0x10, 0x00}; // present only while night light is on
constexpr uint8_t kNightTime[] = {0xd0, 0x0a, 0x02, 0xc6, 0x14};
constexpr uint8_t kNightTail[] = {0x00, 0x00, 0x00, 0x00};
constexpr uint8_t kNightStub[] = {0x43, 0x42, 0x01, 0x00, 0x0a, 0x02, 0x01, 0x00, 0x2a, 0x2a, 0x00, 0x00, 0x00};
constexpr size_t kStampLen = 5, kFileTimeLen = 9; // leb128 widths windows uses today; other widths are refused
constexpr uint8_t kInnerOff = 0x13, kInnerOn = 0x15; // inner record length byte
constexpr size_t kNightOffLen = 41, kNightOnLen = 43; // the whole blob, matching the layouts above
constexpr DWORD kNightMaxLen = 256; // anything bigger is a layout we don't know

enum class Night { Bad, Off, On }; // Bad: unknown layout, never written over

bool Has(const std::vector<uint8_t>& b, size_t at, const uint8_t* p, size_t n) { // bounds-checked match of n bytes at at
    return at + n <= b.size() && std::equal(p, p + n, b.begin() + at);
}

bool Leb(const std::vector<uint8_t>& b, size_t at, size_t n, uint64_t& v) { // exactly n bytes, last one ends it
    if (at + n > b.size()) return false;
    v = 0;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t x = b[at + i];
        if ((i + 1 < n) != ((x & 0x80) != 0)) return false; // continuation bits must say "n bytes"
        v |= uint64_t(x & 0x7f) << (7 * i); // little-endian groups of seven bits
    }
    return true;
}

bool PutLeb(std::vector<uint8_t>& b, uint64_t v, size_t n) { // refuses values that don't fill exactly n bytes
    if (v >> (7 * n) || !(v >> (7 * (n - 1)))) return false; // too big, or small enough that windows would write it shorter
    for (size_t i = 0; i < n; ++i, v >>= 7) b.push_back(uint8_t(v & 0x7f) | (i + 1 < n ? 0x80 : 0));
    return true;
}

Night ParseNight(const std::vector<uint8_t>& b, uint64_t* stamp = nullptr) {
    if (b.size() == sizeof(kNightStub) && Has(b, 0, kNightStub, sizeof(kNightStub))) { // never toggled on this account
        if (stamp) *stamp = 0;
        return Night::Off;
    }
    const bool on = b.size() == kNightOnLen; // the length alone tells the two layouts apart
    if (!on && b.size() != kNightOffLen) return Night::Bad;
    uint64_t ts = 0, ft = 0;
    size_t at = 0;
    if (!Has(b, at, kNightHead, sizeof(kNightHead))) return Night::Bad;
    at += sizeof(kNightHead);
    if (!Leb(b, at, kStampLen, ts)) return Night::Bad;
    at += kStampLen;
    if (!Has(b, at, kNightMid, sizeof(kNightMid))) return Night::Bad;
    at += sizeof(kNightMid);
    if (b[at++] != (on ? kInnerOn : kInnerOff)) return Night::Bad; // the inner length must agree with the outer one
    if (!Has(b, at, kNightInner, sizeof(kNightInner))) return Night::Bad;
    at += sizeof(kNightInner);
    if (on) {
        if (!Has(b, at, kNightOnFlag, sizeof(kNightOnFlag))) return Night::Bad;
        at += sizeof(kNightOnFlag);
    }
    if (!Has(b, at, kNightTime, sizeof(kNightTime))) return Night::Bad;
    at += sizeof(kNightTime);
    if (!Leb(b, at, kFileTimeLen, ft)) return Night::Bad;
    at += kFileTimeLen;
    if (!Has(b, at, kNightTail, sizeof(kNightTail)) || at + sizeof(kNightTail) != b.size()) return Night::Bad; // nothing may trail the tail
    if (stamp) *stamp = ts;
    return on ? Night::On : Night::Off;
}

bool ReadNight(std::vector<uint8_t>& out) {
    DWORD size = 0, type = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, &type, nullptr, &size) != ERROR_SUCCESS) return false; // size first
    if (size == 0 || size > kNightMaxLen) return false;
    out.resize(size);
    if (RegGetValueW(HKEY_CURRENT_USER, kNightKey, L"Data", RRF_RT_REG_BINARY, &type, out.data(), &size) != ERROR_SUCCESS) return false; // fails if it grew meanwhile
    out.resize(size);
    return true;
}

template <size_t N>
void Append(std::vector<uint8_t>& b, const uint8_t (&p)[N]) { b.insert(b.end(), p, p + N); }

bool BuildNight(bool on, uint64_t prevStamp, std::vector<uint8_t>& b) { // fresh blob in the layout windows writes
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    const uint64_t ft = (uint64_t(now.dwHighDateTime) << 32) | now.dwLowDateTime; // 100 ns ticks since 1601
    constexpr uint64_t kUnixEpoch = 116444736000000000ull, kTicksPerSec = 10000000ull; // 1970 in filetime ticks
    const uint64_t secs = (std::max)((ft - kUnixEpoch) / kTicksPerSec, prevStamp + 1); // must move forward or the shell ignores it
    b.clear();
    Append(b, kNightHead);
    if (!PutLeb(b, secs, kStampLen)) return false;
    Append(b, kNightMid);
    b.push_back(on ? kInnerOn : kInnerOff);
    Append(b, kNightInner);
    if (on) Append(b, kNightOnFlag);
    Append(b, kNightTime);
    if (!PutLeb(b, ft, kFileTimeLen)) return false;
    Append(b, kNightTail);
    return b.size() == (on ? kNightOnLen : kNightOffLen); // a wrong length means a field came out a different width
}

}

// ---- hotspot ----

namespace {

struct HotspotState { // what workers write and the ui thread reads
    std::atomic<bool> available{false}, on{false};
    std::atomic<int> busy{0}; // count, a refresh and a set may overlap
    mutable std::mutex lock; // only the name needs it; the flags are atomic
    std::wstring name;
};

struct BusyDone { // drops the count the caller took, even when the worker throws
    std::atomic<int>& n;
    ~BusyDone() { --n; }
};

struct Mta { // worker apartment; balanced only if init really happened
    bool ok = false;
    Mta() {
        try {
            winrt::init_apartment(winrt::apartment_type::multi_threaded); // .get() on an async op is only allowed off an sta
            ok = true;
        } catch (const winrt::hresult_error&) {
            OutputDebugStringW(L"[toggles] init_apartment failed\n");
        }
    }
    ~Mta() {
        if (ok) winrt::uninit_apartment();
    }
};

WNO::NetworkOperatorTetheringManager TetherManager(bool& capable) { // null when there is no internet profile
    capable = false;
    const auto profile = WNC::NetworkInformation::GetInternetConnectionProfile(); // the connection that would be shared
    if (!profile) return nullptr; // offline: nothing to share
    capable = WNO::NetworkOperatorTetheringManager::GetTetheringCapabilityFromConnectionProfile(profile) ==
              WNO::TetheringCapability::Enabled;
    if (!capable) return nullptr;
    return WNO::NetworkOperatorTetheringManager::CreateFromConnectionProfile(profile);
}

void ReadHotspot(HotspotState& s) { // call on an mta worker
    bool capable = false;
    const auto mgr = TetherManager(capable);
    s.available = capable && mgr;
    if (!mgr) {
        s.on = false;
        return;
    }
    s.on = mgr.TetheringOperationalState() == WNO::TetheringOperationalState::On;
    std::wstring ssid;
    try {
        if (const auto cfg = mgr.GetCurrentAccessPointConfiguration()) ssid = cfg.Ssid().c_str();
    } catch (const winrt::hresult_error&) {
        OutputDebugStringW(L"[toggles] hotspot ssid unreadable\n"); // keep the state we already have
    }
    std::lock_guard g(s.lock);
    s.name = std::move(ssid);
}

void Post(HWND notify, UINT msg, WPARAM ok) { // tells the ui thread to redraw; wparam carries success
    if (notify && msg && !PostMessageW(notify, msg, ok, 0)) OutputDebugStringW(L"[toggles] PostMessage failed\n");
}

}

struct Hotspot::Impl : HotspotState {};

Hotspot::Hotspot() : impl(std::make_shared<Impl>()) {}
Hotspot::~Hotspot() = default;

bool Hotspot::Available() const { return impl->available; }
bool Hotspot::On() const { return impl->on; }
bool Hotspot::Busy() const { return impl->busy > 0; }

std::wstring Hotspot::Name() const {
    std::lock_guard g(impl->lock);
    return impl->name;
}

void Hotspot::Refresh(HWND notify, UINT msg) {
    auto s = impl; // the worker holds its own share: the bar may go first
    ++s->busy; // counted before the thread starts so Busy() is right the moment this returns
    try {
        std::thread([s, notify, msg] {
            BusyDone done{s->busy};
            bool ok = false;
            try {
                Mta mta;
                ReadHotspot(*s);
                ok = true;
            } catch (const winrt::hresult_error& e) {
                s->available = false; // no profile, radio off or service missing all mean "can't offer it"
                OutputDebugStringW((L"[toggles] hotspot refresh: " + std::wstring(e.message().c_str()) + L"\n").c_str());
            }
            Post(notify, msg, ok);
        }).detach();
    } catch (const std::system_error&) { // no thread: give back the count it would have dropped
        --s->busy;
        OutputDebugStringW(L"[toggles] hotspot thread failed\n");
    }
}

void Hotspot::Set(bool on, HWND notify, UINT msg) {
    auto s = impl;
    ++s->busy;
    try {
        std::thread([s, on, notify, msg] {
            BusyDone done{s->busy};
            bool ok = false;
            try {
                Mta mta;
                bool capable = false;
                if (const auto mgr = TetherManager(capable)) {
                    const auto r = on ? mgr.StartTetheringAsync().get() : mgr.StopTetheringAsync().get(); // blocking is fine here, worker
                    ok = r.Status() == WNO::TetheringOperationStatus::Success; // refusals come back as a status, not a throw
                    if (!ok) OutputDebugStringW((L"[toggles] hotspot: " + std::wstring(r.AdditionalErrorMessage().c_str()) + L"\n").c_str());
                }
                ReadHotspot(*s); // report what really happened, not what was asked
            } catch (const winrt::hresult_error& e) {
                OutputDebugStringW((L"[toggles] hotspot set: " + std::wstring(e.message().c_str()) + L"\n").c_str());
            }
            Post(notify, msg, ok);
        }).detach();
    } catch (const std::system_error&) {
        --s->busy;
        OutputDebugStringW(L"[toggles] hotspot thread failed\n");
    }
}

// ---- energy saver ----

bool SaverAvailable() {
    DWORD mode = 0;
    if (SaverMode(mode)) return true; // 24h2+, works on desktops too
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && !(ps.BatteryFlag & BATTERY_FLAG_NO_BATTERY) && ps.BatteryFlag != BATTERY_FLAG_UNKNOWN; // older builds: battery saver needs a battery
}

bool SaverOn() {
    WatchSaver();
    const int st = gSaverStatus.load();
    if (st >= 0) return st != ENERGY_SAVER_OFF; // the flag below stays 0 on ac even when energy saver is on
    SYSTEM_POWER_STATUS ps{};
    return GetSystemPowerStatus(&ps) && ps.SystemStatusFlag == 1; // until the notification lands: right on battery at least
}

bool SetSaver(bool on) {
    DWORD mode = 0;
    const Wnf& w = Ntdll();
    if (SaverMode(mode) && w.publish) {
        const DWORD want = on ? kSaverAlways : kSaverAuto; // auto still engages below the user's battery threshold
        if (mode == want) return true; // already there: don't wake the power service
        if (w.publish(kWnfEnergySaver, nullptr, &want, sizeof(want), nullptr) >= 0) return true;
        OutputDebugStringW(L"[toggles] energy saver publish refused\n");
    }
    ShellAsync(L"ms-settings:batterysaver"); // older builds: battery saver has no user-mode switch
    return false;
}

// ---- night light ----

bool NightLightAvailable() {
    std::vector<uint8_t> b;
    return ReadNight(b) && ParseNight(b) != Night::Bad;
}

bool NightLightOn() {
    std::vector<uint8_t> b;
    return ReadNight(b) && ParseNight(b) == Night::On;
}

bool SetNightLight(bool on) {
    std::vector<uint8_t> cur, next, back;
    uint64_t stamp = 0;
    if (!ReadNight(cur)) return false; // no key: night light was never set up here
    const Night now = ParseNight(cur, &stamp); // the old stamp, so the new one can be later
    if (now == Night::Bad) {
        OutputDebugStringW(L"[toggles] night light blob has an unknown layout, not touching it\n");
        return false;
    }
    if ((now == Night::On) == on) return true;
    if (!BuildNight(on, stamp, next) || ParseNight(next) != (on ? Night::On : Night::Off)) return false; // our own output must parse
    if (RegSetKeyValueW(HKEY_CURRENT_USER, kNightKey, L"Data", REG_BINARY, next.data(), (DWORD)next.size()) != ERROR_SUCCESS) {
        OutputDebugStringW(L"[toggles] night light write failed\n");
        return false;
    }
    return ReadNight(back) && back == next; // read back, the shell may have raced us
}

// ---- launchers ----

bool LiveCaptions() { return SendChord({VK_LWIN, VK_CONTROL, 'L'}); } // no api toggles captions; windows' own shortcut does
bool Project() { return SendChord({VK_LWIN, 'P'}); } // opens windows' own project flyout

bool Accessibility() {
    ShellAsync(L"ms-settings:easeofaccess");
    return true;
}

bool NearbySharing() {
    ShellAsync(L"ms-settings:crossdevice");
    return true;
}

bool FocusSettings() {
    ShellAsync(L"ms-settings:quiethours"); // still the focus page on 24h2+; ms-settings:focus is not registered
    return true;
}

}
