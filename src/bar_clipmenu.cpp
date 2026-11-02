// The clipboard panel's actions: an item's right-click menu, notes, clearing by time, and dragging an item out.
#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <ctime>

#include "clips.h"
#include "glyphs.h"
#include "popmenu.h"

namespace {

constexpr UINT kSaveMs = 1500; // as bar_clip.cpp: one index write after a burst
constexpr int SysEyeOff = glyph::kSys | 0xED1A, SysColor = glyph::kSys | 0xE790;

}

void Bar::ClipNoteDone(bool keep) {
    clip::Store& st = clip::History();
    for (size_t i = 0; keep && i < st.items.size(); ++i)
        if (st.items[i].id == clipNoteId) st.SetMemo(i, clipNoteText), SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    clipNoteId = 0, clipNoteText.clear();
    Redraw();
}

void Bar::ClipClearSince(int64_t secs) { // what's shown, copied within secs (0: all of it); pins and notes stay
    ClipFilter();
    clip::Store& st = clip::History();
    int64_t cut = secs ? (int64_t)_time64(nullptr) - secs : INT64_MIN;
    std::vector<uint64_t> ids;
    for (int at : clipShown)
        if (at >= 0 && at < (int)st.items.size() && st.items[(size_t)at].at >= cut) ids.push_back(st.items[(size_t)at].id);
    if (st.Drop(ids)) SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr), Redraw();
}

void Bar::ClipDragOut() { // after the frame that saw the drag: ole runs its own loop until the drop
    clip::Store& st = clip::History();
    bool held = GetAsyncKeyState(GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON) < 0; // let go already: no drag, or it would drop wherever the mouse is
    uint64_t id = clipDragId;
    clipDragId = 0;
    for (size_t i = 0; held && i < st.items.size(); ++i)
        if (st.items[i].id == id) {
            const clip::Item it = st.items[i]; // a copy: the history may change during the drag
            clip::DragOut(hwnd, st, it);
            break;
        }
    if (wgOpen && wgKind == kWgClip) Redraw();
}

void Bar::ClipMenu() { // an item's right click: every way to put it back, pin, open, never keep its source, forget
    clip::Store& st = clip::History();
    if (clipMenuAt < 0 || clipMenuAt >= (int)st.items.size()) return;
    const clip::Item it = st.items[(size_t)clipMenuAt]; // a copy: the list may change under the menu
    enum : UINT { kPaste = 1, kPlain, kCopy, kPin, kOpen, kShow, kNoApp, kNoSite, kForget, kNote, kNoteOff, kClean, kPath, kSave, kClearHour, kClearDay, kClearAll, kFmt = 100 };
    constexpr int64_t kHour = 3600, kDay = 86400;
    bool files = it.kind == clip::kFiles || (it.kind == clip::kMedia && !it.image);
    std::vector<popmenu::Item> items;
    items.push_back({.id = kPaste, .glyph = glyph::SysPaste, .label = L"Paste", .hint = L"Enter"});
    if (!it.image) items.push_back({.id = kPlain, .glyph = glyph::Doc, .label = files ? L"Paste paths as plain text" : L"Paste as plain text", .hint = L"Shift+Enter"});
    items.push_back({.id = kCopy, .glyph = glyph::Copy, .label = L"Copy only"});
    items.push_back({.id = kPin, .glyph = glyph::Pin, .label = it.pinned ? L"Unpin" : L"Pin to the top", .hint = L"Ctrl+P"});
    items.push_back({.id = kNote, .glyph = glyph::Edit, .label = it.memo.empty() ? L"Add a note" : L"Edit note", .hint = L"Kept, never cleared"});
    if (!it.memo.empty()) items.push_back({.id = kNoteOff, .glyph = glyph::Edit, .label = L"Remove note"});
    std::vector<clip::Fmt> fmts;
    if (it.kind == clip::kColor) {
        popmenu::Item as{.glyph = SysColor, .label = L"Copy as"};
        fmts = clip::Formats(it.argb);
        for (size_t i = 0; i < fmts.size(); ++i) as.sub.push_back({.id = kFmt + (UINT)i, .label = fmts[i].name, .hint = fmts[i].text});
        items.push_back(std::move(as));
    }
    bool tracked = false;
    std::wstring clean = it.kind == clip::kLink ? clip::CleanLink(it.head, tracked) : std::wstring();
    if (it.kind == clip::kLink) {
        items.push_back({.id = kOpen, .glyph = glyph::OpenNew, .label = L"Open link"});
        items.push_back({.id = kClean, .glyph = glyph::Link, .label = L"Copy without tracking", .hint = tracked ? L"" : L"Already clean", .disabled = !tracked});
    }
    if (files) {
        items.push_back({.id = kShow, .glyph = glyph::Folder, .label = L"Show in folder"});
        items.push_back({.id = kPath, .glyph = glyph::Copy, .label = it.count > 1 ? L"Copy paths" : L"Copy path"});
    }
    if (it.image) items.push_back({.id = kSave, .glyph = glyph::SysDownload, .label = L"Save to Downloads"});
    std::wstring site = it.kind == clip::kLink ? clip::Host(it.head) : clip::Host(it.src);
    items.push_back(popmenu::Sep());
    popmenu::Item clr{.glyph = glyph::History, .label = L"Clear"};
    std::wstring scope = !clipQuery.empty() ? L"These results" : clipTab == clip::kTabAll ? L"All kinds" : std::wstring(clip::TabName(clipTab));
    clr.sub.push_back(popmenu::Section(scope + L"  ·  pins and notes stay"));
    clr.sub.push_back({.id = kClearHour, .label = L"Last hour"});
    clr.sub.push_back({.id = kClearDay, .label = L"Last 24 hours"});
    clr.sub.push_back({.id = kClearAll, .label = L"Everything", .danger = true});
    items.push_back(std::move(clr));
    if (!it.app.empty()) items.push_back({.id = kNoApp, .glyph = SysEyeOff, .label = L"Never keep from " + it.app});
    if (!site.empty()) items.push_back({.id = kNoSite, .glyph = SysEyeOff, .label = L"Never keep from " + site});
    items.push_back({.id = kForget, .glyph = glyph::Delete, .label = L"Forget", .hint = L"Del", .danger = true});
    POINT pt;
    GetCursorPos(&pt);
    clipMenuUp = true;
    UINT pick = popmenu::Pick(hwnd, items, {.at = pt, .h = popmenu::Place::Left, .v = popmenu::Place::Auto}, shellNow, scale);
    clipMenuUp = false;
    int at = -1; // found again by id: the list may have moved
    for (size_t i = 0; i < st.items.size(); ++i)
        if (st.items[i].id == it.id) at = (int)i;
    if (wgOpen && wgKind == kWgClip) SetForegroundWindow(hwnd); // the keyboard back on the panel
    if (at < 0 || !pick) return;
    clip::Options& o = st.opt;
    switch (pick) {
    case kPaste: ClipPick(at, false); return;
    case kPlain: ClipPick(at, true); return;
    case kCopy: st.Put(hwnd, it, false), st.Touch((size_t)at); break;
    case kPin: ClipPin(at); break;
    case kOpen: ShellExecuteW(nullptr, L"open", (it.head.rfind(L"www.", 0) == 0 ? L"https://" + it.head : it.head).c_str(), nullptr, nullptr, SW_SHOWNORMAL); break;
    case kShow:
        if (PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(it.head.substr(0, it.head.find(L'\n')).c_str())) SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0), ILFree(pidl);
        break;
    case kNoApp:
        if (std::find(o.noApps.begin(), o.noApps.end(), it.app) == o.noApps.end()) o.noApps.push_back(it.app);
        st.SaveOptions();
        break;
    case kNoSite:
        if (std::find(o.noSites.begin(), o.noSites.end(), site) == o.noSites.end()) o.noSites.push_back(site);
        st.SaveOptions();
        break;
    case kForget: ClipDrop(at); break;
    case kNote: clipNoteId = it.id, clipNoteText = it.memo, clipPeek = false; break; // typed into the search row, see ClipChar
    case kNoteOff: st.SetMemo((size_t)at, L""); break;
    case kClean: {
        clip::Item as = it;
        as.head = clean, as.more = as.html = as.rtf = false;
        st.Put(hwnd, as, true);
        break;
    }
    case kPath: {
        clip::Item as = it;
        as.kind = clip::kText, as.head.clear();
        for (const wchar_t ch : st.Full(it)) as.head += ch == L'\n' ? std::wstring(L"\r\n") : std::wstring(1, ch); // one path a line, as editors expect
        as.more = as.html = as.rtf = false;
        st.Put(hwnd, as, true);
        break;
    }
    case kSave: {
        std::wstring path;
        if (!st.Export(it, path)) MessageBeep(MB_ICONWARNING); // still being written, or downloads is out of reach
        break;
    }
    case kClearHour: ClipClearSince(kHour); break;
    case kClearDay: ClipClearSince(kDay); break;
    case kClearAll: ClipClearSince(0); break;
    default:
        if (pick >= kFmt && pick - kFmt < fmts.size()) { // the colour, written another way, onto the clipboard
            clip::Item as = it;
            as.head = fmts[pick - kFmt].text, as.more = as.html = as.rtf = false;
            st.Put(hwnd, as, true);
        }
        break;
    }
    SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    Redraw();
}
