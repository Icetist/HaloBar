#pragma once
#include <windows.h>
#include <string>

namespace sys {

struct Status { // what the row's little status cluster shows
    bool online = false; // reaches the internet
    bool wifi = false; // connected over wi-fi (else wired, if online)
    int signal = 0; // wi-fi quality 0..100
    std::wstring ssid;
    bool battery = false; // the pc has one
    int percent = -1;
    bool charging = false, saver = false;

    bool operator==(const Status&) const = default;
};

Status ReadStatus(); // a few cheap calls, fine on a slow timer
void CloseStatus(); // lets go of the wlan handle

}
