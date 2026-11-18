# Haptika

Haptika is a powerful gamepad tester for Linux. It tests and shows buttons (including
pressure-sensitive buttons, such as DualShock 2/3), sticks, triggers, touchpads, motion
sensors, vibration, and speakers (such as DualSense).

It is written in pure C with GTK 4 and Libadwaita, integrating tightly with the GNOME
desktop. SDL3 is the default backend, but raw evdev is also available.

Haptika is also capable of displaying various information such as connection method
(USB vs. Bluetooth), battery levels for Bluetooth controllers, and it integrates
InputPlumber compatibility.

## What it shows

- Buttons and the D-pad, including analog pressure values when the controller exposes
  them, such as DualShock 3.
- Analog sticks with a circular position view, so you can see the full travel and
  centering.
- Triggers and other axes as unipolar or centered bipolar bars, including gyroscope axes.
- Touchpad contacts on DualShock 4 and DualSense, with multi-finger tracking.
- Motion sensors: a tilt view for the accelerometer and separate X, Y, and Z bars for the
  gyroscope.
- Battery level and charging state, plus whether the controller is on USB or Bluetooth.
- Vibration testing on SDL3, with independent low-frequency and high-frequency motor
  sliders.
- A short speaker test for controllers that expose their speaker through PipeWire, such
  as the DualSense over USB.

## Two backends

SDL3 is the default. It provides a standard gamepad mapping, vibration, touchpad and
motion sensor access, and battery telemetry. It is the best choice for most controllers.
SDL3 state is sampled every millisecond while input is changing, then backs off to 4 ms
after a quarter second without changes. GTK still paints on the display frame clock, so
the visualizers update at the monitor refresh rate while short presses between frames
are not missed.

The raw evdev backend exposes every button and axis reported by `libevdev` without a
mapping layer. This is useful for older or unusual controllers, for checking what the
kernel actually reports, and for seeing pressure-sensitive buttons that SDL does not map
as buttons (very rare).

When a controller exposes both backends, the header bar can switch between them without
unplugging the device. The two backends can report different names and axes for the same
pad, and that is often the point of the comparison.

## Sony hardware information

Haptika can read hardware details over hidraw for Sony controllers.

- **DualShock 3:** Bluetooth address and a USB authenticity check.
- **DualShock 4:** build date, hardware and software version, board model (JDM-001
  through JDM-055), Bluetooth address, and a USB authenticity check.
- **DualSense and DualSense Edge:** firmware versions (main, SBL, Venom, Spider), board
  model (BDM-010 through BDM-060X), and, over USB, the factory serial number, color
  variant, MCU unique ID, PCBA ID, battery and VCM barcodes, and Bluetooth address.

The dialogs are fetched off the main thread and can be closed while a read is in
progress.

## InputPlumber integration

Haptika detects virtual controllers managed by InputPlumber and matches them to their
`CompositeDevice` over D-Bus. For a matched device it shows the virtual bus and address,
the active profile, output capabilities, and the physical source devices.
InputPlumber's physical source nodes are hidden from the device list, so you do not see a
broken duplicate next to the virtual pad.

## Dependencies

- GTK 4.16 or newer
- Libadwaita 1.7 or newer
- SDL3 3.2 or newer
- libevdev 1.11 or newer
- libudev 249 or newer
- Meson 0.63 or newer
- libpipewire 0.3 for the speaker test (optional)

### Ubuntu

Ubuntu 26.04 LTS or newer is required. Ubuntu 24.04 LTS ships GTK 4.14 and
libadwaita 1.5, below the minimum, and does not package SDL3.

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config gettext \
  libgtk-4-dev libadwaita-1-dev libsdl3-dev libevdev-dev libudev-dev \
  libpipewire-0.3-dev
```

### Debian

Debian 13 (trixie) or newer is required. Debian 12 ships GTK 4.8 and
libadwaita 1.2, below the minimum.

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config gettext \
  libgtk-4-dev libadwaita-1-dev libsdl3-dev libevdev-dev libudev-dev \
  libpipewire-0.3-dev
```

### Arch Linux

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf gettext \
  gtk4 libadwaita sdl3 libevdev systemd-libs pipewire
```

`libpipewire-0.3-dev` on Debian/Ubuntu and `pipewire` on Arch are optional and
only required for the DualSense speaker test. The desktop-file and metainfo
validation tests also need `desktop-file-utils` and `appstream`, but `meson test`
skips those tests when the tools are missing.

## Building and installing

```sh
meson setup build --prefix=$HOME/.local
meson compile -C build
meson test -C build
meson install -C build
```

## Permissions

Haptika needs read access to `/dev/input/event*` devices. If your controller does not
appear, add your user to the `input` group and log out and back in:

```sh
sudo usermod -aG input $USER
```

Hardware info for DualShock 4 and DualSense also reads `/dev/hidraw*`. Most modern
distributions grant access to active desktop sessions via udev `uaccess`; if reading
hardware info fails, check your hidraw rules for Sony devices (`054c:*`).

## What to expect

- SDL3 and evdev can report different names and axes for the same pad (this is especially
  true for evdev).
- The DualSense speaker test needs USB and PipeWire.
- InputPlumber virtual pads are matched over D-Bus.
- Hardware info needs hidraw access.
- A virtual controller may show fewer or different elements than the physical pad it is
  built from, depending on the active InputPlumber profile.

## Status

Haptika is hobbyist software. I made it in order to personally test some of my unusual
controllers and adapters. Expectations should be in line with that. Code changes and
updates occur locally most of the time, or in a different branch, as I'm not very active
on GitHub.
No translation catalog ships yet. Many controllers are untested. If you got a controller
with strange behavior (wrong name, buttons, mapping, anything), open an issue with the
model and the `lsusb` line. Even though I'm a controller collector of sorts, I don't have
every controller ever released, so contributions are very welcome.

## Acknowledgments

- [SDL3](https://libsdl.org/) for the gamepad abstraction layer
- [libevdev](https://freedesktop.org/wiki/Software/libevdev/) for raw evdev access
- [Controllercons](https://controllercons.github.io) by Kieran McClung for the
  controller and console icons (SIL Open Font License 1.1)
- [dualshock-tools](https://dualshock-tools.github.io/) for DS4 reverse engineering
  reference
- [dualsensectl](https://github.com/nowrep/dualsensectl) for DualSense reverse
  engineering reference
- [DsHidMini](https://github.com/nefarius/DsHidMini) for DualShock 3 OUI database

## License

**Haptika is FREE SOFTWARE, licensed under the GNU General Public License v3.0 or later.**

That is not a footnote. It means you are free to run Haptika for any purpose, read how it
works, change it to suit your controller, and share it with other people. If you pass it
on, the GPLv3 makes sure the next person receives the same freedoms you did.

See [LICENSE](LICENSE) for the full text.

The controller and console icons under `data/icons/` are third-party artwork from
[Controllercons](https://controllercons.github.io), licensed under the SIL Open Font
License 1.1. They are not covered by the GPL.
See `data/icons/NOTICE.md` and `data/icons/OFL.txt`.
