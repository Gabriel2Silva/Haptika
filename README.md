<p align="center">
  <img src="data/icons/hicolor/scalable/apps/io.github.gabriel2silva.Haptika.svg" width="128" height="128" alt="Haptika icon">
</p>

<h1 align="center">Haptika</h1>

<p align="center">
  A fast, interactive gamepad testing application for Linux.<br>
  Built with pure C, GTK 4, and Libadwaita.
</p>

---

Haptika is a native Linux application for testing and inspecting game controllers. It lets you verify buttons, analog sticks, triggers, touchpads, motion sensors, vibration, and DualSense speakers with live visual feedback.

Built for the GNOME desktop in pure C11, GTK 4, and Libadwaita, Haptika follows the GNOME Human Interface Guidelines and supports both SDL3 and raw Linux evdev backends.

## Features

### Input Testing
- **Buttons & D-pad** — lights up each button in real time with accessible state reporting
- **Analog sticks** — circular position visualizers showing continuous travel and deflection
- **Triggers & axes** — unipolar and centered bipolar bars for triggers, analog face buttons, and gyroscope axes

### Advanced Input
- **Touchpad finger tracking** — live multi-touch finger tracking on DualShock 4 and DualSense touchpads
- **Motion sensors** — 2D tilt visualizer for accelerometer data and bipolar bar indicators for gyroscope X/Y/Z axes
- **Pressure-sensitive buttons** — reads analog pressure axes exposed on DualShock 3 controllers

### Dual Backend
- **SDL3** — default backend providing standardized gamepad mapping, vibration, touchpad, motion sensors, and battery telemetry
- **evdev** — raw kernel input backend exposing every axis and button reported by `libevdev` without a mapping layer
- **One-click switching** — switch between SDL3 and evdev on any controller directly from the header bar

### Hardware & InputPlumber Inspection
- **DualShock 3** — Bluetooth address lookup and Sony OUI authenticity check
- **DualShock 4** — firmware build date, hardware/software version, PCB board model (`JDM-001` through `JDM-055`), Bluetooth address, and USB hardware authenticity check
- **DualSense / DualSense Edge** — firmware versions (main, SBL, Venom, Spider), PCB board model (`BDM-010` through `BDM-060X`), plus USB-only factory serial number, color variant, MCU unique ID, PCBA ID, battery/VCM barcodes, and Bluetooth address
- **InputPlumber inspection** — detects virtual UHID/uinput controllers managed by InputPlumber, matches them to their `CompositeDevice` over D-Bus, and displays virtual bus/address, active profile, output capabilities, and physical source devices

### Device Management
- **Hotplug** — live detection and removal via SDL3 and `libudev`
- **Battery telemetry** — charge percentage and charging state in the device list and viewer header bar (including sysfs power supply fallback for DualShock 3)
- **Connection type** — wired USB vs wireless Bluetooth status resolved via SDL3 and sysfs/udev bustype
- **Custom iconography** — 31 custom `-symbolic.svg` controller icons across PlayStation, Xbox, Nintendo, Sega, Atari, and other families
- **Theme switcher** — Follow System, Light, and Dark styles persisted via GSettings

### Output Testing
- **Vibration** — independent low-frequency and high-frequency motor intensity sliders and test trigger
- **Speaker** — plays a 440 Hz test tone through the DualSense controller's built-in speaker via PipeWire (available when connected over USB)

## Installation

### Dependencies

- GCC or Clang (C11)
- Meson and Ninja
- GTK 4
- Libadwaita 1
- SDL3 ≥ 3.2.0
- libevdev ≥ 1.11.0
- libudev ≥ 249
- libpipewire-0.3 (optional, for DualSense speaker testing)

### Building, testing, and installing from source

```bash
meson setup build --prefix=$HOME/.local
meson compile -C build
meson test -C build
meson install -C build
```

Then run `haptika` from your terminal or launch it from your application menu.

## Usage

1. Launch Haptika
2. Connect a controller — it appears automatically in the device list
3. Select a controller to open the input viewer
4. Test buttons, sticks, triggers, touchpad, motion sensors, vibration, and speakers
5. Use the header bar controls to switch between SDL3 and evdev, inspect Sony hardware info or InputPlumber details, and monitor battery/connection status

### Permissions

Haptika needs read access to `/dev/input/event*` devices. If your controller does not appear, add your user to the `input` group and log out and back in:

```bash
sudo usermod -aG input $USER
```

DualShock 4 and DualSense hardware info also reads `/dev/hidraw*`. Most modern distributions grant access to active desktop sessions via udev `uaccess`; if reading hardware info fails, check your hidraw udev rules for Sony devices (`054c:*`).

## Technical Details

Haptika is written in pure C11 (`c_std=c11`) plus optional x86-64 GNU assembly DSP kernels, with no C++ source code or external UI XML files — all interface layout and dialogs are constructed in code using GTK 4 and Libadwaita.

Custom input visualizers (`ConttestAnalogStick`, `ConttestTiltIndicator`, `ConttestTouchpad`, `ConttestTriggerBar`) subclass `GtkWidget` directly and emit GPU render nodes (`GskPath` fills/strokes, rounded clips, and solid color nodes) through `GtkSnapshot` rather than CPU-rasterized Cairo surfaces.

The codebase is organized into focused modules:

- **`device_manager` / `device_matching`** — tracks controllers across SDL3 and `libudev`, correlates SDL devices and evdev syspaths via common `hid`/`bluetooth`/`input`/`usb_interface` ancestors, resolves wired vs wireless bus types, and hides source evdev nodes claimed by InputPlumber
- **`inputplumber` / `inputplumber_logic` / `inputplumber_info`** — monitors `org.shadowblip.InputPlumber` over D-Bus, matches deterministic UHID devices via FNV-1a MAC derivation and uinput devices without ambiguous cross-attribution, and presents virtual/source metadata
- **`input_session_sdl3` / `input_session_evdev` / `input_session`** — `input_session_sdl3` samples controller state on display vsync via `gtk_widget_add_tick_callback`; `input_session_evdev` runs a `poll()` worker thread pushing normalized kernel events into a lock-free C11 SPSC ring buffer (`ConttestEventRing`) scheduled onto the GTK main loop
- **`speaker_test` / `speaker_routing` / `dsp_oscillator`** — performs two-phase PipeWire registry and `pw_node_info` discovery to target the DualSense ALSA speaker sink without fallback routing, masks haptic channels silent, and synthesizes a 440 Hz tone using a pure C11 oscillator or runtime-dispatched x86-64 `AVX2+FMA` / `SSE2` assembly kernels (`src/dsp_oscillator_x86_64.S`)
- **`controller_info_parsers` / `controller_info_utils` / `ds*_info`** — asynchronous `GTask` hidraw feature-report and udev OUI parsers with UTF-8 sanitization
- **`widgets/`** — custom `GtkWidget` indicators and GPU `GtkSnapshot` vector drawing utilities (`drawing_utils.c`)

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
