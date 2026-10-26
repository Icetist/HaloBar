## HaloBar 0.0.5

### New
- **Installs itself.** Run HaloBar from anywhere and it moves into `%LOCALAPPDATA%\Programs\HaloBar`, adds itself to the Start menu and starts with Windows from there. A newer download updates that copy, even while it's running.

### Fixes
- **The Windows taskbar stays away on every screen.** With more than one monitor it could keep popping back up next to the dock; now it's gone on all of them while HaloBar runs, and comes back as you had it when HaloBar exits.
- **No more CPU spikes with two monitors.** HaloBar and Windows could keep hiding and showing the taskbar at each other; that loop is gone, with guards so nothing like it can spin again.
- **Docks on every screen work together.** Each screen's dock now shares one list of windows and pins, so pins stay in step, and progress badges show on every screen.
- **Plugging monitors in and out** rebuilds the docks once the screens settle, never in a storm.
- **Lighter at rest.** About a third of the idle CPU it used before: fewer wake-ups while the dock is tucked away, tray icons that resend the same picture no longer wake it, and the screens' docks share their media, status, weather and notification work.

> [!NOTE]
> The executable is not code-signed yet, so Windows SmartScreen may show an "unknown publisher" warning the first time. Click **More info → Run anyway**.
