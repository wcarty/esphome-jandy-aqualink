# 🧠 ESP32 Jandy Aqualink keypad presence and read decoder

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

Date: 2026-05-30
Status: Approved design, pre-implementation
Scope of this push: keypad presence plus full reads into Home Assistant. No writes.

## Goal

Run enough of the Jandy Aqualink RS protocol directly on the M5Stack Atom Lite,
at the bus, to make the panel send us its display and status, then decode those
into Home Assistant values. No separate computer, no WiFi in the timing-critical
path. The aim is a solid, publishable working example, since no public ESP32
Jandy keypad-presence implementation exists today.

This push deliberately stops short of writes (changing a setpoint). Writes touch
real equipment and are a separate, gated effort.

## Background and the key finding

The panel (Aqualink RS-8) is the bus master. It polls each device address in
turn. A live read-only capture off the existing WiFi bridge proved two things:

1. Our frame decoder is correct. About 1,000 consecutive live frames parsed with
   zero checksum errors.
2. With nothing answering polls, the panel transmits only polls. Temperatures,
   setpoints, salt, and the display message never appear, because the panel only
   sends a keypad's display and status once that keypad has checked in.

This was confirmed by stopping the AqualinkD container: every `aqualinkd.*`
entity in Home Assistant went `unavailable`, and the bus fell to polls only.

So the unlock is keypad presence: answer one keypad address's poll within roughly
20 to 40 ms, every time. A WiFi round trip misses that window (the established
Phase 2 conclusion). A chip on the bus meets it easily.

## What already exists

A Python host prototype under `esp32-experiment/` with 32 passing unit tests:

- `jandy/frames.py`: streaming DLE/STX/ETX extraction, byte de-stuffing, checksum.
- `jandy/display.py`: decode cmd 0x25 display lines, pair labels with values.
- `jandy/status.py`: decode the pool-temp byte in the cmd 0x0C status frame.
- `jandy/decoder.py`: ties the layers together, tracks state and message types.
- `capture.py`: read-only live capture and discovery from the TCP bridge.

This logic is the reference oracle. The firmware ports the same state machine to
C++ and is checked against the same frame vectors.

## Architecture

One ESPHome external component, Arduino framework, matching the current firmware.

- The component installs the ESP-IDF UART driver directly on GPIO19 (TX) /
  GPIO22 (RX) at 9600 8N1, and creates a high-priority FreeRTOS task pinned to
  core 1.
- The core-1 task does only the time-critical work: read bytes, assemble frames,
  and the instant it sees a poll addressed to our keypad, write the reply. This
  is the ported frame state machine plus the presence responder.
- ESPHome runs on core 0 and owns everything slow and non-critical: WiFi, the
  Home Assistant native API, OTA with safe-mode and auto-rollback, the AP
  fallback, and a raw-frame debug feed.
- Decoded values pass from the core-1 task to the ESPHome loop through a small
  lock-protected shared state struct.

Rationale: this matches the brief's "RS485 state machine pinned to core 1, WiFi
on core 0" recommendation while staying deployable over the air with rollback
safety, which the wall-mounted Atom needs.

## Protocol specifics

Frame on the wire: `[optional 0x00] 10 02 <dest> <cmd> <data...> <cksum> 10 03`.
Checksum is `sum(logical_frame[:-3]) & 0xFF`. A 0x10 in the payload is stuffed as
`10 00` and de-stuffed on receive. All verified against live frames.

Presence reply (fixed, from the AqualinkD source): `10 02 00 01 00 00 13 10 03`.
Destination 0x00 (the master), command 0x01 (ACK), two data bytes
`[ack_type=0x00, pending_key=0x00]`, checksum 0x13. The reply does not depend on
the poll content, so the core-1 task can emit it immediately on any poll to our
address.

Keypad address: must be one no real keypad uses, or two devices answer the same
poll and corrupt the bus. Default to the exact address AqualinkD was emulating
(known conflict-free); retrieve it from `aqualinkd.conf` before going live. Make
it a component setting.

## Components and data flow

```
UART RX bytes
  -> [core 1] FrameExtractor -> Frame
       -> if poll to our address: write presence ACK immediately
       -> else: DisplayReader + status decode -> update shared state
shared state (mutex)
  -> [core 0] ESPHome loop: publish sensors to HA, broadcast raw frames to debug feed
```

C++ modules mirror the Python ones: a `FrameExtractor`, a `DisplayReader`, a
status decoder, a `PresenceResponder`, and a thin ESPHome glue layer exposing
sensors. Each unit is independently testable with the existing frame vectors.

## Home Assistant integration

Publish decoded values as native ESPHome sensors (the device already speaks the
encrypted API to HA, so no MQTT broker is needed). Expected to surface from the
auto-cycling display: air, pool, and spa temperature, and likely the heater
setpoint and equipment status. Salt percent may require the later navigation
feature; we decode whatever the panel actually sends and confirm empirically.

## Safety

- The pending-key byte is hard-coded to 0x00 (no key). An ACK with no key is
  inert: it announces presence but issues no command and navigates no menu, so it
  cannot change a setpoint, toggle equipment, or move a valve.
- This build contains no code path that ever sends a non-zero key. Writes are out
  of scope and would be a separate, gated feature.
- AqualinkD stays stopped for the duration, so only one device emulates the
  address.
- The current working `pool-bridge.yaml` is saved verbatim; one OTA reverts to
  today's read-only bridge.
- Pool is about 91 F, heater OFF, and stays that way.

## Testing strategy

1. Port the frame, display, and status logic to C++ and check it against the
   same frame vectors the Python suite uses (host build or on-device self-test).
2. Deploy presence. Using the debug feed and ESPHome logs, confirm the panel
   keeps polling our address (does not mark us disconnected) and begins sending
   display and status. This is also the live test of the auto-direction
   assumption: if our reply reaches the bus, presence registers.
3. Measure reply latency in the debug feed to confirm it sits well inside the
   window.
4. Watch the checksum-error rate. A spike means an address collision; revert
   immediately.
5. Confirm decoded values match reality (pool about 91 F).

## Risks and mitigations

- Auto-direction assumption (no DE pin configured today, yet reads work). Proven
  the moment presence registers; if replies do not reach the bus, investigate the
  ATOM RS485 base direction control and add a DE GPIO.
- ESPHome task timing under WiFi load. Mitigated by core-1 pinning; measured via
  reply latency.
- Address collision with a real keypad. Detected by a checksum-error spike;
  reverted via OTA.
- Reply-window margin. Measured live, not assumed.

## Rollback plan

Keep `pool-bridge.yaml` saved. If presence misbehaves or the bus degrades, OTA
the original bridge config back. AqualinkD can be restarted by the founder from
the Unraid Docker tab if we want to return to the prior state entirely.

## Out of scope

- Writes and menu navigation (setpoint changes, equipment toggles).
- Salt percent if it only appears via navigation.
- The production Raspberry Pi track. This experiment is separate and must not run
  a second keypad emulator on the bus at the same time.

## Open items to resolve before go-live

- Retrieve the keypad address AqualinkD was emulating from `aqualinkd.conf`.
- Confirm the exact ack_type value from the AqualinkD source (0x00 expected for a
  normal idle ACK).
