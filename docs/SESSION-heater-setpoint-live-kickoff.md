# 🗃️ Session kickoff: heater setpoint LIVE deploy + survey + setpoint build (Phase 2 continuation)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. The desk-side code is already BUILT, tested, and
reviewed; this session is the founder-at-the-pad live work. Self-contained.

## Paste this to start

> Continue the pool heater temperature setpoint (Phase 2 of heaters). The desk-side
> code is already BUILT, tested, and reviewed (ready to flash). The remaining work is
> the live deploy and survey at the pad, then building the setpoint control from what
> the survey finds, then the live test. I'm at the pool to watch.
>
> Read first, in C:\Users\Falcon\Documents\pool-controller\esp32-experiment: the memory
> topic project_pool_controller_phase2.md (the "Session 9 Phase 2 DESK" section at the
> top), the spec docs/superpowers/specs/2026-06-02-heater-setpoint-design.md, the plan
> docs/superpowers/plans/2026-06-02-heater-setpoint.md, and this doc
> docs/SESSION-heater-setpoint-live-kickoff.md.
>
> State: local master is ahead of origin (HEAD a kickoff-doc commit on top of the desk
> build), pytest 110 green, NOT pushed, NOT flashed; the device still runs the Phase 1
> on/off firmware. The value-frame format is already pinned from AqualinkD's captured
> frames (spa 94 and pool 85 are computed, not guessed). The ONE live unknown is how the
> temperature screen (SET_TEMP 0x39) opens on this screenless panel.
>
> Pick up the EXISTING plan at Task 8 and execute Tasks 8 through 12. Do not re-brainstorm
> or re-plan (the spec and plan are approved); use executing-plans or
> subagent-driven-development. I'm non-technical, so explain in plain English. No em dashes.

## Where we are (confirmed, end of the build session 2026-06-02)

- Desk phase (plan Tasks 1-7) is DONE: built subagent-driven, each task spec-reviewed +
  code-quality-reviewed, plus an opus final whole-implementation review = "READY TO FLASH".
- Local `master` is ahead of `origin/master`, pytest 110 green, both C++ files
  brace-balanced. NOT pushed, NOT flashed. Device `192.168.4.51` still runs the Phase 1
  on/off firmware. Dashboard `http://192.168.1.126:6052`.
- What is built (desk): the temperature value frame (`build_settemp_frame` + `num2iaqt_temp`,
  py + C++ + selftest), clamps (pool 45-90, spa 80-104), the SET_TEMP page gate
  (`settemp_write_allowed`), HOME-page heater on/off decode -> two HA binary_sensors
  (`pool_heat_enabled` / `spa_heat_enabled`), a sticky-water-mode regression test, and a
  gated one-shot page-confirmed `survey_press` + 3 survey buttons.

## The value format is SOLVED (not guessed)

AqualinkD uses the same `num2iaqtRSset` encoder for pump RPM and heater setpoint; its
captured "Set Temp (pool)" frames verify ours byte-for-byte (50F cksum 0xfe, 100F cksum
0x2a, reproduced by hand). Computed targets: 85F `38 35 00 00 30 00` (cksum 0x06), 94F
`39 34 00 00 30 00` (cksum 0x06), 104F `31 30 34 00 30 00` (cksum 0x2e). Full frame:
`10 02 00 24 31 <6-byte digit field> <ten 0xcd> <cksum> 10 03` (24 bytes).

## The one live unknown: how SET_TEMP (0x39) opens on this panel

AqualinkD reaches SET_TEMP via the MENU (KEY04 = 0x14), but this screenless panel's MENU
renders empty, so that route is doubtful. The pump's value page WAS reachable via the
DEVICES "ADJ" item, so the survey tries the DEVICES route FIRST, then MENU as a fallback:

1. DEVICES route: nav Other Devices -> DEVICES (0x36) -> press DEVICES Pool Heat (0x14) /
   Spa Heat (0x15) -> watch the log for `IAQ PAGE SET_TEMP(0x39)`. (Risk: the heat item
   might just toggle the heater instead of opening the screen; founder is watching.)
2. MENU route: nav Menu -> MENU (0x0F) -> press Set Temp (0x14) -> watch for 0x39.

If SET_TEMP opens, capture the page enumeration: the pool/spa button keycodes (to select
the body before writing) and the CURRENT setpoints shown (we should see the spa's stored
~104 directly). If NEITHER route opens 0x39, the on/off + sensors still ship and only the
target-temp part defers.

## Remaining plan (Tasks 8-12; all need the founder at the pad)

- **Task 8 deploy:** `git push origin master`; back up + patch the LIVE dashboard yaml
  (the 2 heat-enabled sensors + 3 survey buttons; the 2 number entities come in Task 10)
  via WebClient to :6052/edit, readback-verify; compile + upload via `esphome_ws.ps1`;
  confirm `selftest PASS` + `checksum_errors=0`, presence ON, interlock OFF.
- **Task 9 LIVE survey** (founder watching): enable Pool Heat, run the survey above,
  record the route + enumeration + current setpoints.
- **Task 10 setpoint state machine + HA number entities:** build `advance_settemp_sequence_`
  + `set_heater_setpoint` + `send_settemp_set_` (mirroring `advance_set_sequence_`) and the
  two "Pool Heat Setpoint" / "Spa Heat Setpoint" number entities, from the survey route.
  ALSO apply the deferred robustness fix (Task 10 step 6): add `|| iaq_survey_key_ >= 0` to
  the busy-guards in set_pump_rpm / press_device_toggle / press_heater (symmetric survey lock).
- **Task 11 LIVE setpoint test** (founder watching): refusal check, set pool 85, set spa 94
  (in spa mode), confirm at the equipment, observe whether the panel drops spa heat on
  spa-mode exit (the open spa-auto-off question). Resting state: leave Pool Heat enabled at
  85 (panel holds 85; June pool is above 85 so it will not fire); spa heat off.
- **Task 12:** record findings in memory + ROADMAP + spec; finish the branch.

## Durable facts to carry in

- Gate spa-mode on `cs_spa_` (the reliable 0x08 bit), never the flaky iAq home label.
- Page-scoped keycodes: 0x14 is Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU.
  Every keycode confirms the page first; the 0x24 value frame goes out ONLY on SET_TEMP.
- The water-mode reliability "fix" was already done in Phase 1 (cs_spa_); do not redo it.
- Pump-restore-after-spa-exit: switching spa -> pool drops the filter pump; re-press Filter
  Pump after the valves settle (the first press during the transition does not take).
- Build/flash recipe + live-log capture (`C:\Users\Falcon\poollog.ps1`) as in Sessions 8-9.
  Founder is non-technical: plain English, no em dashes, no AI jargon.
