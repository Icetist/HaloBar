// Island's radios, unchanged in behaviour: winrt's Radio api, on its own threads, posting to the bar on a real flip.
#include "radios.h"

#include <winrt/Windows.Devices.Radios.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>

#include <atomic>
#include <mutex>

namespace sys {

namespace {

namespace WR = winrt::Windows::Devices::Radios;

void Log(const wchar_t* what) { OutputDebugStringW((std::wstring(L"[radios] ") + what + L"\n").c_str()); } // debugger only, nothing on disk

struct RadioState {
    std::atomic<HWND> notify{nullptr}; // the bar's window, told on every flip
    std::atomic<UINT> msg{0};
    std::atomic<bool> closed{false}, started{false}; // closed outlives the bar's Radios: late callbacks check it
    std::atomic<bool> hasWifi{false}, hasBt{false}, wifiOn{false}, btOn{false}; // atomics so getters skip the lock

    std::mutex mu; // guards radios + revokers
    WR::Radio wifi{nullptr}, bt{nullptr};
    WR::Radio::StateChanged_revoker wifiRev, btRev;
};
using RadioP = std::shared_ptr<RadioState>; // shared, so a coroutine still running after close has valid state

void Ping(const RadioState& s) {
    if (HWND h = s.notify.load(); h && !s.closed.load()) PostMessageW(h, s.msg.load(), 0, 0); // posted: the bar reads state on its own thread
}

void OnRadio(const std::weak_ptr<RadioState>& w, const WR::Radio& sender) noexcept { // runs on winrt threadpool
    try {
        auto s = w.lock(); // weak: a subscription must not keep the state alive
        if (!s || s->closed.load() || !sender) return;
        const bool on = sender.State() == WR::RadioState::On;
        const auto kind = sender.Kind();
        std::atomic<bool>* slot = kind == WR::RadioKind::WiFi ? &s->wifiOn : kind == WR::RadioKind::Bluetooth ? &s->btOn : nullptr;
        if (slot && slot->exchange(on) != on) Ping(*s); // ping only on real flips
    } catch (...) {
        Log(L"radio changed failed");
    }
}

winrt::fire_and_forget StartRadios(RadioP s) {
    try {
        co_await winrt::resume_background(); // off the ui thread before any blocking winrt call
        const auto access = co_await WR::Radio::RequestAccessAsync(); // needed for SetStateAsync
        if (access != WR::RadioAccessStatus::Allowed) Log(L"access not allowed; states are read-only");
        const auto radios = co_await WR::Radio::GetRadiosAsync(); // empty on a desktop with neither radio
        WR::Radio wifi{nullptr}, bt{nullptr};
        for (const auto& r : radios) {
            const auto kind = r.Kind();
            if (kind == WR::RadioKind::WiFi && !wifi) wifi = r; // first of each kind wins
            else if (kind == WR::RadioKind::Bluetooth && !bt) bt = r;
        }
        std::weak_ptr<RadioState> w = s;
        WR::Radio::StateChanged_revoker wr, br;
        if (wifi) wr = wifi.StateChanged(winrt::auto_revoke, [w](const WR::Radio& r, auto&&) { OnRadio(w, r); });
        if (bt) br = bt.StateChanged(winrt::auto_revoke, [w](const WR::Radio& r, auto&&) { OnRadio(w, r); });
        const bool wifiOn = wifi && wifi.State() == WR::RadioState::On; // read after subscribing so no flip is missed
        const bool btOn = bt && bt.State() == WR::RadioState::On;
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return; // revokers unhook on return
            s->wifi = wifi;
            s->bt = bt;
            s->wifiRev = std::move(wr);
            s->btRev = std::move(br);
            s->hasWifi.store(static_cast<bool>(wifi)); // the quick panel hides a toggle with no radio behind it
            s->hasBt.store(static_cast<bool>(bt));
            s->wifiOn.store(wifiOn);
            s->btOn.store(btOn);
        }
        Ping(*s); // first paint with real state
    } catch (...) {
        Log(L"start failed");
    }
}

winrt::fire_and_forget SetRadio(RadioP s, bool wifiRadio, bool on) {
    try {
        co_await winrt::resume_background();
        WR::Radio r{nullptr};
        {
            std::lock_guard lock(s->mu);
            if (s->closed.load()) co_return;
            r = wifiRadio ? s->wifi : s->bt; // copy ref, await unlocked
        }
        if (!r) co_return; // not started yet, or no such radio
        const auto res = co_await r.SetStateAsync(on ? WR::RadioState::On : WR::RadioState::Off); // StateChanged reports the result
        if (res != WR::RadioAccessStatus::Allowed) Log(L"SetStateAsync was not allowed");
    } catch (...) {
        Log(L"set failed");
    }
}

void CloseRadios(const RadioP& s) noexcept {
    try {
        WR::Radio::StateChanged_revoker a, b; // released after lock scope
        WR::Radio wifi{nullptr}, bt{nullptr}; // released after the lock too, a final release can call back
        {
            std::lock_guard lock(s->mu);
            s->closed.store(true); // stops late callbacks from pinging
            a = std::move(s->wifiRev);
            b = std::move(s->btRev);
            wifi = std::move(s->wifi);
            bt = std::move(s->bt);
        }
    } catch (...) {
        Log(L"close failed");
    }
}

}

struct Radios::Impl {
    RadioP s; // shared with async start/set
};

Radios::Radios() {
    try {
        impl = std::make_unique<Impl>(); // a pimpl keeps the winrt headers out of everything that includes radios.h
        impl->s = std::make_shared<RadioState>();
    } catch (...) {
        impl.reset(); // degrade to no radios
    }
}

Radios::~Radios() {
    if (impl && impl->s) CloseRadios(impl->s); // unhook before window dies
}

void Radios::Start(HWND notify, UINT msg) {
    if (!impl || !impl->s) return;
    auto& s = impl->s;
    s->notify.store(notify); // updated even on a repeat start: the bar's window may be new
    s->msg.store(msg);
    if (s->started.exchange(true)) return; // idempotent
    try {
        StartRadios(s); // returns at its first await; states arrive later by message
    } catch (...) {
        Log(L"Start failed");
    }
}

bool Radios::HasWifi() const { return impl && impl->s && impl->s->hasWifi.load(); }
bool Radios::HasBt() const { return impl && impl->s && impl->s->hasBt.load(); }
bool Radios::WifiOn() const { return impl && impl->s && impl->s->wifiOn.load(); }
bool Radios::BtOn() const { return impl && impl->s && impl->s->btOn.load(); }

void Radios::SetWifi(bool on) {
    if (impl && impl->s) try { SetRadio(impl->s, true, on); } catch (...) {} // fire and forget: the toggle follows StateChanged
}
void Radios::SetBt(bool on) {
    if (impl && impl->s) try { SetRadio(impl->s, false, on); } catch (...) {}
}

}
