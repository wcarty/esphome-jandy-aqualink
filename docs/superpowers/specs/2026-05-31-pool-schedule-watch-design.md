# 🧠 Session 7 design: pump-speed auto-refresh + stored-schedule watch

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

- Date: 2026-05-31
- Status: Approved by founder 2026-05-31. Ready for implementation plan.
- Repo: esphome-jandy-aqualink, branch `master`, base commit `58c7606`.
- Device: `192.168.4.51` (ESPHome node `pool-bridge.yaml`, dashboard `http://192.168.1.126:6052`).

## Context

Session 6 shipped and live-tested pump-speed READ and SET (exact-RPM writes,
readback-verified; presets Night 1100 / Low 2000 / Normal 2750 / High 3200, with
the salt cell flow floor found live at ~1850 RPM). The resting state is safe:
control interlock OFF, iAqualink presence ON, temperatures and on-demand pump
read live.

The panel runs its own stored schedule and changes the pump on its own clock
(observed in Session 4: the pump dropped 2750 -> 1700 with no input, and the
cleaner switched on unprompted). The founder's real concern is that this hidden
schedule will keep fighting whatever Home Assistant tries to do.

## Goal

Capture the panel's self-driven pump schedule by reading pump RPM on a timer over
roughly 24 hours, so Home Assistant history charts when, and to what, the panel
moves the pump on its own. This is the prerequisite for the Session 10 schedule
decision (likely HA-as-scheduler with a salt-floor guard, rather than editing the
panel's internal schedule at all).

## Decision (made with founder 2026-05-31)

- **Gating: Option A.** Un-gate the view-only read from the control interlock;
  keep it behind iAqualink presence. Add an Auto-Refresh switch (default OFF)
  driven by a timer.
- **Interval: 15 minutes.** Longest of the options weighed, least disruptive
  (each read blinks temps out for ~2s), and plenty to map a schedule that shifts
  a few times a day.
- **Reboot behavior:** the Auto-Refresh switch remembers its state across a
  reboot (`RESTORE_DEFAULT_OFF`), so an overnight power blip resumes the watch.

Rejected: Option B (a separate time-boxed monitoring mode: more moving parts for
the same result) and Option C (manual reads only: no unattended watch, cannot
auto-map the schedule).

## Design

### 1. Code change: ungate the view-only read

File `components/jandy_aqualink/jandy_aqualink.cpp`, `read_pump_speed()` (~line 277):

- Remove the `interlock_` guard (the four-line "read pump speed REFUSED: safety
  interlock is OFF" block).
- Keep the `iaq_presence_` guard unchanged.
- No other behavior change: the function still sets `iaq_return_home_ = true` and
  arms STATUS (`0x06`); the core-1 task still auto-arms HOME (`0x01`) once the
  status page is read, so temperatures resume.

File `components/jandy_aqualink/jandy_aqualink.h` (~line 74):

- Update the doc comment above `read_pump_speed()` so it reads "gated by
  iAqualink presence" (drop "the master interlock +").

No other C++ changes. Every write and equipment path keeps its `interlock_` gate
exactly as today: `set_pump_rpm`, the value-set sequence (`iaq_set_step_`),
`iaq_press`, `iaq_nav`, `arm_key`, `request_pool_mode`.

### 2. Firmware yaml additions

File `firmware/pool-bridge.yaml`:

- New template switch, name "Pool Pump Auto-Refresh", id `pump_autorefresh`,
  `optimistic: true`, `restore_mode: RESTORE_DEFAULT_OFF`, icon
  `mdi:timer-refresh`. On/off actions hold state only; the switch sends nothing
  by itself.
- New `interval:` block, `interval: 15min`, with a lambda: if
  `id(pump_autorefresh).state` AND `id(iaq_presence).state`, call
  `id(jandy_comp).read_pump_speed()`. Otherwise do nothing.

Why the timer also checks presence: `read_pump_speed()` already refuses when
presence is off; the extra check in the lambda avoids a "REFUSED" log line every
15 minutes whenever presence happens to be off.

After the firmware build, mirror the same two additions into the LIVE dashboard
config (GET/POST `/edit?configuration=pool-bridge.yaml`, readback-verified) so
the change survives a future dashboard recompile. Back up the live yaml first
(`dashboard-pool-bridge.BACKUP-<stamp>.yaml`).

### 3. Safety model (unchanged except the one read)

- With the interlock OFF (the resting state), every equipment and write path
  still logs REFUSED and transmits nothing on the wire.
- The only action newly allowed without the interlock is the view-only STATUS
  read, which sends only global navigation keys (STATUS `0x06`, then HOME `0x01`)
  and actuates no equipment.
- `set_interlock(false)` remains the hard abort: it clears any armed key, aborts
  an in-progress value-set sequence, and clears `iaq_return_home_`.

### 4. Verification

Build and health gate:

- `pytest` green in the repo root (the frame/decoder suite; unaffected by this
  change, but proves build integrity).
- After flash, confirm on-device `selftest PASS -> 13/13` and checksum errors 0
  over a census window longer than 15s. Do NOT actuate on any FAIL.

Build and flash, from `C:\Users\Falcon\Documents\pool-controller`:

- `esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 600`
- `esphome_ws.ps1 -Action upload  -Config pool-bridge.yaml -Port 192.168.4.51`
- Expect "Successfully compiled/uploaded program." and EXIT CODE 0. Trust the
  human-readable tail lines; the `.out` marker-count grep reads 0 even on success
  (space-between-every-char artifact).

On-device behavioral proof (interlock OFF, presence ON):

1. Press "Read Pump Speed": the Pool Pump Speed sensor updates. Read works
   without the interlock.
2. Press a pump preset (a write): the log shows "set_pump_rpm REFUSED: safety
   interlock is OFF" and the pump RPM is unchanged. Writes stay locked.
3. Turn "Pool Pump Auto-Refresh" ON: a read lands about every 15 minutes in the
   log and in HA history.

Schedule watch:

- Leave Auto-Refresh ON for roughly 24 hours. Read the Pool Pump Speed history
  and Logbook in Home Assistant to map when, and to what, the panel moves the
  pump on its own.
- The outcome feeds the Session 10 decision: a SIMPLE schedule (an HA override
  and salt-floor guard are enough, never touch the panel) versus a BUSY one
  (consider a deeper fix).

## Non-goals / out of scope

- No edit of the panel's internal stored schedule this session. That is the
  Session 10 decision, likely solved by HA-as-scheduler rather than a panel wipe.
- Do not revive the dead iAquaLink 2.0. It fights our 0x33 bus seat (only one
  device can hold 0x33 at a time) and is a last-resort fallback only.
- Heaters stay excluded and last (Session 9). The easy DEVICES toggles (spa
  light, extra aux, sprinklers) are Session 8.
- No new Python test required: gating is C++ runtime behavior, not frame logic,
  and the selftest covers frame vectors only.

## Watchpoints

- ESPHome object-id REST URLs (`/button/<id>/press`, `/switch/<id>/turn_on`) are
  deprecated and removed in 2026.7.0. Drive controls via HA service calls
  (`button.press` / `switch.turn_on` / `number.set_value`). Do not reintroduce
  object-id URLs in any new automation built on this.
- Each auto-refresh read blinks the temperatures out for about 2 seconds. At a
  15-minute interval that is about 96 reads per 24 hours, which is acceptable. If
  the founder finds the blink disruptive, lengthen the interval.
