// The dock's widgets: which ones are in it, in what order and size, and the little that each needs kept fresh.
// Every source starts only while its widget is in the dock: a listener, a 2 s tick, a fetch every few minutes, and nothing at all for the rest.
#include <winsock2.h>
#include <ws2ipdef.h>
#include <iphlpapi.h>
#include <netioapi.h>

#include "bar.h"

#include <shellapi.h>
#include <shlobj.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <random>

#include "glyphs.h"
#include "picker.h"
#include "widgets.h"

#pragma comment(lib, "iphlpapi.lib")

namespace {

constexpr int kFilesMax = 12; // downloads and folder items shown
constexpr float kXSize = 15; // edit mode's remove badge
constexpr float kDragStart = 6; // dips moved before a press is a drag

const wg::Meta kMeta[] = { // in Bar::Wg order
    {L"Now Playing", L"The song, its cover and the little bars", wg::kCatMedia, glyph::Music, 7, 1},
    {L"Recycle Bin", L"Open it, or empty it from its menu", wg::kCatMedia, glyph::Delete, 1, 0},
    {L"Weather", L"Now, the next hours and the week", wg::kCatTools, glyph::PartlyDay, 7, 1},
    {L"System Stats", L"Processor, memory and graphics load", wg::kCatSystem, glyph::Cpu, 7, 1},
    {L"Focus Timer", L"A pomodoro: work, then a short break", wg::kCatWork, glyph::SysStopwatch, 7, 0},
    {L"Drink Water", L"Glasses today, against your goal", wg::kCatWork, glyph::SysCup, 7, 0},
    {L"Clipboard", L"Everything you copied, a click to copy again", wg::kCatTools, glyph::SysPaste, 7, 0},
    {L"Color Picker", L"Any pixel's color, copied as hex", wg::kCatTools, glyph::SysPicker, 7, 0},
    {L"Screenshot", L"Windows' own snipping bar", wg::kCatMedia, glyph::Snip, 1, 0},
    {L"Emoji", L"Windows' emoji panel, into what you type", wg::kCatTools, glyph::SysEmoji, 1, 0},
    {L"Downloads", L"The newest files, a click away", wg::kCatMedia, glyph::SysDownload, 7, 0},
    {L"Notes", L"A quick note, kept between restarts", wg::kCatWork, glyph::SysNote, 7, 0},
    {L"Calendar", L"Your next event, and today's", wg::kCatWork, glyph::Calendar, 7, 1},
    {L"To-Do", L"A short list to tick off", wg::kCatWork, glyph::TaskAlt, 7, 1},
    {L"Clock", L"A flip clock, and the world's time", wg::kCatTools, glyph::Schedule, 7, 1},
    {L"Progress", L"How far the day, week and year are", wg::kCatWork, glyph::SysChart, 7, 1},
    {L"Quick Toggles", L"Wi-Fi, Bluetooth, quiet, night light", wg::kCatSystem, glyph::Tune, 6, 1},
    {L"Volume", L"A slider, and where the sound goes", wg::kCatSystem, glyph::VolumeUp, 7, 1},
    {L"Search", L"Find and start any app", wg::kCatTools, glyph::Search, 7, 0},
    {L"App Switcher", L"Every window, side by side", wg::kCatTools, glyph::TaskView, 1, 0},
    {L"Folder", L"A favourite folder's contents", wg::kCatMedia, glyph::SysFolder, 7, 0},
    {L"Network Speed", L"Down and up, right now", wg::kCatSystem, glyph::Down, 7, 1},
    {L"Battery", L"Charge, and whether it's charging", wg::kCatSystem, glyph::Battery, 7, 0},
    {L"Habits", L"Daily habits and their streaks", wg::kCatWork, glyph::Check, 7, 1},
    {L"", L"", wg::kCatTools, glyph::Bolt, 0, 0}, // retired, never offered
    {L"Photo", L"A picture from your Pictures folder", wg::kCatMedia, glyph::SysPhoto, 7, 1},
    {L"Divider", L"A line between widgets, as many as you like", wg::kCatTools, glyph::Drag, 1, 0},
    {L"Apps", L"Your pinned and open apps", wg::kCatTools, glyph::Apps, 1, 0}, // always there, never offered: it only moves
    {L"Drives", L"Your drives, their free space, eject", wg::kCatSystem, glyph::SysDrive, 1, 0},
    {L"Space", L"Empty room between widgets, any width", wg::kCatTools, glyph::More, 1, 0},
};
static_assert(sizeof(kMeta) / sizeof(kMeta[0]) == wg::kKinds, "a catalogue entry per widget");

std::wstring LocalFile(const wchar_t* name) { // %localappdata%\HaloBar\name
    PWSTR base = nullptr;
    std::wstring p;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) p = std::wstring(base) + L"\\HaloBar";
    CoTaskMemFree(base);
    if (!p.empty()) CreateDirectoryW(p.c_str(), nullptr), p += std::wstring(L"\\") + name;
    return p;
}

std::wstring ReadUtf8(const std::wstring& path) {
    std::wstring out;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return out;
    std::string buf(64 * 1024, '\0');
    DWORD got = 0;
    if (ReadFile(f, buf.data(), (DWORD)buf.size(), &got, nullptr) && got) {
        int n = MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)got, nullptr, 0);
        out.assign(n, L'\0');
        MultiByteToWideChar(CP_UTF8, 0, buf.data(), (int)got, out.data(), n);
    }
    CloseHandle(f);
    return out;
}

void WriteUtf8(const std::wstring& path, const std::wstring& text) {
    if (path.empty()) return;
    int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), nullptr, 0, nullptr, nullptr);
    std::string u(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), (int)text.size(), u.data(), n, nullptr, nullptr);
    HANDLE f = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD w = 0;
    WriteFile(f, u.data(), (DWORD)u.size(), &w, nullptr);
    CloseHandle(f);
}

}

namespace wg {

const Meta& Info(int kind) { return kMeta[(std::clamp)(kind, 0, wg::kKinds - 1)]; }

const wchar_t* CatName(int cat) {
    static const wchar_t* const kNames[] = {L"All", L"Media & Files", L"Productivity", L"System", L"Tools"};
    return kNames[(std::clamp)(cat, 0, (int)kCatCount - 1)];
}

}

int WgToday() { // yyyymmdd, local
    SYSTEMTIME st;
    GetLocalTime(&st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

int WgDayShift(int ymd, int days) { // the day so many days before or after
    SYSTEMTIME st = {(WORD)(ymd / 10000), (WORD)(ymd / 100 % 100), 0, (WORD)(ymd % 100)};
    FILETIME ft;
    SystemTimeToFileTime(&st, &ft);
    ULARGE_INTEGER u{{ft.dwLowDateTime, ft.dwHighDateTime}};
    u.QuadPart += (ULONGLONG)((LONGLONG)days * 864000000000LL);
    ft = {u.LowPart, u.HighPart};
    FileTimeToSystemTime(&ft, &st);
    return st.wYear * 10000 + st.wMonth * 100 + st.wDay;
}

// ---- which widgets, in what order ----

bool Bar::HasWg(int w) const {
    for (const WgSlot& s : wgSlots)
        if (s.kind == w) return true;
    return false;
}

constexpr float kGrip = 5; // half the space widget's edge handle, either side of its edge

void Bar::LoadSlots() {
    wgSlots.clear();
    std::wstring s = sys::ReadText(L"WidgetLayout"); // "kind:size,kind:size": the dock's order
    for (size_t i = 0; i < s.size();) {
        size_t e = s.find(L',', i);
        std::wstring part = s.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i);
        int k = -1, z = 1;
        if (swscanf_s(part.c_str(), L"%d:%d", &k, &z) == 2 && k >= 0 && k < kWgCount && k != kWgRetired && (Many(k) || !HasWg(k)))
            wgSlots.push_back({k, k == kWgSpace ? (std::clamp)(z, (int)kSpaceMin, (int)kSpaceMax) : (std::clamp)(z, 0, 2)}); // a space keeps its width
        if (e == std::wstring::npos) break;
        i = e + 1;
    }
    if (s.empty()) { // first run on this version: the song, the old widgets as they were picked, the bin
        wgSlots.push_back({kWgMedia, 1});
        for (int old = 0; old < 10; ++old)
            if ((cfg.widgets >> old) & 1) wgSlots.push_back({old + 2, (old == 0 || old == 1) ? 1 : 0}); // weather and stats were medium, the rest small
        wgSlots.push_back({kWgBin, 0});
    }
    if (!HasWg(kWgApps)) { // the apps were fixed ahead of the widgets once: now a block that moves like them, first by default
        bool more = !wgSlots.empty();
        wgSlots.insert(wgSlots.begin(), {kWgApps, 0});
        if (more) wgSlots.insert(wgSlots.begin() + 1, {kWgDivider, 0}); // where the fixed separator was
    }
    focusMin = (std::clamp)(prefs::ReadNum(L"WgFocusMin", 25), 5, 120);
    waterGoal = (std::clamp)(prefs::ReadNum(L"WgWaterGoal", 8), 1, 30);
    fahrenheit = prefs::ReadNum(L"WgFahrenheit", 0) != 0;
    folderPick = (std::clamp)(prefs::ReadNum(L"WgFolder", 0), 0, 4);
}

void Bar::SaveSlots() {
    std::wstring s;
    for (const WgSlot& w : wgSlots) s += (s.empty() ? L"" : L",") + std::to_wstring(w.kind) + L":" + std::to_wstring(w.size);
    sys::SaveText(L"WidgetLayout", s);
    SyncWidgets();
    Redraw();
}

void Bar::SyncWidgets() {
    std::fill(std::begin(fitW), std::end(fitW), 0.0f); // a widget added or resized: measured afresh
    SyncFit();
    SyncClip(); // the history loads, listens and takes its quick key only while the widget is in a dock
    if (HasWg(kWgStats) && !statSrc && !statMaking) { // opening the performance counters can take seconds: never on the ui thread
        statMaking = true;
        std::thread([h = hwnd] {
            auto* src = new sys::StatSrc();
            src->Sample(); // the first sample only sets the baseline
            if (!PostMessageW(h, WM_APP_STATSRC, 0, (LPARAM)src)) delete src;
        }).detach();
    } else if (!HasWg(kWgStats)) {
        statSrc.reset();
    }
    if (HasWg(kWgStats) || HasWg(kWgNet)) SetCoalescableTimer(hwnd, kStatsTimer, 2000, nullptr, 500); // both read every 2 s while in sight
    else KillTimer(hwnd, kStatsTimer);
    if (HasWg(kWgWeather)) RefreshWx(); // cached half an hour: a no-op most of the time
    if (HasWg(kWgWater)) water = prefs::ReadNum(L"WaterDay", 0) == WgToday() ? prefs::ReadNum(L"WaterCount", 0) : 0; // a new day starts at none
    if ((HasWg(kWgNotes)) && !noteLoaded) noteLoaded = true, note = ReadUtf8(LocalFile(L"notes.txt"));
    if ((HasWg(kWgTodo) || HasWg(kWgHabit)) && !listsLoaded) { // read once: the panels write them back
        listsLoaded = true;
        std::wstring t = ReadUtf8(LocalFile(L"todos.txt")), h = ReadUtf8(LocalFile(L"habits.txt"));
        for (size_t i = 0; i < t.size();) {
            size_t e = t.find(L'\n', i);
            std::wstring line = t.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i);
            if (line.size() > 2 && line[1] == L'\t') todos.push_back({line.substr(2), line[0] == L'1'});
            if (e == std::wstring::npos) break;
            i = e + 1;
        }
        for (size_t i = 0; i < h.size();) {
            size_t e = h.find(L'\n', i);
            std::wstring line = h.substr(i, e == std::wstring::npos ? std::wstring::npos : e - i);
            size_t a = line.find(L'\t'), b = a == std::wstring::npos ? a : line.find(L'\t', a + 1);
            if (b != std::wstring::npos) habits.push_back({line.substr(0, a), _wtoi(line.c_str() + a + 1), _wtoi(line.c_str() + b + 1)});
            if (e == std::wstring::npos) break;
            i = e + 1;
        }
    }
    if (HasWg(kWgPhoto) && !photoSrc) PickPhoto();
    if (!HasWg(kWgPhoto)) photoSrc.Reset(), photoBmp.Reset(); // gone: its pixels go too
    if (HasWg(kWgFiles) && files.empty()) LoadFiles(L""); // the newest download, for the medium and large faces
    if (HasWg(kWgDrives) && drives.empty()) LoadDrives(); // the count on its face
}

void Bar::WidgetTick() { // every 2 s, only while the dock or a panel shows: unseen numbers aren't read
    if (full || (sink > 0.5f && !wgOpen)) return;
    bool moved = false;
    if (statSrc) {
        sys::Stats s = statSrc->Sample();
        moved = std::lround(s.cpu * 40) != std::lround(stats.cpu * 40) || std::lround(s.mem * 40) != std::lround(stats.mem * 40); // a visible step of the bars
        stats = s;
        cpuHist[cpuAt] = (float)s.cpu, cpuAt = (cpuAt + 1) % 30;
    }
    if (HasWg(kWgNet)) {
        MIB_IF_TABLE2* tab = nullptr;
        if (GetIfTable2(&tab) == NO_ERROR && tab) {
            uint64_t in = 0, out = 0;
            for (ULONG i = 0; i < tab->NumEntries; ++i) {
                const MIB_IF_ROW2& r = tab->Table[i];
                if (r.Type == IF_TYPE_SOFTWARE_LOOPBACK || r.OperStatus != IfOperStatusUp || !r.InterfaceAndOperStatusFlags.HardwareInterface) continue; // real adapters only, counted once
                in += r.InOctets, out += r.OutOctets;
            }
            FreeMibTable(tab);
            double now = (double)GetTickCount64() / 1000, dt = now - netAt;
            if (netAt > 0 && dt > 0.2 && in >= netIn && out >= netOut) {
                double d = (in - netIn) / dt, u = (out - netOut) / dt;
                moved = moved || std::fabs(d - netDown) > 1024 || std::fabs(u - netUp) > 1024;
                netDown = d, netUp = u;
            }
            netIn = in, netOut = out, netAt = now;
        }
    }
    SyncFit();
    if (moved || (wgOpen && wgKind == kWgStats)) Redraw();
}

// ---- the sources ----

void Bar::OnPick(WPARAM rgb, LPARAM ok) {
    if (ok) pickHex = (int)(rgb & 0xFFFFFF), Redraw(); // the picker copied its hex already
}

void Bar::StepFocus() {
    KillTimer(hwnd, kFocusTimer);
    if (focusEnd <= 0) return;
    double left = focusEnd - (double)GetTickCount64() / 1000;
    if (left <= 0) { // done: a pill says so, and a soft chime
        focusEnd = 0;
        MessageBeep(MB_ICONASTERISK);
        pillNote = {};
        pillNote.app = L"Focus";
        pillNote.title = L"Session done";
        pillNote.body = L"Take five, then start another";
        pillUp = true;
        SetTimer(hwnd, kPillTimer, kPillMs, nullptr);
    } else {
        double toFlip = std::fmod(left, 60.0); // the shown minute changes then
        SetTimer(hwnd, kFocusTimer, (UINT)((toFlip < 0.5 ? 60.0 : toFlip) * 1000) + 50, nullptr);
    }
    Redraw();
}

void Bar::LoadFiles(const std::wstring& dirIn) { // the newest few in a folder, with their icons: read when asked, never polled
    files.clear();
    std::wstring base = dirIn;
    if (base.empty()) {
        PWSTR dir = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_Downloads, 0, nullptr, &dir))) return;
        base = dir;
        CoTaskMemFree(dir);
    }
    filesDir = base;
    struct Found {
        std::wstring name;
        FILETIME at;
    };
    std::vector<Found> all;
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        if (fd.cFileName[0] == L'.' || (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM))) continue;
        if (wcsstr(fd.cFileName, L".crdownload") || wcsstr(fd.cFileName, L".part") || !_wcsicmp(fd.cFileName, L"desktop.ini")) continue; // still arriving
        all.push_back({fd.cFileName, fd.ftLastWriteTime});
    } while (FindNextFileW(f, &fd));
    FindClose(f);
    size_t keep = (std::min)((size_t)kFilesMax, all.size());
    std::partial_sort(all.begin(), all.begin() + keep, all.end(), [](const Found& a, const Found& b) { return CompareFileTime(&a.at, &b.at) > 0; });
    UINT px = (UINT)std::lround(36 * scale);
    for (size_t i = 0; i < keep; ++i) {
        FileRow r{all[i].name, base + L"\\" + all[i].name, nullptr, nullptr};
        ComPtr<IShellItemImageFactory> img;
        HBITMAP hb = nullptr;
        if (SUCCEEDED(SHCreateItemFromParsingName(r.path.c_str(), nullptr, IID_PPV_ARGS(&img))) && SUCCEEDED(img->GetImage({(LONG)px, (LONG)px}, SIIGBF_BIGGERSIZEOK, &hb))) {
            r.pixels = gfx::FromShellBitmap(hb); // pictures and videos get their thumbnail, the rest their icon
            DeleteObject(hb);
        }
        files.push_back(std::move(r));
    }
}

void Bar::WgChar(wchar_t ch) {
    if (ch == VK_ESCAPE) return SetPanel(Panel::None);
    std::wstring& s = wgKind == kWgNotes ? note : draft;
    if (ch == VK_BACK) {
        if (!s.empty()) s.pop_back();
    } else if (ch == VK_RETURN && wgKind == kWgNotes) {
        s += L'\n';
    } else if (ch == VK_RETURN) { // a new to-do, or a new habit
        size_t a = draft.find_first_not_of(L' ');
        if (a != std::wstring::npos) {
            if (wgKind == kWgTodo) todos.push_back({draft.substr(a), false});
            else habits.push_back({draft.substr(a), 0, 0});
            SaveLists();
        }
        draft.clear();
    } else if (ch >= 32 && s.size() < 4000) {
        s += ch;
    }
    Redraw();
}

void Bar::SaveNote() { WriteUtf8(LocalFile(L"notes.txt"), note); }

void Bar::SaveLists() {
    std::wstring t, h;
    for (const Todo& d : todos) t += (d.done ? L"1\t" : L"0\t") + d.text + L"\n";
    for (const Habit& b : habits) h += b.name + L"\t" + std::to_wstring(b.last) + L"\t" + std::to_wstring(b.streak) + L"\n";
    WriteUtf8(LocalFile(L"todos.txt"), t);
    WriteUtf8(LocalFile(L"habits.txt"), h);
}

void Bar::PickPhoto() { // a random picture from Pictures and its first folders, decoded small on its own thread
    if (photoBusy.exchange(true)) return;
    UINT px = (UINT)std::lround(200 * scale);
    std::thread([h = hwnd, px] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        std::vector<std::wstring> found;
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &dir))) {
            std::vector<std::wstring> dirs{dir};
            for (size_t d = 0; d < dirs.size() && found.size() < 400; ++d) {
                WIN32_FIND_DATAW fd;
                HANDLE f = FindFirstFileExW((dirs[d] + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
                if (f == INVALID_HANDLE_VALUE) continue;
                do {
                    if (fd.cFileName[0] == L'.') continue;
                    std::wstring p = dirs[d] + L"\\" + fd.cFileName;
                    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                        if (d == 0 && dirs.size() < 24) dirs.push_back(p); // one level down, a few folders
                    } else if (const wchar_t* ext = wcsrchr(fd.cFileName, L'.'); ext && (!_wcsicmp(ext, L".jpg") || !_wcsicmp(ext, L".jpeg") || !_wcsicmp(ext, L".png") || !_wcsicmp(ext, L".webp"))) {
                        found.push_back(p);
                    }
                } while (FindNextFileW(f, &fd) && found.size() < 400);
                FindClose(f);
            }
        }
        CoTaskMemFree(dir);
        struct Pic {
            std::wstring path;
            ComPtr<IWICBitmapSource> src;
        };
        auto* pic = new Pic;
        if (!found.empty()) {
            std::mt19937 rng((unsigned)GetTickCount64());
            pic->path = found[rng() % found.size()];
            pic->src = gfx::DecodeFile(pic->path.c_str(), px);
        }
        if (!PostMessageW(h, WM_APP_PHOTO, 0, (LPARAM)pic)) delete pic;
        CoUninitialize();
    }).detach();
}

void Bar::OpenWidget(int w, float x) {
    if (wgOpen && wgKind == w) return SetPanel(Panel::None); // asked again: it closes
    static const KNOWNFOLDERID* const kFolders[] = {&FOLDERID_Documents, &FOLDERID_Desktop, &FOLDERID_Pictures, &FOLDERID_Music, &FOLDERID_Videos};
    if (w == kWgFiles) LoadFiles(L"");
    if (w == kWgDrives) LoadDrives(); // fresh as it opens
    if (w == kWgFolder) {
        PWSTR dir = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(*kFolders[folderPick], 0, nullptr, &dir))) LoadFiles(dir);
        CoTaskMemFree(dir);
    }
    if (w == kWgNotes && !noteLoaded) noteLoaded = true, note = ReadUtf8(LocalFile(L"notes.txt"));
    draft.clear();
    wgKind = w;
    Anchor(x, false); // over the dock, from the widget that asked
    SetPanel(Panel::Widget);
    if (Typing()) SetForegroundWindow(hwnd); // typing goes here while it's open, as the launcher
}

void Bar::StartEdit() {
    if (editing) return SetPanel(Panel::None); // asked again: done
    wgKind = kWgGallery;
    wgSel = -1, galleryScroll = 0;
    Anchor(W / 2, false);
    SetPanel(Panel::Widget);
    editing = true;
}

// ---- the group in the dock ----

void Bar::DrawWidgets(const Theme& t, float x, float top) { // x: where along the dock they start; top: across it
    Canvas& c = ui.C();
    auto tip = [&](const D2D1_RECT_F& over, const std::wstring& name) { // the widget's name, on a small pill beside it
        D2D1_RECT_F r = TipRect(over, (std::min)(TextW(name, Weight::Medium, 12) + 20, 300.0f));
        c.Fill(r, 12, t.bg.Alpha(1).Mix(t.text, 0.08f));
        c.StrokeV(r, 12, t.hairTop, t.hairBot, 1);
        c.Text(name, Inset(r, 10, 0), Weight::Medium, 12, t.text, Align::Center);
    };
    // in edit mode a held widget follows the mouse and the others make room where it would land
    int drop = -1;
    if (editing && dragSlot >= 0 && dragSlot < (int)wgSlots.size()) {
        float cx = x;
        drop = 0;
        for (int i = 0; i < (int)wgSlots.size(); ++i) {
            if (i == dragSlot) continue;
            float w = WidgetW(wgSlots[i].kind, wgSlots[i].size);
            if (Along(input.mouse) > cx + w / 2) drop = i < dragSlot ? i + 1 : i;
            cx += w + kWegGap;
        }
        drop = (std::clamp)(drop, 0, (int)wgSlots.size() - 1);
    }
    std::vector<int> order; // the slots in the order they show this frame
    for (int i = 0; i < (int)wgSlots.size(); ++i)
        if (i != dragSlot || drop < 0) order.push_back(i);
    if (drop >= 0) order.insert(order.begin() + drop, dragSlot);
    int removeAt = -1, pressAt = -1;
    std::vector<int> nth(wgSlots.size()); // dividers come many to a dock: each its own id, by which one it is
    for (int i = 0, d = 0; i < (int)wgSlots.size(); ++i) nth[(size_t)i] = Many(wgSlots[(size_t)i].kind) ? 1000 * ++d : 0;
    for (int i : order) {
        const WgSlot s = wgSlots[i];
        float w = WidgetW(s.kind, s.size);
        if (w <= 0 && s.kind == kWgMedia) continue; // the song switched off in settings: not drawn, no room kept
        int z = Vert() || s.kind == kWgSpace ? 0 : s.size; // a column holds every widget at its small face; a space's size is its width
        uint32_t id = Ui::Id("wg.slot", s.kind + nth[(size_t)i]);
        float sx = ui.Smooth(id + 5, x, 22); // the others slide when one moves
        if (editing && i == dragSlot) sx = Along(input.mouse) - wgGrab; // the held one rides the mouse
        D2D1_RECT_F r = Spot(sx, w, top);
        x += w + kWegGap;
        bool open = wgOpen && wgKind == s.kind;
        if (editing) {
            bool fixed = s.kind == kWgApps; // the apps move, but stay
            D2D1_POINT_2F bx = {r.left + 2, r.top + 2}; // remove, at its corner
            D2D1_RECT_F xb = Rect(bx.x - kXSize / 2, bx.y - kXSize / 2, kXSize, kXSize);
            Ui::Hit xh = fixed ? Ui::Hit{} : ui.Button(Ui::Id("wg.x", s.kind + nth[(size_t)i]), xb); // before the tile's: the first button under a press takes it
            if (s.kind == kWgSpace && dragSlot < 0) { // its far edge: grab and pull to make the room wider or narrower
                D2D1_RECT_F eg = Vert() ? Rect(r.left, r.bottom - kGrip, Width(r), kGrip * 2) : Rect(r.right - kGrip, r.top, kGrip * 2, Height(r));
                uint32_t gid = Ui::Id("wg.grip", s.kind + nth[(size_t)i]);
                Ui::Hit gh = ui.Button(gid, eg);
                if (ui.IsActive(gid) && input.pressed) spaceFrom = (int)w; // as it shows: maybe already cut for the apps
                if (ui.IsActive(gid) && input.down) {
                    int most = (std::clamp)((int)(w + spaceSlack), (int)kSpaceMin, (int)kSpaceMax); // never so wide an app goes to the "+N" tile
                    int want = (std::clamp)(spaceFrom + (int)std::lround(Along(input.mouse) - Along(input.pressPos)), (int)kSpaceMin, most);
                    if (want != wgSlots[i].size) wgSlots[i].size = want, Redraw();
                }
                if (gh.clicked || (ui.IsActive(gid) && input.released)) SaveSlots(); // kept as it's let go
                float gm = Vert() ? (eg.top + eg.bottom) / 2 : (eg.left + eg.right) / 2;
                Color ink = gh.hover || ui.IsActive(gid) ? t.accent : t.text.Alpha(0.35f);
                if (Vert()) c.Fill(Rect((r.left + r.right) / 2 - 8, gm - 1.5f, 16, 3), 1.5f, ink);
                else c.Fill(Rect(gm - 1.5f, (r.top + r.bottom) / 2 - 8, 3, 16), 1.5f, ink); // a small bar on the edge says it pulls
            }
            Ui::Hit h = ui.Button(id, r);
            DrawTile(t, r, 0.6f, 1);
            if (fixed) DrawApps(t, r, false);
            else DrawWg(t, s.kind, z, r, false);
            bool sel = wgSel == i;
            c.Stroke(Inset(r, -1, -1), TileRadius() + 1, t.accent.Alpha(sel ? 0.95f : 0.35f), sel ? 2.0f : 1.2f);
            if (!fixed) {
                c.Dot(bx, kXSize / 2, xh.hover ? Color::Rgb(0xFF6B6B) : t.bg.Alpha(1).Mix(t.text, 0.25f));
                c.Icon(glyph::Close, bx, 11, Color{1, 1, 1, 1});
            }
            if (xh.clicked) removeAt = i;
            else if (ui.IsActive(id) && input.pressed && !xh.hover) pressAt = i, wgGrab = Along(input.mouse) - (Vert() ? r.top : r.left);
            if (ui.IsActive(id) && input.down && dragSlot < 0 && std::fabs(Along(input.mouse) - Along(input.pressPos)) > kDragStart) dragSlot = i;
            if (h.clicked && dragSlot < 0) wgSel = i, galleryScroll = 0; // picked: its size and options show in the gallery
            if (h.hover && dragSlot < 0) tip(r, wg::Info(s.kind).name);
            continue;
        }
        if (s.kind == kWgApps) {
            DrawApps(t, r, true);
            continue;
        }
        if (s.kind == kWgSpace) continue; // just room: nothing drawn, nothing to hover or click
        if (s.kind == kWgDivider) { // just its hairline: nothing to hover or click
            DrawWg(t, s.kind, z, r, true);
            continue;
        }
        if (s.kind == kWgMedia) { // the song has its own buttons and tip
            DrawWg(t, s.kind, z, r, true);
            continue;
        }
        DrawTile(t, r, ui.Smooth(id + 1, Inside(r, input.mouse) || open ? 1.0f : 0, 18), 1);
        DrawWg(t, s.kind, z, r, true); // its own buttons first, toggles and mute: the first button under a press takes it
        Ui::Hit h = ui.Button(id, r);
        bool right = input.rclick && Inside(r, input.mouse);
        if (h.clicked || right) WgClick(s.kind, right, Mid(r));
        if (h.hover && !open) tip(r, WgTip(s.kind));
    }
    if (editing && dragSlot >= 0 && input.released) { // dropped: the order is kept
        if (drop >= 0 && drop != dragSlot) {
            WgSlot moving = wgSlots[dragSlot];
            wgSlots.erase(wgSlots.begin() + dragSlot);
            wgSlots.insert(wgSlots.begin() + drop, moving);
            wgSel = drop;
            SaveSlots();
        }
        dragSlot = -1;
    }
    if (pressAt < 0 && input.released) dragSlot = -1;
    if (removeAt >= 0) {
        wgSlots.erase(wgSlots.begin() + removeAt);
        wgSel = -1;
        SaveSlots();
    }
    if (editing) { // the gallery's handle in the dock: more widgets
        D2D1_RECT_F r = Spot(x, kIcon, top);
        Ui::Hit h = ui.Button(Ui::Id("wg.add"), r);
        c.Stroke(Inset(r, 3, 3), TileRadius(), t.accent.Alpha(h.hover ? 0.9f : 0.5f), 1.4f);
        c.Icon(glyph::Add, {(r.left + r.right) / 2, (r.top + r.bottom) / 2}, 20, t.accentHi);
        if (h.clicked) wgSel = -1, galleryScroll = 0;
        if (h.hover) tip(r, L"Add widgets");
    }
}

void Bar::LoadDrives() { // letters, labels and space: a few cheap calls, made only when asked
    drives.clear();
    DWORD mask = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(mask & (1u << i))) continue;
        wchar_t root[] = {(wchar_t)(L'A' + i), L':', L'\\', 0};
        UINT type = GetDriveTypeW(root);
        if (type != DRIVE_FIXED && type != DRIVE_REMOVABLE && type != DRIVE_REMOTE && type != DRIVE_CDROM) continue;
        DriveRow d;
        d.letter = root[0];
        d.removable = type == DRIVE_REMOVABLE || type == DRIVE_CDROM;
        wchar_t label[MAX_PATH] = {};
        UINT was = SetErrorMode(SEM_FAILCRITICALERRORS); // an empty card reader stays quiet
        GetVolumeInformationW(root, label, MAX_PATH, nullptr, nullptr, nullptr, nullptr, 0);
        ULARGE_INTEGER freeBytes{}, total{};
        bool ready = GetDiskFreeSpaceExW(root, &freeBytes, &total, nullptr) != FALSE;
        SetErrorMode(was);
        if (!ready && type == DRIVE_CDROM) continue; // no disc in it
        d.label = label[0] ? label : (type == DRIVE_REMOTE ? L"Network drive" : d.removable ? L"Removable drive" : L"Local disk");
        d.total = total.QuadPart, d.free = freeBytes.QuadPart;
        drives.push_back(d);
    }
}

void Bar::Eject(wchar_t letter) {
    wchar_t root[] = {letter, L':', L'\\', 0};
    ComPtr<IShellItem> item;
    ComPtr<IContextMenu> menu;
    if (FAILED(SHCreateItemFromParsingName(root, nullptr, IID_PPV_ARGS(&item))) || FAILED(item->BindToHandler(nullptr, BHID_SFUIObject, IID_PPV_ARGS(&menu)))) return;
    if (HMENU m = CreatePopupMenu()) {
        if (SUCCEEDED(menu->QueryContextMenu(m, 0, 1, 0x7FFF, CMF_NORMAL))) { // the verb only exists once the menu is built
            CMINVOKECOMMANDINFO ci = {sizeof(ci)};
            ci.lpVerb = "eject";
            ci.nShow = SW_SHOWNORMAL;
            menu->InvokeCommand(&ci);
        }
        DestroyMenu(m);
    }
}
