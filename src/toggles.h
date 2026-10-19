#pragma once
#include <windows.h>
#include <memory>
#include <string>

// quick settings toggles the windows 11 flyout has and the bar's own quick panel lacks
namespace sys {

class Hotspot { // mobile hotspot through NetworkOperatorTetheringManager; winrt work runs on worker threads
public:
    Hotspot();
    ~Hotspot();
    Hotspot(const Hotspot&) = delete;
    Hotspot& operator=(const Hotspot&) = delete;
    void Refresh(HWND notify, UINT msg); // async: reads capability, state and ssid, posts msg when known
    bool Available() const; // internet profile exists and tethering is enabled on it
    bool On() const;
    bool Busy() const; // a refresh or start/stop is in flight
    void Set(bool on, HWND notify, UINT msg); // start/stop on a worker, posts msg when done (wparam 1 = ok)
    std::wstring Name() const; // hotspot ssid, empty until a refresh found one
private:
    struct Impl;
    std::shared_ptr<Impl> impl; // shared with workers so a late finish never touches a freed object
};

// energy saver (24h2+, battery saver before it)
bool SaverAvailable();
bool SaverOn();
bool SetSaver(bool on); // false if it had to fall back to opening settings

// night light, through the cloudstore blob windows itself watches
bool NightLightAvailable();
bool NightLightOn();
bool SetNightLight(bool on); // false if the blob isn't a layout we know; nothing is written then

// launchers, all non-blocking; true once dispatched
bool LiveCaptions(); // win+ctrl+l
bool Project(); // win+p
bool Accessibility(); // ms-settings:easeofaccess; the quick settings accessibility flyout has no uri or hotkey
bool NearbySharing(); // ms-settings:crossdevice
bool FocusSettings(); // ms-settings:quiethours, which lands on system > focus

}
