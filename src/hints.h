#pragma once
// a switch in settings, hovered a moment: a small see-through pill by the mouse sketches what it does. astraeus has the same, for its own switches
#include "gfx.h"
#include "theme.h"
#include "ui.h"

namespace hints {

enum Kind {
    kNone,
    kAppsTheme, kWinKeys, kAdaptive, kTrayApps, kHoverPlay, kArtBg, kLauncher, kPills, kDate, k24h, kSplit, kSong, kDockLyrics, // the dock
    kClipClean, kClipWipe, kClipTitles, kClipPause, kClipPass, kClipAdult, kClipPaste, kClipPlain, // the clipboard
    kA11yFollow, kA11yKeys, kA11yReader, kTourNext,
};

void Want(Ui& ui, int kind, bool hover); // a control says whether it's under the mouse this frame
int ForId(const char* id); // a switch's id to what it sketches, kNone when it has no picture
void Draw(Ui& ui, const Theme& t, const D2D1_RECT_F& bounds); // once, last, over the open panel

}
