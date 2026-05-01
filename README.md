<p align="center">
  <img src="data/icons/hicolor/scalable/apps/io.github.gabriel2silva.Haptika.svg" width="128" height="128" alt="Haptika icon">
</p>

<h1 align="center">Haptika</h1>

<p align="center">
  A fast, interactive gamepad testing application for Linux.<br>
  Built with pure C, GTK 4, and Libadwaita.
</p>

---

Haptika is a native Linux application for testing and inspecting game controllers. It lets you verify that every button, stick, trigger, touchpad, and sensor on your gamepad is working correctly, with real-time visual feedback and sub-millisecond input latency.

Unlike browser-based gamepad testers or aging CLI tools, Haptika is a proper desktop application that integrates perfectly with the GNOME desktop, follows the GNOME Human Interface Guidelines, and talks directly to the kernel's input subsystem for maximum accuracy.

## Why Haptika?

| | Haptika | jstest-gtk | Browser testers |
|---|---|---|---|
| Real-time visual feedback | ✅ | ✅ | ✅ |
| Analog stick visualization | ✅ | ✅ | ✅ |
| Pressure-sensitive buttons (DS3) | ✅ | ❌ | ❌ |
| Touchpad tracking (DS4/DS5) | ✅ | ❌ | ❌ |
| Motion sensors (accel/gyro) | ✅ | ⚠️ | ⚠️ |
| Battery & connection info | ✅ | ❌ | ❌ |
| Controller hardware info | ✅ | ❌ | ❌ |
| Clone/authenticity detection | ✅ | ❌ | ❌ |
| Speaker testing | ✅ | ❌ | ❌ |
| Vibration/rumble testing | ✅ | ❌ | ⚠️ |
| Hotplug support | ✅ | ❌ | ✅ |
| SDL3 + evdev dual backend | ✅ | ❌ | ❌ |

## Features

### Input Testing
- **Buttons/D-PAD** — every button lights up in real time when pressed
- **Analog sticks** — circular visualizer showing precise stick position and travel
- **Triggers** — vertical bar indicators for analog trigger depth

### Advanced Input
- **Touchpad finger tracking** — real-time visualization of finger position on DualShock 4 and DualSense touchpads
- **Motion sensors** — tilt indicator for accelerometer data and bar graphs for gyroscope X/Y/Z axes
- **Pressure-sensitive buttons** — reads the analog pressure values from DualShock 3 face buttons, shoulder buttons, and D-pad **(the only Linux tool that does this)**

### Dual Backend
- **SDL3** — the default backend, provides standardized button/axis mapping, rumble, touchpad, sensors, and battery info for most controllers
- **evdev** — raw Linux input backend, shows every axis and button the kernel exposes without any mapping layer
- **One-click switching** — toggle between SDL3 and evdev on the same controller without disconnecting

### Hardware Information
- **DualShock 3** — Bluetooth address, clone/authenticity detection
- **DualShock 4** — firmware build date, hardware/software version, board model (JDM-001 through JDM-055), Bluetooth address, clone/authenticity detection
- **DualSense / DualSense Edge** — firmware versions (main, SBL, Venom, Spider), board model (BDM-010 through BDM-060X), serial number, color variant, MCU unique ID, PCBA ID, battery barcode, VCM barcodes, Bluetooth address

### Device Management
- **Hotplug** — controllers are detected and removed in real time
- **Battery status** — percentage and charging state displayed in the header bar and device list
- **Connection type** — Bluetooth or USB indicator
- **Controller icons** — 30+ custom SVG icons for PlayStation, Xbox, Nintendo, Sega, Atari, and other controller families

### Output Testing
- **Vibration** — adjustable low-frequency and high-frequency motor intensity
- **Speaker** — plays a 440 Hz test tone through the DualSense's built-in speaker via PipeWire (USB only)

## Installation

### Dependencies

- GCC or Clang (C11)
- Meson ≥ 0.59
- GTK 4
- Libadwaita 1
- SDL3 ≥ 3.2.0
- libevdev ≥ 1.11.0
- libudev ≥ 249
- libpipewire 0.3 (optional, for speaker testing)

### Building from source

```bash
meson setup build --prefix=$HOME/.local
ninja -C build
ninja -C build install
```

Then run `haptika` from your terminal or find it in your application launcher.

## Usage

1. Launch Haptika
2. Connect a controller — it appears in the device list automatically
3. Click a controller to open the input viewer
4. Press buttons, move sticks, pull triggers — everything updates in real time
5. Use the header bar buttons to:
   - Switch between SDL3 and evdev backends
   - View controller hardware info (Sony controllers)
   - See battery level and connection type

### Permissions

Haptika needs read access to `/dev/input/event*` devices. If your controller doesn't appear, you may need to add your user to the `input` group:

```bash
sudo usermod -aG input $USER
```

Then log out and back in.

For DualShock 4/DualSense hardware info, Haptika also needs access to `/dev/hidraw*` devices. On most modern distributions this works automatically. If the info dialog shows an error, your system may need a udev rule granting access to Sony HID devices.

## Technical Details

Haptika is written in pure C11 with no C++ dependencies. The UI is built entirely in code (no `.ui` XML files) using GTK 4 and Libadwaita widgets. Custom widgets for analog sticks, touchpad, tilt indicator, and button indicators use `GtkDrawingArea` with Cairo rendering.

The architecture follows a clean module separation:

- **device_manager** — unified device tracking across SDL3 and udev, with topological hardware matching to merge both views of the same physical device
- **input_session** — abstract polling interface with SDL3 and evdev implementations
- **input_viewer** — dynamic UI that builds itself from the device's reported capabilities
- **widgets/** — reusable custom GTK 4 widgets for gamepad visualization

The SDL3 backend polls at 1000 Hz (1 ms timer) for sub-millisecond input latency. The evdev backend uses `poll()` for event-driven, zero-latency input processing. So Haptika can also be used as an input lag tester of sorts!

## License

Haptika is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).

## Contributing

Contributions are VERY welcome (I don't have every controller ever released). Please open an issue first to discuss what you'd like to change.

## Acknowledgments

- [SDL3](https://libsdl.org/) for the gamepad abstraction layer
- [libevdev](https://freedesktop.org/wiki/Software/libevdev/) for raw evdev access
- [dualshock-tools](https://dualshock-tools.github.io/) for DS4 reverse engineering reference
- [dualsensectl](https://github.com/nowrep/dualsensectl) for DualSense reverse engineering reference
- [DsHidMini](https://github.com/nefarius/DsHidMini) for DualShock 3 OUI database
