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
void OpenUri(const wchar_t* uri);

}
