#pragma once
// The dock's widget catalogue: what each one is called, says about itself, where the gallery files it, and the sizes it comes in.

namespace wg {

enum Cat { kCatAll, kCatMedia, kCatWork, kCatSystem, kCatTools, kCatCount };
inline constexpr int kKinds = 28; // matches Bar::kWgCount

struct Meta {
    const wchar_t* name;
    const wchar_t* about; // one line under its name in the gallery
    int cat;
    int glyph;
    int sizes; // bits: 1 small, 2 medium, 4 large
    int defSize; // what it's added at
};

const Meta& Info(int kind);
const wchar_t* CatName(int cat);

}
