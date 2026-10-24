// What sits beside the clock on the windows taskbar, grown out of the island instead of a flyout: network, sound and
// battery at a glance in the row; quick settings with radios, volume, brightness, Wi-Fi and Bluetooth lists and power;
// a calendar from the clock.
#include "bar.h"

#include <shellapi.h>

#include <cmath>
#include <thread>
#include <cwctype>

#include "glyphs.h"

namespace {

constexpr float kPad = 16, kTileH = 74, kGapY = 8, kPillH = 40, kFootH = 40; // island's quick settings grid: tile, row gap, slider, footer
constexpr int kQsCols = 4, kQsTiles = 14, kQsCommon = 8; // the first eight always show, the rest behind "more"
constexpr float kQsSetW = 140; // the named settings button in the footer
constexpr float kMoreH = 26; // windows' own set, four across
constexpr float kListHead = 52, kListRow = 40, kListFoot = 44, kMixRow = 50, kMixHead = 26; // a mixer row: the app's name over its own slider
constexpr int kListRows = 6; // networks, devices or outputs shown at once; the rest are in settings
constexpr float kCalHead = 40, kCalDow = 22, kCalCell = 34, kCalFoot = 44; // month title, weekday initials, one day, today's date
constexpr float kCalAgenda = 112; // the picked day's events, under the month: a head and three rows

}

void Keys(std::initializer_list<WORD> keys) { // a windows shortcut, as if typed: the flyouts windows keeps for itself
    INPUT in[8] = {}; // four keys down and up at most
    UINT n = 0;
    for (WORD k : keys) in[n].type = INPUT_KEYBOARD, in[n++].ki.wVk = k;
    for (auto it = std::rbegin(keys); it != std::rend(keys); ++it) in[n].type = INPUT_KEYBOARD, in[n].ki.wVk = *it, in[n++].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(n, in, sizeof(INPUT)); // one call, so nothing typed by the user can land in between
}

namespace {
void Open(const wchar_t* uri) { ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL); }
}

int WifiGlyph(const sys::Status& s) {
    if (!s.online && !s.wifi) return glyph::WifiNone;
    if (!s.wifi) return glyph::Lan; // online without wi-fi: a cable
    int bars = s.signal >= 80 ? 4 : s.signal >= 55 ? 3 : s.signal >= 30 ? 2 : 1; // never zero bars while connected
    return glyph::WifiBars[bars];
}

int BatteryGlyph(const sys::Status& s) {
    if (s.charging) return glyph::BatteryCharging;
    if (s.percent < 0) return glyph::Battery; // unknown level
    if (s.percent <= 10) return glyph::BatteryAlert;
    return s.percent >= 95 ? glyph::Battery : glyph::BatteryBars[(std::min)(6, s.percent * 7 / 100)]; // seven steps, then full
}

int VolumeGlyph(float level, bool muted) {
    if (muted || level <= 0.001f) return glyph::VolumeOff;
    return level < 0.34f ? glyph::VolumeMute : level < 0.67f ? glyph::VolumeDown : glyph::VolumeUp; // thirds, as the windows sound icon
}

namespace {

int BtGlyph(int kind) {
    switch (kind) {
    case sys::kBtAudio: return glyph::Headphones;
    case sys::kBtKeys: return glyph::Keys;
    case sys::kBtMouse: return glyph::Mouse;
    case sys::kBtPhone: return glyph::Phone;
    case sys::kBtPc: return glyph::Pc;
    case sys::kBtPad: return glyph::Pad;
    case sys::kBtWatch: return glyph::Watch;
    default: return glyph::Bluetooth; // a kind windows doesn't tell us
    }
}

}


void Bar::SyncAudio() {
    if (!spk) spk = std::make_unique<sys::AudioDev>(sys::AudioDev::Spk()); // made on first need, not at start
    if (!mic) mic = std::make_unique<sys::AudioDev>(sys::AudioDev::Micro());
    spk->Refresh(); // the default device may have changed since
    mic->Refresh();
}

void Bar::PollStatus() { // the slow timer: anything the bars show that windows doesn't tell us about by itself
    if (pollWake) SetEvent(pollWake); // network, battery and the bin are read on the poller: their answers come back as WM_APP_POLLED
    SyncDesktops();
    bool changed = false;
    if (spk) {
        spk->Refresh();
        float v = spk->Level();
        bool m = spk->Muted();
        changed = changed || std::fabs(v - volShown) > 0.004f || m != muteShown; // a volume set elsewhere, e.g. by the keyboard's keys
        volShown = v, muteShown = m;
    }
    std::wstring was = lang;
    SyncLang();
    if (int64_t s = sys::NotifStamp(); s != notifStamp) { // the notification store changed: something new may have come
        if (notifStamp && !calOpen) notifUnread = changed = true; // the first read only sets the baseline
        if (notifStamp) LoadNotifs(); // read it off the ui thread: the icons' counts follow
        notifStamp = s;
    }
    if (changed || lang != was) Redraw();
}

void Bar::StartPoller() { // one thread that sleeps until asked: the wlan, network list and recycle bin calls block for 30 to 150 ms each
    pollWake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!pollWake) return;
    poller = std::thread([this, wake = pollWake, h = hwnd] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); // the network list manager is com
        int round = 0;
        while (WaitForSingleObject(wake, INFINITE) == WAIT_OBJECT_0 && !pollStop) {
            auto* r = new Polled{sys::ReadStatus(), -1};
            if (binAsk.exchange(false) || round++ % 6 == 0) { // the bin each minute, or at once when asked: it walks every drive's bin
                SHQUERYRBINFO q = {sizeof(q)};
                r->bin = SUCCEEDED(SHQueryRecycleBinW(nullptr, &q)) && q.i64NumItems > 0 ? 1 : 0;
            }
            if (!PostMessageW(h, WM_APP_POLLED, 0, (LPARAM)r)) delete r; // the window went: nobody to tell
        }
        sys::CloseStatus();
        CoUninitialize();
    });
}

void Bar::StopPoller() {
    if (!poller.joinable()) return;
    pollStop = true;
    SetEvent(pollWake);
    poller.join();
    CloseHandle(pollWake), pollWake = nullptr;
}

void Bar::OnPolled(LPARAM lp) {
    std::unique_ptr<Polled> r(reinterpret_cast<Polled*>(lp));
    bool changed = !(r->status == status);
    status = std::move(r->status);
    if (r->bin >= 0 && (r->bin != 0) != binFull) binFull = r->bin != 0, changed = true;
    if (changed) SyncFit(), Redraw(); // the battery widget's words may have changed length
}

void Bar::SyncLang() {
    HKL list[16]; // more layouts than this is past what anyone switches between
    int n = GetKeyboardLayoutList(16, list);
    if (n < 2) { // one layout: nothing to switch, nothing to show
        lang.clear();
        return;
    }
    HKL hkl = GetKeyboardLayout(GetWindowThreadProcessId(GetForegroundWindow(), nullptr)); // the app being typed into
    uint64_t key = (uint64_t)(ULONG_PTR)hkl * 31 + n;
    for (int i = 0; i < n; ++i) key = key * 1099511628211ull ^ (uint64_t)(ULONG_PTR)list[i]; // fnv-style mix of the installed set
    if (key == langKey && !lang.empty()) return; // same layout as last look: every window switch asks
    langKey = key;
    wchar_t name[LOCALE_NAME_MAX_LENGTH] = {}, abbr[16] = {}, ctry[16] = {};
    if (LCIDToLocaleName(MAKELCID(LOWORD((ULONG_PTR)hkl), SORT_DEFAULT), name, LOCALE_NAME_MAX_LENGTH, 0) && // the low word is the language
        GetLocaleInfoEx(name, LOCALE_SISO639LANGNAME2, abbr, 16)) {
        lang = abbr;
        for (auto& ch : lang) ch = (wchar_t)towupper(ch); // "ENG" over "US", as windows shows it
        GetLocaleInfoEx(name, LOCALE_SISO3166CTRYNAME, ctry, 16);
        langSub = ctry; // the region under it, until a layout name is needed instead
    }
    // the layout's own name ("US", "United Kingdom", "Colemak"): from its id, or for a variant, the layout whose "Layout Id" it carries
    constexpr wchar_t kLayouts[] = L"SYSTEM\\CurrentControlSet\\Control\\Keyboard Layouts";
    WORD dev = HIWORD((ULONG_PTR)hkl); // the high word is the device: the layout itself
    std::wstring found;
    if ((dev & 0xF000) == 0xF000) { // 0xFxxx: a variant layout, named only by its layout id
        HKEY root = nullptr;
        if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, kLayouts, 0, KEY_READ, &root) == ERROR_SUCCESS) {
            wchar_t sub[16], lid[16];
            for (DWORD i = 0; found.empty(); ++i) {
                DWORD sn = 16, ln = sizeof(lid);
                if (RegEnumKeyExW(root, i, sub, &sn, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break; // the end of the list, or an error: no name then
                if (RegGetValueW(root, sub, L"Layout Id", RRF_RT_REG_SZ, nullptr, lid, &ln) == ERROR_SUCCESS && wcstoul(lid, nullptr, 16) == (dev & 0x0FFFu)) found = sub;
            }
            RegCloseKey(root);
        }
    } else {
        wchar_t klid[16];
        swprintf_s(klid, L"%08X", (unsigned)(dev ? dev : LOWORD((ULONG_PTR)hkl))); // the registry key is the klid in eight hex digits
        found = klid;
    }
    langName.clear();
    if (!found.empty()) {
        wchar_t text[128] = {};
        DWORD tn = sizeof(text);
        if (RegGetValueW(HKEY_LOCAL_MACHINE, (std::wstring(kLayouts) + L"\\" + found).c_str(), L"Layout Text", RRF_RT_REG_SZ, nullptr, text, &tn) == ERROR_SUCCESS) langName = text;
    }
    struct Known {
        const wchar_t* klid;
        const wchar_t* shortName;
    };
    static const Known kKnown[] = {{L"00000409", L"US"}, {L"00020409", L"INTL"}, {L"00010409", L"DVORAK"}, {L"00000809", L"UK"}, {L"00001809", L"IE"},
                                   {L"00001009", L"CA"}, {L"00000C09", L"AU"}, {L"00000407", L"DE"}, {L"0000040C", L"FR"}, {L"0000080C", L"BE"},
                                   {L"0000040A", L"ES"}, {L"00000410", L"IT"}, {L"00000416", L"BR"}, {L"00000816", L"PT"}, {L"00000439", L"IN"}}; // windows' own short names under the language
    for (const Known& k : kKnown)
        if (!_wcsicmp(found.c_str(), k.klid)) {
            langSub = k.shortName;
            return;
        }
    int same = 0; // several layouts for this same language and region: the region alone can't tell them apart, the layout's name can
    for (int i = 0; i < n; ++i) same += LOWORD((ULONG_PTR)list[i]) == LOWORD((ULONG_PTR)hkl);
    if (same > 1 && !langName.empty()) {
        std::wstring shortName; // "United Kingdom" is UK, "United States-International" USI; one word keeps its start, "Colemak"
        bool start = true;
        int words = 0;
        for (wchar_t ch : langName) {
            bool sep = ch == L' ' || ch == L'-' || ch == L'(' || ch == L')';
            if (!sep && start) shortName.push_back((wchar_t)towupper(ch)), ++words;
            start = sep;
        }
        if (words < 2) shortName = langName.substr(0, 6);
        langSub = shortName.substr(0, 5); // fits under the language in the row's narrow slot
    }
}

// ---- quick settings ----

float Bar::QuickRise() const {
    int rows = ((qsMore ? kQsTiles : kQsCommon) + kQsCols - 1) / kQsCols; // rounded up: a part row still takes a full row
    // the grid page: tiles, more, volume, brightness (none on desktops), footer
    if (qsPage == 0) return kPad + rows * (kTileH + kGapY) - 4 + kMoreH + kPillH + (brightPct >= 0 ? kPillH + 6 : 0) + 6 + kFootH + 10;
    size_t n = qsPage == 1 ? nets.size() : qsPage == 2 ? devs.size() : outs.size(); // a list page grows with its rows, up to kListRows
    if (qsPage == 3) return kListHead + (float)(std::min)(n, (size_t)kListRows) * kListRow + kMixHead + (float)MixFit() * kMixRow + kListFoot + 8;
    if (qsPage == 1 && !passFor.empty()) n += 1; // room for the password field
    return kListHead + (float)(std::min)(n, (size_t)kListRows) * kListRow + kListFoot + 8;
}

void Bar::OpenQuickPage(int page) {
    if (page != 1) EndPass(); // leaving the wi-fi list: no field left holding the keyboard
    qsPage = page; // 0 the grid, 1 wi-fi, 2 bluetooth, 3 sound
    if (page == 1) {
        wifi.Open(hwnd, WM_APP_WIFI); // starts a scan; posts as networks come in
        wifi.Snap(nets); // whatever is known already, so the list isn't empty while it scans
    } else {
        wifi.Close(); // no scanning while nobody looks
    }
    if (page == 2) {
        bt.Open(hwnd, WM_APP_BT);
        bt.Scan(true); // look for unpaired devices too, as windows' list does while open
        bt.Snap(devs);
    } else {
        bt.Scan(false); // discovery drains batteries and slows the radio: off as soon as the page goes
        bt.Close();
    }
    if (page == 3) outs = sys::ListOuts(outId); // also fills in which one is the default
    if (page == 3) { // outputs and, under them, every app's own level
        mixer.Open(); // sessions read now and once a second while it shows
        SetTimer(hwnd, kMixTimer, 1000, nullptr);
    } else if (mixer.IsOpen()) {
        KillTimer(hwnd, kMixTimer);
        mixer.Close();
        mixIcons.clear(); // their device bitmaps, made again next time
    }
    Redraw();
}

void Bar::DrawQuick(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, qsSpring.value);
    if (rise < 1 || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 40) / (QuickRise() - 40)); // the first 40 dips of growth stay empty, then contents fade in
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    D2D1_RECT_F area = {W / 2 - kQsW / 2 + kPad, pill.top + CallH() + kPad, W / 2 + kQsW / 2 - kPad, rowTop - 6};
    qsRight = false;
    if (qsPage == 0) QuickMain(t, area);
    else QuickList(t, area);
    if (!qsRight && ui.RClick(pill) && input.mouse.y < rowTop) qsPage ? OpenQuickPage(0) : SetPanel(Panel::None); // right click steps back: list, grid, closed
    rt->PopLayer();
}

void Bar::QuickMain(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    float tw = (Width(r) - (kQsCols - 1) * 10) / kQsCols, y = r.top; // 10 dips between tiles
    bool wifiOn = radios.WifiOn(), btOn = radios.BtOn();
    bool air = radios.HasWifi() && !wifiOn && (!radios.HasBt() || !btOn); // shown as airplane mode when every radio is off
    std::wstring wifiSub = !radios.HasWifi() ? (status.online ? L"Ethernet" : L"No adapter") : !wifiOn ? L"Off" : status.wifi ? status.ssid : L"Not connected";
    bool micOff = mic && mic->Ready() && mic->Muted(); // no microphone reads as on, with the tile disabled
    bool saver = sys::SaverOn(), night = sys::NightLightOn();
    int k = 0;
    auto tile = [&](const char* id, int glyph, const wchar_t* label, const std::wstring& sub, bool on, bool enabled) { // the next cell of the grid
        D2D1_RECT_F cell = Rect(r.left + (k % kQsCols) * (tw + 10), y + (k / kQsCols) * (kTileH + kGapY), tw, kTileH);
        ++k;
        lastTile = cell; // for drawing over a tile after it, as the cc badge does
        return w::Tile(ui, t, Ui::Id(id), cell, glyph, label, sub, on, enabled);
    };
    auto jump = [&](int page, int mod) { // right click: that page, moved under its own module in the bar if it has one there, else right here
        qsRight = true;
        if (modAt[mod] >= 0) Anchor(modAt[mod], TopOn());
        OpenQuickPage(page);
    };
    if (tile("qs.wifi", status.wifi ? glyph::WifiBars[4] : glyph::Wifi, L"Wi-Fi", wifiSub, wifiOn, radios.HasWifi())) OpenQuickPage(1);
    if (ui.RClick(lastTile) && radios.HasWifi()) jump(1, kModNet);
    if (tile("qs.bt", btOn ? glyph::Bluetooth : glyph::BluetoothOff, L"Bluetooth", btOn ? L"On" : L"Off", btOn, radios.HasBt())) OpenQuickPage(2);
    if (ui.RClick(lastTile) && radios.HasBt()) jump(2, kModBt);
    if (tile("qs.air", air ? glyph::Airplane : glyph::AirplaneOff, L"Airplane", air ? L"On" : L"Off", air, radios.HasWifi() || radios.HasBt())) { // every radio off, or back on
        radios.SetWifi(air); // air was on: the radios come back
        radios.SetBt(air);
    }
    bool hotBusy = hotspotSwitching && hotspot.Busy(); // a re-read as the panel opens keeps showing what was last known
    if (tile("qs.hot", 0xE1E2, L"Hotspot", hotBusy ? L"Working\u2026" : hotspot.On() ? hotspot.Name() : L"Off", hotspot.On(), hotspot.Available() && !hotBusy))
        hotspotSwitching = true, hotspot.Set(!hotspot.On(), hwnd, WM_APP_HOTSPOT); // windows' own tethering, shared from this pc's connection
    if (tile("qs.saver", 0xEA35, L"Energy saver", saver ? L"On" : L"Off", saver, true) && !sys::SetSaver(!saver)) SetPanel(Panel::None); // couldn't switch it here: settings opened instead
    if (tile("qs.night", 0xF03D, L"Night light", night ? L"On" : L"Off", night, sys::NightLightAvailable())) sys::SetNightLight(!night);
    if (tile("qs.mic", micOff ? glyph::MicOff : glyph::Mic, L"Microphone", micOff ? L"Muted" : L"On", !micOff, mic && mic->Ready())) mic->SetMuted(!micOff);
    if (tile("qs.dnd", glyph::Dnd, L"Do not disturb", dnd ? L"On" : L"Off", dnd, true)) SetQuiet(!dnd); // the same switch as the calendar's
    if (qsMore) { // the ones used now and then, behind "more"
    if (tile("qs.cc", 0, L"Live captions", L"Win+Ctrl+L", false, true)) SetPanel(Panel::None), sys::LiveCaptions();
    { // the icon font has no captions glyph: a small cc badge where the icon sits
        D2D1_RECT_F b = Rect(lastTile.left + 14, lastTile.top + 17, 21, 15);
        c.Stroke(b, 3.5f, t.text, 1.5f);
        c.Text(L"CC", b, Weight::Semi, 8, t.text, Align::Center);
    }
    if (tile("qs.cast", glyph::Cast, L"Cast", L"To a screen", false, true)) SetPanel(Panel::None), Keys({VK_LWIN, 'K'}); // windows' own cast flyout has no public api
    if (tile("qs.proj", 0xE0E2, L"Project", L"Display mode", false, true)) SetPanel(Panel::None), sys::Project();
    if (tile("qs.near", glyph::Devices, L"Nearby share", L"Settings", false, true)) SetPanel(Panel::None), sys::NearbySharing();
    if (tile("qs.acc", 0xE92C, L"Accessibility", L"Settings", false, true)) SetPanel(Panel::None), sys::Accessibility();
    if (tile("qs.snip", glyph::Snip, L"Snip", L"Screenshot", false, true)) SetPanel(Panel::None), sys::OpenSnip();
    }
    y += ((k + kQsCols - 1) / kQsCols) * (kTileH + kGapY) - 4; // past the grid, however many rows it took
    { // more or less, as one quiet line
        D2D1_RECT_F ex = Rect(r.left + Width(r) / 2 - 50, y, 100, kMoreH - 4);
        Ui::Hit h = ui.Button(Ui::Id("qs.more.tiles"), ex);
        float hov = ui.Smooth(Ui::Id("qs.more.tiles", 1), h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(ex, Height(ex) / 2, t.text.Alpha(0.08f * hov));
        float flip = ui.Smooth(Ui::Id("qs.more.tiles", 2), qsMore ? 1.0f : 0, 14), d = 2.5f * (1 - 2 * flip); // the chevron turns over as it opens
        float cy = (ex.top + ex.bottom) / 2, cx = ex.left + 70;
        c.Text(qsMore ? L"Less" : L"More", Rect(ex.left + 14, ex.top, 46, Height(ex)), Weight::Medium, 11.5f, t.textDim.Mix(t.text, hov), Align::Right);
        c.Line({cx - 4, cy - d}, {cx, cy + d}, t.textDim.Mix(t.text, hov), 1.6f);
        c.Line({cx, cy + d}, {cx + 4, cy - d}, t.textDim.Mix(t.text, hov), 1.6f);
        if (h.clicked) qsMore = !qsMore; // the panel's spring follows QuickRise()
        y += kMoreH;
    }

    if (spk && spk->Ready()) { // volume: tap the icon to mute, the arrow picks where sound goes
        float v = spk->Level();
        bool tap = false, muted = spk->Muted();
        D2D1_RECT_F pr = Rect(r.left, y, Width(r) - 34, kPillH); // room on the right for the outputs arrow
        if (w::Pill(ui, t, Ui::Id("qs.vol"), pr, VolumeGlyph(v, muted), v, &tap, muted)) spk->SetLevel(v), volShown = v;
        if (tap) spk->SetMuted(!muted), muteShown = !muted; // the row's glyph follows at once
        if (w::IconBtn(ui, t, Ui::Id("qs.outs"), {r.right - 15, y + kPillH / 2}, 30, glyph::ChevronRight, 20)) OpenQuickPage(3);
        if (ui.RClick(pr)) jump(3, kModVol); // the sound page, under the volume module
        y += kPillH + 6;
    }
    if (brightPct >= 0) { // negative: no display whose brightness can be set, e.g. an external monitor
        float b = brightPct / 100.0f;
        if (w::Pill(ui, t, Ui::Id("qs.bright"), Rect(r.left, y, Width(r) - 34, kPillH), glyph::Bright, b, nullptr)) {
            brightPct = (int)std::lround(b * 100);
            bright.Set(brightPct); // whole percents, as windows' own slider
        }
        y += kPillH + 6;
    }

    float fy = r.bottom - kFootH; // battery, then settings and power
    c.Line({r.left, fy}, {r.right, fy}, t.text.Alpha(0.06f), 1);
    float cy = fy + kFootH / 2 + 2;
    if (powerRow) { // the session: lock, sleep, sign out, restart, shut down
        struct P {
            int glyph;
            void (*run)();
        };
        const P ps[] = {{glyph::Lock, sys::power::Lock}, {glyph::Sleep, sys::power::Sleep}, {glyph::SignOut, sys::power::SignOut},
                        {glyph::Restart, sys::power::Restart}, {glyph::Power, sys::power::Shutdown}};
        for (int i = 0; i < 5; ++i)
            if (w::IconBtn(ui, t, Ui::Id("qs.pw", i), {r.left + 18 + i * 40.0f, cy}, 32, ps[i].glyph, 19)) {
                SetPanel(Panel::None); // closed first: lock and sleep would otherwise come back to it open
                ps[i].run();
            }
    } else if (status.battery) {
        c.Icon(BatteryGlyph(status), {r.left + 12, cy}, 19, status.saver ? t.accentHi : t.textDim);
        std::wstring s = status.percent >= 0 ? std::to_wstring(status.percent) + L"%" : L"Battery"; // a battery that won't report its level
        if (status.charging) s += L"  ·  charging";
        else if (status.saver) s += L"  ·  saver on";
        c.Text(s, Rect(r.left + 28, cy - 9, 200, 18), Weight::Medium, 12, t.textDim);
    } else {
        c.Text(status.online ? (status.wifi ? status.ssid : std::wstring(L"Connected")) : std::wstring(L"Offline"), Rect(r.left + 4, cy - 9, 220, 18),
               Weight::Medium, 12, t.textDim);
    }
    D2D1_RECT_F set = Rect(r.right - 36 - kQsSetW, cy - 15, kQsSetW, 30); // halobar's own settings, named: a bare gear reads as windows'
    if (LabelButton(t, Ui::Id("qs.set"), set, glyph::Settings, L"HaloBar settings")) SetSettingsOpen(true);
    if (ui.RClick(set)) SetPanel(Panel::None), Open(L"ms-settings:"); // right click: windows' settings
    // the power button swaps the footer for the session buttons
    if (w::IconBtn(ui, t, Ui::Id("qs.power"), {r.right - 16, cy}, 32, powerRow ? glyph::Close : glyph::PowerNew, 19, true, powerRow)) powerRow = !powerRow;
}

void Bar::BeginPass(const sys::WifiNet& n) {
    EndPass(); // one field at a time: another network's half-typed key is wiped
    passFor = n.raw;
    pass.reserve(64); // room up front: no reallocation leaves copies of it behind
    wifi.ForgetWrongKey(); // a new attempt: the old "wrong password" note goes
    SetForegroundWindow(hwnd); // typing needs the keyboard; it goes back to the app when the field closes
    SetFocus(hwnd);
    Redraw();
}

void Bar::EndPass() {
    if (passFor.empty() && pass.empty()) return; // no field open: leave the foreground alone
    if (!pass.empty()) SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t)); // not optimised away, unlike a plain memset
    pass.clear();
    passFor.clear();
    HWND back = tasks ? tasks->Fore() : nullptr;
    if (GetForegroundWindow() == hwnd && back && IsWindow(back)) SetForegroundWindow(back); // the keyboard goes back to the app it was taken from
    Redraw();
}

void Bar::SubmitPass() {
    for (const auto& n : nets)
        if (n.raw == passFor) {
            std::wstring key = pass; // join zeroes its copy, ours goes in EndPass
            wifi.Join(n, key);
            break;
        }
    EndPass();
}

float Bar::MixRows(const Theme& t, const D2D1_RECT_F& r, float y, int rows) {
    Canvas& c = ui.C();
    c.Text(L"APPS", Rect(r.left + 2, y + 6, 100, 12), Weight::Medium, 9.5f, t.textLow);
    std::wstring dev = mixer.Device();
    if (!dev.empty()) c.Text(dev, Rect(r.left + 60, y + 4, Width(r) - 60, 14), Weight::Light, 10.5f, t.textLow, Align::Right); // whose sessions these are
    y += kMixHead;
    const auto& apps = mixer.Apps();
    if (apps.empty()) c.Text(L"Nothing is making sound", Rect(r.left, y + 14, Width(r), 18), Weight::Light, 12.5f, t.textLow, Align::Center);
    for (int i = 0; i < (int)apps.size() && i < rows; ++i) {
        const auto& a = apps[i];
        float cy = y + kMixRow / 2;
        D2D1_RECT_F ic = Rect(r.left + 2, cy - 12, 24, 24);
        ComPtr<ID2D1Bitmap>& bmp = mixIcons[a.key]; // made once per session, kept while the page shows
        if (!bmp && a.icon && ctx) {
            D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            ctx->CreateBitmap({32, 32}, a.icon->data(), 32 * 4, bp, &bmp); // the mixer reads every icon at 32 px, premultiplied
        }
        if (bmp) c.Image(bmp.Get(), ic, 0, a.muted ? 0.4f : 1);
        else c.Icon(a.key == L"system" ? glyph::Bell : glyph::Speaker, {ic.left + 12, cy}, 19, t.textDim); // system sounds have no exe to take an icon from
        c.Text(a.name, Rect(r.left + 36, y + 2, Width(r) - 90, 15), Weight::Medium, 11.5f, a.playing ? t.text : t.textDim);
        float v = a.volume;
        bool tap = false;
        if (w::Pill(ui, t, Ui::Id("qs.mixv", i), Rect(r.left + 36, y + 19, Width(r) - 36, 26), VolumeGlyph(v, a.muted), v, &tap, a.muted)) mixer.SetVolume(a.key, v);
        if (tap) mixer.SetMuted(a.key, !a.muted);
        y += kMixRow;
    }
    return y + (apps.empty() ? kMixRow : 0); // the empty note takes a row's room
}

int Bar::MixFit() const { // as many app rows as fit under the outputs in the window's room
    float room = kRoom - kRowH - 40 - (kListHead + (float)(std::min)(outs.size(), (size_t)kListRows) * kListRow + kMixHead + kListFoot + 8);
    int fit = (std::max)(1, (int)(room / kMixRow)); // at least one, even when the room is tight
    return (std::clamp)((int)mixer.Apps().size(), 1, fit);
}

void Bar::QuickList(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    static const wchar_t* const kTitles[] = {L"", L"Wi-Fi", L"Bluetooth", L"Sound"}; // by qsPage; 0 is the grid, never a list
    if (w::IconBtn(ui, t, Ui::Id("qs.back"), {r.left + 12, r.top + 12}, 30, glyph::ChevronLeft, 20)) {
        OpenQuickPage(0);
        return; // the grid draws from the next frame
    }
    c.Text(kTitles[qsPage], Rect(r.left + 32, r.top + 2, 200, 20), Weight::Medium, 14, t.text);
    if (qsPage == 1 && radios.HasWifi() && w::Switch(ui, t, Ui::Id("qs.wifisw"), {r.right - 25, r.top + 12}, radios.WifiOn())) radios.SetWifi(!radios.WifiOn());
    if (qsPage == 2 && radios.HasBt() && w::Switch(ui, t, Ui::Id("qs.btsw"), {r.right - 25, r.top + 12}, radios.BtOn())) radios.SetBt(!radios.BtOn());
    float y = r.top + kListHead - kPad;

    D2D1_RECT_F lastRow{};
    auto row = [&](int i, int glyph, const std::wstring& name, const std::wstring& sub, bool on) {
        D2D1_RECT_F rr = Rect(r.left - 6, y, Width(r) + 12, kListRow - 4);
        lastRow = rr;
        uint32_t id = Ui::Id("qs.row", i);
        Ui::Hit h = ui.Button(id, rr);
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        if (on) c.Fill(rr, 12, t.accent.Alpha(0.14f)); // the connected one sits on the accent
        if (hov > 0.01f) c.Fill(rr, 12, t.text.Alpha(0.09f * hov));
        c.Icon(glyph, {rr.left + 20, (rr.top + rr.bottom) / 2}, 19, on ? t.accentHi : t.textDim, on);
        float cy = (rr.top + rr.bottom) / 2;
        if (sub.empty()) { // one line, centred: the outputs list
            c.Text(name, Rect(rr.left + 40, cy - 9, Width(rr) - 50, 18), Weight::Medium, 12.5f, t.text);
        } else {
            c.Text(name, Rect(rr.left + 40, cy - 15, Width(rr) - 50, 16), Weight::Medium, 12.5f, t.text);
            c.Text(sub, Rect(rr.left + 40, cy + 1, Width(rr) - 50, 14), Weight::Light, 10.5f, on ? t.accentHi : t.textDim);
        }
        y += kListRow;
        return h.clicked;
    };

    const wchar_t* more = L"";
    const wchar_t* moreUri = L"";
    if (qsPage == 1) {
        for (int i = 0; i < (int)nets.size() && i < kListRows; ++i) {
            const auto& n = nets[i];
            bool wrong = !n.raw.empty() && wifi.WrongKey() == n.raw; // the last join to this network was refused
            std::wstring sub = n.pending ? L"Connecting…" : n.on ? L"Connected" : wrong ? L"Wrong password, try again" : !n.profile.empty() ? L"Saved  ·  right-click to forget"
                             : n.secured ? L"Secured" : L"Open";
            int level = n.signal >= 80 ? 4 : n.signal >= 55 ? 3 : n.signal >= 30 ? 2 : 1;
            if (row(i, glyph::WifiBars[level], n.ssid.empty() ? L"Hidden network" : n.ssid, sub, n.on)) {
                if (n.on) wifi.Disconnect(); // clicking the connected one lets it go
                else if (wifi.Connect(n)) EndPass(); // saved or open: straight on
                else if (wifi.CanJoin(n)) passFor == n.raw ? EndPass() : BeginPass(n); // asks for its password right here
                else SetPanel(Panel::None), Open(L"ms-availablenetworks:"); // enterprise and the like: windows' own sign-in
            }
            if (!n.profile.empty() && !n.on && ui.RClick(lastRow)) wifi.Forget(n); // as windows' forget
            if (!passFor.empty() && passFor == n.raw) { // the field, right under its network
                D2D1_RECT_F f = Rect(r.left + 34, y - 2, Width(r) - 34 - 74, 34);
                c.Fill(f, 17, t.text.Alpha(0.07f));
                c.Stroke(f, 17, t.accent.Alpha(0.6f), 1.2f);
                std::wstring dots(pass.size(), L'\u2022'); // never drawn as text: only its length shows
                float tw = TextW(dots.empty() ? L" " : dots, Weight::Medium, 13);
                if (dots.empty()) c.Text(L"Password", Rect(f.left + 14, f.top, Width(f) - 20, Height(f)), Weight::Light, 12.5f, t.textLow);
                else c.Text(dots, Rect(f.left + 14, f.top, Width(f) - 20, Height(f)), Weight::Medium, 13, t.text);
                if (Caret()) c.Fill(Rect(f.left + 14 + (dots.empty() ? 0 : tw) + 1, f.top + 9, 1.5f, Height(f) - 18), 0.75f, t.accentHi); // caret
                D2D1_RECT_F go = Rect(f.right + 8, f.top, 66, Height(f));
                Ui::Hit gh = ui.Button(Ui::Id("qs.join"), go);
                c.Fill(go, Height(go) / 2, pass.size() >= 8 ? t.accent : t.text.Alpha(0.08f)); // wpa keys are 8 characters at least
                c.Text(L"Join", go, Weight::Medium, 12.5f, pass.size() >= 8 ? t.onAccent : t.textLow, Align::Center);
                if (gh.clicked && pass.size() >= 8) SubmitPass();
                y += kListRow;
            }
        }
        more = L"More Wi-Fi settings", moreUri = L"ms-settings:network-wifi";
    } else if (qsPage == 2) {
        for (int i = 0; i < (int)devs.size() && i < kListRows; ++i) {
            const auto& d = devs[i];
            std::wstring sub = d.pairing ? L"Pairing…" : d.until ? L"Working…" : d.on ? L"Connected" : d.paired ? L"Paired" : L"Tap to pair";
            if (d.battery >= 0) sub += L"  ·  " + std::to_wstring(d.battery) + L"%"; // only devices that report it, mostly headphones
            if (row(i, BtGlyph(d.kind), d.name, sub, d.on)) {
                if (d.paired) bt.Connect(d.id, !d.on); // a toggle: connect or disconnect
                else bt.Pair(d.id); // windows may show its own pin prompt
            }
        }
        more = L"More Bluetooth settings", moreUri = L"ms-settings:bluetooth";
    } else {
        for (int i = 0; i < (int)outs.size() && i < kListRows; ++i) {
            const auto& o = outs[i];
            bool on = o.id == outId;
            if (row(i, o.headphones ? glyph::Headphones : glyph::Speaker, o.name, L"", on) && !on && sys::SetOut(o.id)) {
                outId = o.id;
                if (spk) spk->Refresh(); // the volume slider now talks to the new device
                meter.Refresh(); // and the visualizer listens to it
            }
        }
        y = MixRows(t, r, y, MixFit()); // and every app's own level, island's mixer
        more = L"Sound settings", moreUri = L"ms-settings:sound";
    }
    D2D1_RECT_F foot = Rect(r.left - 6, r.bottom - 34, Width(r) + 12, 32);
    Ui::Hit h = ui.Button(Ui::Id("qs.more"), foot);
    float hov = ui.Smooth(Ui::Id("qs.more", 1), h.hover ? 1.0f : 0, 16);
    if (hov > 0.01f) c.Fill(foot, 12, t.card.Alpha(hov));
    c.Text(more, Rect(foot.left + 14, foot.top, Width(foot) - 40, Height(foot)), Weight::Medium, 12, t.accentHi);
    c.Icon(glyph::OpenNew, {foot.right - 18, (foot.top + foot.bottom) / 2}, 16, t.accentHi);
    if (h.clicked) SetPanel(Panel::None), Open(moreUri);
}

// ---- calendar ----

float Bar::CalRise() const { return kPad + kCalHead + kCalDow + 6 * kCalCell + kCalAgenda + kCalFoot + 4; }

void Bar::CountUnread() {
    unread.clear();
    int cleared = prefs::ReadNum(L"NotifCleared", 0);
    for (const auto& n : notifs) {
        if (n.id <= cleared || notifGone.count(n.id) || n.aumid.empty()) continue;
        std::wstring k = n.aumid;
        for (auto& ch : k) ch = (wchar_t)towlower(ch);
        auto seen = notifSeen.find(k);
        if (seen != notifSeen.end() && n.id <= seen->second) continue;
        ++unread[k];
    }
}

void Bar::SeeNotifs(const std::wstring& appKey) {
    int64_t top = 0;
    for (const auto& n : notifs) top = (std::max)(top, n.id);
    if (appKey.empty()) { // the notifications were looked at: every count clears
        for (const auto& n : notifs) {
            std::wstring k = n.aumid;
            for (auto& ch : k) ch = (wchar_t)towlower(ch);
            notifSeen[k] = top;
        }
    } else {
        notifSeen[appKey] = top;
    }
    CountUnread();
}

void Bar::LoadNotifs() {
    if (notifLoading) return;
    notifLoading = true;
    HWND to = hwnd;
    std::thread([to] { // the first read asks the shell for app names: far too slow for the ui thread
        HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        bool ok = false;
        std::vector<sys::Notif> read = sys::ReadNotifs(40, &ok);
        auto* got = ok ? new std::vector<sys::Notif>(std::move(read)) : nullptr; // windows busy writing its store: no answer, the list shown stays as it was
        if (SUCCEEDED(co)) CoUninitialize();
        if (!PostMessageW(to, WM_APP_NOTIFS, 0, (LPARAM)got)) delete got; // the bar went away meanwhile
    }).detach();
}

void Bar::DrawNotifs(const Theme& t, const D2D1_RECT_F& r) {
    Canvas& c = ui.C();
    int cleared = prefs::ReadNum(L"NotifCleared", 0); // everything up to this id was cleared here
    std::vector<const sys::Notif*> shown;
    for (const auto& n : notifs)
        if (n.id > cleared && !notifGone.count(n.id)) shown.push_back(&n);
    c.Text(L"Notifications", Rect(r.left + 4, r.top + 2, 160, 22), Weight::Medium, 16, t.text);
    c.Text(L"Do not disturb", Rect(r.right - 168, r.top + 5, 110, 16), Weight::Light, 11, t.textDim, Align::Right);
    if (w::Switch(ui, t, Ui::Id("nt.dnd"), {r.right - 25, r.top + 13}, dnd)) SetQuiet(!dnd); // no pills, no banners: the list keeps filling
    float y0 = r.top + kCalHead, bottom = r.bottom - 40;
    if (notifLoading && notifs.empty()) {
        c.Text(L"Loading\u2026", Rect(r.left, y0 + 20, Width(r), 18), Weight::Light, 12.5f, t.textLow, Align::Center);
    } else if (shown.empty()) {
        float cy = (y0 + bottom) / 2;
        c.Icon(glyph::BellOff, {(r.left + r.right) / 2, cy - 14}, 30, t.textLow);
        c.Text(L"You're all caught up", Rect(r.left, cy + 8, Width(r), 18), Weight::Light, 12.5f, t.textLow, Align::Center);
    }
    constexpr float kCard = 66, kGapC = 8;
    float maxScroll = (std::max)(0.0f, shown.size() * (kCard + kGapC) - (bottom - y0));
    if (float n = ui.Wheel({r.left, y0, r.right, bottom}); n != 0) notifScroll = std::clamp(notifScroll - n * 40, 0.0f, maxScroll);
    float sc = ui.Smooth(Ui::Id("nt.scroll"), notifScroll, 14);
    c.PushClip({r.left - 6, y0 - 2, r.right + 6, bottom});
    FILETIME nowFt;
    GetSystemTimeAsFileTime(&nowFt);
    int64_t now = ((int64_t)nowFt.dwHighDateTime << 32) | nowFt.dwLowDateTime;
    int64_t gone = -1, open = -1;
    for (size_t i = 0; i < shown.size(); ++i) {
        const sys::Notif& n = *shown[i];
        D2D1_RECT_F tile = Rect(r.left - 2, y0 + i * (kCard + kGapC) - sc, Width(r) + 4, kCard);
        if (tile.bottom < y0 - 2 || tile.top > bottom) continue;
        uint32_t id = Ui::Id("nt.tile", (int)(n.id & 0x7FFFFFFF));
        Ui::Hit h = ui.Button(id, {tile.left, (std::max)(tile.top, y0), tile.right, (std::min)(tile.bottom, bottom)}); // only the part that shows takes clicks
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        c.Fill(tile, 14, t.text.Alpha(0.05f + 0.05f * hov)); // a soft tile, island's ink
        int64_t mins = (now - n.arrived) / 600000000; // filetime is 100ns
        std::wstring ago = mins < 1 ? L"now" : mins < 60 ? std::to_wstring(mins) + L"m" : mins < 1440 ? std::to_wstring(mins / 60) + L"h" : std::to_wstring(mins / 1440) + L"d";
        c.Text(n.app, Rect(tile.left + 12, tile.top + 7, Width(tile) - 70, 14), Weight::Medium, 10.5f, t.accentHi);
        c.Text(ago, Rect(tile.right - 60, tile.top + 7, 26, 14), Weight::Light, 10.5f, t.textLow, Align::Right);
        c.Text(n.title, Rect(tile.left + 12, tile.top + 22, Width(tile) - 24, 18), Weight::Medium, 12.5f, t.text);
        if (!n.body.empty()) c.Text(n.body.substr(0, n.body.find(L'\n')), Rect(tile.left + 12, tile.top + 41, Width(tile) - 24, 16), Weight::Light, 11.5f, t.textDim);
        D2D1_POINT_2F x = {tile.right - 16, tile.top + 14};
        if (hov > 0.01f) { // its x, on hover
            bool onX = std::hypot(input.mouse.x - x.x, input.mouse.y - x.y) < 10;
            if (onX) c.Dot(x, 10, t.text.Alpha(0.12f));
            c.Line({x.x - 3.5f, x.y - 3.5f}, {x.x + 3.5f, x.y + 3.5f}, t.textDim.Alpha(hov), 1.5f);
            c.Line({x.x + 3.5f, x.y - 3.5f}, {x.x - 3.5f, x.y + 3.5f}, t.textDim.Alpha(hov), 1.5f);
            if (h.clicked) (onX ? gone : open) = n.id;
        }
    }
    c.PopClip();
    if (gone >= 0) notifGone.insert(gone);
    if (open >= 0)
        for (const auto& n : notifs)
            if (n.id == open) {
                notifGone.insert(n.id); // opened: it's been seen
                SetPanel(Panel::None);
                sys::OpenNotifApp(n);
                break;
            }
    float fy = r.bottom - 34;
    if (!shown.empty()) {
        D2D1_RECT_F clr = Rect(r.left - 2, fy, 90, 30);
        Ui::Hit h = ui.Button(Ui::Id("nt.clear"), clr);
        float hov = ui.Smooth(Ui::Id("nt.clear", 1), h.hover ? 1.0f : 0, 16);
        if (hov > 0.01f) c.Fill(clr, 15, t.text.Alpha(0.09f * hov));
        c.Text(L"Clear all", clr, Weight::Medium, 12, t.accentHi, Align::Center);
        if (h.clicked) {
            int64_t top = 0;
            for (const auto& n : notifs) top = (std::max)(top, n.id);
            prefs::SaveNum(L"NotifCleared", (int)top);
            notifGone.clear();
        }
    }
    D2D1_RECT_F all = Rect(r.right - 132, fy, 134, 30); // windows' own centre, for actions only it can take
    Ui::Hit ah = ui.Button(Ui::Id("nt.all"), all);
    float ahov = ui.Smooth(Ui::Id("nt.all", 1), ah.hover ? 1.0f : 0, 16);
    if (ahov > 0.01f) c.Fill(all, 15, t.text.Alpha(0.09f * ahov));
    c.Text(L"Windows centre", Rect(all.left, all.top, Width(all) - 26, Height(all)), Weight::Medium, 12, t.textDim, Align::Right);
    c.Icon(glyph::OpenNew, {all.right - 14, (all.top + all.bottom) / 2}, 15, t.textDim);
    if (ah.clicked) SetPanel(Panel::None), Keys({VK_LWIN, 'N'});
}

void Bar::DrawCalendar(const Theme& t, ID2D1Geometry* shape) {
    float rise = (std::max)(0.0f, calSpring.value);
    if (rise < 1 || !shape) return;
    Canvas& c = ui.C();
    ID2D1RenderTarget* rt = c.Target();
    float a = anim::Clamp01((rise - 40) / (CalRise() - 40));
    if (a <= 0.01f) return;
    rt->PushLayer(D2D1::LayerParameters(pill, shape, D2D1_ANTIALIAS_MODE_PER_PRIMITIVE, D2D1::IdentityMatrix(), a), nullptr);
    D2D1_RECT_F nr = {W / 2 - kCalW / 2 + kPad, pill.top + CallH() + kPad, W / 2 - kCalW / 2 + kNotifW, rowTop - 6};
    DrawNotifs(t, nr);
    c.Line({nr.right + 12, nr.top + 6}, {nr.right + 12, nr.bottom - 6}, t.line, 1);
    D2D1_RECT_F r = {nr.right + 26, pill.top + CallH() + kPad, W / 2 + kCalW / 2 - kPad, rowTop - 6};
    SYSTEMTIME now;
    GetLocalTime(&now);
    if (!calYear) calYear = now.wYear, calMonth = now.wMonth;

    SYSTEMTIME first = {};
    first.wYear = (WORD)calYear, first.wMonth = (WORD)calMonth, first.wDay = 1;
    FILETIME ft;
    SystemTimeToFileTime(&first, &ft);
    FileTimeToSystemTime(&ft, &first); // fills in the weekday
    wchar_t head[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &first, L"MMMM yyyy", head, 64, nullptr);
    c.Text(head, Rect(r.left + 4, r.top + 2, 200, 22), Weight::Medium, 16, t.text);
    int step = 0;
    if (w::IconBtn(ui, t, Ui::Id("cal.prev"), {r.right - 50, r.top + 12}, 30, glyph::ChevronLeft, 20)) step = -1;
    if (w::IconBtn(ui, t, Ui::Id("cal.next"), {r.right - 14, r.top + 12}, 30, glyph::ChevronRight, 20)) step = 1;
    if (step) {
        calPick = 0; // a new month: nothing picked in it
        calMonth += step;
        if (calMonth < 1) calMonth = 12, --calYear;
        if (calMonth > 12) calMonth = 1, ++calYear;
    }

    DWORD firstDow = 0; // the user's first day of the week: 0 monday .. 6 sunday
    GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER, (LPWSTR)&firstDow, sizeof(firstDow) / sizeof(wchar_t));
    float cw = Width(r) / 7, y = r.top + kCalHead;
    for (int i = 0; i < 7; ++i) { // weekday initials
        wchar_t d[8] = {};
        GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_SSHORTESTDAYNAME1 + (LCTYPE)((firstDow + i) % 7), d, 8);
        c.Text(d, Rect(r.left + i * cw, y, cw, 16), Weight::Medium, 10.5f, t.textLow, Align::Center);
    }
    y += kCalDow;
    int lead = ((first.wDayOfWeek + 6) % 7 - (int)firstDow + 7) % 7; // wDayOfWeek: 0 sunday
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    int days = kDays[calMonth - 1] + (calMonth == 2 && ((calYear % 4 == 0 && calYear % 100 != 0) || calYear % 400 == 0) ? 1 : 0);
    for (int d = 1; d <= days; ++d) {
        int cell = lead + d - 1;
        D2D1_POINT_2F o = {r.left + (cell % 7 + 0.5f) * cw, y + (cell / 7 + 0.5f) * kCalCell};
        bool today = calYear == now.wYear && calMonth == now.wMonth && d == now.wDay;
        bool picked = calPick ? d == calPick : today;
        uint32_t id = Ui::Id("cal.day", d);
        Ui::Hit h = ui.Button(id, {o.x - 15, o.y - 15, o.x + 15, o.y + 15});
        float hov = ui.Smooth(id + 1, h.hover ? 1.0f : 0, 16);
        if (h.clicked) calPick = d;
        if (today) {
            c.Glow(o, 22, 22, t.accent.Alpha(0.25f));
            c.Dot(o, 14, t.accent);
        } else if (picked || hov > 0.01f) {
            c.Dot(o, 14, t.text.Alpha(picked ? 0.14f : 0.08f * hov));
        }
        if (picked && today) c.Ring(o, 16, t.accentHi, 1.5f);
        c.Text(std::to_wstring(d), {o.x - cw / 2, o.y - 9, o.x + cw / 2, o.y + 9}, today ? Weight::Semi : Weight::Regular, 12.5f, today ? t.onAccent : t.text,
               Align::Center);
        if (cals.HasEvents(calYear, calMonth, d)) c.Dot({o.x, o.y + 11}, 1.8f, today ? t.onAccent : t.accentHi); // something on that day
    }
    float ay = y + 6 * kCalCell + 4;
    c.Line({r.left, ay}, {r.right, ay}, t.line, 1);
    DrawAgenda(t, {r.left, ay + 8, r.right, ay + kCalAgenda - 4}, calYear, calMonth, calPick ? calPick : (calYear == now.wYear && calMonth == now.wMonth ? now.wDay : 1));

    float fy = r.bottom - kCalFoot + 8, cy = fy + (kCalFoot - 8) / 2;
    c.Line({r.left, fy}, {r.right, fy}, t.line, 1);
    wchar_t longDate[64] = {};
    GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &now, nullptr, longDate, 64, nullptr);
    c.Text(longDate, Rect(r.left + 4, cy - 9, Width(r) - 90, 18), Weight::Medium, 12, t.textDim);
    if (w::IconBtn(ui, t, Ui::Id("cal.today"), {r.right - 50, cy}, 30, glyph::Today, 18)) calYear = now.wYear, calMonth = now.wMonth, calPick = 0;
    if (ui.RClick(pill) && input.mouse.y < rowTop) SetPanel(Panel::None);
    rt->PopLayer();
}
