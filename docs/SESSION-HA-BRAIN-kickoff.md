# 🗃️ Session kickoff: the pool HA brain (HA-as-scheduler + filtration failsafe)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. This is the project endgame: make Home
Assistant the smart scheduler for the pool, driving the panel through the ESP
bridge box's existing (already-shipped, live-proven) controls, while the panel's
own minimal schedule stays in place as a hardware failsafe. Brainstorm first.

All the control pieces the brain needs are done. Heater on/off shipped + live-tested
2026-06-02; heater temperature setpoints (pool + spa) shipped + live-tested 2026-06-03
(origin/master HEAD `d2f79d0`). So this session is the orchestration layer, not new
device control.

## Paste this to start

> **Start the pool HA brain.** Make Home Assistant the smart scheduler for the pool's
> pump, cleaner, and filtration, driving the panel through the ESP bridge box's
> already-shipped, live-proven controls, with a hard filtration failsafe so the water
> never goes stagnant if HA or the box hiccups. Brainstorm the architecture with me
> FIRST, then write a spec, then build incrementally.
>
> Read first, in `C:\Users\Falcon\Documents\pool-controller\esp32-experiment`: the
> memory topic `project_pool_controller_phase2.md` (the whole file, especially the
> Session 7 schedule-watch, the Session 8 "HA brain is next" framing and strategic
> pivot, and the Session 9 setpoint entries), `docs/ROADMAP.md`, this doc
> `docs/SESSION-HA-BRAIN-kickoff.md`, and `docs/SESSION-10-schedule-decision-kickoff.md`
> if present.
>
> Carry these in:
> - The ESP box (`192.168.4.51`, repo `esphome-jandy-aqualink`, origin/master HEAD
>   `d2f79d0`) holds Jandy keypad + iAqualink presence and exposes ALL controls to HA,
>   gated behind a master interlock that boots OFF: filter pump on/off, pump speed (RPM
>   + presets Night 1100 / Low 2000 / Normal 2750 / High 3200), cleaner, lights, blower,
>   pool/spa mode, DEVICES toggles, heaters on/off + setpoints. The brain mainly needs
>   the pump + cleaner + filtration controls, all shipped and live-tested.
> - The panel KEEPS its own stored schedule. We could not wipe it (its menu renders
>   blank to our emulation, conclusively dead). So HA and the panel must COEXIST: HA
>   layers smarter scheduling on top, and the panel's own minimal schedule is the
>   HARDWARE FAILSAFE that keeps water moving if HA or the ESP dies. The panel can
>   override the pump within ~1 second (seen live in Session 7), so the brain must
>   coordinate with panel overrides, not assume sole control.
> - FAILSAFE is the founder's hard constraint: never let the water go stagnant on an
>   HA/ESP outage. The panel's schedule is the floor; the design must not defeat it.
> - SALT FLOOR: the salt cell needs ~1850 RPM minimum to chlorinate; keep the pump above
>   that whenever it is running for chlorination.
> - Central design tension to resolve in the brainstorm: every WRITE is gated by the
>   master interlock, which boots OFF for safety. An autonomous scheduler needs to write
>   (set pump speed, run cleaner) unattended. Session 7 already un-gated the view-only
>   pump READ from the interlock (kept it behind presence). Decide how autonomous WRITES
>   coexist with the safety interlock: un-gate specific scheduler writes, a separate
>   "scheduler armed" switch, or the brain arming around each action? This is the key
>   architectural call.
> - Session 7's 15-min pump-speed auto-refresh + HA history lets the brain SEE when the
>   panel moves the pump on its own, useful for detecting and coordinating with panel
>   overrides.
>
> Current resting state: pool mode, filter pump on, interlock OFF, presence ON, heater
> setpoints written (pool 85, spa 94), device on HEAD `d2f79d0`.
>
> I'm non-technical: plain English alongside any code, no em dashes, no AI jargon.
> Brainstorm with me before building anything.

## The goal

Home Assistant becomes the brain: it runs a smart pump / cleaner / filtration schedule
through the box's controls, instead of leaving it to the panel's fixed program. But the
panel's own minimal schedule stays in place underneath as a hardware failsafe, so the
water keeps circulating even if HA or the ESP box goes down.

## Why HA and the panel COEXIST (no wipe)

The original plan was to wipe the panel's stored schedule and make HA the sole brain.
That is dead: the panel's program editor lives in the iAqualink MENU, which renders
BLANK to our 0x33 emulation (confirmed conclusively across Sessions 4, 6, and a
dedicated "try harder" experiment). AqualinkD also does not edit schedules over the bus
(it uses an external cron scheduler). So we do not delete the panel's schedule. We layer
HA on top and treat the panel's own schedule as the safety floor.

## The brain's toolkit (all shipped + live-proven)

- Filter pump on/off; pump speed (RPM 600-3450 + presets Night 1100 / Low 2000 /
  Normal 2750 / High 3200, tuned to the ~1850 RPM salt-cell flow floor).
- Cleaner on/off; pool/spa mode switch; lights; blower; DEVICES toggles.
- Heaters on/off (pool + spa) and temperature setpoints (pool + spa).
- A 15-min pump-speed auto-refresh + HA history (Session 7) so HA can observe the
  panel's own pump moves.
- Read-only equipment-state sensors from the 0x08 status stream: filter pump, cleaner,
  spa mode, air blower (reliable). NOTE: the heater-enabled sensors are NOT reliable on
  this panel (trust the panel's heat light, not the sensor).

## Hard constraints

- FAILSAFE: never let the water go stagnant on an HA/ESP outage. The panel's schedule is
  the floor; do not defeat it.
- SALT FLOOR: keep the pump above ~1850 RPM when running for chlorination.
- SAFETY: writes are gated by the master interlock (boots OFF) + iAqualink presence.

## Things to figure out in the brainstorm

1. Interlock vs autonomous writes (the central tension above): how does a scheduler
   that writes unattended coexist with an interlock that boots OFF? (un-gate specific
   scheduler writes / a "scheduler armed" switch / arm-around-each-action.)
2. HA vs panel coordination: the panel can re-assert its own pump moves within ~1s. Does
   HA work WITH the panel's schedule (nudge within it), or actively override + re-assert
   (whack-a-mole risk)? Use the Session 7 watch to detect panel moves.
3. The failsafe mechanism: confirm the panel's stored schedule actually guarantees a
   minimum daily circulation, and that nothing HA does suppresses it. Possibly a
   watchdog (if HA/ESP silent for N hours, the panel's schedule must have run).
4. What schedule HA should run (founder's filtration goals, salt chlorination window,
   quiet overnight, cleaner windows) and how it respects the salt floor.
5. Optional: fold heater coordination in later (run the pump when heating; the setpoints
   are now controllable), but keep the first cut to pump/cleaner/filtration.

## Current state + recipe

- Resting state: pool mode, filter pump on, interlock OFF, presence ON, sniff +
  auto-refresh OFF, heater setpoints written (pool 85, spa 94). Device on origin/master
  HEAD `d2f79d0`.
- Build/flash: `esphome_ws.ps1` (compile then upload via the dashboard at
  192.168.1.126:6052). Live-log capture: `C:\Users\Falcon\poollog.ps1`. HA control via
  the `mcp__Home_Assistant__*` tools.
- Founder is non-technical: plain English, no em dashes, no AI jargon. Brainstorm before
  building.
