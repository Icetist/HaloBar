#pragma once
// Settings' building blocks, shared with the panels that carry options of their own (the clipboard's).
#include "ui.h"

namespace setui {

constexpr float kNoteLine = 14; // a quiet note's line

D2D1_RECT_F Box(Canvas& c, const Theme& t, const D2D1_RECT_F& r, const wchar_t* title); // island's card shell; returns its inner area
int Segmented(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& r, const wchar_t* const* labels, int n, int cur); // the picked one, -1 none
void Toggle(Ui& ui, const Theme& t, const char* id, const D2D1_RECT_F& row, const wchar_t* label, bool on, bool& flip); // flip set when clicked
void FreeWheel(); // the custom look's wheel bitmap and the device it was made on
void Note(Canvas& c, const Theme& t, const D2D1_RECT_F& in, float y, const wchar_t* text);

}
