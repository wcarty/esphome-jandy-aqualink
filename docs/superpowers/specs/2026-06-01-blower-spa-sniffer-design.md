# 🧠 Spec: blower / spa-mode sniffer on the inert keypad seat

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

Date: 2026-06-01
Status: approved (brainstorm), pre-implementation
Repo HEAD at design time: `a724398`

## Why

Since the evening of 2026-05-31 the spa air blower has been switching on by
itself, repeatedly, on the panel's own clock. The founder put the panel in
Service mode for relief and wants to understand what is happening before
choosing any fix. The root-cause work in `project_pool_controller_phase2.md`
established that the blower is a symptom of spa mode, and that the panel's stored
schedule forces spa mode on. What is missing is a clear, trustworthy record of
when the spa/blower events fire, and confirmation that the panel is doing it on
its own rather than our box.

This spec covers a read-only monitor that answers those questions. It is also a
useful stepping stone to any later fix (HA-as-scheduler or a panel wipe), because
both require knowing what the panel actually does and when.

## What it answers, and what it cannot

- WHEN: yes. Log every spa-mode and blower on/off transition with a timestamp and
  chart it in Home Assistant.
- IS IT US: yes. The monitor runs on the inert AllButton keypad seat (address
  0x08), which is a separate mechanism from the iAquaLink controller emulation
  (0x33) the founder worried about. The 0x33 seat stays OFF for this work.
- WHY, as in which stored program by name: NO. The schedule executes inside the
  panel's memory and never announces itself on the bus. We see the effect, not
  the program. Naming the program is the deeper MENU-reading effort, out of scope
  here.

## Background: what we already have

- Our ESP box continuously holds AllButton keypad presence at address 0x08. The
  reply is `ACK_PRESENCE = 10 02 00 01 80 00 93 10 03` (ack_type 0x80 =
  ACK_ALLB_SIM). This is unconditional in `task_loop` (`is_poll_to(f,
  keypad_addr_)`), so it is on right now and has been all along. "Silent" in prior
  notes meant the 0x33 emulation was off, not that the box was off the bus.
- Because we answer as an AllButton keypad, the panel streams us a steady run of
  `CMD_STATUS` (0x02) frames: the equipment LED bitmap (example seen Session 2:
  `10 02 08 02 10 14 00 00 00 40`).
- `observe_frame()` currently feeds every frame to the passive temp reader and
  records the frame in a dest/cmd census, but it does NOT decode the LED bitmap.
  The status frames arrive and are counted; their bits are never parsed.
- Reference for the bitmap layout: AqualinkD `source/allbutton.c`
  `processLEDstate()`. Each circuit LED occupies TWO bits in the status payload,
  an "on" bit and an adjacent "flash" bit (`(raw>>(bit+1))&1` = flash,
  `(raw>>bit)&1` = on), packed in the panel's button order. Heaters use two LED
  slots (off/enabled/on). We will use this as the layout hypothesis and confirm
  the exact bit positions on this panel empirically (see Discovery).

## Design

### Principle: strictly read-only

The box adds NO new transmissions to the bus. It already replies to 0x08 polls
with the inert presence ack; this work only decodes inbound `CMD_STATUS` frames
and publishes the result. The control interlock stays OFF. No key is ever armed
or sent. The 0x33 iAquaLink presence stays OFF. Safety posture is identical to
the current resting state.

### Circuits to decode

Priority (the founder's worry): spa mode, air blower. Free context from the same
frame (relevant to understanding the schedule): filter pump, cleaner. We do NOT
attempt to map all 15+ circuits (that is the full Phase 4 effort and is not
needed here).

### Status decoder (pure, testable)

A pure function takes a checksum-valid `CMD_STATUS` payload and returns the
on/off state of the named circuits, given a small per-panel bit map
(circuit -> byte index + bit offset). Keeping it pure lets us unit-test it
against captured real payloads, matching the project's existing TDD pattern
(Python suite mirrored by an on-device selftest). The per-panel bit map is a
constant filled in after Discovery.

### Change detection + logging

On each decoded status frame, compare against the last decoded state. On any
change to a tracked circuit, write one timestamped log line (for example
`STATUS CHANGE: spa_mode 0->1`) and update shared state for the publishing core.
This change-on-transition logging is the forensic record and avoids log spam from
the steady stream of unchanged frames.

### Home Assistant surface

Publish a binary_sensor per tracked circuit: Pool Spa Mode, Pool Air Blower, Pool
Filter Pump (status), Pool Cleaner. Home Assistant records their history
automatically, giving the founder a chart of exactly when each switched. Names
follow the existing `Pool ...` convention in `firmware/pool-bridge.yaml`.

### Threading and data flow

Unchanged from the current architecture. Core 1 runs the hard-real-time
poll/reply loop and calls `observe_frame()` after each reply (never before, so
the in-slot reply timing is preserved). `observe_frame()` runs the status decoder
and stores results in `volatile` fields under the existing `mux_`. Core 0's
`loop()`/`update()` reads those fields and publishes the binary_sensors and any
queued transition logs. No new bus I/O is introduced on either core.

## Discovery: confirming the bit positions safely

Before trusting any auto-published states, confirm which bits are spa mode and
the blower on THIS panel, with zero guessing:

1. Panel stays in Service mode (schedule paused, founder operates the panel).
2. Firmware (Build 1) logs the full raw `CMD_STATUS` payload as hex whenever any
   byte changes.
3. Founder toggles, one at a time, at the panel: blower on, blower off; pool->spa,
   spa->pool; (optionally filter pump, cleaner). Each single toggle isolates the
   bit(s) that moved.
4. Diff the before/after payloads to pin each circuit's byte+bit. Save the labeled
   payloads as test fixtures.

This is completely safe: Service mode, the founder's hand on the panel, our box
only listening.

## Phasing

1. Build 1: raw status-change logger. Adds the "log full payload hex on change"
   path in `observe_frame` for `CMD_STATUS` to 0x08. No new sensors yet. Deploy
   (compile + OTA via the ESPHome dashboard at `192.168.1.126:6052`, watch for
   `selftest PASS`). Read-only.
2. Discovery (founder, Service mode): the toggle test above. Produces the bit map
   and labeled fixtures.
3. Build 2: status decoder (TDD against the fixtures) + the four binary_sensors +
   transition logging. Mirror the decoder in the on-device selftest. Deploy.
4. Capture (founder, Auto mode): return the panel to Auto and let the monitor
   record the timeline. Founder can return to Service mode for quiet at any time
   (coverage pauses during those stretches).

Optional later: a fully-dark causality check (drop even the AllButton presence
for a window, confirm the blower still fires with the box provably off the bus).
Offered, not required, since the overnight run with the 0x33 seat already off is
strong evidence.

## Testing

- Unit tests (Python suite) for the status decoder against the labeled real
  payloads from Discovery: each fixture asserts the expected circuit states.
- On-device C++ selftest mirrors the same vectors (existing pattern; do not
  actuate on a FAIL).
- Live verification: Service-mode toggles show the binary_sensors tracking the
  founder's manual changes correctly before any autonomous capture is trusted.

## Success criteria

- In Service mode, toggling the blower and pool/spa at the panel flips the
  matching Home Assistant binary_sensor every time, correctly.
- In Auto mode, Home Assistant history shows a clean timeline of spa-mode and
  blower on/off events with timestamps.
- Zero new bus transmissions introduced; checksum errors stay at 0; reply latency
  unchanged; the interlock and 0x33 presence remain OFF throughout.

## Out of scope

- Naming the stored program responsible (needs the MENU/program-editor read).
- Editing, deleting, or wiping any schedule or program.
- Mapping the full equipment LED set beyond the four tracked circuits.
- Any control action. This is monitoring only.

## Risks / open questions

- The AllButton LED order on this panel may differ from AqualinkD's default
  button order. Mitigated by empirical Discovery rather than assuming the layout.
- Spa mode may surface as a valve/Spa LED rather than a distinct "spa mode" bit.
  Either way the blower bit alone answers the founder's literal question; spa mode
  is confirmed during Discovery.
- Service mode could alter what the panel reports on the bus. If the status
  stream looks different in Service vs Auto, note it during Discovery and re-pin
  positions from an Auto-mode capture if needed.
