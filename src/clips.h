#pragma once
// The clipboard's history: what was copied, sorted by kind, kept under %localappdata%\HaloBar\clips, and put back on demand.
// Only small heads of text live in memory; full texts, rich formats and pictures wait on disk until pasted or previewed.
#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

#include "gfx.h"

namespace clip {

enum Kind : uint8_t { kText, kCode, kLink, kFiles, kMedia, kColor, kKinds };
enum Tab { kTabAll, kTabPinned, kTabText, kTabCode, kTabLinks, kTabFiles, kTabMedia, kTabColors, kTabCount };

constexpr size_t kHeadMax = 1024; // chars kept in memory, shown and searched; the rest waits in its own file
constexpr size_t kTextMax = 1u << 20; // chars kept at all: a copied novel is cut there
constexpr size_t kRichMax = 1u << 20; // bytes of html or rtf kept per item
constexpr int kMaxLow = 25, kMaxHigh = 1000, kMaxDef = 200; // the history's size, as the slider allows
constexpr int kThemes = 7; // code colour themes
constexpr int kKeys = 5; // the quick key: off, alt+shift+v, ctrl+alt+v, win+alt+v, ctrl+shift+v

struct Item {
    uint64_t id = 0; // also its files' names
    int64_t at = 0; // unix seconds it was last copied
    Kind kind = kText;
    bool pinned = false, image = false, more = false, html = false, rtf = false; // more: the whole text sits in its own file
    uint32_t w = 0, h = 0; // a picture's size
    uint32_t argb = 0; // a colour's value
    uint32_t count = 0; // files in a file copy
    uint64_t hash = 0; // what it holds, for "copied again"
    std::wstring head; // the text or its start; one path per line for files; a colour as it was written
    std::wstring app; // the app it came from, its exe name
    std::wstring note; // a link's page title, or code's language
    std::wstring src; // the page it was copied from, when the browser says
    // made only while it's on screen, dropped as the panel closes
    ComPtr<IWICBitmapSource> px;
    ComPtr<ID2D1Bitmap> bmp;
    bool pxTried = false;
};

struct Options {
    int max = kMaxDef;
    int paste = 1; // a pick pastes into the app that had the keyboard; 0 only copies
    int plain = 0; // always paste as plain text
    int clean = 1; // tracking bits come off copied links
    int noPass = 1; // password managers' copies are never kept
    int noAdult = 1; // adult sites' links and copies are never kept
    int titles = 0; // a copied link's page title is fetched: the only thing that ever goes online
    int view = 0; // 0 list, 1 grid
    int size = 1; // the panel: 0 compact, 1 normal, 2 large
    int key = 1; // the quick key, see kKeys
    int theme = 0; // code colours
    int paused = 0; // nothing is kept while on
    std::vector<std::wstring> noApps, noSites; // lowercased exe names and site hosts never kept
};

class Store {
public:
    std::vector<Item> items; // newest first
    Options opt;

    void Load(); // once: the file, then the options
    bool Loaded() const { return loaded; }
    void SaveOptions() const;
    void Save(); // the index file; pictures and long texts were written as they came
    bool dirty = false; // an index write is owed
    uint64_t ver = 0; // bumped on every change: the panel's filtered list is made again only then

    // what the clipboard holds right now, read and kept: 1 kept or moved up, 0 skipped, -1 the clipboard was busy (ask again).
    // a picture's files are written on a worker, which posts doneMsg (wparam: the id) to notify
    int Capture(HWND hwnd, HWND notify, UINT doneMsg);
    bool Put(HWND hwnd, const Item& it, bool plain) const; // back onto the clipboard
    void Touch(size_t i); // copied again: to the top, newer
    void Remove(size_t i);
    void Clear(); // everything but the pins
    void Trim(); // the oldest unpinned past the size go
    void Unload(); // the widget left the dock: memory back, the files stay
    std::wstring Full(const Item& it) const; // the whole text, from its file when it's long
    std::wstring Dir() const { return dir; }
    std::wstring File(uint64_t id, const wchar_t* ext) const;
    void DropPixels(); // previews made for the panel go

private:
    bool loaded = false;
    std::wstring dir;
    uint64_t lastId = 0;
    uint64_t NewId();
    void Insert(Item&& it);
    void Forget(const Item& it); // its files
};

Store& History(); // one history for every dock

// sorting what was copied
bool IsLink(const std::wstring& s);
std::wstring CleanLink(const std::wstring& url, bool& changed); // utm_ and friends gone
std::wstring Host(const std::wstring& url); // lowercase, no www.
bool Adult(const std::wstring& host);
bool ParseColor(const std::wstring& s, uint32_t& argb); // 20+ notations, to srgb
std::wstring CodeLang(const std::wstring& s); // empty when it doesn't read as code
bool MediaFile(const std::wstring& path);
bool ImageFile(const std::wstring& path);

struct Fmt { // one way of writing a colour, for its preview
    const wchar_t* name;
    std::wstring text;
};
std::vector<Fmt> Formats(uint32_t argb);

enum Role : uint8_t { kPlain, kWord, kString, kComment, kNumber, kCall, kRoles };
struct Span {
    uint32_t at, len;
    Role role;
};
std::vector<Span> Highlight(const std::wstring& s, const std::wstring& lang); // a light tokenizer: keywords, strings, comments, numbers, calls
const wchar_t* ThemeName(int theme);
uint32_t ThemeInk(int theme, Role r); // 0xRRGGBB; theme 0 follows the accent: callers paint kWord with it
uint32_t ThemeBack(int theme); // the code card's colour, 0 for the panel's own

int Fuzzy(const std::wstring& hay, const std::wstring& needle); // -1 no match, else higher is better
std::wstring Ago(int64_t at);
std::wstring Line(const Item& it); // what a row says
const wchar_t* TabName(int tab);
bool InTab(const Item& it, int tab);
const wchar_t* KeyName(int key);
bool KeyMods(int key, UINT& mods, UINT& vk); // for RegisterHotKey; false for off

}
