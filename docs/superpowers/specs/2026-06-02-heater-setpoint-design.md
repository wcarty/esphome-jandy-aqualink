# 🧠 Session 9 (Phase 2) design: heater temperature setpoint + status sensors

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

- Date: 2026-06-02
- Status: SHIPPED + founder-live-tested 2026-06-03 (origin/master `a2039e2`). The DEVICES -> SET_TEMP route and the `0x80`/`0x24` value-set both worked live: pool physically fired at 90; spa heated and the panel auto-offed spa heat at its 94 setpoint, below the 104 ceiling. Live quirks (detail in memory `project_pool_controller_phase2.md`, "Session 9 Phase 2 LIVE"): SET_TEMP renders blind to our 0x33 emulation (no setpoint readback), the heat-item screen-open is ~50% flaky (auto-retry shipped), and the `*_heat_enabled` sensors decode unreliably. (Originally: approved by founder 2026-06-02 brainstorm.)
- Repo: esphome-jandy-aqualink, branch `master`, base commit `da1e9da` (after Session 9 Phase 1).
- Device `192.168.4.51`, ESPHome node `pool-bridge.yaml`, dashboard `http://192.168.1.126:6052`.
- Builds on: `docs/superpowers/specs/2026-06-02-heaters-design.md` (the Phase 1 on/off + survey design)
  and the Phase 1 plan `docs/superpowers/plans/2026-06-02-heater-onoff-and-survey.md`.

## Context and goal

Heater on/off (pool + spa) shipped and was live-tested in Session 9 Phase 1 (both heaters proven;
the spa heater physically fired). This Phase 2 adds the temperature TARGET so the **spa heats to 94F**
(the founder's old iAquaLink setting) instead of running toward the 104F hardware max, and the
**pool holds 85F**. Without a target, enabling spa heat runs toward the stored setpoint (believed
104), too hot, so spa heat was left off pending this build.

Scope chosen by the founder this session ("Fuller Phase 2"): the survey, the temperature setpoint,
the two heater on/off status sensors, the under-the-hood water-mode reliability fix, and the live test.

## The key finding that defines this session (corrects the Phase 1 assumption)

The HOME heat button is a **pure on/off toggle**; pressing it does NOT open a temperature screen
(proven live, Session 9). So SET_TEMP is reached another way, and there is genuine uncertainty about
how, or whether, it opens on this screenless RS-8 power center. Two candidate routes:

1. **Equipment-page (DEVICES) route, most promising here.** The pump's value screen (SET_VSP `0x1E`)
   was reachable on this panel by pressing the VSP-adjust item on DEVICES, bypassing the MENU. The
   heater analogue: navigate to DEVICES (`0x36`), press the heat item there (DEVICES Pool Heat `0x14`,
   Spa Heat `0x15`, per `docs/PANEL-CAPABILITY-MAP.md`), and watch for SET_TEMP (`0x39`). RISK: those
   heat items are plain toggle items (not labeled "ADJ" like the pump item), so a press there may just
   toggle the heater rather than open SET_TEMP. Unknown until surveyed.
2. **MENU route (what AqualinkD actually uses, doubtful here).** AqualinkD reaches SET_TEMP via the
   MENU: send `KEY_IAQTCH_MENU` (`0x02`) to reach MENU (`0x0F`), then `KEY_IAQTCH_SET_TEMP`
   (`0x14`, = KEY04) to reach SET_TEMP (`iaqtouch_aq_programmer.c` `goto_iaqt_page`). But this panel's
   MENU renders EMPTY (confirmed conclusively, Sessions 4 and 6), so this route may do nothing. Cheap
   to try as a fallback because the key is fixed, not discovered from the (empty) menu enumeration.

The survey (founder watching) tries both, equipment-page first, and captures whatever opens SET_TEMP.
If neither opens it, the status sensors and the water-mode fix still ship; only the target-temp part
defers (same fallback as the Phase 1 spec).

## What is already proven and reused (the hard part is done)

The temperature WRITE reuses the pump-speed value-set machinery verbatim. In AqualinkD,
`queue_iaqt_control_command(0, val)` builds the SAME `0x24` value frame for the pump RPM
(`iaqtouch_aq_programmer.c:976`) and the heater setpoint (`:1445`): `00 24 31 <digits> <0xcd pad to
18> cksum`, preceded by the `ACK_CMD_READY_CTRL` (`0x80`) control request. Our codebase already has
this, proven live on the pump in Session 6:

- `iaq_ctrl_ready_ack()` (the `0x80` control request), `frames.py`.
- `build_vsp_set_frame(rpm)` (the `0x24` value frame), `frames.py`.
- `num2iaqt_rpm(n)` (ASCII digits, NUL-padded to a 5-byte field), `frames.py`.
- The gated multi-step state machine `advance_set_sequence_` in `jandy_aqualink.cpp`.

So the only genuinely new wire detail is the **digit format for a 2-to-3-digit degrees-F value**
(85, 94, 104), and the deep dive into AqualinkD RESOLVED it; we are not guessing. `num2iaqtRSset`
(`iaqtouch.c`) is the authoritative encoder, used for BOTH pump RPM and heater setpoint. It writes the
ASCII digits then pads a 6-byte field, with one quirk: a `0x30` ('0') at index 4 for sub-1000 values.
The full Set-Temp frame AqualinkD sends is `10 02 00 24 31 <6-byte digit field> <0xcd x10> <cksum>
10 03`. AqualinkD's own captured "Set Temp (pool)" frames in `iaqtouch.h` confirm it, and reproduce
byte-for-byte including the checksum:

- 50F  -> digit field `35 30 00 00 30 00`  (captured)
- 100F -> digit field `31 30 30 00 30 00`, full frame cksum `0x2a` (captured, verified by hand)

So our targets are COMPUTED, not guessed:

- 85F  -> `38 35 00 00 30 00`   (full-frame cksum `0x06`)
- 94F  -> `39 34 00 00 30 00`   (full-frame cksum `0x06`)
- 104F -> `31 30 34 00 30 00`

IMPORTANT: this padding differs from the pump's `num2iaqt_rpm` (which pads to 5 bytes and omits the
index-4 '0'), so the setpoint gets its OWN faithful `num2iaqtRSset` mirror, TDD'd against the captured
50F/100F frames; the proven pump encoder is left untouched.

## Design

### 1. Survey (the gate, founder watching) -- confirm the route, capture the format

- Safe starting state: pool mode, filter pump running, presence on; then arm the interlock.
- Enable Pool Heat first (the founder's reported constraint: the temperature screen needs a heater
  on). On/off is shipped and proven.
- A small, heavily-gated **survey instrument**: a method that sends ONE specified keycode but ONLY
  while the decoder confirms the panel is on a specified page (interlock + presence + page-confirm +
  founder watching). This lets us try the candidate presses safely and reversibly. It is removed or
  left inert after the survey; the real setpoint path is the proper state machine in section 2.
- Try, in order, watching the compact page decoder for `IAQ PAGE ...(0x39)` (SET_TEMP):
  1. DEVICES route: HOME -> Other Devices (`0x18`) -> confirm DEVICES (`0x36`) -> press Pool Heat
     (`0x14`) -> does SET_TEMP open, or does the heater just toggle?
  2. MENU route: HOME -> MENU (`0x02`) -> send Set Temp key (`0x14`) -> does SET_TEMP open?
- On reaching SET_TEMP, capture: (a) the exact key + page that opened it for pool vs spa; (b) the page
  enumeration. AqualinkD selects a button by label ("Pool Heat NN" / "Spa Heat NN") before sending the
  value (`set_aqualink_iaqtouch_heater_setpoint`), so we need the pool/spa button keycodes from the
  enumeration, and the labels also expose the CURRENT setpoints (we should see the spa's stored ~104
  directly). The value-frame format is already resolved from AqualinkD captures (see above), so a live
  frame is now a nice confirmation, NOT a prerequisite. The survey's real job shrinks to one yes/no:
  does SET_TEMP (`0x39`) open on this panel, and does it enumerate the pool/spa items.
- If neither route opens SET_TEMP, conclude it is not reachable on this panel; ship sections 4 and 5
  (status sensors + water-mode fix) and defer the setpoint.

### 2. Temperature setpoint write (built from the survey capture, TDD)

- A new gated routine mirroring the proven pump-speed setter: open SET_TEMP the way the survey found,
  select the body (press the Pool Heat / Spa Heat button on SET_TEMP, keycode from the enumeration),
  reply `ACK_CMD_READY_CTRL` (`0x80`) on a poll, the panel sends CMD_IAQ_CTRL_READY (`0x31`), transmit
  the `0x24` value frame with the ASCII degree digits, read back, return HOME. Same shape and mutual
  exclusion as `advance_set_sequence_`. Order mirrors AqualinkD `set_aqualink_iaqtouch_heater_setpoint`.
- Digit encoder for degrees F: a faithful `num2iaqtRSset` mirror (6-byte field, `0x30` at index 4 for
  sub-1000), TDD'd against AqualinkD's captured Set-Temp frames (50F = `35 30 00 00 30 00`, 100F =
  `31 30 30 00 30 00`) plus the computed 85/94/104 frames and their checksums. This is its OWN encoder,
  NOT the pump's `num2iaqt_rpm` (the padding differs); the pump path is left untouched.
- Clamps: pool 45 to 90F, spa 80 to 104F. Refuse out of range (mirror `rpm_check`).
- HA controls: a `number` entity per body, "Pool Heat Setpoint" and "Spa Heat Setpoint", gated exactly
  like the pump-speed slider (master interlock + presence). Defaults seeded to 85 (pool) and 94 (spa).
- Spa note: setting the spa TARGET just stores a number, it does not fire the heater, so the setpoint
  write itself should not require spa mode (only ENABLING spa heat does, already gated). Reaching
  SET_TEMP may still require a heater enabled (the founder's constraint); confirm in the survey.

### 3. Safety model (non-negotiable)

- **Page-context guard on every keycode.** `0x14` is Spa Heat on HOME, Pool Heat on DEVICES, and Set
  Temp on MENU; the value-set keys are page-scoped too. Every keycode confirms `current_page()` first,
  re-checked at the transmit point. This is the central safety property of the whole project.
- The `0x24` temperature digits are transmitted ONLY when the decoder confirms `page == SET_TEMP
  (0x39)`.
- Master interlock + iAqualink presence gate everything; both off by default. One control sequence at
  a time (setpoint mutually exclusive with pump-set and device-toggle). Interlock-off aborts mid-run.

### 4. Heater on/off status sensors

- Two `binary_sensor`s, `pool_heat_enabled` and `spa_heat_enabled`, decoded from the HOME heater
  button states (Session 9 captures: enabled = `B2/B3 s3 t11`, off = `s0 t5`). The `IaqReader` already
  parses HOME buttons. Read-only, continuous from the HOME enumeration plus deltas. Useful on their own
  and a prerequisite for any future spa auto-off automation (so it never toggles the wrong way).
- BONUS (only if the survey shows SET_TEMP enumerates): publish the CURRENT pool/spa setpoints as
  read-only number/sensors, parsed from the SET_TEMP button labels ("Pool Heat NN"), mirroring
  AqualinkD `get_aqualink_iaqtouch_setpoints`. This gives a true readback that a write took, and shows
  the spa's stored target. Skip if the page does not enumerate (YAGNI).

### 5. Water-mode reliability fix (root cause)

- The `IaqReader` resets `water_mode` to 0 ("unknown") on a partial HOME page, which made spa-mode
  reads briefly flaky (it spuriously refused Spa Heat and the mode switches in Session 9, worked around
  with the `cs_spa_` bit). Fix: the reader KEEPS the last-known `water_mode` instead of resetting to 0
  on a partial page. This also fixes the pool-temp "unknown" flicker.
- Keep the proven `cs_spa_` (0x08 status bit) gate for spa-mode-dependent checks as a backstop;
  belt and suspenders.

### 6. Live test (founder at the pad and spa)

- Refusal check: a setpoint while disarmed logs REFUSED and sends nothing.
- Pool: set 85, read back, confirm.
- Spa: switch to spa mode, set 94, read back, confirm at the spa that it heads toward 94 not 104.
- Spa auto-off (still UNSETTLED): switch spa -> pool with spa heat enabled and observe whether the
  panel drops spa heat on its own. Records whether a future HA auto-off automation is needed.
- Resting state: leave **Pool Heat enabled at 85** so the panel holds the pool at 85 going forward
  (June, pool above 85, so it will not fire until it drops); leave **spa heat off** until a soak.
  Confirm presence ON, interlock OFF at the end.

### 7. Testing (TDD)

- Python (pytest) + C++ on-device selftest mirror: the temperature digit encoder + the `0x24` value
  frame (against the captured SET_TEMP frame), the clamps (pool 45-90, spa 80-104), the page guard
  (temperature digits refused unless `page == SET_TEMP`), and the heat-state decode (HOME-button
  fixtures). Full suite green before any flash; new byte-builders mirrored in the on-device selftest.

## Build order (writing-plans splits at the survey seam)

1. Desk, TDD, route-independent: the clamps + the temperature encoder (faithful `num2iaqtRSset` mirror,
   TDD against AqualinkD's captured 50F/100F Set-Temp frames + the computed 85/94/104 + checksums), the
   heat-state status decode + sensors, the water-mode reader fix, and the gated survey instrument.
2. Live survey (founder watching): confirm the NAVIGATION route to SET_TEMP and that it enumerates the
   pool/spa items (the value-frame format is already resolved on the desk, step 1).
3. Desk, TDD: finalize the gated setpoint state machine + HA number entities, wiring in the route the
   survey confirmed and the pool/spa button keycodes it captured.
4. Live test: set spa 94 / pool 85, confirm, observe spa auto-off, set the resting state.

No guessed bytes: the value frame is verified on the desk against AqualinkD's real captured Set-Temp
frames before any flash; the only thing the live survey adds is the navigation route (which key on
which page opens `0x39`), which no source can confirm for this specific screenless panel.

## Non-goals / out of scope

- The HA brain (scheduling pump/cleaner/filtration with a filtration failsafe) -- next session.
- A spa auto-off HA automation -- only built if the live test shows the panel does NOT drop spa heat
  on spa-mode exit; decided by the test, not pre-built (YAGNI).
- Solar heat; decoding every circuit state (Phase 4 polish).

## Watchpoints

- Page-scoped keycodes: `0x14` means three different things on HOME / DEVICES / MENU; confirm the page
  first, every time. The temperature digits go out only on confirmed SET_TEMP.
- The temperature screen may be unreachable on this screenless panel (the MENU is empty, the DEVICES
  heat items may only toggle). Honest possibility; the on/off + sensors + reliability fix still ship.
- Gate spa-mode on `cs_spa_` (the reliable 0x08 bit), never the flaky `iaq_water_mode_` home label
  (kept as a backstop even after the reader fix).
- Pump-restore-after-spa-exit: switching spa -> pool drops the filter pump; the first re-press during
  the ~1-2 min valve transition does not take, a second press after the valves settle sticks.
- Build/flash recipe + live-log capture (`C:\Users\Falcon\poollog.ps1`) as in Sessions 8-9. ESPHome
  object-id REST URLs are deprecated; drive via HA service calls.
- No em dashes, no AI jargon, plain-English explanations for the founder.
