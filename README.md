<div align="center">

# HaloBar

**A floating dock for Windows 11, with widgets.**

[![Latest release](https://img.shields.io/github/v/release/Icetist/HaloBar?style=flat-square&color=9F8CFF)](https://github.com/Icetist/HaloBar/releases/latest)
[![Build](https://img.shields.io/github/actions/workflow/status/Icetist/HaloBar/release.yml?style=flat-square&label=build)](https://github.com/Icetist/HaloBar/actions/workflows/release.yml)
![Platform](https://img.shields.io/badge/platform-Windows%2011-0078D4?style=flat-square&logo=windows)
![C++20](https://img.shields.io/badge/C%2B%2B-20-00599C?style=flat-square&logo=cplusplus)

[Features](#-features) •
[Installation](#-installation) •
[Usage](#%EF%B8%8F-usage) •
[Building](#%EF%B8%8F-building-from-source) •
[FAQ](#-faq)

</div>

HaloBar replaces the Windows taskbar with a floating dock at the bottom of your screen: your apps, the widgets you pick, and your status icons in one row. Click anything and a panel grows out of it, then settles back down.

It is a sibling of [Astraeus](https://github.com/Icetist/Astraeus): it wears the same themes and leaves the top of the screen to Astraeus, but runs entirely on its own. No Electron, no web view, no runtime to install: one small native executable written in C++ with Direct2D and DirectComposition.

> [!NOTE]
> HaloBar is in its first releases. It already does everything the Windows taskbar does day to day, but expect rough edges, and please report what you find.

## ✨ Features

- **Apps.** Pinned and running apps, in the same order and with the same pins as the Windows taskbar. Hover for live window previews, right-click for the app's windows and recent files, middle-click for a new window, scroll over an app to change its own volume. When they don't all fit, a **+N** tile holds the rest.
- **Widgets.** Pick from 25 widgets in a gallery with live previews: now playing, recycle bin, weather, system stats, network speed, battery, focus timer, water, clipboard history, color picker, screenshot, emoji, downloads, a favourite folder, notes, to-do, habits, calendar, world clock, progress of the day, year and more, quick toggles, volume, search, app switcher, photo, and dividers. Most come in small, medium and large.
- **Edit mode.** Right-click the dock to rearrange it: drag widgets, dividers and the whole apps block anywhere in the row, remove what you don't use, size each widget and set its options.
- **Status at the end of the row.** Tray, keyboard layout, Bluetooth, network, volume, battery, notifications, quick settings, the clock and show desktop, each one optional. The keyboard layout names itself (ENG UK, ENG INTL) and a click moves to the next one.
- **Quick settings.** Wi-Fi, Bluetooth, airplane mode, mobile hotspot, energy saver, night light, microphone, Do Not Disturb, live captions, cast, project, nearby sharing, accessibility and a snip, plus volume, brightness and every app's own volume.
- **Notifications and the calendar.** Your notifications beside a month view, with Do Not Disturb and Clear all. Connect Google Calendar, Outlook or iCloud with your calendar's private link; links are stored encrypted and shared with Astraeus.
- **Media.** The playing song with a small visualizer. Click it for a player with album art, shuffle and repeat.
- **Launcher.** Click the Windows logo to search every installed app, with your pins, power options and Windows Start one click away.
- **Your look.** Astraeus' Island, Dark, Light, Glass and Custom themes with your own accent. Auto-hide under windows, or always show and keep the space.
- **Windows' taskbar, any time.** One button in settings hands the taskbar back to Windows; HaloBar waits as a tray icon whose dashboard brings it back.
- **Updates itself** and **starts with Windows.** Turn startup off in Task Manager's Startup apps if you like.

## ⚡ Performance

HaloBar draws only while something on it moves, and only the part that moved. With nothing changing it sleeps: no redraws, efficiency mode on, and its memory handed back to Windows. Slow work, like reading the network and the recycle bin or loading app icons, happens off the drawing thread, and each widget's data source only runs while that widget is in the dock. While the dock is tucked away it uses a fraction of a percent of one core, even with music playing.

## 📦 Installation

1. Download `HaloBar.exe` from the [latest release](https://github.com/Icetist/HaloBar/releases/latest).
2. Put it somewhere it can stay, for example `%LOCALAPPDATA%\Programs\HaloBar`, and run it.

The Windows taskbar steps aside while HaloBar runs and comes back the moment it exits. HaloBar starts with Windows from then on and keeps itself up to date.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.

## 🖱️ Usage

- **Edit the dock:** right-click an empty spot on it, or choose **Customize dock** in settings.
- **Settings:** right-click the Windows logo, or open quick settings and choose **HaloBar settings**.
- **The player:** click the song.
- **Notifications and the calendar:** click the clock.
- **The tray:** click the arrow.
- **Windows Start:** press the Windows key.
- **Show desktop:** click the far right end of the dock.

## 🛠️ Building from source

Requires Visual Studio 2022 or later with the C++ desktop workload and a Windows 11 SDK.

```
build.cmd
```

The executable lands in `build\HaloBar.exe`. Fonts are built into it.

## ❓ FAQ

<details>
<summary><b>How do I get the Windows taskbar back?</b></summary>

Open HaloBar's settings and choose **Go back to Windows taskbar**. HaloBar stays in the tray; click its icon and choose **Bring back HaloBar** whenever you want it again. Exiting HaloBar from the Windows logo's right-click menu brings the Windows taskbar back too, and it also returns on its own if HaloBar ever closes unexpectedly.

</details>

<details>
<summary><b>Do I need Astraeus?</b></summary>

No. HaloBar works on its own. If Astraeus is installed, HaloBar can follow its look, shares its calendar, and keeps Astraeus above it.

</details>

## 🙏 Credits

- [Outfit](https://github.com/Outfitio/Outfit-Fonts) by the Outfit Project Authors, licensed under the [SIL Open Font License 1.1](./LICENSES/Outfit-OFL.txt).
- [Material Symbols](https://github.com/google/material-design-icons) by Google, licensed under the [Apache License 2.0](./LICENSES/MaterialSymbols-Apache-2.0.txt).
- Weather from [Open-Meteo](https://open-meteo.com).
