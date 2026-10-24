#pragma once

namespace glyph {
constexpr int Overview = 0xE9B0; // material symbols codepoints, page tabs
constexpr int Music = 0xE405;
constexpr int System = 0xF190;
constexpr int Network = 0xE63E;
constexpr int Tools = 0xF10B;
constexpr int Play = 0xE037, Pause = 0xE034, Next = 0xE044, Prev = 0xE045; // media transport
constexpr int Shuffle = 0xE043, Repeat = 0xE040, RepeatOne = 0xE041;
constexpr int Wifi = 0xE63E, WifiOff = 0xE648, Bluetooth = 0xE1A7, BluetoothOff = 0xE1A9;
constexpr int Coffee = 0xEFEF, Mic = 0xE029, MicOff = 0xE02B;
constexpr int Eco = 0xEA35, Balance = 0xEAF6, Bolt = 0xEA0B;
constexpr int Headphones = 0xE310, Speaker = 0xE32D;
constexpr int VolumeUp = 0xE050, VolumeOff = 0xE04F, Bright = 0xE518;
constexpr int Cpu = 0xE322, Ram = 0xF7A3, Gpu = 0xE30D;
constexpr int Lan = 0xEB2F, Up = 0xE5D8, Down = 0xE5DB;
constexpr int Lock = 0xE897, Sleep = 0xEF44, Restart = 0xF053, Power = 0xE8AC, SignOut = 0xE9BA; // session/power actions
constexpr int Snip = 0xEC08, Timer = 0xE425, Stop = 0xE047;
constexpr int Settings = 0xE8B8, Copy = 0xE14D, Check = 0xE5CA;
constexpr int ClearDay = 0xF157, ClearNight = 0xF159, PartlyDay = 0xF172, PartlyNight = 0xF174; // weather conditions
constexpr int Cloud = 0xE2BD, Fog = 0xE818, Rain = 0xF176, Snow = 0xE2CD, Storm = 0xEBDB;
constexpr int BtConn = 0xE1A8, BtSearch = 0xE1AA, Devices = 0xE1B1; // bluetooth center
constexpr int Keys = 0xE312, Mouse = 0xE323, Phone = 0xE32C, Pc = 0xE30A, Pad = 0xEA28, Watch = 0xE334;
constexpr int Delete = 0xE872, Add = 0xE145, Close = 0xE5CD, OpenNew = 0xE89E;
constexpr int Sparkle = 0xE65F, Edit = 0xE3C9, Rocket = 0xEB9B; // tray menu
constexpr int kSys = 0x10000; // above the bmp: a codepoint in windows' segoe fluent icons instead of our subset
constexpr int SysPaste = kSys | 0xE77F, SysDownload = kSys | 0xE896, SysEmoji = kSys | 0xE76E, SysPicker = kSys | 0xEF3C, SysNote = kSys | 0xE70B;
constexpr int SysPhoto = kSys | 0xE91B, SysCup = kSys | 0xEC32, SysFolder = kSys | 0xE8B7, SysStopwatch = kSys | 0xE916, SysChart = kSys | 0xE9D2, SysClip = kSys | 0xF7ED;
constexpr int SysDrive = kSys | 0xEDA2, SysUsb = kSys | 0xE88E; // the drives widget: a fixed disk, a removable one
constexpr int SysContact = kSys | 0xE77B, SysVideo = kSys | 0xE8B2; // menus: the account, the videos folder

// the bar's own: pins, quick settings, calendar
constexpr int Pin = 0xF10D, Unpin = 0xE6F9, Apps = 0xE5C3, Search = 0xEF7A, Widgets = 0xE1BD, TaskView = 0xE6FA;
constexpr int Bell = 0xE7F5, BellOff = 0xE7F6, Dnd = 0xF08F;
constexpr int Battery = 0xE1A5, BatteryCharging = 0xE1A3, BatterySaver = 0xEFDE, BatteryAlert = 0xE19C;
constexpr int BatteryBars[] = {0xEBDC, 0xF09C, 0xF09D, 0xF09E, 0xF09F, 0xF0A0, 0xF0A1}; // 0..6 bars
constexpr int WifiBars[] = {0xF065, 0xEBE4, 0xEBD6, 0xEBE1, 0xE63E}; // empty, 1, 2, 3 bars, full
constexpr int WifiNone = 0xE1DA, Airplane = 0xE53D, AirplaneOff = 0xE194, Hotspot = 0xE1E2, Vpn = 0xE0DA, Globe = 0xE80B;
constexpr int VolumeDown = 0xE04D, VolumeMute = 0xE04E, BrightMid = 0xE1AE;
constexpr int NightLight = 0xF03D, DarkMode = 0xE51C, Contrast = 0xEB37, Access = 0xE92C, Cast = 0xE307, Location = 0xF1DB;
constexpr int Language = 0xEA07, Calendar = 0xEBCC, Event = 0xE878, Schedule = 0xEFD6, Today = 0xE8DF;
constexpr int ChevronLeft = 0xE5CB, ChevronRight = 0xE5CC, ExpandLess = 0xE5CE, ExpandMore = 0xE5CF, More = 0xE5D3;
constexpr int PowerNew = 0xF8C7, Folder = 0xE2C7, Doc = 0xE873, History = 0xE8B3, Star = 0xF09A, Minimize = 0xE931;
constexpr int Terminal = 0xEB8E, Home = 0xE9B2, Desktop = 0xE30C, TaskAlt = 0xE2E6, Monitor = 0xEF5B, Refresh = 0xE5D5;
constexpr int CloseSmall = 0xF508, Tune = 0xE429, DevicesOther = 0xE337, Link = 0xE250, Admin = 0xEF3D, Drag = 0xE945;
}
