#pragma once
#include <windows.h>
#include <dwmapi.h>

#include <memory>
#include <string>
#include <vector>

#include "gfx.h"
#include "theme.h"

// The card that rises over an app in the row while it's hovered: a live thumbnail of each of its windows, as the
// windows taskbar shows. Click one to bring it forward, its x or a middle click to close it. A real top-level
// window, since dwm draws thumbnails only into those; built on show, freed on hide.
class Preview {
public:
    struct Win {
        HWND hwnd;
        std::wstring title;
    };
    Preview() = default;
    ~Preview();
    Preview(const Preview&) = delete;
    Preview& operator=(const Preview&) = delete;

    enum Media { kHasMedia = 1, kPlaying = 2, kCanPrev = 4, kCanNext = 8 }; // the hovered app is what's playing: transport buttons under its windows
    void Show(HWND owner, const std::wstring& key, const std::vector<Win>& wins, POINT anchor, float scale, const Theme& t, int media = 0); // anchor: bottom centre, screen px
    void Hide();
    bool Shown() const { return hwnd != nullptr; }
    bool Hovered() const { return hover >= -1 && inside; } // the mouse is on the card
    const std::wstring& Key() const { return key; }

    static constexpr UINT kLeft = WM_APP + 40; // posted to the owner when the mouse leaves the card
    static constexpr UINT kDone = WM_APP + 41; // posted when a click on the card has done its job
    static constexpr UINT kMediaCmd = WM_APP + 42; // wparam 0 previous, 1 play or pause, 2 next

private:
    static LRESULT CALLBACK Proc(HWND, UINT, WPARAM, LPARAM);
    LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
    void Layout();
    void Paint();
    int HitTile(float x, float y, bool* onClose) const;
    int HitMedia(float x, float y) const; // 0..2 a transport button, -1 none
    D2D1_POINT_2F MediaAt(int i) const;
    void Thumbs();
    void Unthumb();

    struct Tile {
        Win win;
        D2D1_RECT_F box{}; // the whole tile, dips
        D2D1_RECT_F shot{}; // where the thumbnail sits
        HTHUMBNAIL thumb = nullptr;
    };
    HWND hwnd = nullptr, owner = nullptr;
    std::wstring key;
    std::vector<Tile> tiles;
    Theme t;
    float scale = 1, W = 0, H = 0;
    POINT anchor{};
    int hover = -2; // tile under the mouse, -1 the card itself, -2 nowhere
    int media = 0, hoverMedia = -1;
    bool hoverClose = false, inside = false, tracking = false;
    ComPtr<ID2D1HwndRenderTarget> rt;
    std::unique_ptr<Canvas> canvas;
};
