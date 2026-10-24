#pragma once
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "tasks.h"

// App icons, read from the shell once per app and kept as pixels; the gpu copy is made per device and dropped with it.
class Icons {
public:
    ~Icons() { Stop(); }
    void Start(HWND notify, UINT msg); // icons are read on their own thread from now on: the shell can take a few hundred ms per app
    void Stop();
    bool Deliver(LPARAM lp); // a finished read posted back as msg; true if it was still wanted
    bool Pending(const std::wstring& key) const; // asked for, still being read
    ID2D1Bitmap* Get(ID2D1DeviceContext* ctx, const TaskApp& app, UINT px); // null until it's read, or if the app has no icon at all
    IWICBitmapSource* Pixels(const std::wstring& key) const { // what's been read for an app, null if nothing yet: for windows with their own targets
        auto it = map.find(key);
        return it == map.end() ? nullptr : it->second.px.Get();
    }
    void Drop(); // the device is going: gpu copies go, pixels stay
    void Keep(const std::vector<TaskApp>& apps); // forget apps that have closed, look again for ones that had no icon yet

private:
    struct Ent {
        ComPtr<IWICBitmapSource> px;
        ComPtr<ID2D1Bitmap> bmp;
        bool tried = false; // looked once, nothing there: don't ask the shell every frame
        bool pending = false; // on the reader's queue
        UINT size = 0; // pixels it was made at: a new screen scale makes it again
    };
    struct Job {
        std::wstring key, aumid, exe;
        std::vector<BYTE> pidl;
        HWND win = nullptr;
        UINT px = 0;
    };
    std::unordered_map<std::wstring, Ent> map;
    std::thread reader;
    std::mutex lock;
    std::condition_variable wake;
    std::deque<Job> jobs;
    bool stopping = false;
    HWND notify = nullptr;
    UINT msg = 0;

public:
    struct Result {
        std::wstring key;
        UINT px = 0;
        ComPtr<IWICBitmapSource> pixels;
    };
};
