# Enttec DMX-USB-Pro inbound — hardware-required test

The Phase 4 Enttec path (Windows-only v1) is not exercised by any CI
test because:

1. The plugin's serial reader opens a real FTDI VCP COM port (Win32
   `CreateFile("\\\\.\\COM<n>", ...)`), which requires actual
   hardware or a virtual COM pair (see "Optional com0com test"
   below).
2. The Pro spec'd DMX framing requires either a real DMX-USB-Pro
   firmware (which speaks the framing) or a faithful emulator.
3. The downstream "did a mapping fire?" gate already has unit-test
   coverage for the JSON parser (`DmxMappingTests`) and integration-
   test coverage for the UniverseTable arbitration path
   (`dmx_artnet_loopback_test.py`); the only new code under test
   here is the COM port enumeration + Enttec frame state machine.

## Manual hardware gate (run before merging Phase-4 changes)

You'll need:

- One Enttec DMX-USB-Pro (USB → DMX-512) with current firmware.
- One DMX-DMX loopback cable, OR a DMX fixture that drives channel 1
  high on a fixed schedule (a dimmer pack with channel 1 routed to a
  fader at 100% works).
- Windows with the FTDI Virtual COM Port driver installed (Enttec
  ships this; it's standard on modern Windows).

### Recipe

1. Plug the Pro into a USB port. Confirm it enumerates as a COM port
   in Device Manager → Ports.
2. Wire the loopback cable: DMX OUT → DMX IN on the same Pro (Pro
   ignores its own output by default; the loopback exercises the
   in-frame parse via a separate DMX source instead — see
   alternative below).

   Alternative: bring the second DMX source online, set its first
   universe to drive channel 1 = 255.
3. Edit `%APPDATA%/Entity/settings.json` and set:
   ```json
   {
     "dmxEnttecEnabled": true,
     "dmxEnttecPort": "",
     "dmxEnttecUniverse": 0
   }
   ```
   Leaving `dmxEnttecPort` empty makes the plugin auto-pick the
   first FTDI device.
4. Launch `EntityMediaEditor.exe` (no `--headless`).
5. Open Preferences → DMX section, confirm the Enttec port dropdown
   shows your device name (the plugin writes
   `%APPDATA%/Entity/dmx_enttec_devices.json` on register; the UI
   reads it).
6. Send channel 1 = 255 from the external source.
7. Editor log (stdout) should show:
   ```
   [plugin:engine] [INFO] dmx-artnet: Enttec opened on COMx
   [plugin:engine] [INFO] fired Play (inbound u=0 ch=1 value=255 label='Play')
   ```
8. Confirm the Timeline transport flipped to Playing.

### Recording the pass

Note the result in the PR description under a "Manual gates" section:

```
- [x] Enttec DMX-USB-Pro inbound, Windows, COM<N>, fired Play on ch1 edge
```

## Optional com0com test (no real hardware)

[com0com](https://com0com.sourceforge.net/) creates a virtual COM port
pair (e.g. COM10 ↔ COM11). One side opens as a normal COM port to the
editor; the other side accepts writes from a test script. The plugin
can't tell the difference from a real FTDI device IF you spoof the
right hardware ID — which com0com doesn't do by default.

This means the editor's `EnttecSerial::enumerateEnttecDevices` won't
find a com0com port (because they don't carry `VID_0403&PID_6001`).
You'd need to either:

1. Modify the test to pre-write
   `%APPDATA%/Entity/dmx_enttec_devices.json` with the com0com port
   so the editor reads it via the sidecar (but this only affects the
   UI dropdown; the plugin's auto-pick path still calls
   `enumerateEnttecDevices`).
2. Add an opt-in env var `ENTITY_DMX_ENTTEC_PORT_OVERRIDE=COM11`
   (already supported via `ENTITY_DMX_ENTTEC_PORT`) that bypasses
   enumeration and just opens the named port.

If you want to wire option (2) into CI:

```python
# scripts/integration/dmx_enttec_com0com_test.py
# Skipped cleanly if com0com isn't installed or the COM pair isn't
# available. Drives the editor's Enttec inbound via the virtual pair.
```

The test isn't shipped in this PR — it's a follow-up worth filing
once anyone actually has com0com on a CI runner. Right now the
hardware gate is the only real verification path.

## Pre-flight checks

If the editor doesn't open your Enttec:

- `Get-PnpDevice -Class Ports | Where-Object FriendlyName -Like '*USB Serial*'`
  in PowerShell should list your device.
- Confirm the hardware ID matches `VID_0403&PID_6001`. Older Pros
  use that; some "Pro Mk2" variants use a different PID and won't
  be detected by v1 of this plugin.
- The plugin opens the port at 250000 baud, 8N2. If something else
  (Enttec's own DMX Workshop, for instance) is holding the port,
  `CreateFile` will fail with `ERROR_ACCESS_DENIED`. Close the
  conflicting app.

## Out-of-scope for v1

- USB-Pro Mk2 (different protocol).
- Enttec Open-DMX (no firmware framing — uses raw FT232 timing).
- macOS / Linux Enttec (no `SetupAPI`; needs libftdi or libserialport).
- Hot-plug detection.
- Enttec outbound (v1 is read-only).
