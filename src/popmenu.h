#pragma once
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gfx.h"
#include "theme.h"

// The bar's right-click menus, drawn as island draws its own: the island's black (or the light look's paper, or glass), glyph
// rows, the accent under the hovered one. Pick runs its own loop until something is chosen or the menu is dismissed and returns
// the id, as TrackPopupMenuEx with TPM_RETURNCMD does; its windows exist only while it runs.
namespace popmenu {

struct Item {
    enum Kind : uint8_t { kRow, kSep, kSection }; // a choice, a divider, a small heading over the rows under it
    UINT id = 0; // what Pick returns; a row with a submenu returns nothing itself
    int glyph = 0; // glyphs.h; 0 for none
    std::wstring label, hint; // hint: right-aligned, a shortcut or a note
    bool checked = false, disabled = false, danger = false; // danger: destructive, it reddens
    std::vector<Item> sub; // one level deep
    Kind kind = kRow;
};

inline Item Sep() { return {.kind = Item::kSep}; }
inline Item Section(std::wstring label) { return {.label = std::move(label), .kind = Item::kSection}; }

struct Head { // the card's top: whose menu this is
    std::wstring title, sub;
    IWICBitmapSource* pic = nullptr; // an app's icon or the user's picture, borrowed for the call
    bool round = false; // the picture is a face: cut to a circle
    bool logo = false; // halobar's own icon, from the exe
    int glyph = 0; // no picture: a glyph on an accent dot
};

struct Place { // where the menu goes, in screen pixels, as TrackPopupMenuEx's flags put it
    enum H : uint8_t { Left, Center, Right }; // which part of the menu sits on at.x
    enum V : uint8_t { Below, Above, Auto }; // below at.y, above it, or above in the screen's lower half
    POINT at{};
    H h = Left;
    V v = Auto;
};

UINT Pick(HWND owner, const std::vector<Item>& items, const Place& place, const Theme& t, float scale, const Head* head = nullptr); // 0 when dismissed

}
