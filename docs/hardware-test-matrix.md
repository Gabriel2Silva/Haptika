# Hardware Test Matrix

This checklist covers behavior that the unit tests cannot reach: SDL
re-enumeration, device matching on real buses, InputPlumber targets, and
PipeWire playback. Run it before merging changes to `device_manager.c`,
`device_matching.c`, `input_session_sdl3.c`, or the InputPlumber code.

Record the result per controller in the table at the end. A row is only
"pass" when every step listed for it was performed on the exact connection
type in the `Connection` column.

## Preparation

```sh
# Watch kernel events while plugging devices in and out.
sudo udevadm monitor --udev --subsystem-match=input --subsystem-match=hid --subsystem-match=power_supply

# Run Haptika with debug logging.
G_MESSAGES_DEBUG=all ./build/haptika
```

Use a build from the branch under test. Have at least two identical
controllers available for the receiver and duplicate-device checks.

## 1. SDL re-enumeration (H-1)

The manager must rebind an existing row to the new SDL instance ID instead of
keeping the old handle or creating a second row.

For each device class:

1. Connect the controller and open it in the viewer with the SDL3 backend.
2. Verify input, battery, and the connection icon.
3. Remove and reconnect the controller with a short gap (under 2 seconds).
4. Verify:
   - exactly one row for the controller in the selector;
   - the row opens with the SDL3 backend and input works;
   - battery and connection state refresh;
   - the touchpad and its contacts are still present in the viewer;
   - no `device-added`/`device-removed` churn for the same pad in the log.
5. Repeat the reconnect with a gap longer than the 50 ms safety-net tick.
6. Repeat over Bluetooth by power-cycling the controller, not by toggling the
   adapter.

Also run the same sequence after suspending and resuming the machine while the
controller stays connected.

## 2. Session loss (H-2)

1. Open a controller in the viewer.
2. Remove the controller while the viewer is open.
3. Expected: the window returns to the selector and shows a
   "Controller disconnected." toast. No frozen page, no blank visualizers.
4. Repeat with a Bluetooth controller by switching it off.
5. Repeat by suspending and resuming the machine while the viewer is open.
6. Verify that reopening the controller after the loss gives a working viewer.

A single toast is expected when the viewer notices the loss before the device
manager; two toasts indicate the loss was reported twice.

## 3. Multi-controller receivers and shared HID nodes (M-1)

Use a receiver with two controllers of the same model attached at once.

1. Connect both controllers through the receiver.
2. Confirm two rows, both usable, with correct input.
3. Open one, then the other, and confirm the viewer follows the selected row.
4. Remove one controller and confirm only that row disappears and the other
   keeps working.
5. Remove and reconnect the first controller while the second stays attached.
6. Expected: no merged row and no duplicate row for the same controller.

## 4. Multi-port adapters

Use the GameCube adapter with as many ports populated as available.

1. Confirm one row per populated port.
2. Confirm button names (Z, ZL/ZR full press) and axis behavior per port.
3. Remove the adapter and confirm all rows disappear.

## 5. InputPlumber targets

Run InputPlumber with each available target: DualSense USB emulation, DSX, and
Steam Deck (or whichever targets the installed version provides).

1. Confirm the virtual row shows the virtual bus, address, profile, and
   capabilities.
2. Confirm the physical source device is hidden from the selector and the
   window does not show a disconnection toast for the hidden physical node.
3. Switch the active profile and confirm the virtual row updates without a
   restart.
4. Restart InputPlumber and confirm Haptika recovers the virtual row (this also
   exercises the D-Bus monitor recovery path once implemented).
5. Repeat with a Bluetooth InputPlumber target if one is available.

## 6. Speaker test and PipeWire

Use a DualSense over USB.

1. Confirm "Test Speaker" is enabled and audible.
2. Confirm the hint appears and the button is disabled for a wireless
   DualSense.
3. Start the test and close the viewer immediately; confirm no crash and no
   stuck audio.
4. Restart PipeWire while the viewer is open and reopen the controller.

## 7. Generic HID controllers (extra axes)

Use a wheel, flight stick, or any pad that exposes more than six SDL joystick
axes.

1. Confirm extra axes are labeled `Axis N`, not DualShock 3 pressure names.
2. Confirm extra axes are not forced into unipolar trigger bars.

## Automated coverage

`meson test` covers part of this matrix without hardware:

- `unit-device-manager` creates a synthetic uinput gamepad and checks attach,
  ordered fast replug, replug with a lost SDL removal event, and a slow
  reconnect. Skips when `/dev/uinput` is not writable.
- `unit-inputplumber-monitor` runs a private D-Bus daemon with a fake
  InputPlumber service and checks that the snapshot is cleared on connection
  loss and rebuilt after a bus restart. Skips when `dbus-daemon` is missing.
- `unit-device-match` covers the hardware-key ambiguity rules and malformed
  D-Bus property types.

Everything else in this file needs the physical devices listed above.

## Results

### Verification record

`2026-11-18`, branch `inputplumber-and-fixes` with the SDL lifecycle fixes:

- DualShock 4 over USB: sections 1-2 pass. Fast replug (down to roughly half a
  second) keeps the SDL backend, battery indicator, and touchpad. Before the
  stale-handle release fix the same sequence left the pad on SDL's generic
  joystick driver, without touchpad or battery.
- DualShock 4 over Bluetooth: sections 1-2 pass. The controller itself takes
  several seconds to reconnect.
- Two DualShock 3 pads, one USB and one Bluetooth: section 3 pass. Two rows,
  both usable with battery state; removing one does not affect the other; no
  merges or duplicates.
- GameCube adapter with one controller across all ports: sections 1 and 4 pass.
  The row disappears and returns normally on each port change.
- InputPlumber DualSense target (GuliKit pad over Bluetooth): section 5 pass.
  The virtual row shows the full metadata, the physical source stays hidden
  with no toast, and disabling InputPlumber restores the physical pad row.
- DualSense speaker: playback works over USB, the wireless hint and disabled
  button appear over Bluetooth, and closing the viewer stops playback.
  Restarting PipeWire (section 6, step 4) was not exercised.

Not covered: two identical pads behind a shared USB receiver (section 3 with a
receiver), multiple populated GameCube ports, and section 7 (extra axes).

| Date | Device | Connection | Sections | Result | Notes |
|---|---|---|---|---|---|
| | DualShock 3 | USB | 1, 2, 6 | | |
| | DualShock 3 | Bluetooth | 1, 2 | | |
| | DualShock 4 | USB | 1, 2, 6 | | |
| | DualShock 4 | Bluetooth | 1, 2 | | |
| | DualSense | USB | 1, 2, 6 | | |
| | DualSense | Bluetooth | 1, 2 | | |
| | GameCube adapter | USB | 1, 4 | | |
| | USB receiver, 2 pads | USB | 1, 2, 3 | | |
| | InputPlumber DS5 | USB | 5 | | |
| | InputPlumber DSX | USB | 5 | | |
| | Generic HID wheel/stick | USB | 1, 7 | | |
