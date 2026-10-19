#pragma once
#include <windows.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace sys {

struct MixApp {
    std::wstring key; // stable per app: its exe path, or "system" for system sounds
    std::wstring name; // what people call it ("Spotify", "System sounds")
    float volume = 1; // 0..1, the app's own level under the master volume
    bool muted = false;
    bool playing = false; // making sound right now
    std::shared_ptr<const std::vector<uint32_t>> icon; // 32x32 premultiplied bgra, null if none
};

// Per-app volume (ported from Astraeus Island), from Windows' audio sessions on the default output. Holds nothing until Open, frees everything on Close.
class Mixer {
public:
    Mixer();
    ~Mixer();
    Mixer(const Mixer&) = delete;
    Mixer& operator=(const Mixer&) = delete;
    bool Open();
    void Close();
    bool IsOpen() const;
    void Refresh(); // re-reads apps and levels; cheap, call about once a second while shown
    const std::vector<MixApp>& Apps() const;
    std::wstring Device() const; // the output it mixes for, "Speakers (Realtek(R) Audio)"
    void SetVolume(const std::wstring& key, float v); // every session of that app
    void SetMuted(const std::wstring& key, bool muted);
    bool HasApp(const std::wstring& exePathLower) const; // a session for that exe (the lowercased path, same key), as of the last refresh
    float VolumeOf(const std::wstring& exePathLower) const; // its level 0..1, -1 if none or closed

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
