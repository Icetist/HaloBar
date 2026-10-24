#include "apps.h"

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <algorithm>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>

namespace sys {
namespace {

constexpr int kFolderDepth = 3; // folders inside start folders, this deep

constexpr wchar_t kAppsPrefix[] = L"shell:AppsFolder\\"; // start's own folder: win32 shortcuts and store apps alike
constexpr DWORD kFetch = 64;  // items per IEnumShellItems::Next, fewer cross-apartment round trips

// score bands, far enough apart that the in-band bonus never crosses them
constexpr int kNamePrefix = 5000; // the name starts with the query: best of all
constexpr int kLeadInitials = 4000;  // "vs" -> Visual Studio: initials from the first word read as a name prefix
constexpr int kWordPrefix = 3000; // a later word starts with it, "code" -> visual studio code
constexpr int kInitials = 2500;      // initials skipping words, "vc" -> visual studio code
constexpr int kSubstring = 2000; // anywhere inside a word
constexpr int kSubseq = 1000; // scattered letters in order, the weakest match that still shows
constexpr int kBonusMax = 400;  // in-band nudge toward tighter matches

template <class T> struct Com {  // tiny raii holder, keeps the file free of wrl
    T* p = nullptr;
    Com() = default;
    Com(const Com&) = delete;
    Com& operator=(const Com&) = delete;
    ~Com() { if (p) p->Release(); }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

struct ComScope {  // init com only when the caller didn't; mismatched apartments still let us run
    bool owned = false;
    ComScope() {
        const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        owned = SUCCEEDED(hr);  // S_FALSE also needs the matching uninit
    }
    ~ComScope() { if (owned) CoUninitialize(); }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;
};

std::wstring Name(IShellItem* item, SIGDN kind) {
    PWSTR s = nullptr;
    if (FAILED(item->GetDisplayName(kind, &s)) || !s) return {}; // an empty name drops the item, never a blank row
    std::wstring out(s);
    CoTaskMemFree(s);
    return out;
}

std::wstring Lower(const std::wstring& s) {
    if (s.empty()) return {};
    std::wstring out(s);
    const int n = LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, s.c_str(), static_cast<int>(s.size()), // invariant: the same fold whatever the ui language
                                out.data(), static_cast<int>(out.size()), nullptr, nullptr, 0);
    if (n != static_cast<int>(s.size())) {  // length-changing mapping is not expected; fall back to per-char
        for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    }
    return out;
}

bool IsWordChar(wchar_t c) { return std::iswalnum(c) != 0; } // spaces, dashes, dots and brackets all split words

bool WordStart(const std::wstring& s, size_t i) {  // start of string or right after a separator; names are lowered so no camel humps
    if (i == 0) return true;
    return !IsWordChar(s[i - 1]) && IsWordChar(s[i]);
}

int Bonus(size_t nameLen, size_t at) {  // shorter names and earlier hits rank higher within a band
    const int len = static_cast<int>((std::min)(nameLen, size_t{200}));
    const int pos = static_cast<int>((std::min)(at, size_t{200}));
    return (std::max)(0, kBonusMax - len - pos); // both capped at 200 so the bonus stays inside its band
}

int InitialsMatch(const std::wstring& s, const std::wstring& q) {  // 2 if q is the leading initials, 1 if a subsequence of them, 0 if neither
    size_t qi = 0;
    bool lead = true;  // every word so far supplied the next query letter
    for (size_t i = 0; i < s.size() && qi < q.size(); ++i) {
        if (!WordStart(s, i)) continue; // only first letters of words count
        if (s[i] == q[qi]) ++qi;
        else lead = false; // a word was skipped: still initials, just not the leading run
    }
    if (qi != q.size()) return 0; // ran out of words before the query
    return lead ? 2 : 1;
}

int SubseqScore(const std::wstring& s, const std::wstring& q) {  // in-order letters; fewer gaps is better
    size_t first = 0;
    while (first < s.size() && !(s[first] == q[0] && WordStart(s, first))) ++first;  // anchor on a word start, else "spot" hits "support"
    if (first == s.size()) return -1; // -1: no match, hidden from results
    size_t qi = 1, last = first;
    for (size_t i = first + 1; i < s.size() && qi < q.size(); ++i) {
        if (s[i] != q[qi]) continue;
        last = i;
        ++qi;
    }
    if (qi != q.size()) return -1;
    const size_t span = last - first + 1; // how spread out the letters are; each gap costs 8
    return kSubseq + (std::max)(0, kBonusMax - static_cast<int>(span - q.size()) * 8 - static_cast<int>(s.size()));
}

std::wstring Trim(const std::wstring& s) {
    const size_t b = s.find_first_not_of(L" \t"); // stray spaces in the search box shouldn't change the results
    if (b == std::wstring::npos) return {};
    const size_t e = s.find_last_not_of(L" \t");
    return s.substr(b, e - b + 1);
}

std::wstring Fold(std::wstring s) {
    CharLowerBuffW(s.data(), (DWORD)s.size());
    return s;
}

void Shortcuts(const std::wstring& dir, const std::wstring& folder, std::unordered_map<std::wstring, std::wstring>& into, int depth) { // every shortcut under a start folder, by its name
    WIN32_FIND_DATAW fd;
    HANDLE f = FindFirstFileExW((dir + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, FIND_FIRST_EX_LARGE_FETCH);
    if (f == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth < kFolderDepth) Shortcuts(dir + L"\\" + n, folder, into, depth + 1); // a folder inside a folder: start shows its apps in the outer one
            continue;
        }
        size_t dot = n.rfind(L'.');
        if (dot == std::wstring::npos) continue;
        std::wstring ext = Fold(n.substr(dot));
        if (ext == L".lnk" || ext == L".url") into.emplace(Fold(n.substr(0, dot)), folder); // the shortcut's name is the name all apps lists
    } while (FindNextFileW(f, &fd));
    FindClose(f);
}

std::unordered_map<std::wstring, std::wstring> StartFolders() { // the folders start's all apps shows: the subfolders of both start menus, by app name
    std::unordered_map<std::wstring, std::wstring> out;
    for (REFKNOWNFOLDERID id : {FOLDERID_Programs, FOLDERID_CommonPrograms}) { // the user's own first: its name wins on a clash
        PWSTR root = nullptr;
        if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &root))) continue;
        std::wstring base = root;
        CoTaskMemFree(root);
        WIN32_FIND_DATAW fd;
        HANDLE f = FindFirstFileExW((base + L"\\*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
        if (f == INVALID_HANDLE_VALUE) continue;
        do {
            std::wstring n = fd.cFileName;
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN)) continue;
            if (Fold(n) == L"startup") continue; // what runs at sign-in, not a group start shows
            Shortcuts(base + L"\\" + n, n, out, 0);
        } while (FindNextFileW(f, &fd));
        FindClose(f);
    }
    return out;
}

void Collect(IEnumShellItems* items, std::vector<AppEntry>& out, std::unordered_set<std::wstring>& seen) {
    IShellItem* batch[kFetch] = {};
    for (;;) {
        ULONG got = 0;
        const HRESULT hr = items->Next(kFetch, batch, &got);
        for (ULONG i = 0; i < got; ++i) {
            IShellItem* item = batch[i];
            SFGAOF attrs = 0;
            const bool folder = SUCCEEDED(item->GetAttributes(SFGAO_FOLDER, &attrs)) && (attrs & SFGAO_FOLDER);
            if (!folder) { // start menu folders aren't apps; their contents already appear flat
                AppEntry e;
                e.name = Name(item, SIGDN_NORMALDISPLAY);
                e.id = Name(item, SIGDN_PARENTRELATIVEPARSING);  // the AppsFolder-relative id: aumid or path
                if (!e.name.empty() && !e.id.empty() && seen.insert(Lower(e.id)).second) { // the same app can be listed for user and machine
                    e.parse = kAppsPrefix + e.id;
                    e.lower = Lower(e.name);
                    out.push_back(std::move(e));
                }
            }
            item->Release(); // Next hands each item over with a reference we own
        }
        if (hr != S_OK || got == 0) break;  // S_FALSE means the last partial batch
    }
}

std::wstring Target(const AppEntry& a) noexcept {  // empty on bad_alloc, which callers treat as failure
    try {
        return !a.id.empty() ? kAppsPrefix + a.id : a.parse; // rebuilt from the id when there is one, so it's always canonical
    } catch (...) {
        return {};
    }
}

}

std::vector<AppEntry> ListApps() {
    std::vector<AppEntry> out;
    try {
        ComScope com;
        Com<IShellItem> folder;
        HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&folder)); // the virtual folder start's all apps list shows
        if (FAILED(hr)) {
            OutputDebugStringW(L"[apps] AppsFolder unavailable\n");
            return out;
        }
        Com<IEnumShellItems> items;
        hr = folder->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&items));
        if (FAILED(hr)) {
            OutputDebugStringW(L"[apps] AppsFolder enum failed\n");
            return out;
        }
        out.reserve(512); // a typical machine lists a few hundred
        std::unordered_set<std::wstring> seen;
        Collect(items.p, out, seen);
        std::sort(out.begin(), out.end(), [](const AppEntry& a, const AppEntry& b) {
            return a.lower != b.lower ? a.lower < b.lower : a.name < b.name;  // stable order for equal folds
        });
        const auto folders = StartFolders();
        for (AppEntry& e : out)
            if (auto it = folders.find(e.lower); it != folders.end()) e.folder = it->second; // packaged apps never match: start keeps them at the top level too
    } catch (...) {  // bad_alloc mid-enum: hand back what we have rather than unwind into a worker thread
        OutputDebugStringW(L"[apps] enumeration aborted\n");
    }
    return out;
}

bool LaunchApp(const AppEntry& a, bool admin) {
    const std::wstring target = Target(a);
    if (target.empty()) return false;
    AllowSetForegroundWindow(ASFW_ANY);  // the bar is foreground-less; let the new app take focus
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_ASYNCOK;  // never block the ui thread on the launch
    sei.lpVerb = admin ? L"runas" : nullptr; // runas asks for elevation; null takes the item's default verb
    sei.lpFile = target.c_str();
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) { // also false when the uac prompt is declined
        OutputDebugStringW(L"[apps] launch failed\n");
        return false;
    }
    return true;
}

HBITMAP AppIcon(const AppEntry& a, int px) {
    if (px <= 0) return nullptr;
    const std::wstring target = Target(a);
    if (target.empty()) return nullptr;
    ComScope com; // icons load on a worker thread that may not have com yet
    Com<IShellItemImageFactory> factory;
    if (FAILED(SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&factory)))) return nullptr;
    HBITMAP bmp = nullptr;
    if (FAILED(factory->GetImage(SIZE{px, px}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp))) return nullptr; // icon only: no file thumbnails
    return bmp; // the caller owns it and deletes it
}

int Score(const AppEntry& a, const std::wstring& q) {
    const std::wstring& s = a.lower;
    if (q.empty() || q.size() > s.size()) return -1;  // nothing longer than the name can match it
    if (s.compare(0, q.size(), q) == 0) return kNamePrefix + Bonus(s.size(), 0);
    const int initials = q.size() >= 2 ? InitialsMatch(s, q) : 0;  // one letter is a name or word prefix already
    if (initials == 2) return kLeadInitials + Bonus(s.size(), 0);
    size_t at = s.find(q);
    const size_t firstAny = at;
    while (at != std::wstring::npos) {  // a later hit may sit on a word start even if the first doesn't
        if (WordStart(s, at)) return kWordPrefix + Bonus(s.size(), at);
        at = s.find(q, at + 1);
    }
    if (initials == 1) return kInitials + Bonus(s.size(), 0);
    if (firstAny != std::wstring::npos) return kSubstring + Bonus(s.size(), firstAny);
    return SubseqScore(s, q); // last resort, or -1
}

std::vector<int> Search(const std::vector<AppEntry>& apps, const std::wstring& query, size_t max) {
    std::vector<int> out;
    try {
        const std::wstring q = Lower(Trim(query));
        if (q.empty() || max == 0) return out;
        struct Hit { int score; int index; };
        std::vector<Hit> hits;
        hits.reserve(apps.size()); // one pass over every app per keystroke; a few hundred is cheap
        for (size_t i = 0; i < apps.size(); ++i) {
            const int sc = Score(apps[i], q);
            if (sc >= 0) hits.push_back({sc, static_cast<int>(i)});
        }
        const auto better = [&](const Hit& x, const Hit& y) {
            if (x.score != y.score) return x.score > y.score;
            const auto& a = apps[static_cast<size_t>(x.index)].lower;
            const auto& b = apps[static_cast<size_t>(y.index)].lower;
            if (a.size() != b.size()) return a.size() < b.size(); // ties go to the shorter, more exact name
            return a < b; // then alphabetical, so the order never flickers between keystrokes
        };
        const size_t n = (std::min)(max, hits.size());
        std::partial_sort(hits.begin(), hits.begin() + static_cast<std::ptrdiff_t>(n), hits.end(), better);  // only the top few need ordering
        out.reserve(n);
        for (size_t i = 0; i < n; ++i) out.push_back(hits[i].index);
    } catch (...) {  // bad_alloc only; an empty result just shows no matches
        out.clear();
    }
    return out;
}

}
