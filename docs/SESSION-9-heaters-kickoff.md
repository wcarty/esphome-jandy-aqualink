# 🗃️ Session 9 kickoff: heaters (pool + spa) on/off + temperature setpoint

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief. Self-contained. Highest-stakes equipment, so it is LAST among
the control builds and needs a careful founder-watched live test. The good news:
the hard machinery (value-set) is already proven from the pump work.

## Scope (confirmed with founder 2026-05-31)

BOTH pool and spa heat (founder heats both at different times of year; the pool
sits ~91F naturally in TX but is still heated sometimes). Each independently
gated. Two parts per body: (A) on/off toggle, (B) temperature setpoint.

## Where we are (start of Session 9)

- Value-set machinery PROVEN LIVE for pump RPM (Session 6): nav to a value page,
  reply 0x80 (control request), panel sends 0x31, send the 0x24 value frame with
  ASCII digits, read back via STATUS, return HOME. The setpoint reuses this.
- Heater on/off keycodes already known but deliberately EXCLUDED from every
  allowlist so far. Sessions 7 (schedule-watch) and 8 (easy toggles) should be
  done first; the page-context guard pattern gets exercised on harmless gear in 8.
- Repo + device + dashboard as in the other kickoffs (HEAD on origin/master,
  device 192.168.4.51, dashboard 192.168.1.126:6052). Resting state safe.

## Part A: on/off toggles

- HOME page keycodes (see docs/PANEL-CAPABILITY-MAP.md): **Pool Heat 0x13**,
  **Spa Heat 0x14** (keycode = 0x11 + home index; index 2 + 3).
- PAGE-CONTEXT TRAP, the whole reason heaters are careful: 0x13 on HOME is Pool
  Heat, but 0x13 on the DEVICES page is VSP1 Spd ADJ (the pump). And on DEVICES,
  0x14 is Pool Heat while on HOME 0x14 is Spa Heat. So heater on/off MUST confirm
  `current_page() == HOME (0x01)` before sending, and drive ONLY from HOME. Pick
  HOME and lock it (that is where the simple on/off lives).
- Implement as dedicated gated methods (preferred over just widening the
  allowlist), each confirming page == HOME, each behind interlock + presence.
  Two HA buttons: Pool Heat toggle, Spa Heat toggle.

## Part B: temperature setpoint

- Lives on the SET_TEMP page (id 0x39). **The nav path to SET_TEMP is NOT YET
  MAPPED** (PANEL-CAPABILITY-MAP marks it unknown). So step one of this session
  is a SURVEY, reusing the Session 4 technique:
  - Cycle presence OFF/ON to force a fresh full page enumeration.
  - From HOME, try opening the heat setpoint (likely a press on the Pool Heat /
    Spa Heat item from a specific page, or a dedicated menu key). Log pages with
    the on-device compact decoder until SET_TEMP (0x39) appears, and record the
    exact keycode + page that opens it for pool vs spa.
  - AqualinkD `iaqtouch_aq_programmer.c` (cloned at `..\AqualinkD-ref`) is the
    reference for the setpoint command sequence.
- Once mapped, the write is the proven value-set: nav to SET_TEMP, 0x80 control
  request, 0x24 value frame with the ASCII setpoint digits, STATUS readback,
  return HOME.
- Digit encoder: reuse `num2iaqt` from the pump work, but setpoints are 2-3 digit
  degrees F, not 4-digit RPM. Re-check the width/padding and the sub-1000 quirk
  for this range; TDD the encoder against any captures found in the survey or in
  iaqtouch.h. Add C++ selftest vectors.
- Clamps (decide with founder): sane pool + spa ranges, e.g. pool ~60-95F, spa
  up to ~104F. Refuse out-of-range.

## Safety rails (heaters are the high-stakes build)

- Page-context guard on EVERY heater keycode (0x13/0x14 only on HOME; setpoint
  keys only on the confirmed SET_TEMP nav path). This is non-negotiable given the
  0x13 = pump-adjust collision.
- Panel's own flow interlock: in Auto mode the panel should refuse to fire a
  heater without the filter pump running. CONFIRM this holds in the live test
  before trusting it; do not rely on it blind.
- Gate behind master interlock + presence like everything else.
- Consider an optional auto-off / max-runtime guard so a heater can't be left on
  indefinitely by a bug. Discuss with founder.
- Heater type (gas vs heat pump) per body is TBD; the control path is panel-
  abstracted either way, it only affects how fast it heats and what the live test
  should expect to see.

## Live test (founder watching, at the pad/spa)

- Refusal check with interlock OFF (no actuation).
- Arm. Spa heat on, confirm the heater actually fires AND that the panel enforced
  the pump-running interlock. Set a spa setpoint, confirm via STATUS readback and
  at the spa. Repeat for pool heat. Turn heaters back off. Disarm.
- Watch for side effects (mode changes, pump behavior).

## Build/flash + TDD + git

Same recipe as Sessions 6-8 (esphome_ws.ps1 compile then upload -Port
192.168.4.51; patch the live dashboard yaml + back up; confirm selftest PASS
13/13 on boot; background ws log capture read via shared-read open; pytest green
before flashing; `-m` before `--`, `git add` new files, GitHub-noreply identity,
no em dashes).

## After this

Only the schedule decision remains (Session 10). Then Phase 4 (decode the
equipment LED bitmap into named on/off states) is the optional polish.
