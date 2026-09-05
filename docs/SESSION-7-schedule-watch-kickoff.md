# 🗃️ Session 7 kickoff: pump-speed auto-refresh + stored-schedule watch

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for the next session. Self-contained. Brainstorm the gating
decision with the founder FIRST (it changes the safety model), then build.

## Where we are (start of Session 7)

- Repo HEAD `67b4c3f` on origin/master, tree clean. Device `192.168.4.51`
  (ESPHome node `pool-bridge.yaml`, dashboard `http://192.168.1.126:6052`).
- Pump speed SETTING shipped + live-tested 2026-05-31 (Session 6). Exact-RPM
  writes work and are readback-verified. Presets tuned and flashed:
  Night 1100 / Low 2000 / Normal 2750 / High 3200.
- Salt cell flow floor found live: **~1850 RPM** (flow switch cuts out below it,
  much higher than the assumed ~1450). Low = 1850 floor + 150 cushion = 2000.
- Resting state: interlock OFF, presence ON, temps + on-demand pump read live,
  pump last left at 2750. Selftest PASS 13/13, checksum errors 0.

## The goal

The founder wants to know when the panel's OWN stored schedule changes the pump
speed on its own clock (it self-schedules: cleaner switches on unprompted, pump
went 2750 -> 1700 during the Session 4 survey with no input). Reading the RPM
over ~24h reverse-engineers that hidden schedule.

Two pieces:
1. **Auto-refresh**: read the pump speed on a timer (e.g. every 5-15 min) instead
   of only on a button press, so HA history captures the schedule's changes.
2. **Schedule watch**: with auto-refresh running, log/observe RPM over a day and
   report when + to what the panel changes it.

## Why this matters (the founder's real concern)

The founder's worry: the panel's stored schedule will keep fighting whatever
Home Assistant tries to do. That is a real conflict, since today the panel's
schedule is the only thing driving the pool. This watch is the prerequisite for
resolving it.

The reframe (decided with the founder 2026-05-31): now that reliable WRITE
control is proven, we probably do NOT need to edit the panel's internal schedule
at all. Instead let HA be the scheduler and either absorb/work around the panel
schedule once we know what it does, or at minimum GUARD THE SALT FLOOR: if the
pump ever drops below ~1850 during the day, HA bumps it back up. The panel
schedule becomes toothless without ever opening its hidden editor.

So this watch decides which world we are in: a SIMPLE schedule (HA override is
enough, never touch the panel) or a BUSY one (consider an actual wipe, see the
Session 10 kickoff). Do NOT revive the dead iAquaLink 2.0 to wipe the schedule
before this watch is done: it is maybe-dead hardware, it fights our 0x33
emulation for the same bus seat (only one device can hold 0x33 at a time), and
it is just one of several wipe paths. Keep it as a drawer fallback; decide in
Session 10.

## The decision to make FIRST (brainstorm with founder)

Today, reading the speed requires the CONTROL interlock to be armed (see code
below). Auto-refresh on a timer would therefore mean leaving control armed
unattended, which we will not do. The clean fix:

> **Un-gate the view-only read from the control interlock.** Let
> `read_pump_speed()` run under iAqualink PRESENCE only, dropping its
> `interlock_` check. Keep presence as the gate so it is still a deliberate,
> switchable feature.

Why this is safe: `read_pump_speed()` only ever sends VIEW-ONLY navigation keys
(STATUS 0x06 to see the page, then HOME 0x01 to return). It never sends an
equipment keycode and never enters the value-set sequence. Navigation alone
moves no equipment. Every WRITE path (`set_pump_rpm`, `iaq_press`, `iaq_nav`,
`arm_key`, `request_pool_mode`) stays gated by `interlock_` exactly as now.

Options to weigh with the founder:
- A) Drop `interlock_` from `read_pump_speed()` only; add an "Auto-refresh pump
  speed" switch (default OFF) that drives a periodic read. Simplest; recommended.
- B) Keep the interlock gate but add a separate, time-boxed "monitoring" mode.
  More moving parts; probably not worth it.
- C) Leave gating as-is and only ever read manually. No unattended watch. (This
  is the do-nothing option; name it so the choice is explicit.)

Confirm the refresh interval too (5 / 10 / 15 min). Each read briefly navigates
to STATUS then back to HOME, so temps blink out for ~2s per read; longer interval
= less disruption. 10-15 min is plenty to map a schedule.

## Exact code locations

- `components/jandy_aqualink/jandy_aqualink.cpp`
  - `read_pump_speed()` ~line 277. Gated by `interlock_` (the line to remove)
    AND `iaq_presence_` (keep). Sends STATUS 0x06, sets `iaq_return_home_`.
  - `set_pump_rpm()` ~line 295, `iaq_press()` ~231, `iaq_nav()` ~250,
    `arm_key()` ~202: ALL keep their `interlock_` gate. Do not touch.
  - core-1 auto-HOME after a STATUS read: ~line 171 (`iaq_return_home_`).
- `firmware/pool-bridge.yaml`
  - "Read Pump Speed" button ~line 225-229 (calls `read_pump_speed()`).
  - Presets ~line 234-253. Slider ("Pump Speed Set") ~line 258-270.
  - If adding an auto-refresh switch: add a `switch: platform template` + an
    `interval:` that calls `read_pump_speed()` when the switch is on. Mirror the
    existing presence/interlock switch pattern; default OFF (RESTORE or
    ALWAYS_OFF). Whatever C++ method the switch needs, add it like the others.
- Sensors already exist: `sensor.pool_rs485_bridge_pool_pump_speed` (RPM) and
  `_pool_pump_watts`. Auto-refresh just makes them update on a timer, so HA
  history/Logbook does the schedule capture for free.

## TDD + build/flash recipe (proven Session 6)

- Python suite is the fast gate: `pytest` in the repo root (82+ green). Add a
  test if you change any frame/encoder logic (a pure read-gating change may not
  need one, but mirror any C++ selftest vector you touch).
- Build/flash from `C:\Users\Falcon\Documents\pool-controller` (one level above
  the repo) via `esphome_ws.ps1`:
  - `esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 600`
  - `esphome_ws.ps1 -Action upload  -Config pool-bridge.yaml -Port 192.168.4.51`
  - Expect `Successfully compiled/uploaded program.` + `EXIT CODE 0`. The .out
    files write with a space-between-every-char artifact, so marker COUNT greps
    read 0 even on success; trust the human-readable tail lines.
- Also patch the LIVE dashboard yaml (GET/POST `/edit?configuration=pool-bridge.yaml`,
  readback-verify) so the change survives a future dashboard recompile. Back up
  first (`dashboard-pool-bridge.BACKUP-<stamp>.yaml`).
- Watch the live log via a BACKGROUND ws capture to a file, read it with a
  shared-read open (`[System.IO.File]::Open(path, Open, Read, ReadWrite)`); a
  plain Read hits EBUSY while the writer holds it. Do NOT stack reads behind a
  long blocking capture call in one message (they queue and look like a dead
  channel; that was a self-inflicted misread in Session 6).
- On boot after flash, confirm `selftest PASS -> 13/13` (the census burst logs
  every 12s; capture a >15s window). Do NOT actuate on any FAIL.

## Safety rails

- Verify the refusal path still holds: with interlock OFF, `set_pump_rpm` and
  every equipment key must still log REFUSED and send nothing. Only the
  view-only read should newly work without the interlock.
- Heaters stay excluded and LAST. The easy DEVICES toggles (Spa Light 0x19,
  Extra Aux 0x1d, Sprinklers 0x1e) come before heaters, after this.
- `git commit` gotcha: put `-m "msg"` BEFORE `--  <path>`. `commit --only -- <path> -m <msg>`
  parses the message as pathspecs and fails.
- Commit identity uses the GitHub noreply, not the personal email. No em dashes.

## Watchpoint (carry forward)

ESPHome object-id REST URLs (`/button/<id>/press`) are deprecated and removed in
2026.7.0. We drive controls via HA service calls (button.press / switch.turn_on /
number.set_value), which are unaffected. Just don't reintroduce object-id URLs.
