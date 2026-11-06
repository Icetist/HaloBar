// The clipboard widget's panel: everything copied, by kind, searchable as you type, pinned, previewed with alt, pasted straight
// into the app you came from. Its own quick key opens it anywhere; its options sit behind the gear, in settings' cards.
#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cwctype>
#include <thread>

#include "clips.h"
#include "glyphs.h"
#include "popmenu.h"
#include "setui.h"

namespace {

constexpr float kClipW[3] = {500, 620, 760}, kClipRise[3] = {400, 470, 560}; // compact, normal, large
constexpr int kGridCols[3] = {3, 4, 5};
constexpr float kRowH = 44, kCardH = 112, kCardGap = 8; // a list row; a grid card
constexpr float kTop = 34, kTabsH = 26, kFoot = 18; // the search row, the kinds, the hint line
constexpr float kThumb = 30; // a row's picture
constexpr int kThumbsPerFrame = 6; // previews made per frame while scrolling: the rest next frame
constexpr int kQueryMax = 200;
constexpr UINT kPasteMs = 60, kPasteTries = 12; // after the app has the keyboard back: ctrl+v, once the user's own keys are up
constexpr UINT kSaveMs = 1500; // copies come in bursts: one index write after
constexpr int kHotkeyId = 0x4842;
constexpr size_t kPeekChars = 6000; // a preview shows this much text
constexpr float kSetupLineH = 34;
constexpr float kDragStart = 6; // dips the mouse travels with the button down before an item is being dragged out
constexpr int SysGrid = glyph::kSys | 0xF0E2, SysList = glyph::kSys | 0xEA37, SysEyeOff = glyph::kSys | 0xED1A, SysEye = glyph::kSys | 0xE890;
constexpr int SysCode = glyph::kSys | 0xE943, SysColor = glyph::kSys | 0xE790, SysDoc = glyph::kSys | 0xE8A5, SysVideo = glyph::kSys | 0xE714, SysMusic = glyph::kSys | 0xEC4F;

int KindGlyph(const clip::Item& it) {
    switch (it.kind) {
    case clip::kCode: return SysCode;
    case clip::kLink: return glyph::Link;
    case clip::kFiles: return glyph::SysFolder;
    case clip::kMedia: return it.image ? glyph::SysPhoto : SysVideo;
    case clip::kColor: return SysColor;
    default: return SysDoc;
    }
}

const wchar_t* KindName(const clip::Item& it) {
    static const wchar_t* const kNames[clip::kKinds] = {L"Text", L"Code", L"Link", L"Files", L"Media", L"Color"};
    return it.image ? L"Image" : kNames[it.kind < clip::kKinds ? it.kind : 0];
}

std::wstring Lower(std::wstring s) {
    for (auto& ch : s) ch = (wchar_t)towlower(ch);
    return s;
}

std::wstring TitleOf(const std::string& html) { // <title> of a page, entities undone
    std::string low = html.substr(0, 256 * 1024);
    for (auto& ch : low) ch = (char)tolower((unsigned char)ch);
    size_t a = low.find("<title"), b = a == std::string::npos ? a : low.find('>', a), e = b == std::string::npos ? b : low.find("</title", b);
    if (e == std::string::npos) return {};
    std::string t = html.substr(b + 1, e - b - 1);
    int n = MultiByteToWideChar(CP_UTF8, 0, t.data(), (int)t.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, t.data(), (int)t.size(), w.data(), n);
    static const std::pair<const wchar_t*, const wchar_t*> kEnt[] = {{L"&amp;", L"&"}, {L"&lt;", L"<"}, {L"&gt;", L">"}, {L"&quot;", L"\""}, {L"&#39;", L"'"}, {L"&#039;", L"'"}, {L"&nbsp;", L" "}};
    for (auto [from, to] : kEnt)
        for (size_t at; (at = w.find(from)) != std::wstring::npos;) w.replace(at, wcslen(from), to);
    for (auto& ch : w) ch = ch == L'\n' || ch == L'\r' || ch == L'\t' ? L' ' : ch;
    size_t f = w.find_first_not_of(L' '), l = w.find_last_not_of(L' ');
    return f == std::wstring::npos ? std::wstring() : w.substr(f, (std::min)(l - f + 1, (size_t)200));
}

struct Titled {
    uint64_t id;
    std::wstring title;
};

}

float Bar::ClipW() const { return kClipW[(std::clamp)(clip::History().opt.size, 0, 2)]; }
float Bar::ClipRise() const { return kClipRise[(std::clamp)(clip::History().opt.size, 0, 2)]; }

void Bar::SyncClip() { // the main dock listens and owns the key for every dock that shows the widget
    bool any = false;
    for (Bar* b : Docks()) any = any || b->HasWg(kWgClip);
    Bar* main = lead ? lead : this;
    clip::Store& st = clip::History();
    if (any && !st.Loaded()) st.Load();
    if (any != main->clipListening) {
        main->clipListening = any;
        if (any) AddClipboardFormatListener(main->hwnd);
        else if (!main->calWaiting) RemoveClipboardFormatListener(main->hwnd);
    }
    int key = any ? st.opt.key : 0;
    if (key != main->clipKeyOn) {
        if (main->clipKeyOn) UnregisterHotKey(main->hwnd, kHotkeyId);
        UINT mods = 0, vk = 0;
        main->clipKeyOn = clip::KeyMods(key, mods, vk) && RegisterHotKey(main->hwnd, kHotkeyId, mods, vk) ? key : 0; // taken by another app: none
    }
    if (!any && st.Loaded()) st.Unload(); // the widget left every dock: its memory goes, its files stay
}

int Bar::OnClipboard2() {
    if (!clipListening) return 0;
    clip::Store& st = clip::History();
    bool aged = st.Expire(); // the auto delete rides on copies: no timer of its own
    int r = st.Capture(hwnd, hwnd, WM_APP_CLIP);
    if (aged || r > 0) SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    if (r <= 0) return r;
    const clip::Item& top = st.items.front();
    if (st.opt.titles && top.kind == clip::kLink && top.note.empty()) { // its page's title, fetched off the ui thread
        std::wstring url = top.head;
        if (url.rfind(L"www.", 0) == 0) url = L"https://" + url;
        std::thread([h = hwnd, id = top.id, url] {
            std::atomic<bool> stop{false};
            std::atomic<void*> slot{nullptr};
            std::string body;
            if (url.rfind(L"http", 0) != 0 || !sys::detail::HttpGet(url.c_str(), body, stop, slot, 2u << 20)) return;
            auto* got = new Titled{id, TitleOf(body)};
            if (got->title.empty() || !PostMessageW(h, WM_APP_CLIPTITLE, 0, (LPARAM)got)) delete got;
        }).detach();
    }
    for (Bar* b : Docks())
        if (b->HasWg(kWgClip) && !b->Hidden()) b->Redraw(); // the medium face shows the newest
    return r;
}

void Bar::OnClipDone(WPARAM id, LPARAM lp) {
    clip::Store& st = clip::History();
    if (auto* got = reinterpret_cast<Titled*>(lp)) { // a link's title came
        for (clip::Item& it : st.items)
            if (it.id == got->id) it.note = got->title, st.dirty = true, ++st.ver, SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
        delete got;
    } else { // a picture's files are written: its preview can be made
        for (clip::Item& it : st.items)
            if (it.id == (uint64_t)id) it.pxTried = false;
    }
    for (Bar* b : Docks())
        if (b->wgOpen && b->wgKind == kWgClip) b->Redraw();
}

void Bar::OpenClip(float x, bool setup) {
    if (wgOpen && wgKind == kWgClip && !setup) return SetPanel(Panel::None); // asked again: it closes
    HWND fore = GetForegroundWindow();
    clipTarget = fore && fore != hwnd ? fore : tasks ? tasks->Fore() : nullptr; // where a pick pastes
    clipQuery.clear();
    clipSel = 0, clipScroll = 0, clipSetupScroll = 0;
    clipSetup = setup, clipPeek = false, clipFollow = true;
    clipNoteId = 0, clipNoteText.clear();
    if (clip::History().Loaded() && clip::History().Expire()) SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr); // and on every opening
    if (!(wgOpen && wgKind == kWgClip)) OpenWidget(kWgClip, x);
    Redraw();
}

void Bar::ClipClosed() { // the panel went: its previews and layouts go with it
    if (clipNoteId) ClipNoteDone(true); // a note being typed is kept, as a field losing focus keeps its text
    clip::History().DropPixels();
    clipLays.clear();
    clipPeekLay.Reset(), clipPeekBmp.Reset(), clipPeekId = 0;
    clipPeek = clipSetup = false;
    if (clip::History().dirty) clip::History().Save();
}

void Bar::ClipFilter() {
    clip::Store& st = clip::History();
    if (clipShownVer == st.ver && clipShownQ == clipQuery && clipShownTab == clipTab) return;
    clipShownVer = st.ver, clipShownQ = clipQuery, clipShownTab = clipTab;
    std::fill(std::begin(clipCounts), std::end(clipCounts), 0);
    std::wstring needle = Lower(clipQuery);
    std::vector<std::pair<int, int>> hits; // score, index
    for (int i = 0; i < (int)st.items.size(); ++i) {
        const clip::Item& it = st.items[(size_t)i];
        for (int k = 0; k < clip::kTabCount; ++k) clipCounts[k] += clip::InTab(it, k) ? 1 : 0;
        if (!clip::InTab(it, clipTab)) continue;
        int s = 0;
        if (!needle.empty()) {
            s = (std::max)({clip::Fuzzy(it.head, needle), clip::Fuzzy(it.note, needle), clip::Fuzzy(it.memo, needle), clip::Fuzzy(it.app, needle) / 2, clip::Fuzzy(it.src, needle) / 2});
            if (s < 0) continue;
        } else if (it.pinned && clipTab == clip::kTabAll) {
            s = 1; // pins first, then the newest
        }
        hits.push_back({s, i});
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    clipShown.clear();
    for (auto& h : hits) clipShown.push_back(h.second);
    clipSel = (std::clamp)(clipSel, 0, (std::max)(0, (int)clipShown.size() - 1));
}

void Bar::ClipPick(int at, bool plain) {
    clip::Store& st = clip::History();
    if (at < 0 || at >= (int)st.items.size()) return;
    const clip::Item& it = st.items[(size_t)at];
    bool files = it.kind == clip::kFiles || it.kind == clip::kMedia;
    if (!st.Put(hwnd, it, plain || (st.opt.plain && !files))) return; // files as their paths only when asked each time: "always plain" is about words
    st.Touch((size_t)at);
    SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    HWND back = clipTarget;
    bool paste = st.opt.paste && back && IsWindow(back);
    SetPanel(Panel::None); // the keyboard goes back to the app that had it
    if (!paste) return;
    if (GetForegroundWindow() != back) SetForegroundWindow(back);
    clipPasteTo = back, clipPasteTries = 0;
    SetTimer(hwnd, kClipPasteTimer, kPasteMs, nullptr);
}

void Bar::ClipPaste() { // the timer: ctrl+v into the app, once it's in front and the user's own keys are up
    if (!clipPasteTo || GetForegroundWindow() != clipPasteTo) return void(clipPasteTo = nullptr);
    bool held = GetAsyncKeyState(VK_SHIFT) < 0 || GetAsyncKeyState(VK_MENU) < 0 || GetAsyncKeyState(VK_CONTROL) < 0 || GetAsyncKeyState(VK_LWIN) < 0 || GetAsyncKeyState(VK_RWIN) < 0;
    if (held && ++clipPasteTries < kPasteTries) return void(SetTimer(hwnd, kClipPasteTimer, kPasteMs, nullptr));
    clipPasteTo = nullptr;
    Keys({VK_CONTROL, 'V'});
}

void Bar::ClipPin(int at) {
    clip::Store& st = clip::History();
    if (at < 0 || at >= (int)st.items.size()) return;
    st.items[(size_t)at].pinned = !st.items[(size_t)at].pinned;
    st.dirty = true, ++st.ver;
    SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    Redraw();
}

void Bar::ClipDrop(int at) {
    clip::History().Remove((size_t)at);
    SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
    Redraw();
}

bool Bar::ClipKey(WPARAM vk, bool down) {
    if (!(wgOpen && wgKind == kWgClip)) return false;
    if (vk == VK_MENU) { // held: the selected or hovered item's preview
        if (clipPeek != down) clipPeek = down, Redraw();
        return true;
    }
    if (!down) return false;
    if (clipNoteId) { // typing a note: enter keeps it, esc drops it, nothing else acts on the list
        if (vk == VK_RETURN || vk == VK_ESCAPE) ClipNoteDone(vk == VK_RETURN);
        else if (vk == VK_BACK && GetKeyState(VK_CONTROL) < 0) clipNoteText.clear(), Redraw();
        return vk != VK_F4; // alt+f4 still closes; tab and the arrows wait for the note
    }
    bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0;
    ClipFilter();
    int n = (int)clipShown.size(), cols = clip::History().opt.view ? kGridCols[(std::clamp)(clip::History().opt.size, 0, 2)] : 1;
    auto go = [&](int by) { clipSel = (std::clamp)(clipSel + by, 0, (std::max)(0, n - 1)), clipFollow = true; };
    int sel = n ? clipShown[(size_t)(std::clamp)(clipSel, 0, n - 1)] : -1;
    switch (vk) {
    case VK_ESCAPE:
        if (clipSetup) clipSetup = false;
        else if (!clipQuery.empty()) clipQuery.clear();
        else SetPanel(Panel::None);
        break;
    case VK_UP: go(-cols); break;
    case VK_DOWN: go(cols); break;
    case VK_LEFT:
        if (cols > 1) go(-1);
        else clipTab = (clipTab + clip::kTabCount - 1) % clip::kTabCount, clipSel = 0, clipScroll = 0;
        break;
    case VK_RIGHT:
        if (cols > 1) go(1);
        else clipTab = (clipTab + 1) % clip::kTabCount, clipSel = 0, clipScroll = 0;
        break;
    case VK_TAB: clipTab = (clipTab + (shift ? clip::kTabCount - 1 : 1)) % clip::kTabCount, clipSel = 0, clipScroll = 0; break;
    case VK_PRIOR: go(-6 * cols); break;
    case VK_NEXT: go(6 * cols); break;
    case VK_HOME: clipSel = 0, clipFollow = true; break;
    case VK_END: clipSel = (std::max)(0, n - 1), clipFollow = true; break;
    case VK_RETURN: ClipPick(sel, shift); return true;
    case VK_DELETE:
        if (sel >= 0) ClipDrop(sel);
        break;
    default:
        if (ctrl && vk == 'P' && sel >= 0) ClipPin(sel);
        else if (ctrl && vk >= '1' && vk <= '9' && (int)(vk - '1') < n) return ClipPick(clipShown[vk - '1'], shift), true; // quick paste by place
        else if (ctrl && vk == VK_BACK) clipQuery.clear(), clipSel = 0;
        else return false;
    }
    Redraw();
    return true;
}

void Bar::ClipChar(wchar_t ch) {
    if (clipNoteId) {
        if (ch == VK_BACK && !clipNoteText.empty()) clipNoteText.pop_back();
        else if (ch >= 32 && ch != 127 && clipNoteText.size() < clip::kMemoMax && GetKeyState(VK_CONTROL) >= 0) clipNoteText += ch;
        return Redraw();
    }
    if (ch == VK_BACK) {
        if (!clipQuery.empty()) clipQuery.pop_back();
    } else if (ch >= 32 && ch != 127 && clipQuery.size() < (size_t)kQueryMax && GetKeyState(VK_CONTROL) >= 0) {
        clipQuery += ch;
        clipSetup = false; // typing is searching
    } else {
        return;
    }
    clipSel = 0, clipScroll = 0;
    Redraw();
}

void Bar::ClipThumb(clip::Item& it) { // its picture, made once while it's on screen
    if (it.pxTried) return;
    it.pxTried = true;
    clip::Store& st = clip::History();
    UINT px = (UINT)std::lround(kThumb * 2 * scale); // room for the grid's bigger cards
    if (it.image) {
        it.px = st.Picture(it, true, (UINT)std::lround(kCardH * scale)); // sealed on disk: opened into memory
        if (!it.px) it.pxTried = false; // still being written: tried again when the worker says so
        return;
    }
    if (it.kind != clip::kFiles && it.kind != clip::kMedia) return;
    std::wstring first = it.head.substr(0, it.head.find(L'\n'));
    if (clip::ImageFile(first)) it.px = gfx::DecodeFile(first.c_str(), (UINT)std::lround(kCardH * scale));
    if (it.px) return;
    ComPtr<IShellItemImageFactory> img;
    HBITMAP hb = nullptr;
    if (SUCCEEDED(SHCreateItemFromParsingName(first.c_str(), nullptr, IID_PPV_ARGS(&img))) &&
        SUCCEEDED(img->GetImage({(LONG)px, (LONG)px}, SIIGBF_BIGGERSIZEOK | SIIGBF_INCACHEONLY, &hb))) { // a cached thumbnail, never a slow fresh one
        it.px = gfx::FromShellBitmap(hb);
        DeleteObject(hb);
    } else if (img && SUCCEEDED(img->GetImage({(LONG)px, (LONG)px}, SIIGBF_ICONONLY, &hb))) {
        it.px = gfx::FromShellBitmap(hb);
        DeleteObject(hb);
    }
}

ID2D1SolidColorBrush* Bar::ClipInk(Color c) { // the code colours' brushes: few, kept per colour while the panel is up
    uint32_t key = (uint32_t)(c.r * 255) << 16 | (uint32_t)(c.g * 255) << 8 | (uint32_t)(c.b * 255);
    auto it = clipInks.find(key);
    if (it != clipInks.end()) return it->second.Get();
    ComPtr<ID2D1SolidColorBrush> b;
    if (!ctx || FAILED(ctx->CreateSolidColorBrush(c.D2D(), &b))) return nullptr;
    return (clipInks[key] = b).Get();
}

IDWriteTextLayout* Bar::ClipLayout(const clip::Item& it, const std::wstring& text, float w, float h, float size, const Theme& t, bool big) {
    uint64_t key = it.id * 31 + (big ? 7 : 0) + (uint64_t)clip::History().opt.theme * 131 + (uint64_t)std::lround(w);
    ComPtr<IDWriteTextLayout>* slot = nullptr;
    if (big) {
        if (clipPeekId == key && clipPeekLay) return clipPeekLay.Get();
        clipPeekId = key, clipPeekLay.Reset(), slot = &clipPeekLay;
    } else {
        auto f = clipLays.find(key);
        if (f != clipLays.end()) return f->second.Get();
        if (clipLays.size() > 64) clipLays.clear(); // scrolled a long way: start again
        slot = &clipLays[key];
    }
    ComPtr<IDWriteTextFormat> fmt;
    bool code = it.kind == clip::kCode;
    if (FAILED(gfx::DWrite()->CreateTextFormat(code ? L"Cascadia Mono" : L"Segoe UI Variable Text", nullptr, DWRITE_FONT_WEIGHT_REGULAR, DWRITE_FONT_STYLE_NORMAL,
                                               DWRITE_FONT_STRETCH_NORMAL, size, L"", &fmt)) ||
        FAILED(gfx::DWrite()->CreateTextLayout(text.c_str(), (UINT32)text.size(), fmt.Get(), w, h, slot->ReleaseAndGetAddressOf())))
        return nullptr;
    (*slot)->SetWordWrapping(code ? DWRITE_WORD_WRAPPING_CHARACTER : DWRITE_WORD_WRAPPING_WRAP);
    if (code) { // the theme's colours over the tokens
        int th = clip::History().opt.theme;
        for (const clip::Span& s : clip::Highlight(text, it.note)) {
            Color ink = th == 0 && s.role == clip::kWord ? t.accentHi : Color::Rgb(clip::ThemeInk(th, s.role));
            if (ID2D1SolidColorBrush* b = ClipInk(ink)) (*slot)->SetDrawingEffect(b, {s.at, s.len});
        }
    }
    return slot->Get();
}

void Bar::DrawClip(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    clip::Store& st = clip::History();
    if (!st.Loaded()) st.Load();
    ClipFilter();
    bool focused = GetForegroundWindow() == hwnd;

    // the search row: what's typed, and the panel's own buttons
    D2D1_RECT_F field = Rect(r.left - 6, r.top - 4, Width(r) - 146, 30);
    c.Fill(field, 15, t.text.Alpha(focused ? 0.08f : 0.05f));
    c.Icon(glyph::Search, {field.left + 18, (field.top + field.bottom) / 2}, 16, t.textDim);
    D2D1_RECT_F words = {field.left + 34, field.top, field.right - 12, field.bottom};
    if (clipNoteId && std::none_of(st.items.begin(), st.items.end(), [&](const clip::Item& i) { return i.id == clipNoteId; })) clipNoteId = 0, clipNoteText.clear(); // its item went
    const std::wstring& typed = clipNoteId ? clipNoteText : clipQuery;
    if (typed.empty()) {
        const wchar_t* hint = clipNoteId ? L"A note for this item: Enter keeps it, Esc cancels" : st.opt.paused ? L"Paused: nothing new is kept" : L"Search everything you copied";
        c.Text(hint, words, Weight::Regular, 12.5f, st.opt.paused || clipNoteId ? t.accentHi : t.textLow);
    }
    std::wstring shown = typed + (focused && Caret() ? L"│" : L"");
    c.Text(shown, words, Weight::Regular, 12.5f, clipNoteId ? t.accentHi : t.text);
    float bx = r.right - 14;
    if (w::IconBtn(ui, t, Ui::Id("clip.setup"), {bx, field.top + 15}, 28, glyph::Settings, 17, true, clipSetup)) clipSetup = !clipSetup, clipSetupScroll = 0;
    bx -= 34;
    if (w::IconBtn(ui, t, Ui::Id("clip.view"), {bx, field.top + 15}, 28, st.opt.view ? SysList : SysGrid, 15)) st.opt.view ^= 1, st.SaveOptions(), clipFollow = true;
    bx -= 34;
    if (w::IconBtn(ui, t, Ui::Id("clip.pause"), {bx, field.top + 15}, 28, st.opt.paused ? SysEyeOff : SysEye, 15, true, st.opt.paused != 0)) st.opt.paused ^= 1, st.SaveOptions();
    bx -= 34;
    D2D1_RECT_F wipeBox = {bx - 14, field.top + 1, bx + 14, field.top + 29};
    if (clipSetup || !Inside(wipeBox, input.mouse)) clipWipeArmed = false; // the second click has to follow the first, right there
    if (!clipSetup) {
        if (clipWipeArmed) c.Fill(wipeBox, 14, Color::Rgb(0xFF6B6B).Alpha(0.28f));
        if (w::IconBtn(ui, t, Ui::Id("clip.wipe"), {bx, field.top + 15}, 28, glyph::Delete, 16, true, clipWipeArmed)) {
            if (clipWipeArmed) st.Clear(), clipSel = 0, clipScroll = 0, SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr);
            clipWipeArmed = !clipWipeArmed;
            Redraw();
        }
    }

    D2D1_RECT_F body = {r.left - 6, r.top + kTop + kTabsH + 6, r.right + 6, r.bottom - kFoot};
    if (clipSetup) {
        DrawClipSetup(t, {r.left - 6, r.top + kTop, r.right + 6, r.bottom});
        return;
    }
    float tx = r.left - 6; // the kinds, each with how many it holds when there's room to say
    std::wstring labels[clip::kTabCount];
    float total = 0;
    for (int round = 0; round < 2; ++round) {
        total = 0;
        for (int k = 0; k < clip::kTabCount; ++k) {
            labels[k] = clip::TabName(k);
            if (round == 0 && k != clip::kTabAll && clipCounts[k]) labels[k] += L" " + std::to_wstring(clipCounts[k]);
            total += TextW(labels[k], Weight::Medium, 11.5f) + 16 + 5;
        }
        if (total <= Width(r) + 12) break; // fits with the counts
    }
    for (int k = 0; k < clip::kTabCount; ++k) {
        const std::wstring& label = labels[k];
        float w = TextW(label, Weight::Medium, 11.5f) + 16;
        if (tx + w > r.right + 6) break;
        D2D1_RECT_F chip = Rect(tx, r.top + kTop, w, kTabsH - 4);
        uint32_t id = Ui::Id("clip.tab", k);
        Ui::Hit h = ui.Button(id, chip);
        float on = ui.Smooth(id + 10, clipTab == k ? 1.0f : 0, 16);
        c.Fill(chip, Height(chip) / 2, t.text.Alpha(h.hover ? 0.12f : 0.06f).Mix(t.accent, on));
        c.Text(label, chip, Weight::Medium, 11.5f, t.textDim.Mix(t.text, h.hover ? 0.5f : 0).Mix(t.onAccent, on), Align::Center);
        if (h.clicked) clipTab = k, clipSel = 0, clipScroll = 0, Redraw();
        tx += w + 5;
    }

    int peekAt = DrawClipItems(t, body);
    const wchar_t* hint = clipWipeArmed ? L"Click again to clear all  ·  pins and notes stay"
                          : st.opt.view ? L"Enter paste  ·  Shift plain  ·  Ctrl+P pin  ·  Del forget  ·  hold Alt to preview"
                                      : L"Enter paste  ·  Shift+Enter plain text  ·  Ctrl+1…9 quick paste  ·  Ctrl+P pin  ·  hold Alt to preview";
    c.Text(hint, Rect(r.left, r.bottom - 14, Width(r), 14), Weight::Regular, 10.5f, clipWipeArmed ? Color::Rgb(0xFF6B6B) : t.textLow, Align::Center);
    if (clipPeek && peekAt >= 0 && peekAt < (int)st.items.size()) DrawClipPeek(t, body, st.items[(size_t)peekAt]);
}

int Bar::DrawClipItems(const Theme& t, const D2D1_RECT_F& body) { // the list or the grid; returns the item a preview is for
    Canvas& c = ui.C();
    clip::Store& st = clip::History();
    int n = (int)clipShown.size();
    if (n == 0) {
        const wchar_t* why = !clipQuery.empty() ? L"Nothing matches" : st.items.empty() ? L"Copy some text, a link, a picture or files and it shows here" : L"Nothing of this kind yet";
        c.Text(why, Rect(body.left, body.top + 30, Width(body), 18), Weight::Regular, 12.5f, t.textDim, Align::Center);
        return -1;
    }
    bool grid = st.opt.view != 0;
    int cols = grid ? kGridCols[(std::clamp)(st.opt.size, 0, 2)] : 1;
    float cw = grid ? (Width(body) - (cols - 1) * kCardGap) / cols : Width(body), ch = grid ? kCardH : kRowH, step = ch + (grid ? kCardGap : 0);
    int rows = (n + cols - 1) / cols;
    float most = (std::max)(0.0f, rows * step - (grid ? kCardGap : 0) - Height(body));
    if (float notch = ui.Wheel(body); notch != 0) clipScroll -= notch * step * 1.5f, clipFollow = false;
    if (clipFollow) { // the keyboard's pick stays in view
        float y = (clipSel / cols) * step;
        if (y < clipScroll) clipScroll = y;
        else if (y + ch > clipScroll + Height(body)) clipScroll = y + ch - Height(body);
        clipFollow = false;
    }
    clipScroll = (std::clamp)(clipScroll, 0.0f, most);
    float scroll = ui.Smooth(Ui::Id("clip.scroll"), clipScroll, 18);
    bool moved = std::fabs(input.mouse.x - clipMouse.x) + std::fabs(input.mouse.y - clipMouse.y) > 0.5f;
    clipMouse = input.mouse;
    int pick = -1, menu = -1, pin = -1, drop = -1, hovered = -1, made = 0, drag = -1;
    if (clipDragged && !input.down && !input.released) clipDragged = false; // the release after a drag has passed: clicks pick again
    bool plain = GetKeyState(VK_SHIFT) < 0, focusedNow = GetForegroundWindow() == hwnd;
    c.PushClip(Inset(body, 0, -2));
    for (int k = 0; k < n; ++k) {
        float y = body.top + (k / cols) * step - scroll;
        if (y + ch < body.top || y > body.bottom) continue; // out of view: not drawn, not hit
        int at = clipShown[(size_t)k];
        clip::Item& it = st.items[(size_t)at];
        D2D1_RECT_F cell = Rect(body.left + (k % cols) * (cw + kCardGap), y, cw, ch - (grid ? 0 : 2));
        D2D1_RECT_F hit = {cell.left, (std::max)(cell.top, body.top), cell.right, (std::min)(cell.bottom, body.bottom)};
        uint32_t id = Ui::Id("clip.item", (int)(it.id & 0x7FFFFFFF));
        D2D1_RECT_F pinR = Rect(cell.right - 58, cell.top + (grid ? 6 : ch / 2 - 13), 24, 24), dropR = Rect(cell.right - 30, pinR.top, 24, 24);
        Ui::Hit ph = ui.Button(id + 1, pinR), dh = ui.Button(id + 2, dropR); // before the row's own: the first button under a press takes it
        Ui::Hit h = ui.Button(id, hit);
        if (ui.IsActive(id) && input.down && !clipDragged && std::fabs(input.mouse.x - input.pressPos.x) + std::fabs(input.mouse.y - input.pressPos.y) > kDragStart)
            clipDragged = true, drag = at; // pulled, not clicked: dragged out once the frame is done
        if (h.hover) hovered = at;
        if (h.hover && moved) clipSel = k;
        bool sel = clipSel == k;
        float lit = ui.Smooth(id + 3, sel ? 1.0f : h.hover ? 0.7f : 0, 18);
        c.Fill(cell, grid ? 14.0f : 11.0f, t.text.Alpha((grid ? 0.05f : 0) + 0.07f * lit));
        if (sel && focusedNow) c.Stroke(cell, grid ? 14.0f : 11.0f, t.accent.Alpha(0.55f), 1.2f);
        if ((it.image || it.kind == clip::kFiles || it.kind == clip::kMedia) && !it.pxTried && made < kThumbsPerFrame) ClipThumb(it), ++made;
        if (it.px && !it.bmp && ctx) ctx->CreateBitmapFromWicBitmap(it.px.Get(), nullptr, &it.bmp);
        if (grid) DrawClipCard(t, cell, it);
        else DrawClipRow(t, cell, it, k);
        if (h.hover || ph.hover || dh.hover || it.pinned) {
            c.Icon(glyph::Pin, {(pinR.left + pinR.right) / 2, (pinR.top + pinR.bottom) / 2}, 15, it.pinned ? t.accentHi : ph.hover ? t.text : t.textDim, it.pinned);
        }
        if (h.hover || ph.hover || dh.hover) c.Icon(glyph::Close, {(dropR.left + dropR.right) / 2, (dropR.top + dropR.bottom) / 2}, 16, dh.hover ? Color::Rgb(0xFF6B6B) : t.textDim);
        if (ph.clicked) pin = at;
        else if (dh.clicked) drop = at;
        else if (h.clicked && !clipDragged) pick = at;
        if (input.rclick && Inside(hit, input.mouse)) menu = at;
    }
    c.PopClip();
    if (most > 0) { // a slim thumb at the right edge: there's more
        float lane = Height(body) - 8, thumb = (std::max)(24.0f, lane * Height(body) / (Height(body) + most));
        c.Fill(Rect(body.right + 2, body.top + 4 + (lane - thumb) * (scroll / most), 3, thumb), 1.5f, t.text.Alpha(0.14f));
    }
    if (made >= kThumbsPerFrame) ui.Wake(); // more previews to make: next frame
    if (pin >= 0) ClipPin(pin);
    else if (drop >= 0) ClipDrop(drop);
    else if (pick >= 0) ClipPick(pick, plain);
    else if (drag >= 0) clipDragId = st.items[(size_t)drag].id, PostMessageW(hwnd, WM_APP_MENU, 6, 0); // a drag runs its own loop: not from inside a frame
    else if (menu >= 0) clipMenuAt = menu, PostMessageW(hwnd, WM_APP_MENU, 5, 0); // its menu, after the frame
    if (hovered >= 0) return hovered;
    return n ? clipShown[(size_t)(std::clamp)(clipSel, 0, n - 1)] : -1;
}

void Bar::DrawClipRow(const Theme& t, const D2D1_RECT_F& r, clip::Item& it, int place) {
    Canvas& c = ui.C();
    D2D1_RECT_F pic = Rect(r.left + 8, (r.top + r.bottom) / 2 - kThumb / 2, kThumb, kThumb);
    if (it.bmp) c.Image(it.bmp.Get(), pic, 7, 1);
    else if (it.kind == clip::kColor) {
        c.Fill(pic, 8, Color::Rgb(it.argb & 0xFFFFFF, (it.argb >> 24) / 255.0f));
        c.Stroke(pic, 8, t.hairTop, 1);
    } else {
        c.Fill(pic, 8, t.text.Alpha(0.06f));
        c.Icon(KindGlyph(it), {(pic.left + pic.right) / 2, (pic.top + pic.bottom) / 2}, 15, it.kind == clip::kCode || it.kind == clip::kLink ? t.accentHi : t.textDim);
    }
    float tx = pic.right + 10, tw = r.right - 66 - tx;
    std::wstring line = it.kind == clip::kLink && !it.note.empty() ? it.note : clip::Line(it);
    c.Text(line, Rect(tx, r.top + 5, tw, 18), Weight::Regular, 12.5f, t.text);
    std::wstring meta = std::wstring(KindName(it));
    if (it.kind == clip::kCode) meta = it.note;
    if (it.kind == clip::kLink) meta = clip::Host(it.head);
    if (!it.app.empty()) meta += L"  ·  " + it.app.substr(0, it.app.rfind(L'.'));
    meta += L"  ·  " + clip::Ago(it.at);
    if (it.times > 1) meta += L"  ·  ×" + std::to_wstring(it.times);
    if (!it.memo.empty()) meta = L"“" + it.memo + L"”  ·  " + meta; // the user's own words first
    c.Text(meta, Rect(tx, r.top + 23, tw, 15), Weight::Regular, 10.5f, t.textLow);
    if (place < 9 && !Inside(r, input.mouse) && !it.pinned) c.Text(L"^" + std::to_wstring(place + 1), Rect(r.right - 40, r.top, 30, Height(r)), Weight::Regular, 10.5f, t.textLow.Alpha(0.6f), Align::Right);
}

void Bar::DrawClipCard(const Theme& t, const D2D1_RECT_F& r, clip::Item& it) {
    Canvas& c = ui.C();
    D2D1_RECT_F face = Rect(r.left + 6, r.top + 6, Width(r) - 12, Height(r) - 30);
    if (it.bmp && it.image) c.Image(it.bmp.Get(), face, 9, 1);
    else if (it.kind == clip::kColor) {
        c.Fill(face, 9, Color::Rgb(it.argb & 0xFFFFFF, (it.argb >> 24) / 255.0f));
        c.Stroke(face, 9, t.hairTop, 1);
    } else if (it.bmp) {
        float s = (std::min)(Height(face) - 8, 56.0f);
        c.Image(it.bmp.Get(), Rect((face.left + face.right) / 2 - s / 2, (face.top + face.bottom) / 2 - s / 2, s, s), 6, 1);
    } else if (it.kind == clip::kLink) {
        c.Fill(face, 9, t.text.Alpha(0.05f));
        c.Icon(glyph::Link, {face.left + 16, face.top + 16}, 16, t.accentHi);
        c.Text(clip::Host(it.head), Rect(face.left + 30, face.top + 6, Width(face) - 36, 20), Weight::Semi, 12, t.text);
        c.Text(it.note.empty() ? it.head : it.note, Rect(face.left + 10, face.top + 30, Width(face) - 20, 18), Weight::Regular, 11, t.textDim);
    } else { // words or code, a few lines of them
        uint32_t back = it.kind == clip::kCode ? clip::ThemeBack(clip::History().opt.theme) : 0;
        c.Fill(face, 9, back ? Color::Rgb(back) : t.text.Alpha(0.05f));
        std::wstring snip = it.head.substr(0, 400);
        if (IDWriteTextLayout* lay = ClipLayout(it, snip, Width(face) - 16, Height(face) - 10, it.kind == clip::kCode ? 10.5f : 11.5f, t, false)) {
            Color ink = it.kind == clip::kCode ? (back || clip::History().opt.theme ? Color::Rgb(clip::ThemeInk(clip::History().opt.theme, clip::kPlain)) : t.text) : t.textDim.Mix(t.text, 0.5f);
            if (ID2D1SolidColorBrush* b = ClipInk(ink)) {
                c.PushClip(Inset(face, 4, 4));
                c.Target()->DrawTextLayout({face.left + 8, face.top + 5}, lay, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
                c.PopClip();
            }
        }
    }
    std::wstring meta = !it.memo.empty() ? L"“" + it.memo + L"”" : it.kind == clip::kCode ? it.note : it.kind == clip::kColor ? it.head : std::wstring(KindName(it));
    meta += L"  ·  " + clip::Ago(it.at) + (it.times > 1 ? L"  ·  ×" + std::to_wstring(it.times) : L"");
    c.Text(meta, Rect(r.left + 10, r.bottom - 22, Width(r) - 70, 16), Weight::Regular, 10.5f, t.textLow);
}

void Bar::DrawClipPeek(const Theme& t, const D2D1_RECT_F& body, clip::Item& it) { // alt held: the whole thing, big
    Canvas& c = ui.C();
    clip::Store& st = clip::History();
    D2D1_RECT_F sheet = Inset(body, 10, 0);
    c.Fill(sheet, 16, t.bg.Alpha(1).Mix(t.text, 0.07f));
    c.StrokeV(sheet, 16, t.hairTop, t.hairBot, 1);
    D2D1_RECT_F in = Inset(sheet, 16, 14);
    std::wstring meta = std::wstring(KindName(it)) + (it.app.empty() ? L"" : L"  ·  from " + it.app) + L"  ·  " + clip::Ago(it.at);
    if (it.times > 1) meta += L"  ·  copied " + std::to_wstring(it.times) + L" times";
    if (!it.memo.empty()) meta = L"“" + it.memo + L"”  ·  " + meta;
    c.Text(meta, Rect(in.left, in.bottom - 14, Width(in), 14), Weight::Regular, 10.5f, t.textLow);
    in.bottom -= 22;
    if (it.image) {
        if (clipPeekImg != it.id) { // the picture at the card's size, decoded once
            clipPeekImg = it.id, clipPeekBmp.Reset();
            if (auto src = st.Picture(it, false, (UINT)std::lround((std::max)(Width(in), Height(in)) * scale)); src && ctx)
                ctx->CreateBitmapFromWicBitmap(src.Get(), nullptr, &clipPeekBmp);
        }
        if (clipPeekBmp) {
            D2D1_SIZE_F sz = clipPeekBmp->GetSize();
            float k = (std::min)(Width(in) / (std::max)(1.0f, sz.width), Height(in) / (std::max)(1.0f, sz.height));
            float w = sz.width * k, h = sz.height * k;
            c.Image(clipPeekBmp.Get(), Rect((in.left + in.right - w) / 2, (in.top + in.bottom - h) / 2, w, h), 8, 1);
        }
        return;
    }
    if (it.kind == clip::kColor) { // a big swatch, and the colour in other notations: click one to copy it
        D2D1_RECT_F sw = Rect(in.left, in.top, 120, Height(in));
        c.Fill(sw, 14, Color::Rgb(it.argb & 0xFFFFFF, (it.argb >> 24) / 255.0f));
        c.Stroke(sw, 14, t.hairTop, 1);
        float y = in.top;
        for (const clip::Fmt& f : clip::Formats(it.argb)) {
            D2D1_RECT_F row = Rect(sw.right + 14, y, in.right - sw.right - 14, 28);
            c.TextSp(f.name, Rect(row.left, row.top, 60, Height(row)), Weight::Medium, 9.5f, t.textLow, 1.4f);
            c.Text(f.text, Rect(row.left + 62, row.top, Width(row) - 62, Height(row)), Weight::Regular, 12, t.text);
            y += 30;
        }
        return;
    }
    if (it.kind == clip::kFiles || it.kind == clip::kMedia) {
        float y = in.top;
        for (const std::wstring& p : [&] { std::vector<std::wstring> v; size_t a = 0; while (a < it.head.size() && v.size() < 10) { size_t e = it.head.find(L'\n', a); v.push_back(it.head.substr(a, e - a)); if (e == std::wstring::npos) break; a = e + 1; } return v; }()) {
            c.Icon(glyph::SysFolder, {in.left + 10, y + 11}, 15, t.textDim);
            c.Text(p, Rect(in.left + 26, y, Width(in) - 26, 22), Weight::Regular, 12, t.text);
            y += 24;
        }
        if (it.count > 10) c.Text(L"and " + std::to_wstring(it.count - 10) + L" more", Rect(in.left + 26, y, Width(in), 18), Weight::Regular, 11, t.textLow);
        return;
    }
    float top = in.top;
    if (it.kind == clip::kLink) {
        c.Text(it.note.empty() ? clip::Host(it.head) : it.note, Rect(in.left, top, Width(in), 20), Weight::Semi, 14, t.text);
        top += 26;
    }
    if (it.kind == clip::kCode) {
        uint32_t back = clip::ThemeBack(st.opt.theme);
        if (back) c.Fill(Rect(in.left - 6, top - 4, Width(in) + 12, in.bottom - top + 8), 10, Color::Rgb(back));
    }
    std::wstring text = st.Full(it).substr(0, kPeekChars);
    if (IDWriteTextLayout* lay = ClipLayout(it, text, Width(in), in.bottom - top, it.kind == clip::kCode ? 12.0f : 13.0f, t, true)) {
        Color ink = it.kind == clip::kCode && (clip::ThemeBack(st.opt.theme) || st.opt.theme) ? Color::Rgb(clip::ThemeInk(st.opt.theme, clip::kPlain)) : t.text;
        if (ID2D1SolidColorBrush* b = ClipInk(ink)) {
            c.PushClip({in.left, top, in.right, in.bottom});
            c.Target()->DrawTextLayout({in.left, top}, lay, b, D2D1_DRAW_TEXT_OPTIONS_NONE);
            c.PopClip();
        }
    }
}

void Bar::DrawClipSetup(const Theme& t, const D2D1_RECT_F& area) { // the widget's options, in settings' own cards
    Canvas& c = ui.C();
    clip::Store& st = clip::History();
    clip::Options& o = st.opt;
    D2D1_RECT_F view = {area.left, area.top, area.right, area.bottom};
    constexpr float kGap = 10, kHead = 30, kFootPad = 10, kCodeLabelH = 18;
    float half = (Width(view) - kGap) / 2;
    float recH = kHead + 5 * kSetupLineH + kFootPad, pasteH = kHead + 2 * kSetupLineH + setui::kNoteLine + kFootPad;
    constexpr float kAgeRow = 20 + 30; // "delete after" and its choices
    float histH = kHead + 14 + 36 + 34 + kAgeRow + kSetupLineH + kFootPad, keyH = kHead + 30 + setui::kNoteLine + kFootPad, lookH = kHead + 2 * 34 + kCodeLabelH + 34 + kFootPad;
    float neverH = kHead + 30 + setui::kNoteLine + kFootPad + 8;
    float content = (std::max)(recH, histH + kGap + keyH) + kGap + (std::max)(pasteH + kGap + neverH, lookH) + 4;
    float most = (std::max)(0.0f, content - Height(view));
    bool overSlide = clipSlideHot;
    clipSlideHot = false;
    if (float notch = overSlide ? 0 : ui.Wheel(view); notch != 0) clipSetupScroll = (std::clamp)(clipSetupScroll - notch * 60, 0.0f, most);
    clipSetupScroll = (std::min)(clipSetupScroll, most);
    float scroll = ui.Smooth(Ui::Id("clip.setscroll"), clipSetupScroll, 18);
    c.PushClip(Inset(view, -2, 0));
    float y = view.top - scroll, x2 = view.left + half + kGap;
    bool save = false;

    D2D1_RECT_F in = setui::Box(c, t, Rect(view.left, y, half, recH), L"Recording");
    bool f[5] = {};
    setui::Toggle(ui, t, "clip.o.pause", Rect(in.left, in.top, Width(in), kSetupLineH), L"Pause: keep nothing new", o.paused != 0, f[0]);
    setui::Toggle(ui, t, "clip.o.pass", Rect(in.left, in.top + kSetupLineH, Width(in), kSetupLineH), L"Skip password managers", o.noPass != 0, f[1]);
    setui::Toggle(ui, t, "clip.o.adult", Rect(in.left, in.top + 2 * kSetupLineH, Width(in), kSetupLineH), L"Skip adult sites", o.noAdult != 0, f[2]);
    setui::Toggle(ui, t, "clip.o.clean", Rect(in.left, in.top + 3 * kSetupLineH, Width(in), kSetupLineH), L"Strip tracking from links", o.clean != 0, f[3]);
    setui::Toggle(ui, t, "clip.o.titles", Rect(in.left, in.top + 4 * kSetupLineH, Width(in), kSetupLineH), L"Fetch link titles (online)", o.titles != 0, f[4]);
    int* fields[5] = {&o.paused, &o.noPass, &o.noAdult, &o.clean, &o.titles};
    for (int i = 0; i < 5; ++i)
        if (f[i]) *fields[i] ^= 1, save = true;

    in = setui::Box(c, t, Rect(x2, y, half, histH), L"History");
    float v = (float)(o.max - clip::kMaxLow) / (clip::kMaxHigh - clip::kMaxLow);
    c.Text(L"Keep the last", Rect(in.left, in.top, 120, 14), Weight::Regular, 11.5f, t.textDim);
    c.Text(std::to_wstring(o.max) + L" items", Rect(in.right - 100, in.top - 1, 100, 16), Weight::Semi, 12, t.accentHi, Align::Right);
    D2D1_RECT_F row = Rect(in.left - 4, in.top + 16, Width(in) + 8, 32);
    w::SlideLook lk;
    lk.bubble = std::to_wstring(o.max);
    lk.step = 25.0f / (clip::kMaxHigh - clip::kMaxLow);
    uint32_t sid = Ui::Id("clip.o.max");
    if (Inside(row, input.mouse)) clipSlideHot = true;
    if (w::Slide(ui, t, sid, row, v, lk)) {
        o.max = clip::kMaxLow + (int)std::lround(v * (clip::kMaxHigh - clip::kMaxLow) / 25) * 25; // steps of 25
        if (!ui.IsActive(sid)) save = true, st.Trim(), ++st.ver;
    }
    if (ui.IsActive(sid) && input.released) save = true, st.Trim(), ++st.ver; // trimmed once let go: a drag past the old size never loses anything mid-drag
    if (LabelButton(t, Ui::Id("clip.o.clear"), Rect(in.left, row.bottom + 4, 150, 28), glyph::Delete, L"Clear history")) st.Clear(), SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr), Redraw();
    c.Text(L"Pins and notes stay", Rect(in.left + 160, row.bottom + 4, Width(in) - 160, 28), Weight::Light, 10.5f, t.textLow);
    float ay = row.bottom + 4 + 28 + 6;
    c.Text(L"Delete after", Rect(in.left, ay, Width(in), 14), Weight::Regular, 11.5f, t.textDim);
    static const wchar_t* const kAgeNames[clip::kAges] = {L"Never", L"1 h", L"6 h", L"24 h", L"7 d"};
    if (int k = setui::Segmented(ui, t, "clip.o.age", Rect(in.left, ay + 18, Width(in), 26), kAgeNames, clip::kAges, o.age); k >= 0) {
        o.age = k, save = true;
        if (st.Expire()) SetTimer(hwnd, kClipSaveTimer, kSaveMs, nullptr); // a shorter age takes effect at once
    }
    bool wipe = false;
    setui::Toggle(ui, t, "clip.o.wipe", Rect(in.left, ay + kAgeRow, Width(in), kSetupLineH), L"Clear unpinned on restart", o.wipe != 0, wipe);
    if (wipe) o.wipe ^= 1, save = true;

    in = setui::Box(c, t, Rect(x2, y + histH + kGap, half, keyH), L"Quick key");
    static const wchar_t* const kKeyNames[clip::kKeys] = {L"Off", L"Alt⇧V", L"Ctrl Alt V", L"Win Alt V", L"Ctrl⇧V"};
    if (int k = setui::Segmented(ui, t, "clip.o.key", Rect(in.left, in.top, Width(in), 26), kKeyNames, clip::kKeys, o.key); k >= 0) o.key = k, save = true;
    setui::Note(c, t, in, in.top + 30, clipKeyOn || !o.key ? L"Opens this panel from any app" : L"Taken by another app: pick another");

    float y2 = y + (std::max)(recH, histH + kGap + keyH) + kGap;
    in = setui::Box(c, t, Rect(view.left, y2, half, pasteH), L"Pasting");
    bool p[2] = {};
    setui::Toggle(ui, t, "clip.o.paste", Rect(in.left, in.top, Width(in), kSetupLineH), L"Paste into the app on pick", o.paste != 0, p[0]);
    setui::Toggle(ui, t, "clip.o.plain", Rect(in.left, in.top + kSetupLineH, Width(in), kSetupLineH), L"Always paste plain text", o.plain != 0, p[1]);
    setui::Note(c, t, in, in.top + 2 * kSetupLineH, L"Shift while picking: plain text once");
    if (p[0]) o.paste ^= 1, save = true;
    if (p[1]) o.plain ^= 1, save = true;

    in = setui::Box(c, t, Rect(x2, y2, half, lookH), L"Display");
    static const wchar_t* const kViews[] = {L"List", L"Grid"};
    static const wchar_t* const kSizes[] = {L"Compact", L"Normal", L"Large"};
    if (int k = setui::Segmented(ui, t, "clip.o.view", Rect(in.left, in.top, Width(in), 26), kViews, 2, o.view); k >= 0) o.view = k, save = true;
    if (int k = setui::Segmented(ui, t, "clip.o.size", Rect(in.left, in.top + 34, Width(in), 26), kSizes, 3, o.size); k >= 0) o.size = k, save = true;
    const wchar_t* themes[clip::kThemes];
    for (int i = 0; i < clip::kThemes; ++i) themes[i] = clip::ThemeName(i);
    c.Text(L"Colours for copied code", Rect(in.left, in.top + 68, Width(in), 14), Weight::Regular, 11.5f, t.textDim); // what these seven are: syntax colours, nothing to do with the dock's own look
    if (int k = setui::Segmented(ui, t, "clip.o.theme", Rect(in.left, in.top + 68 + kCodeLabelH, Width(in), 26), themes, clip::kThemes, o.theme); k >= 0) o.theme = k, save = true, clipLays.clear(), clipPeekId = 0;

    in = setui::Box(c, t, Rect(view.left, y2 + pasteH + kGap, half, neverH), L"Never keep");
    float cx = in.left;
    int unlist = -1;
    std::vector<std::wstring> all = o.noApps;
    all.insert(all.end(), o.noSites.begin(), o.noSites.end());
    for (size_t i = 0; i < all.size(); ++i) {
        float wdt = (std::min)(TextW(all[i], Weight::Medium, 11) + 34, Width(in));
        if (cx + wdt > in.right) break;
        D2D1_RECT_F chip = Rect(cx, in.top, wdt, 24);
        Ui::Hit h = ui.Button(Ui::Id("clip.o.never", (int)i), chip);
        c.Fill(chip, 12, t.text.Alpha(h.hover ? 0.12f : 0.07f));
        c.Text(all[i], Rect(chip.left + 10, chip.top, wdt - 30, 24), Weight::Medium, 11, t.textDim);
        c.Icon(glyph::Close, {chip.right - 12, chip.top + 12}, 13, h.hover ? Color::Rgb(0xFF6B6B) : t.textLow);
        if (h.clicked) unlist = (int)i;
        cx += wdt + 5;
    }
    if (all.empty()) c.Text(L"Nothing yet", Rect(in.left, in.top, Width(in), 24), Weight::Regular, 11.5f, t.textLow);
    setui::Note(c, t, in, in.top + 30, L"Right click an item: never keep its app or site");
    if (unlist >= 0) {
        if (unlist < (int)o.noApps.size()) o.noApps.erase(o.noApps.begin() + unlist);
        else o.noSites.erase(o.noSites.begin() + (unlist - (int)o.noApps.size()));
        save = true;
    }
    c.PopClip();
    if (save) st.SaveOptions(), SyncClip(), Redraw();
}
