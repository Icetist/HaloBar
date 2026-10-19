#pragma once
#include <windows.h>
#include <memory>

namespace sys {

// Wi-Fi and Bluetooth radios, on and off, as island's: winrt keeps them current and posts msg when one flips.
class Radios {
public:
    Radios();
    ~Radios();
    Radios(const Radios&) = delete;
    Radios& operator=(const Radios&) = delete;
    void Start(HWND notify, UINT msg);
    bool HasWifi() const;
    bool HasBt() const;
    bool WifiOn() const;
    bool BtOn() const;
    void SetWifi(bool on);
    void SetBt(bool on);
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
