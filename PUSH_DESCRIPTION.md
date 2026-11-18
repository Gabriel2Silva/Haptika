This merge is the result of over seven months of work on a separate branch.

Haptika started as a quick hobby project to test my controllers more
thoroughly. I import controllers from overseas, and most of them are
problematic or outright counterfeit. I was dissatisfied with older tools such
as jstest, and web-based HTML5 testers have always been unreliable in my
experience, so I wrote my own. I ended up dedicating so much time to it that
it grew far bigger and more complex than I ever imagined.

The size of this diff reflects that time. Most of the added lines are new
subsystems (InputPlumber, PipeWire, the DSP, the device manager and matching
layer, the GSK widgets, and a real test suite). This is a major refactoring of
Haptika 1.0.0, released as 2.0.0. It includes, but is not limited to:

**New features**

- InputPlumber integration: virtual controllers are detected, matched to their
  CompositeDevice over D-Bus, and shown with their profile, output capabilities,
  and physical sources. Source devices captured by InputPlumber are hidden so
  they no longer appear as broken duplicates. The monitor survives daemon
  restarts and system bus restarts.
- DualSense speaker test over PipeWire, with a portable C oscillator and
  validated playback formats.
- Multi-finger touchpad tracking, accelerometer tilt, gyroscope bars, and
  independent low/high vibration motors.

**Device management and reliability**

- A rewritten device manager with GObject-owned devices, off-thread udev
  probing, and a matching layer that coalesces the SDL and evdev nodes
  belonging to the same physical controller.
- Fast unplug/replug is handled without duplicate rows or stranded sessions.
  Stale SDL handles are released before a replay can fall back to the generic
  driver, which preserves battery and touchpad support after a reconnect.
- Input sessions report a lost device and return to the controller list instead
  of leaving a frozen viewer.
- Async tasks, cancellation, and sequence numbers guard device metadata and
  InputPlumber snapshots against races.

**Backends and input pipeline**

- SDL3 backend: standard gamepad mapping, battery and connection state,
  vibration, touchpad, and motion sensors, with a 1 kHz sampler that backs off
  to 4 ms when nothing changes.
- evdev backend: raw libevdev access to every button and axis, including
  pressure-sensitive controls that SDL does not expose, behind a bounded
  lock-free SPSC event ring with overflow detection and state resync.
- One-click switching between both backends when a controller exposes them.

**Interface**

- Symbolic controller icons (Controllercons, SIL OFL 1.1) rendered through GSK,
  with cached geometry for sticks, crosshairs, trigger bars, and the touchpad.
- A reworked controller list with battery level, connection type,
  virtual-device badges, and InputPlumber metadata.
- Theme persistence, accessible labels for icon-only controls, and error text
  rendered literally instead of as markup.

**Build, tests, and code quality**

- Warning level 3 builds cleanly with `-Dwerror=true`, with third-party and
  generated code kept out of the project warning set.
- Nine tests: unit coverage for the DSP, ring buffer, parsers, device matching,
  and speaker routing, plus integration tests for device-manager
  hotplug/reconnect (uinput) and InputPlumber monitor recovery over a private
  D-Bus daemon.

**Compatibility**

- This release requires GTK 4.16 and libadwaita 1.7 or newer. Distributions
  that built 1.0.0 will need to update those dependencies.

And many other things that I honestly don't remember, as I've been cooking on
this branch for more than seven months...

Many controllers are still untested. If something behaves oddly, an issue with
the model and the `lsusb` line is always welcome.

Haptika is currently in a very nice state, and I am finally satisfied with it.

Thanks to everyone whose tools and reverse-engineering notes made this
possible, and to the Controllercons author for the icon set.
