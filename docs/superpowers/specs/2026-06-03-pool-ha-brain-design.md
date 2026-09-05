# 🧠 Pool HA Brain: Home Assistant as scheduler, panel as failsafe

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

Status: APPROVED in brainstorm with the founder 2026-06-03. Ready to turn into an
implementation plan (writing-plans next). Builds on origin/master HEAD `3025c6f`
(heater setpoint work complete), repo `esp32-experiment` / `esphome-jandy-aqualink`,
device `192.168.4.51`.

## What this is

Home Assistant becomes the brain of the pool. It runs a smart pump, cleaner, and
filtration schedule through the ESP bridge box's already shipped, live proven controls.
The AquaLink panel keeps its own minimal stored schedule underneath as a hardware
failsafe, so the water keeps circulating even if HA or the box goes down.

All the device control the brain needs is already built and live tested (pump on/off,
pump speed presets, cleaner, pool/spa mode, status sensors). This project is the
orchestration layer in Home Assistant plus one small firmware change to let the
scheduler write unattended. It is not new device control.

## Goals (from the founder)

The founder wants all four of these, and a fully automated pool side:

- Healthy chlorination: guarantee the salt cell gets enough run time above its 1850 RPM
  flow floor each day, instead of dipping out at 1700 the way the panel does now.
- Energy savings: run the pump as slow and as few hours as the jobs allow (flat rate
  power, mid size pool roughly 10,000 to 20,000 gallons).
- Quiet when it matters: low and silent overnight and in the evening.
- Hands off reliability: it just works without babysitting, and never lets the water go
  stagnant.

The founder's stated end state: a finely tuned pool system where the only manual actions
are spa related, switching to spa mode, optionally changing spa temperature, choosing
whether to run the blower, and switching back to pool mode. Everything on the pool side
is automated.

## The core principle: override, do not delete

We cannot erase the panel's stored schedule. That was proven conclusively across several
sessions: the panel's program editor renders blank to our 0x33 emulation, and AqualinkD
itself does not edit schedules over the bus. We do not need it erased, and we want it
kept as the failsafe.

So "total control" works like a thermostat war that HA always wins:

- HA holds the desired pump speed and cleaner state for the current time of day.
- The panel occasionally reaches over and nudges the pump on its own clock (the Session 7
  watch showed this happening roughly once an hour overnight, swinging between 1700 and
  2750 RPM, once to 2900).
- HA notices the deviation and sets it back, within a check interval of a few minutes.
- In daily life, what the founder programs is what holds.

Honest limit, recorded so expectations are exact: HA does not prevent the panel's command
from firing, it reverses it shortly after. This is near total control with a short
correction delay measured in minutes, not the panel being physically silenced. With the
corroded spa side remote now unplugged, the only autonomous mover left is the panel's
gentle, roughly hourly pump nudge, so there is no rapid tug of war.

## The daily schedule

Phase boundaries and targets (local time, America/Chicago):

| Phase | Time | Pump target | Cleaner | Purpose |
|-------|------|-------------|---------|---------|
| Quiet | 22:00 to 08:00 | Night, 1100 RPM | off | Near silent overnight circulation, cheapest, below the salt floor on purpose (no chlorine needed in the dark). |
| Morning clean | 08:00 to 10:00 | Normal, 2750 RPM | ON | Morning cleaning pass, higher flow for the cleaner, cell chlorinates. |
| Day | 10:00 to 20:00 | Low, 2000 RPM | off | Workhorse window, above the 1850 salt floor so the cell makes the day's chlorine, covers the 2pm to 5pm pool time, completes a full turnover. |
| Evening clean | 20:00 to 22:00 | Normal, 2750 RPM | ON | Evening cleaning pass, higher flow for the cleaner, cell chlorinates. |

These phases tile the day with no gaps. Chlorinating flow (2000 RPM or above) runs 08:00
to 22:00, about 14 hours, which is generous. The salt cell's own output percentage is the
fine tuning dial for actual chlorine and is set at the cell, not in HA. Pump speed presets
are the existing tuned values: Night 1100, Low 2000, Normal 2750, High 3200.

Notes recorded for later tuning (not blockers): twice daily cleaning is more than many
pools need and the evening clean can be dropped if the morning proves enough; the evening
clean runs the pump up to Normal, slightly louder during evening hours.

## Safety model (the critical section)

There are two permission layers. The existing master interlock stays exactly as it is and
keeps guarding everything risky. A new, narrow scheduler permission lets only the three
safe pool side writes happen unattended.

### Why per keycode matters

The filter pump and cleaner are pressed through `iaq_press(key)`, which shares one
allowlist with the spa toggle, air blower, and pool light:

- 0x11 Filter Pump
- 0x12 Spa toggle
- 0x15 Cleaner
- 0x16 Air Blower
- 0x17 Pool Light

A blanket un-gate of `iaq_press` would also let the scheduler fire the blower and lights.
So the scheduler permission must be scoped to specific keycodes, only Filter Pump (0x11)
and Cleaner (0x15), plus pump speed via `set_pump_rpm`. Everything else still requires the
master interlock.

### The two layers

- Master interlock (`switch.pool_rs485_bridge_pool_keypad_keypress_armed`, firmware
  `restore_mode: ALWAYS_OFF`): unchanged. Boots OFF on every restart. Required for all
  high stakes and manual writes: heaters, heater setpoints, spa mode, valves, pool/spa
  mode switch, blower, pool/spa/solar lights, sprinklers, all DEVICES toggles, nav, and
  survey. This is the founder's single stop everything button.
- Scheduler armed (new `switch.pool_rs485_bridge_pool_scheduler`, firmware
  `restore_mode: RESTORE_DEFAULT_OFF`): defaults OFF on a fresh flash but restores its last
  state through a power blip, so once the founder turns it on the brain self resumes after
  a reboot. When on, it permits exactly three writes, and only these three:
  - `set_pump_rpm` (pump speed)
  - `iaq_press(0x11)` (Filter Pump on/off)
  - `iaq_press(0x15)` (Cleaner on/off)

All writes still require iAqualink presence to be on (unchanged). Reading pump speed is
already presence only (un-gated from the interlock in Session 7), so the watch loop's reads
need no change.

### Gate logic, precisely

- `set_pump_rpm`: refuse only if `!interlock_ && !scheduler_armed_` (allow if either is on),
  presence still required.
- `iaq_press(key)`: refuse if `!interlock_ && !(scheduler_armed_ && is_scheduler_safe_key(key))`,
  where `is_scheduler_safe_key` is true only for 0x11 and 0x15. The existing allowlist and
  presence checks stay.
- Every other write path (`press_heater`, `set_heater_setpoint`, `press_device_toggle`,
  `request_pool_mode`, `request_spa_mode`, `iaq_nav`, survey): unchanged, still gated on
  `interlock_`.

## The brain's behaviors

Everything except the watch loop is instant: phase boundaries, spa mode changes, manual
presses, and restarts all act the moment their trigger happens. Only the watch loop is
periodic, because the pump speed is not announced on the bus and has to be actively read.

1. Phase scheduler. At each boundary (08:00, 10:00, 20:00, 22:00) set the phase's target
   pump speed and desired cleaner state, and apply them immediately. Conditions: scheduler
   armed, not in spa mode.
2. Watch and correct. Every 2 minutes, read the pump speed, and if
   it is off the current target by more than a tolerance, re-apply the target. Conditions:
   scheduler armed, not in spa mode, not within a manual hold. A correction rate cap (for
   example no more than a handful per hour) stops any pathological fight and raises an alert
   instead.
3. Cleaner keeper. The cleaner is a toggle, so HA compares the cleaner status sensor to the
   desired state and presses only on a mismatch, never blindly. Folded into the phase
   scheduler and the watch loop.
4. Spa gets right of way. The moment spa mode turns on, the brain stands down completely:
   every automation above is conditioned on not in spa mode, so it stops touching the pump
   and cleaner. Nothing special needs to fire on spa entry.
5. Pool restore on spa exit. When spa mode turns off, the brain ensures the filter pump is
   back on (switching out of spa drops it, a known panel quirk that needs a second press
   after the valves settle), then re-applies the current phase. Conditions: scheduler armed.
6. Manual changes stick, then bow out. The founder's manual pool actions go through
   dedicated HA controls (a manual speed control and a swim boost button), which set a hold
   until the next phase boundary and apply the change. The watch loop respects the hold, so
   a manual change is not corrected away. Phase boundaries clear the hold. The master kill
   switch still pauses the whole scheduler instantly. Using dedicated HA controls (rather
   than the raw firmware buttons) is how HA tells a human change apart from its own writes.
7. Swim boost. A button that bumps the pump to Normal (2750) and holds until the next phase
   or two hours, whichever comes first. For the 2pm to 5pm pool time, which has no fixed
   schedule.
8. Startup and resync. On HA start, on scheduler armed turning on, and on the bridge coming
   back online, compute the current phase from the time of day and apply it, so the brain
   establishes the right state immediately after any restart rather than waiting for the
   next boundary.
9. Presence keeper (belt and suspenders). If the scheduler is armed and presence is off,
   turn presence on so the brain can act. Presence already restores on after a reboot, so
   this is a safety net.

## The failsafe

Defense in depth, so the water can never go stagnant:

- Never off invariant. No phase of the schedule ever commands the filter pump fully off.
  The slowest state is Night 1100. There is no schedule path where HA stops the pump.
- Panel net underneath. The panel's own stored schedule keeps running the pump and cleaner
  on its own clock with no dependence on the box. If HA dies, the box loses power or wifi,
  or the scheduler is disarmed, the panel still circulates the water.
- For the water to actually stagnate, HA, the box, and the panel's backup schedule would
  all have to fail at once.
- Existing alerts already cover box death: `automation.pool_bridge_offline_15min_soft` and
  `automation.pool_bridge_offline_60min_hard` (plus thermal and wifi warnings). The brain
  reuses these rather than reinventing offline detection.

## Coordination with the iAqualink re-online and panel simplification (planned)

The founder plans, in the next few days, to bring the iAqualink 2.0 back online, map what the
panel and the Jandy cloud hold, simplify the panel's stored programming down to one intentional
failsafe program, and remove the rest. This strengthens the design.

- A calmer override. The messy old programs are what nudge the pump roughly once an hour.
  Stripping them out means the watch loop rarely has anything to correct, it just holds.
- A deliberate net. The safety floor stops being an inherited mess and becomes one program we
  choose. Recommended shape: filter pump at 2000 RPM (Low, above the salt floor) from 10:00 to
  16:00 daily. When the brain is alive this sits inside the Day phase at the same speed, so the
  two agree and never fight; when the brain is down, that one program alone still gives a daily
  turnover plus six hours of chlorination. Safety rule: enter the failsafe program before or at
  the same time as removing the others, so the net is never empty for a moment.

Operating coordination (only one device may hold the 0x33 iAqualink seat at a time):
- Before bringing the iAqualink 2.0 online, turn OFF the Pool Scheduler switch. That fully
  stands the brain down, including the presence keeper, so it cannot fight the iAqualink for the
  seat. Then turn iAqualink presence off and bring the unit online.
- While the iAqualink is online, the brain is paused and the panel program carries the pool.
- After mapping and editing, disconnect the iAqualink so the box reclaims the 0x33 seat, turn
  presence back on, then turn the Pool Scheduler back on.
- The mapping itself is the already-prepared bonus session: cloud-recon tooling at
  `..\cloud-recon\`, the `pool_bus_sniff` switch for passive bus capture, and the kickoff doc
  `docs/SESSION-iaqualink-reonline-and-sniff.md`.

## Firmware change (one small change)

In `firmware/pool-bridge.yaml`: add a `switch` named "Pool Scheduler" (id
`scheduler_armed`, `restore_mode: RESTORE_DEFAULT_OFF`) whose turn_on/turn_off call
`id(jandy_comp).set_scheduler_armed(true/false)`.

In `components/jandy_aqualink`: add `bool scheduler_armed_` plus a setter; add a
frame-logic helper `is_scheduler_safe_key(key)` true only for 0x11 and 0x15 (TDD it in the
Python suite and mirror it in the on-device selftest, same pattern as the existing
allowlist); change the two gates in `set_pump_rpm` and `iaq_press` exactly as in the gate
logic above. No other write path changes. Carry the deferred symmetric busy-guard note from
the heater work if it touches these lines.

This is the same kind of narrow gating change as Session 7's read un-gate. The real compile
gate is the deploy; selftest must pass and checksum stay 0 after flashing.

## Home Assistant components

Built as HA helpers and automations through the MCP config tools, never as raw YAML the
founder hand edits.

Helpers:
- `input_number.pool_target_rpm`: the desired pump RPM for the active phase (or a manual
  value during a hold). Read by the watch loop.
- `input_boolean.pool_cleaner_should_run`: desired cleaner state for the active phase.
- `input_datetime.pool_manual_hold_until`: timestamp the watch loop checks to suppress
  correction after a manual change.
- `input_select.pool_phase` (Quiet / Morning clean / Day / Evening clean): for visibility
  and debugging.

Automations (one per behavior above): phase scheduler, watch and correct, pool restore on
spa exit, manual hold capture, swim boost, startup and resync, presence keeper. The
firmware `switch.pool_rs485_bridge_pool_scheduler` is the master enable that every
automation conditions on.

## Entity reference (live, verified 2026-06-03)

Controls the brain uses:
- `button.pool_rs485_bridge_pump_speed_night` / `_low` / `_normal` / `_high`
- `number.pool_rs485_bridge_pump_speed_set` (RPM slider, 600 to 3450)
- `button.pool_rs485_bridge_filter_pump` (toggle)
- `button.pool_rs485_bridge_cleaner` (toggle)
- `button.pool_rs485_bridge_read_pump_speed`

Switches:
- `switch.pool_rs485_bridge_pool_keypad_keypress_armed` (master interlock)
- `switch.pool_rs485_bridge_iaqualink_presence`
- `switch.pool_rs485_bridge_pool_scheduler` (NEW, to be added)
- `switch.pool_rs485_bridge_pool_pump_auto_refresh` (existing 15-min read interval; may be
  retired in favor of the HA driven read cadence, or left as is)

Sensors the brain reads:
- `sensor.pool_rs485_bridge_pool_pump_speed`
- `binary_sensor.pool_rs485_bridge_pool_spa_mode` (reliable)
- `binary_sensor.pool_rs485_bridge_pool_filter_pump_status` (reliable)
- `binary_sensor.pool_rs485_bridge_pool_cleaner_status` (reliable)
- `binary_sensor.pool_rs485_bridge_pool_bridge_status` (online/offline)

Not used (unreliable on this panel): `binary_sensor.pool_rs485_bridge_pool_heat_enabled`,
`binary_sensor.pool_rs485_bridge_spa_heat_enabled`.

## Build phases (incremental)

- Phase A, firmware permission. Add the scheduler armed switch and the per keycode gate
  change, TDD the helper, deploy, and verify the safety gates: with interlock off and
  scheduler off a pump speed set REFUSES; with scheduler on (interlock still off) a pump
  speed set is ALLOWED while a heater or blower press still REFUSES (proves the per keycode
  scoping). This safety verification is the gate to Phase B.
- Phase B, HA core schedule. Helpers, phase scheduler, watch and correct, cleaner keeper,
  startup and resync. Test with an accelerated schedule (temporary near term boundaries or
  manual triggers) and confirm the pump follows and self corrects.
- Phase C, coexistence and manual. Spa stand-down and pool restore on spa exit, manual hold
  and swim boost, presence keeper.
- Phase D, failsafe verification and polish. Confirm the never off invariant and the panel
  net, add the correction rate cap and any pump health alert, document.
- Later (out of scope for the first cut): pool and spa heater coordination (run the pump
  high enough while heating, the setpoints are already controllable); salt cell reading
  (decode the cell's own bus frames for real chlorine visibility); instant panel-move
  detection (sniff the panel's own pump speed command on the bus so a correction is immediate,
  removing the read patrol for pump speed); energy reporting.

## Testing approach

- Firmware safety gate test (Phase A): the REFUSE/ALLOW matrix above is the must pass. It
  proves the scheduler permission is correctly narrow.
- Accelerated schedule test (Phase B): shift phase times near now, or fire the phase
  automations by hand, and watch the pump track each target. Force a wrong speed and confirm
  the watch loop restores it within about 2 minutes.
- Coexistence test (Phase C): switch to spa, confirm the brain stops touching the pump and
  cleaner; switch back to pool, confirm the filter pump comes back and the schedule resumes.
  Use the swim boost and confirm it holds until the next phase.
- The first real writes are low risk (pump speed and cleaner, both already live proven), so
  this does not require the founder at the pad, though an early run with him watching is
  welcome.

## Open questions and deferred decisions

- Watch interval: decided at 2 minutes, everywhere, all the time (founder, 2026-06-03).
- Cleaner speed: 2750 (Normal) assumed for both clean windows. If the cleaner works at a
  lower flow, drop it for energy. Tunable later.
- Whether to retire the firmware 15-min auto-refresh in favor of HA driven reads, or keep
  both. Minor.

## Constraints

- Non-technical founder: plain English alongside any code, no em dashes, no AI jargon, no
  self deprecation.
- HA changes are built as helpers and automations via the MCP config tools, not raw YAML the
  founder hand edits.
- Keep Phase A's firmware change as narrow as the gate logic above. Do not widen the
  scheduler permission beyond pump speed, filter pump, and cleaner.
