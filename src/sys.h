#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

// Astraeus Island's system pieces the bar shares: sound, output devices, brightness, power. Same files, same behaviour.
namespace sys {

namespace detail { // winhttp get, as island's: one kept-alive session, cancellable from another thread
bool HttpGet(const wchar_t* url, std::string& body, std::atomic<bool>& stop, std::atomic<void*>& slot, size_t maxBytes = 256 * 1024);
void HttpCancel(std::atomic<bool>& stop, std::atomic<void*>& slot);
}

class AudioDev {
public:
    static AudioDev Spk();  // default render endpoint
    static AudioDev Micro();  // default capture endpoint
    AudioDev(AudioDev&&) noexcept;
    AudioDev& operator=(AudioDev&&) noexcept;
    ~AudioDev();
    bool Refresh();  // rebind if default device changed
    bool Ready() const;
    float Level() const;  // scalar 0..1
    void SetLevel(float level);
    bool Muted() const;
    void SetMuted(bool muted);
private:
    explicit AudioDev(int flow);  // EDataFlow as int, avoids mmdeviceapi include
    struct Impl;
    std::unique_ptr<Impl> impl;
};

struct OutDev {
    std::wstring id;  // endpoint id string
    std::wstring name;
    bool headphones = false;
};

std::vector<OutDev> ListOuts(std::wstring& defaultId);
bool SetOut(const std::wstring& id);

bool IsPortable();

class Meter { // peak level of the default output, what the row's little bars bounce to
public:
    Meter();
    ~Meter();
    Meter(const Meter&) = delete;
    Meter& operator=(const Meter&) = delete;
    bool Refresh();
    bool Read(float& left, float& right);  // peak per channel 0..1
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

class Bright {  // wmi brightness on worker thread
public:
    Bright();
    ~Bright();
    Bright(const Bright&) = delete;
    Bright& operator=(const Bright&) = delete;
    void ReadAsync(HWND notify, UINT msg);  // posts msg with percent in wparam, -1 if unknown
    void Set(int percent);  // non-blocking, latest value wins
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

namespace power {
void Lock();
void Sleep();
void Restart();
void Shutdown();
void SignOut();
}

class Updater { // checks halobar's github releases, swaps in a newer exe
public:
    Updater();
    ~Updater();
    Updater(const Updater&) = delete;
    Updater& operator=(const Updater&) = delete;
    enum Result : WPARAM { kInstalled, kCurrent, kFailed, kDownloading }; // wparam of msg; downloading only for manual checks
    void Check(HWND notify, UINT msg, bool manual = false); // background; posts kInstalled once the new exe is in place, a manual check also posts the other outcomes
    std::wstring Applied() const; // tag that was installed, or being fetched
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
bool FinishUpdate(); // at launch: clean up, true right after an update

void SetEco(bool on); // efficiency mode + low memory priority while idle
void SetAwake(bool on);  // keep display awake
void OpenSnip();

struct Stats { // astraeus island's: cpu, memory and gpu load, sampled on a timer
    double cpu = 0;  // 0..1
    double mem = 0;  // used fraction 0..1
    double memUsedGb = 0, memTotalGb = 0;
    double gpu = -1;  // -1 = no gpu counter
    int threads = 0;  // logical cpus
};

class StatSrc {  // pimpl keeps pdh headers out of here
public:
    StatSrc();
    ~StatSrc();
    StatSrc(const StatSrc&) = delete;
    StatSrc& operator=(const StatSrc&) = delete;
    Stats Sample();  // delta since last call, call on a timer
private:
    struct Impl;
    std::unique_ptr<Impl> impl;  // null if init failed
};

struct WxHour {
    int hour = 0;  // local hour of day
    double tempC = 0;
    int code = 0;
    bool day = true;
};

struct WxDay {
    int y = 0, m = 0, d = 0;
    double hiC = 0, loC = 0;
    int code = 0;
};

struct Wx {
    bool valid = false;
    double tempC = 0, highC = 0, lowC = 0;
    int code = 0;  // wmo weather code
    bool isDay = true;
    std::wstring place;
    WxHour hours[8];  // the next hours, from the one after now
    int nHours = 0;
    WxDay days[5];  // the days after today
    int nDays = 0;
};

class WxSvc { // astraeus island's forecast: open-meteo, located once from the ip, cached half an hour
public:
    WxSvc();
    ~WxSvc();
    WxSvc(const WxSvc&) = delete;
    WxSvc& operator=(const WxSvc&) = delete;
    void Refresh(HWND notify, UINT msg);  // no-op while cached or busy
    Wx Get() const;  // thread-safe snapshot
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

std::wstring ReadText(const wchar_t* name); // the bar's own text setting, else island's: the weather's place is shared with it
void SaveText(const wchar_t* name, const std::wstring& value); // the bar's own key only
void OpenUri(const wchar_t* uri);

}
