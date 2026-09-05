# 🧠 Session 9 design: pool + spa heater control (on/off + temperature setpoint)

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

- Date: 2026-06-02
- Status: Approved by founder 2026-06-02 (brainstorm). Ready for implementation plan.
- Repo: esphome-jandy-aqualink, branch `master`, base commit `5bb767f` (after Session 8).
- Device `192.168.4.51`, ESPHome node `pool-bridge.yaml`, dashboard `http://192.168.1.126:6052`.

## Context and goal

Heaters are the highest-stakes equipment and the last control build. The founder
wants BOTH pool and spa heat, each with on/off AND a temperature target. The
pump-speed value-set machinery (Session 6, the 0x80 control request plus the 0x24
value frame) is the proven basis for the setpoint write. Heater on/off keycodes
have been deliberately excluded from every keypress allowlist until now.

## Founder use case (drives the design)

- **Pool**: kept at a MINIMUM of 85F year-round (winter included). "Set and hold,"
  no auto-off. Clamp 45 to 90F (45 = winter freeze low, 90 = max).
- **Spa**: heated for soaks; spa heat should ONLY run while the system is in spa
  mode, never outside it. Auto-off backstop wanted. Clamp 80 to 104F.
- The heater hardware tops out at 104F for both bodies.

## Key constraint (founder-reported): the temperature screen is only reachable with the heater ON

The founder reports the panel will not let you reach or change a heater's target
temperature unless that heater is enabled. And in Auto mode the only way to enable
a heater is through our box (the panel's own buttons need Service mode). So the
build order is forced: build on/off FIRST, use it to enable a heater with the
founder watching, THEN survey the setpoint screen.

## Build order

1. **On/off enables** (pool + spa) + **heater enable-state sensors**. Desk-built
   with TDD, then a quick watched check.
2. **SET_TEMP survey**: heater on, founder watching, map the nav path to the
   setpoint screen and the value wire format, for pool vs spa.
3. **Temperature setpoint**: built from what the survey finds.
4. **Founder-watched live test** of the whole thing; verify the panel's
   pump-running flow interlock and its spa-heat-follows-spa-mode behavior.

If the survey shows the setpoint screen is unreachable, the on/off enables plus the
state sensors still ship, and only the target-temp part defers.

## Design

### 1. On/off enables (HOME page)

- HOME keycodes: **Pool Heat 0x13, Spa Heat 0x14** (keycode 0x11 + home index 2/3),
  confirmed in `docs/PANEL-CAPABILITY-MAP.md`.
- PAGE-CONTEXT TRAP: 0x13 on HOME is Pool Heat but on DEVICES is the VSP-adjust;
  0x14 on HOME is Spa Heat but on DEVICES is Pool Heat. So heater on/off MUST
  confirm `current_page() == HOME (0x01)` before sending, and drive only from HOME.
- Dedicated gated methods (preferred over widening the `iaq_press` allowlist):
  a short core-1 sequence that ensures HOME (arm HOME if not there, confirm
  `page == HOME`), then sends the ONE heater keycode. Allowlist `{0x13, 0x14}`.
  Gated by the master interlock + iAq presence. For **Spa Heat (0x14)** add a hard
  gate: refuse unless `water_mode == spa (3)` (the decoder already tracks
  water_mode, as used by `request_pool_mode`). Interlock-off aborts mid-sequence.
- These are TOGGLES (a press flips the enable), matching the panel.

### 2. Heater enable-state sensors

- Publish `binary_sensor` **pool_heat_enabled** and **spa_heat_enabled**, decoded
  from the HOME-page heater button states (the survey already logged
  `IAQ B2 ... Pool Heat` / `IAQ B3 ... Spa Heat` with an `s<state>`; the `IaqReader`
  already parses HOME buttons). Read-only, continuous from the HOME enumeration plus
  deltas. Two reasons: the spa auto-off must know the current enable to avoid
  toggling it the wrong way, and seeing heat state in HA is useful on its own.

### 3. Temperature setpoint

- Lives on **SET_TEMP (page 0x39)**. The NAV PATH IS UNMAPPED; the survey (build
  step 2) finds it. Survey method: with a heater enabled (founder watching), cycle
  presence off/on for a fresh full enumeration, then from HOME try opening the heat
  setpoint (likely a press on the Pool Heat / Spa Heat item, or a dedicated key),
  log pages with the on-device compact decoder until SET_TEMP (0x39) appears, and
  record the exact key + page that opens the pool vs spa setpoint and the value wire
  format. Reference: AqualinkD `iaqtouch_aq_programmer.c` (cloned at `..\AqualinkD-ref`).
- Write: reuse the proven value-set handshake. Navigate to SET_TEMP, reply
  ACK_CMD_READY_CTRL (0x80) on a poll, the panel sends CMD_IAQ_CTRL_READY (0x31),
  transmit the 0x24 value frame with the ASCII setpoint digits, read back, return
  HOME. Digit encoder: adapt `num2iaqt` for 2-3 digit degrees F (re-check the
  width/padding and the sub-1000 behavior for this range; TDD against captures from
  the survey or `iaqtouch.h`). Mirror new byte-builders in the C++ on-device selftest.
- Clamps: pool 45 to 90F, spa 80 to 104F. Refuse out of range.
- HA controls: a `number` entity per body ("Pool Heat Setpoint", "Spa Heat
  Setpoint"), gated exactly like the pump-speed slider.

### 4. Safety model

- **Page-context guard** on every heater keycode (0x13/0x14 only on HOME; setpoint
  keys only on the confirmed SET_TEMP path). Non-negotiable given the 0x13 collision.
- **Spa heat coupled to spa mode**: the firmware refuses to ENABLE spa heat unless
  `water_mode == spa`. For the auto-off backstop, first verify the panel's own
  behavior in the live test (does switching spa to pool drop spa heat?). If the
  panel drops it, rely on that plus the enable-gate (YAGNI, no extra automation). If
  it does not, add an HA automation: when `spa_mode` turns off and `spa_heat_enabled`
  is on, briefly arm the interlock, press Spa Heat to disable, and disarm; plus an
  optional tunable max-runtime backstop (for example 3 hours).
- **Pool heat**: no auto-off (it should maintain the target).
- **Panel flow interlock**: in Auto the panel should refuse to fire a heater without
  the filter pump running. CONFIRM this live before trusting it.
- **Master interlock + presence** gate everything; off by default.
- **Pool maintenance is the panel's own thermostat, in scope for free.** Once Pool
  Heat is enabled with the 85 setpoint, the panel itself fires the heater whenever
  the pool is below 85 AND the pump is circulating, and stops at 85. So this
  session's "enable + setpoint" delivers the maintain-85 behavior directly, via the
  panel, with no HA automation. Caveat: the panel can only heat while the pump runs
  (the flow interlock), so the pool holds 85 whenever the pump is circulating;
  coordinating the pump schedule with heating (so the pool does not drift while the
  pump is off) is the HA brain's job next session. Other optional brain additions
  later: seasonal target changes, ensuring the enable persists. None of that is
  needed for the basic "drop below 85, heater kicks on" maintenance.

### 5. Testing (TDD)

- Python (pytest) + C++ selftest mirror: the temperature digit encoder + the
  SET_TEMP value frame builder (against survey / `iaqtouch.h` captures), the heater
  allowlist `{0x13, 0x14}`, the page guard (heater key refused unless `page == HOME`),
  the spa-mode gate (spa heat refused unless `water_mode == spa`), the clamps (pool
  45-90, spa 80-104), and the heater enable-state decode (against HOME-button fixtures).
- `pytest` green before any flash. New C++ constants/byte-builders mirrored in the
  on-device selftest vectors.

### 6. Live test (founder watching, at the pad/spa)

- Refusal checks: a heater press while disarmed logs REFUSED and actuates nothing;
  a Spa Heat enable while NOT in spa mode is REFUSED.
- On/off: enable Pool Heat, confirm it fires AND that the panel's pump-running
  interlock holds (a heater must not fire without the pump). Enable Spa Heat (in spa
  mode), confirm.
- Survey: with a heater on, map SET_TEMP for pool and spa.
- Setpoint: set pool to 85, confirm via readback and at the equipment; set the spa
  to a soak temp, confirm.
- Spa-mode coupling: switch spa to pool and observe whether the panel drops spa
  heat (this decides whether the HA auto-off automation is needed).
- Turn heaters off, disarm. Note any side effects.

## Non-goals / out of scope

- Automatic pool-temperature maintenance scheduling (HA brain, next session).
- Solar heat (a separate circuit; the Solar Light shipped in Session 8 is unrelated).
- Decoding every circuit state (Phase 4 polish).

## Watchpoints

- Page-scoped keycodes: every heater keycode MUST confirm the page first (0x13/0x14
  only on HOME). This is the central safety property.
- The setpoint survey likely requires enabling a heater (the founder's constraint),
  so it is founder-watched, not pure desk work.
- Heater on/off is a toggle (a press flips the enable), so any automation must read
  the enable-state sensor first to avoid flipping it the wrong way.
- ESPHome object-id REST URLs are deprecated; drive via HA service calls.
