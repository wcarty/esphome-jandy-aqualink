# ESPHome Jandy AquaLink (ESP32-native, at the bus)

An ESPHome external component that talks to a **Jandy AquaLink RS** pool panel over
its RS485 bus from an ESP32 wired directly to the bus. It does the timing-critical
work **at the bus**, on a dedicated CPU core, so there is no separate Linux box and
no WiFi in the real-time path. It reports into, and is controlled from, **Home
Assistant natively**.

Built and proven against a real AquaLink RS-8 panel with a Pentair IntelliFlo VS
pump on the same bus, on an M5Stack Atom Lite (about $20 of hardware).

## What it does

Reading (no actuation):
- Holds an emulated AllButton keypad (`0x08`) and answers the panel's poll in-slot
  in about **110 microseconds**, against the panel's ~20-40 ms reply window, framing
  the whole bus with **zero checksum errors**.
- Emulates the iAqualink controller (`0x33`) to read **pool / spa / air temperatures**
  and decode live **equipment status** (filter pump, cleaner, spa mode, blower).
- Reads **pump speed (RPM) and watts** from the panel's status page.
- Passively reads **AquaPure salt chlorinator** output percentage, salt level,
  operating state, and faults from its normal RS485 poll/reply traffic.

Control (all gated, off by default, see Safety):
- **Pump speed** by exact RPM or presets, salt-cell-flow aware.
- **Filter pump, cleaner, lights, blower**, and **pool / spa mode** switching.
- **AUX2** and **AUX6** direct circuit toggles (for the configured color wheel
  and Stenner dosing pump).
- **Heaters** on/off and **temperature setpoints** (pool and spa).

Home Assistant can run it as a **scheduler**: a narrow, restart-surviving permission
lets HA set pump speed and toggle the filter pump and cleaner unattended, while
everything riskier stays behind the master interlock.

## Safety model

This drives live equipment, so writes are conservative by construction:
- A **master interlock** switch gates every write and **boots OFF** on every restart.
  With it off, the component is inert presence only.
- Writes also require the iAqualink presence channel to be on.
- The autonomous **scheduler** permission is scoped **per keycode** to exactly the
  filter pump and cleaner (plus pump speed). It can never fire a heater, valve, or
  light, even when armed.
- Reads are view-only and never send an equipment key.
- Only one device may emulate a given bus address at a time. Do not run another
  emulator (AqualinkD, aquaweb) on the same address concurrently.

The control surface is the **encrypted Home Assistant native API** plus OTA. The
ESPHome `web_server` is intentionally left disabled, because it would otherwise be an
unauthenticated control endpoint on the local network.

## Why this is interesting

Established local Jandy control (AqualinkD, the gold standard) runs on a separate
always-on Linux box wired to the bus, because the reply window is too tight for a
WiFi bridge. This component meets that window **on the ESP32 itself**: the bus state
machine runs in a FreeRTOS task pinned to core 1, while WiFi and the Home Assistant
API run on core 0, so the network can never delay a reply. The result is full panel
presence, reading, and gated control on roughly $20 of hardware, native to Home
Assistant, over the air updatable, with no extra computer.

## Hardware

- An ESP32. Tested on an **M5Stack Atom Lite**.
- An RS485 transceiver. Tested with the **M5Stack ATOM RS485 base** (SP3485,
  automatic direction control, so no direction GPIO is needed).
- A connection to the panel's RS485 bus (the AUX / RS485 terminals): A, B, and
  ground. Get A/B polarity right; reversed shows no valid frames, correct shows a
  steady stream of checksum-clean frames.

Default pins match the M5 ATOM RS485 base: **GPIO19 = TX, GPIO22 = RX**, 9600 baud,
8N1. Override them in the config if your wiring differs.

## Install

This is an ESPHome external component. A complete, working example device config is
in [`firmware/pool-bridge.yaml`](firmware/pool-bridge.yaml). Minimal start:

```yaml
external_components:
  - source: github://4pBdhJoZ3Xy3reVvBoU9C3YPzyXDDU/esphome-jandy-aqualink
    refresh: 0s

jandy_aqualink:
  keypad_address: 0x08
  polls_answered:
    name: Jandy Keypad Polls Answered
  reply_latency:
    name: Jandy Keypad Reply Latency
  checksum_errors:
    name: Jandy Bus Checksum Errors
```

After it boots, `Polls Answered` should climb steadily and `Checksum Errors` should
stay at zero.

## Picking a keypad address

The panel supports up to four AllButton keypads at `0x08` to `0x0B`. Emulate one no
real keypad uses, or two devices will answer the same poll and corrupt the bus.
`0x08` is a good first try. Watch `Checksum Errors`: zero means no collision; if it
climbs, switch addresses. Temperatures and the richer control use the iAqualink
controller slot (`0x33`), which on the test panel was free because its wireless
controller had long since died.

## Notes and limitations

- Setpoint writes are confirmed by equipment behavior, not by a readback. The panel
  does not echo the target back to this controller, so a write is verified by the
  heater firing or settling, not by reading the number back.
- Salt chlorinator readings are read-only. The component does not change output,
  boost, or any other AquaPure setting.
- The heater-enabled status decode is unreliable on the test panel; trust the panel's
  own heat indicator over that sensor.

## Repository layout

- `components/jandy_aqualink/` the ESPHome component. `jandy_proto.*` is the pure
  protocol logic (framing, de-stuffing, checksum, decoders, frame builders) with no
  Arduino or ESPHome dependency; `jandy_aqualink.*` is the hardware and ESPHome glue
  with the gated control state machines; `__init__.py` is the config codegen.
- `firmware/pool-bridge.yaml` a complete example device config.
- `jandy/`, `tests/` a Python reference implementation of the protocol logic and its
  unit tests. The C++ mirrors these and self-tests the same vectors on boot.
- `docs/` the roadmap and the design and build history.

## Credits

Protocol understanding stands on the shoulders of
[AqualinkD](https://github.com/sfeakes/AqualinkD) and
[aquaweb](https://github.com/earlephilhower/aquaweb). This is an independent
ESP32-native reimplementation, not affiliated with Jandy or Zodiac.
