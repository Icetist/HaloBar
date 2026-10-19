#include "status.h"

#include <netlistmgr.h>
#include <wlanapi.h>
#include <wrl/client.h>

#pragma comment(lib, "wlanapi.lib")

using Microsoft::WRL::ComPtr;

namespace sys {

namespace {

HANDLE wlan = nullptr; // opened once, on first read
bool wlanTried = false;
ComPtr<INetworkListManager> nlm; // kept: it lives in the network list service, and making one is a round trip there every poll

void ReadWifi(Status& s) {
    if (!wlanTried) {
        wlanTried = true;
        DWORD ver = 0;
        if (WlanOpenHandle(2, nullptr, &ver, &wlan) != ERROR_SUCCESS) wlan = nullptr; // no wlan service: wired only
    }
    if (!wlan) return;
    PWLAN_INTERFACE_INFO_LIST list = nullptr;
    if (WlanEnumInterfaces(wlan, nullptr, &list) != ERROR_SUCCESS || !list) return;
    for (DWORD i = 0; i < list->dwNumberOfItems && !s.wifi; ++i) {
        const auto& in = list->InterfaceInfo[i];
        if (in.isState != wlan_interface_state_connected) continue;
        PWLAN_CONNECTION_ATTRIBUTES conn = nullptr;
        DWORD size = 0;
        if (WlanQueryInterface(wlan, &in.InterfaceGuid, wlan_intf_opcode_current_connection, nullptr, &size, (PVOID*)&conn, nullptr) != ERROR_SUCCESS || !conn)
            continue;
        const DOT11_SSID& id = conn->wlanAssociationAttributes.dot11Ssid;
        int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)id.ucSSID, (int)id.uSSIDLength, nullptr, 0);
        s.ssid.assign(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, (const char*)id.ucSSID, (int)id.uSSIDLength, s.ssid.data(), n);
        s.signal = (int)conn->wlanAssociationAttributes.wlanSignalQuality;
        s.wifi = true;
        WlanFreeMemory(conn);
    }
    WlanFreeMemory(list);
}

}

Status ReadStatus() {
    Status s;
    NLM_CONNECTIVITY c = NLM_CONNECTIVITY_DISCONNECTED;
    if (!nlm && FAILED(CoCreateInstance(CLSID_NetworkListManager, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&nlm)))) nlm.Reset();
    if (nlm && FAILED(nlm->GetConnectivity(&c))) nlm.Reset(), c = NLM_CONNECTIVITY_DISCONNECTED; // the service restarted: a fresh one next poll
    s.online = (c & (NLM_CONNECTIVITY_IPV4_INTERNET | NLM_CONNECTIVITY_IPV6_INTERNET)) != 0;
    ReadWifi(s);
    SYSTEM_POWER_STATUS p{};
    if (GetSystemPowerStatus(&p)) {
        s.battery = p.BatteryFlag != 128 && p.BatteryFlag != 255; // 128: no battery, 255: unknown
        s.percent = p.BatteryLifePercent <= 100 ? p.BatteryLifePercent : -1;
        s.charging = p.ACLineStatus == 1;
        s.saver = p.SystemStatusFlag == 1;
    }
    return s;
}

void CloseStatus() {
    if (wlan) WlanCloseHandle(wlan, nullptr);
    wlan = nullptr;
    wlanTried = false;
    nlm.Reset(); // before com goes away
}

}
