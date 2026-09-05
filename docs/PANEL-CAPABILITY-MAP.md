# Pool Panel Capability Map

> **Current feature note (2026-09-05):** This page began as the 2026-05-31
> read-only panel survey below. The component now also passively reads AquaPure
> salt chlorinator output, salt level, and faults; it provides direct,
> interlock-gated AUX2 and AUX6 controls. The slot map remains a snapshot of the
> surveyed panel, not a universal layout.

Date: 2026-05-31 (survey snapshot)
Source: live read-only survey of the AquaLink RS power center via the iAqualink
emulator at address 0x33 (firmware HEAD with compact page logging). Navigation
was view-only: no equipment, value, or commit button was pressed. Pump is a
Pentair Intelliflo VS (variable speed). All values are as observed during the
survey (pool mode, filter pump running).

## Pages this panel exposes over iAqualink

| Page | Type | How reached | Contents |
|---|---|---|---|
| HOME | 0x01 | pushed automatically | equipment buttons + temperatures |
| DEVICES | 0x36 | Home then Other Devices (key 0x18) | full equipment list |
| DEVICES2 | 0x35 | Next Page (key 0x21) from Status | macros / modes |
| STATUS2 | 0x2A | Status (key 0x06) | live pump model, RPM, watts |
| MENU | 0x0F | Menu (key 0x02) | renders EMPTY to our device (no items enumerated) |

The panel enumerates a page in full only on the first visit after our device
registers, then sends only changed buttons. To re-capture a full page, drop and
re-add presence (forces a fresh registration), then navigate.

Reference (AqualinkD) also defines SET_VSP 0x1e, SET_TEMP 0x39, SET_SWG 0x30,
MENU 0x0f, ONETOUCH 0x4d, COLOR_LIGHT 0x48. These were not walked this session.

## Readable values

| Value | Source | Wire format | Observed |
|---|---|---|---|
| Pool water temp | HOME, value 4 indices before "Pool Temp" | int degF | 89 |
| Spa water temp | HOME, "Spa Temp" (spa mode only) | int degF | not in pool mode |
| Air temp | HOME, "Air Temp" | int degF | 168 (sun-baked sensor) |
| Pump model | STATUS2 message line | text | "Intelliflo VS 1" |
| Pump RPM | STATUS2 line "    RPM: NNNN" | int after "RPM:" | 2750 |
| Pump watts | STATUS2 line "  Watts: NNNN" | int after "Watts:" | 1263 |
| AquaPure output | Passive command to SWG address `0x50`-`0x53` | percent | supported |
| Salt level | Passive AquaPure poll reply | ppm | supported |
| AquaPure status | Passive AquaPure poll reply | on / fault text | supported |

Notes:
- Pump RPM/watts are NOT on the bus passively. The panel sends the pump (address
  0x60) only empty polls; RPM/watts appear only as text on the STATUS2 page. So
  reading speed requires viewing STATUS2 (a brief navigation), unlike temps which
  the HOME page provides on its own.
- SWG / salt readings are NOT on any reachable iAqualink page. Checked 2026-05-31:
  HOME, DEVICES, DEVICES2, STATUS2, and MENU (which renders empty to us); paging
  forward from STATUS2 only reaches DEVICES2. Phase 1 saw "Generating" via
  AqualinkD, which almost certainly read the salt cell's own RS-bus frames, not an
  iAqualink page. This is now handled by the passive AquaPure decoder: output is
  read from panel command `0x11`, and salt ppm plus status are read from the
  following cell reply `0x16`.
- Pump RPM changes on the pump's own schedule (observed 2750 RPM / 1263 W stepping
  to 1700 RPM / 293 W during the survey, with no input from us), which confirms
  the reading tracks the live pump.

## Direct AllButton circuits

The component also supports the two configured direct circuit keys below. These
commands are not iAqualink grid keys, so they do not depend on the DEVICES-page
slot layout. They still require the master interlock.

| Circuit | AllButton key | Home Assistant entity |
| --- | --- | --- |
| AUX2 color wheel | `0x0A` | Pool Color Wheel |
| AUX6 Stenner dosing pump | `0x10` | Pool Stenner Dosing Pump |

## Controllable items: DEVICES page (0x36)

Keycode = 0x11 + slot, and is valid ONLY while the panel is displaying this page.

| Slot | Keycode | Device | Observed state | Risk |
|---|---|---|---|---|
| 0 | 0x11 | Filter Pump | ON | low (shipped) |
| 1 | 0x12 | Spa (valve) | OFF | medium (valves) |
| 2 | 0x13 | VSP1 Spd ADJ (opens the speed-set page) | - | medium (value set) |
| 3 | 0x14 | Pool Heat | OFF | high (heater) |
| 4 | 0x15 | Spa Heat | ENABLED | high (heater) |
| 5 | 0x16 | Cleaner | OFF | low (shipped) |
| 6 | 0x17 | Air Blower | OFF | low (shipped) |
| 7 | 0x18 | Pool Light | OFF | low (shipped) |
| 8 | 0x19 | Spa Light | OFF | low |
| 9 | 0x1a | HIGH SPEED | OFF | medium |
| 10 | 0x1b | Not Used | OFF | n/a (unconfigured) |
| 11 | 0x1c | Not Used | OFF | n/a (unconfigured) |
| 12 | 0x1d | Extra Aux | OFF | low |
| 13 | 0x1e | Sprinklers | OFF | low |
| 14 | 0x1f | Spa Mode | OFF | medium (valves) |

Slots 15-16 are blank scroll/indicator buttons (state 0xFF), not equipment.

## Controllable items: HOME page (0x01)

Keycode = 0x11 + slot, valid ONLY on the home page. These are the currently
shipped controls.

| Slot | Keycode | Device | Notes |
|---|---|---|---|
| 0 | 0x11 | Filter Pump | shipped toggle |
| 1 | 0x12 | Spa | shipped (pool/spa valve toggle) |
| 2 | 0x13 | Pool Heat | excluded (heater) |
| 3 | 0x14 | Spa Heat | excluded (heater) |
| 4 | 0x15 | Cleaner | shipped toggle |
| 5 | 0x16 | Air Blower | shipped toggle |
| 6 | 0x17 | Pool Light | shipped toggle |

## Macros: DEVICES2 page (0x35)

Keycode = 0x11 + slot, valid only on this page.

| Slot | Keycode | Macro |
|---|---|---|
| 0 | 0x11 | Clean Mode |
| 1 | 0x12 | Pool Mode |
| 2 | 0x13 | Day Party |
| 3 | 0x14 | All Off |

These are the OneTouch macros (the dedicated OneTouch key 0x03 reaches the same
set). The MENU page (key 0x02, type 0x0F) renders empty to the emulated device.

## CRITICAL: keycode meaning is page-dependent

The same keycode means different things on different pages. Examples:
- 0x13 = Pool Heat on HOME, but = VSP1 Spd ADJ on DEVICES, and = Day Party on DEVICES2.
- 0x14 = Spa Heat on HOME, but = Pool Heat on DEVICES, and = All Off on DEVICES2.

Any control that sends a grid keycode (0x11-0x1f) MUST confirm `current_page`
first. Sending the VSP-adjust keycode (0x13) while on HOME would toggle a heater.
This is the central safety rule for all future control work here.

## Pump speed SET path (next session, not built yet)

1. Navigate to DEVICES (Home, then Other Devices key 0x18); confirm page 0x36.
2. Press slot-2 keycode 0x13 (VSP1 Spd ADJ); confirm the panel opens SET_VSP (0x1e).
3. Value-set handshake (AqualinkD iaqtouch): reply ACK_CMD_READY_CTRL (0x80) to an
   ordinary poll to request the control slot; the panel then sends CMD_IAQ_CTRL_READY
   (0x31); reply with the 0x24 value frame carrying ASCII RPM digits (e.g. "2750").
4. Clamp 600-3450 RPM (Intelliflo VS = RPM/Speed mode, confirmed by the "Spd"
   label). The panel rejects out-of-range with an error popup.
5. Confirm by reading RPM back from STATUS2.

## Build roadmap (safe order)

1. Pump RPM/watts READING: DONE 2026-05-31 (STATUS2 decode + on-demand Read Pump
   Speed button, publishes Pool Pump Speed / Pool Pump Watts).
2. Pump RPM SETTING (the value-set handshake above): heavily gated, founder watching.
3. Spa Light, Extra Aux, Sprinklers (DEVICES toggles, low risk): add to a
   devices-page allowlist; press on the devices page only.
4. Spa Mode and other valve modes (medium risk): investigate valve behavior first.
5. Pool Heat and Spa Heat (high risk, LAST per founder): heaters.
6. SWG / salt READING (separate effort, not page-based): locate the salt cell in
   the bus census and decode its salt %/ppm/state frames passively. Not on any
   iAqualink page, so this is bus-frame reverse-engineering, lower priority.
