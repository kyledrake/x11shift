# X11Shift

X11Shift is a desktop-neutral GTK night-light application for Linux systems
running X11. It changes each display's XRandR color lookup table directly, so it does
not depend on Redshift, Wayland, a particular desktop environment, a compositor,
or an online service.

The application provides:

- sunrise/sunset scheduling from manually entered latitude and longitude;
- custom day and night clock times;
- fixed day or fixed night modes;
- separate day/night color temperatures and brightness;
- configurable color strength and smooth transition duration;
- one-click restoration of the default color and brightness values;
- per-display selection for multi-monitor setups;
- a notification-area menu, including a one-hour pause;
- optional login autostart; and
- an application-menu launcher and Debian package metadata.

## Requirements

- A desktop or window manager running an X11 session
- XRandR 1.3 or newer and a display driver with writable gamma ramps or a
  native `GAMMA_LUT` output property
- GTK 3

It can run under Xfce, MATE, Cinnamon/Xorg, LXDE, LXQt/X11, Plasma/X11,
GNOME/Xorg, standalone window managers, and other X11 environments. The
notification-area icon uses GTK's broadly supported legacy tray protocol. On a
desktop without that protocol, the GUI labels its action **Hide window**;
launching X11Shift again from the application menu reopens the existing process.

Build packages on Debian or Ubuntu:

```sh
sudo apt install build-essential pkg-config libgtk-3-dev libx11-dev libxrandr-dev
```

## Build and run

```sh
make
./x11shift
```

The GUI saves changes immediately in `~/.config/x11shift/config.ini`. Closing
the window keeps the program running in the background. Use **Restore colors
and quit** to put back the gamma ramps that were present when X11Shift started.
If sunrise-to-sunset scheduling is selected before a location is entered,
X11Shift safely uses the configured custom day/night times and explains the
fallback in its status panel.

X11Shift refreshes the current schedule immediately after suspend or hibernate
on systems with logind resume notifications. It also reapplies colors every
five seconds, including while displays reconnect. Display reconfiguration keeps
the original gamma ramps so a pre-suspend night tint does not become permanent.

Transitions begin at the configured day/night start time (or sunrise/sunset).
For example, a 07:00 day start with a 180-minute transition reaches full daylight
colors at 10:00. Reduce **Transition** for an earlier change to full
daylight colors.
During a transition, the status panel and tray tooltip show **Transitioning to
daylight** or **Transitioning to night**, along with the percentage complete and
minutes remaining.

To install under `/usr/local` and add X11Shift to the application menu:

```sh
sudo make install
```

If your desktop's application menu is already open, it may take a few seconds
to notice the new entry. It normally appears under **Settings**.

## Build a Debian package

Install the build dependencies, then run:

```sh
dpkg-buildpackage -us -uc -b
```

The resulting `x11shift_1.1.6-1_*.deb` is written to the parent directory.
Install it with:

```sh
sudo apt install ../x11shift_1.1.6-1_*.deb
```

The package installs the executable, desktop entry, icon, AppStream metadata,
and manual page. It does not enable autostart globally; that remains a per-user
choice in the GUI.

### Upgrading from XfceShift

X11Shift automatically imports `~/.config/xfceshift/config.ini` when it does not
yet have its own configuration. It also replaces the old XFCE-only autostart
entry with a desktop-neutral one. The Debian package replaces the old package.

## Command-line behavior

Running `x11shift` opens the GUI. `x11shift --background` is reserved for the
login autostart entry. `x11shift --reset` is an emergency escape hatch that
sets active XRandR gamma ramps and native lookup tables to linear if the
previous process was killed before it could restore them.

When the driver exposes `GAMMA_LUT`, X11Shift uses it to commit color changes
to the GPU. This also handles AMD Xorg sessions where the legacy gamma API
updates X11's cached ramp without changing the hardware colors. The original
lookup table is restored when adjustment is disabled or the application quits.
X11Shift saves the original native lookup table in the X11 session before
adjusting it. After an interrupted process, it recognizes its own leftover
tint and recovers the original colors, including when starting with adjustment
disabled. It does not reuse that baseline if another color manager has changed
the lookup table independently.

## Limitations

Gamma-ramp adjustment affects the complete output. It cannot exclude individual
windows, screenshots, or screen sharing. HDR outputs, color-management daemons,
and some proprietary drivers may replace or reject gamma changes. Avoid running
another night-light or gamma utility at the same time because the programs will
compete for the same ramps.

## License

GPL-3.0-or-later. See [LICENSE](LICENSE).
