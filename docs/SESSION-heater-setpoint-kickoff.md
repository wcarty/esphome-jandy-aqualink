# 🗃️ Session kickoff: heater temperature setpoint (Phase 2 of heaters)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. Self-contained. Brainstorm before building.

## Paste this to start

> Let's build the pool heater temperature setpoint (Phase 2 of heaters): so the
> spa heats to my target of 94 instead of running to the 104 max, and the pool
> holds 85. Read the memory topic project_pool_controller_phase2.md (the Session 9
> section) first, plus the spec docs/superpowers/specs/2026-06-02-heaters-design.md
> and the Phase 1 plan docs/superpowers/plans/2026-06-02-heater-onoff-and-survey.md
> in C:\Users\Falcon\Documents\pool-controller\esp32-experiment. Heater on/off
> already works and is live-tested. The HOME heat button is just on/off (it does
> NOT open the temperature screen), so the setpoint is reached the other way, most
> likely the DEVICES route like the pump speed. Brainstorm with me first, then build
> with TDD and a founder-watched live test, starting with a short read-only survey
> to confirm how the temperature screen opens. I'm non-technical, so explain in
> plain English.

## Where we are (confirmed, Session 9, 2026-06-02)

- Heater **on/off** (pool + spa) is SHIPPED and live-tested. Both proven; the spa
  heater physically fired when enabled in spa mode. Repo HEAD on `master`, pushed.
- Resting state: pool mode, filter pump on, **both heaters off** (left off until we
  can set targets), interlock OFF, presence ON, auto-refresh/sniff OFF. Device
  `192.168.4.51`, dashboard `192.168.1.126:6052`.
- New controls this session: Pool Heat, Spa Heat on/off buttons; a "Switch to Spa
  Mode" button (mirror of Switch to Pool Mode).

## The key finding that defines this session

The HOME heat button is a **pure on/off toggle**. Pressing it does NOT open a
temperature screen (we stayed on HOME; two presses just toggle enable). So SET_TEMP
is NOT reachable from HOME via the heat button. **Most likely path** is the DEVICES
route, exactly like the pump speed value-set:

1. Navigate to DEVICES (Home, then Other Devices 0x18; confirm page 0x36).
2. Press the heat item ON DEVICES (DEVICES Pool Heat = `0x14`, Spa Heat = `0x15`,
   per docs/PANEL-CAPABILITY-MAP.md) to open SET_TEMP (page `0x39`).
3. Value-set handshake (already proven for pump RPM): reply ACK_CMD_READY_CTRL
   (`0x80`) on a poll, the panel sends CMD_IAQ_CTRL_READY (`0x31`), transmit the
   `0x24` value frame with the ASCII degree digits, read back, return HOME.

Step 1 of the build is a short read-only-ish survey (founder watching) to CONFIRM
this route and capture a real SET_TEMP value frame for the digit format. Reference:
AqualinkD `iaqtouch_aq_programmer.c` (cloned at `..\AqualinkD-ref`).

## The plan (Phase 2)

1. **Survey** (founder watching): confirm DEVICES -> press heat item -> SET_TEMP
   (0x39) for pool vs spa; capture a SET_TEMP value frame per body. Heater may need
   to be ON first (the founder's earlier constraint), so enable it first.
2. **Setpoint value-set** (TDD): a temperature digit encoder for 2-3 digit degrees F
   (re-check width/padding vs the 4-digit pump RPM `num2iaqt`; TDD against the
   captured frames), the SET_TEMP value frame builder, and a gated nav sequence
   (DEVICES -> heat item -> SET_TEMP -> 0x80 -> 0x24 -> readback -> HOME). Mirror new
   byte-builders in the on-device selftest.
3. **Clamps**: pool 45-90F, spa 80-104F. Refuse out of range.
4. **HA controls**: a `number` entity per body (Pool Heat Setpoint, Spa Heat
   Setpoint), gated like the pump-speed slider.
5. **Heat-state sensors**: `pool_heat_enabled` / `spa_heat_enabled` binary_sensors
   decoded from the HOME heater button states (captured: enabled = `B2/B3 s3 t11`,
   off = `s0 t5`).
6. **Optional root fix**: the IaqReader resets `water_mode` to 0 on a partial HOME
   page (caused the spa-mode flakiness, now worked around with `cs_spa_`, and causes
   the pool-temp "unknown" flicker). Cleaner fix: make the reader KEEP the last-known
   water_mode. Decide whether to do it here.
7. **Live test** (founder at the pad/spa): set spa to 94, confirm via readback and at
   the spa; set pool to 85, confirm; observe whether the panel drops spa heat on
   spa-mode exit (the spa auto-off question, still UNSETTLED).

## Durable facts to carry in

- **Gate spa-mode on `cs_spa_`** (the reliable always-on 0x08 keypad-status spa bit),
  NEVER the iAqualink home label (`iaq_water_mode_`), which reads "unknown" between
  the panel's intermittent full HOME updates. (Bit it bit us live this session.)
- Page-scoped keycodes: any DEVICES keycode MUST confirm `current_page() == 0x36`
  first (0x14 on DEVICES is Pool Heat, but 0x14 on HOME is Spa Heat).
- Pump-restore-after-spa-exit: switching spa->pool drops the filter pump; the first
  re-press during the ~1-2 min valve transition does not take; a second press after
  the valves settle sticks.
- Build/flash recipe + live-log capture (`C:\Users\Falcon\poollog.ps1`) as in
  Sessions 8-9. No em dashes, no AI jargon, plain-English explanations.

## After this

The HA brain (the headline): HA schedules the pump, cleaner, and filtration through
the box, with the panel keeping a minimal filtration schedule as a hardware failsafe
and a watchdog so water never goes stagnant. It needs only controls we already have.
