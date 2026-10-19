#include <windows.h>
#include <winhttp.h>

#include "sys.h"

#include <atomic>
#include <cstdio>
#include <string>

#pragma comment(lib, "winhttp.lib")

// HTTP GET over one shared WinHTTP session, as Island's, for calendars: cancellable from another thread.
namespace sys {

namespace detail {

constexpr int kResolveMs = 10000;  // dns timeout
constexpr int kConnMs = 10000;
constexpr int kSendMs = 10000;
constexpr int kRecvMs = 15000;  // longer, slow apis stream late

namespace {

void LogHttp(const wchar_t* what, DWORD code) {
    wchar_t buf[128];
    swprintf_s(buf, L"[sys] HTTP %ls failed: %lu\n", what, code);
    OutputDebugStringW(buf);
}

struct HttpH {  // raii winhttp handle
    HINTERNET h = nullptr;
    HttpH() = default;
    explicit HttpH(HINTERNET handle) : h(handle) {}
    HttpH(const HttpH&) = delete;  // no copy, would double close
    HttpH& operator=(const HttpH&) = delete;
    ~HttpH() {
        if (h) WinHttpCloseHandle(h);
    }
};

class LiveReq {  // publishes request handle so another thread can cancel
public:
    LiveReq(HINTERNET req, std::atomic<bool>& stop, std::atomic<void*>& slot)
        : stop_(stop), slot_(slot) {
        slot_.store(req);  // visible to HttpCancel from now on
    }
    LiveReq(const LiveReq&) = delete;
    LiveReq& operator=(const LiveReq&) = delete;
    ~LiveReq() {
        if (void* h = slot_.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));  // exchange: only one side closes
    }
    bool Cancelled() const { return stop_.load(); }

private:
    std::atomic<bool>& stop_;
    std::atomic<void*>& slot_;
};

bool ReadBody(HINTERNET req, std::string& body, const LiveReq& active, size_t maxBytes) {
    char chunk[8192];
    for (;;) {
        if (active.Cancelled()) return false;  // check between chunks
        DWORD read = 0;
        if (!WinHttpReadData(req, chunk, sizeof(chunk), &read)) return false;  // fails if handle closed by cancel
        if (read == 0) return true;  // zero bytes = end of body
        if (body.size() + read > maxBytes) return false;  // too big, treat as error
        body.append(chunk, read);
    }
}

}

void HttpCancel(std::atomic<bool>& stop, std::atomic<void*>& slot) {
    stop.store(true);  // set flag first so worker bails after unblock
    if (void* h = slot.exchange(nullptr)) WinHttpCloseHandle(static_cast<HINTERNET>(h));  // cancel closes handle to unblock read
}

bool HttpGet(const wchar_t* url, std::string& body, std::atomic<bool>& stop, std::atomic<void*>& slot, size_t maxBytes) {
    body.clear();
    if (!url || stop.load()) return false;

    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    URL_COMPONENTS uc{};
    uc.dwStructSize = sizeof(uc);
    uc.lpszHostName = host;  // caller buffers, crack copies into them
    uc.dwHostNameLength = ARRAYSIZE(host);
    uc.lpszUrlPath = path;
    uc.dwUrlPathLength = ARRAYSIZE(path);
    wchar_t extra[2048] = {};
    uc.lpszExtraInfo = extra;  // query string lands here, not in path
    uc.dwExtraInfoLength = ARRAYSIZE(extra);
    if (!WinHttpCrackUrl(url, 0, 0, &uc)) {
        OutputDebugStringW(L"[sys] WinHttpCrackUrl failed\n");
        return false;
    }
    std::wstring object = std::wstring(path) + extra;  // rejoin path + query for request
    const bool secure = uc.nScheme == INTERNET_SCHEME_HTTPS;

    static const HINTERNET session = [] {  // one session for the app's life: connections to a host are kept alive and reused, no new handshake per request
        HINTERNET s = WinHttpOpen(L"HaloBar/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);  // honors system/pac proxy
        if (s) WinHttpSetTimeouts(s, kResolveMs, kConnMs, kSendMs, kRecvMs);  // ms; defaults are too long
        return s;
    }();
    if (!session) {
        OutputDebugStringW(L"[sys] WinHttpOpen failed\n");
        return false;
    }
    HttpH connect(WinHttpConnect(session, host, uc.nPort, 0));  // no network io yet, just binds host
    if (!connect.h) return false;

    HINTERNET req = WinHttpOpenRequest(connect.h, L"GET", object.c_str(), nullptr, WINHTTP_NO_REFERER,
                                       WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
    if (!req) return false;
    LiveReq active(req, stop, slot);  // owns req; closes it on scope exit
    if (active.Cancelled()) return false;  // cancel may have raced before slot was set

    if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) ||
        active.Cancelled() || !WinHttpReceiveResponse(req, nullptr) || active.Cancelled()) {  // recheck after each blocking call
        if (!active.Cancelled()) LogHttp(L"request", GetLastError());  // cancel errors are expected, skip log
        return false;
    }
    DWORD status = 0;
    DWORD size = sizeof(status);
    if (!WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,  // status as dword, not text
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) ||
        status != 200) {
        LogHttp(L"status", status);
        return false;
    }
    return ReadBody(req, body, active, maxBytes) && !body.empty();  // empty body counts as failure
}

}

}
