#pragma once
#include <windows.h>
#include <string>
#include <vector>

// everything the start menu can launch, for the bar's type-to-launch box. shell:AppsFolder is the one list that holds both packaged and desktop apps.
namespace sys {

struct AppEntry {
    std::wstring name;   // display name
    std::wstring parse;  // what to hand ShellExecuteEx, "shell:AppsFolder\<id>"
    std::wstring id;     // AppsFolder item id: aumid for packaged, path or known-folder id for desktop
    std::wstring lower;  // name lowercased once, so matching never allocates
};

std::vector<AppEntry> ListApps();  // blocking, worker-thread safe; sorted by name, deduped by id
bool LaunchApp(const AppEntry& a, bool admin = false);  // async shell launch, "runas" when admin
HBITMAP AppIcon(const AppEntry& a, int px);  // caller owns it; 32bpp premultiplied as the shell hands it; nullptr on failure
int Score(const AppEntry& a, const std::wstring& queryLower);  // higher is better, -1 is no match
std::vector<int> Search(const std::vector<AppEntry>& apps, const std::wstring& query, size_t max);  // best first; empty query, empty result

}
