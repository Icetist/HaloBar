<div align="center">

# HaloBar

**A native taskbar for Windows 11 that floats as a single island.**

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

HaloBar replaces the Windows taskbar with one floating island at the bottom of your screen: the song you're playing, your apps, the tray, quick settings and the clock. Click anything and the island grows up into it, a player, a calendar, quick settings, then settles back down.

It is a sibling of [Astraeus](https://github.com/Icetist/Astraeus) and wears the same look, but runs entirely on its own. No Electron, no web view, no runtime to install: one small native executable written in C++ with Direct2D and DirectComposition.

> [!NOTE]
> HaloBar is in its first releases. It already does everything the Windows taskbar does day to day, but expect rough edges, and please report what you find.

## ✨ Features

- **Apps.** Pinned and running apps share the row, in the same order and with the same pins as the Windows taskbar. Hover for live window previews with media controls, right-click for the app's windows and recent files, middle-click for a new window, scroll over an app to change its own volume.
- **System tray.** Every tray icon, behind the arrow, with the app's own menus. Icons survive HaloBar restarts.
- **Quick settings.** Wi-Fi, Bluetooth, airplane mode, mobile hotspot, energy saver, night light, microphone, Do Not Disturb, live captions, cast, project, nearby sharing, accessibility and a snip, plus volume and brightness. Join Wi-Fi networks with their password and connect Bluetooth devices right in the panel, and set every app's volume from the arrow next to the volume slider.
- **Notifications.** Your notifications in HaloBar's own panel beside the calendar, with Do Not Disturb and Clear all. New ones rise out of the island as small pills.
- **Calendar.** A month at a glance. Connect Google Calendar, Outlook or iCloud by copying your calendar's private link: days with events get a dot and the day you pick shows its agenda. Links are stored encrypted and shared with Astraeus.
- **Media.** The playing song sits in the island with a small visualizer. Click it for a player with album art, shuffle, repeat and word-by-word synced lyrics.
- **Launcher.** Click the Windows logo to search every installed app, with your pins, power options, Windows Start and HaloBar settings one click away.
- **Your look.** Follow Astraeus, or pick Dark, Light, Glass or Custom with its own accent and background colors, in three sizes. Auto-hide under maximized windows, or always show and keep the space.
- **Keyboard layouts.** The current layout, named, next to the tray.
- **Updates itself** and **starts with Windows.** Turn startup off in Task Manager's Startup apps if you like.

## ⚡ Performance

HaloBar draws only while something on it moves, and only the part that moved. With nothing changing it sleeps: no redraws, efficiency mode on, a fraction of a percent of one core even with busy tray icons, and its memory handed back to Windows. With music playing and the visualizer dancing it sits around 1% CPU, in well under 30 MB of memory.

## 📦 Installation

1. Download `HaloBar.exe` from the [latest release](https://github.com/Icetist/HaloBar/releases/latest).
2. Put it somewhere it can stay, for example `%LOCALAPPDATA%\Programs\HaloBar`, and run it.

The Windows taskbar steps aside while HaloBar runs and comes back the moment it exits. HaloBar starts with Windows from then on and keeps itself up to date.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.

## 🖱️ Usage

- **Settings:** click the Wi-Fi, volume and battery icons, then **HaloBar settings**. Or right-click the Windows logo.
- **The player:** click the song.
- **Notifications and the calendar:** click the clock.
- **The tray:** click the arrow.
- **Windows Start:** right-click the Windows logo, or press the Windows key.
- **Show desktop:** click the far right end of the island.
- **Quit or restart:** right-click the Windows logo.

## 🛠️ Building from source

Requires Visual Studio 2022 or later with the C++ desktop workload and a Windows 11 SDK.

```
build.cmd
```

The executable lands in `build\HaloBar.exe`. Fonts are built into it.

## ❓ FAQ

<details>
<summary><b>How do I get the Windows taskbar back?</b></summary>

Right-click the Windows logo and choose **Exit**. The Windows taskbar returns straight away, exactly as you had it. It also comes back on its own if HaloBar ever closes unexpectedly.

</details>

<details>
<summary><b>Do I need Astraeus?</b></summary>

No. HaloBar works on its own. If Astraeus is installed, HaloBar can follow its look and shares its calendar and lyrics.

</details>

## 🙏 Credits

- [Outfit](https://github.com/Outfitio/Outfit-Fonts) by the Outfit Project Authors, licensed under the [SIL Open Font License 1.1](./LICENSES/Outfit-OFL.txt).
- [Material Symbols](https://github.com/google/material-design-icons) by Google, licensed under the [Apache License 2.0](./LICENSES/MaterialSymbols-Apache-2.0.txt).
- Synced lyrics from [LRCLIB](https://lrclib.net), NetEase Cloud Music and Kugou.
