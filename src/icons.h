#pragma once
#include <string>
#include <unordered_map>
#include <vector>

#include "gfx.h"
#include "tasks.h"

// App icons, read from the shell once per app and kept as pixels; the gpu copy is made per device and dropped with it.
class Icons {
public:
    ID2D1Bitmap* Get(ID2D1DeviceContext* ctx, const TaskApp& app, UINT px); // null if the app has no icon at all
    void Drop(); // the device is going: gpu copies go, pixels stay
    void Keep(const std::vector<TaskApp>& apps); // forget apps that have closed, look again for ones that had no icon yet

private:
    struct Ent {
        ComPtr<IWICBitmapSource> px;
        ComPtr<ID2D1Bitmap> bmp;
        bool tried = false; // looked once, nothing there: don't ask the shell every frame
        UINT size = 0; // pixels it was made at: a new screen scale makes it again
    };
    std::unordered_map<std::wstring, Ent> map;
};
