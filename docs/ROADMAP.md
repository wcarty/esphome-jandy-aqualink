# 🗺️ Roadmap

> **Current reference for delivered features, known constraints, and planned
> work.**
>
> **Current build (2026-09-05):** The component reads AquaPure output, salt,
> faults, and TrueSense pH/ORP; provides state-aware equipment controls,
> pump/heater setpoints, direct AUX2/AUX6 controls, and an authenticated
> dashboard. Earlier sections are retained as project history; use the README
> and capability map for the current supported surface.

> [!IMPORTANT]
> Current priorities: preserve the physical iAqualink controller's `0x33` slot,
> capture AllButton traffic through the authenticated web log stream, then add
> an AllButton-only AquaPure output/Boost path in
> [issue #1](https://github.com/wcarty/esphome-jandy-aqualink/issues/1).

> [🏠 Current README](../README.md) · [🧩 Capability map](PANEL-CAPABILITY-MAP.md) · [📚 Documentation guide](README.md)

## 📜 Historical roadmap

The sections below document the project history. Current behavior and constraints
are summarized in the [README](../README.md) and
[capability map](PANEL-CAPABILITY-MAP.md).

## v1 (done): keypad presence + health diagnostics

Hold an emulated AllButton keypad on the bus, reply to polls in-slot on core 1,
and publish presence-health sensors (polls answered, reply latency, checksum
errors). Read-only, no equipment actuation. Proven on a real RS-8 panel.

## Phase 2: read temperatures and setpoints

> **Status update (Session 2, 2026-05-30).** The AllButton keypress approach
> below was built and proven safe, but it does not yield temperatures on the test
> panel: that panel has **no LCD keypad**, so it emits no `CMD_MSG` display text
> to read. A full bus census found the panel broadcasts no temperatures passively
> at all; it only sends the equipment LED bitmap (`08/02`). The panel does poll
> the dead iAquaLink's slot (0x33) every cycle, and that channel carries the
> temperature text (`CMD_IAQ_PAGE_MSG` 0x25, which the Reader already decodes).
> So the real path on this panel is **emulating the iAqualink device at 0x33**,
> not AllButton keypresses. See `SESSION-3-iaqualink-kickoff.md`. The keypress
> machinery below remains correct and reusable for iAqualink navigation.

**The problem.** A registered keypad receives two kinds of traffic: binary
status frames (equipment/LED states), which the panel sends continuously, and
display-text frames, which carry the human-readable temperatures and setpoints.
On the test panel the display-text channel is only pushed in response to keypad
button presses. Sitting passively yields status but not the temperature text.

**The approach.** Emulate the keypad more fully: send button presses to walk the
panel's menu so it redraws the display, then parse the redrawn text. This is the
technique AqualinkD uses. The reference is `allbutton_aq_programmer.c` in
AqualinkD (select_menu_item, the read loop that waits for the display, parses
the value, and presses a navigation key). The pending-key byte in our
acknowledgement is the mechanism: instead of always `0x00`, send a navigation
keycode for one reply, then read the display the panel sends back.

**The risk.** Keycodes can actuate equipment, not just navigate. This phase must
map keycodes carefully, send one key at a time, log every transmitted byte, keep
a hard abort, and ideally be tested while someone can watch the equipment. It is
the same machinery as writing.

**Decode targets.** For AllButton, the display text arrives as `CMD_MSG` (0x03)
and `CMD_MSG_LONG` (0x04); the existing `Reader` in `jandy_proto` already pairs
label lines with value lines and can be extended to these command codes. For the
iAqualink protocol (address 0x33 on this panel) the text arrives as page
messages (`CMD_IAQ_PAGE_MSG` 0x25, framed by 0x23 start and 0x28 end); that path
also registered successfully in testing and is an alternative for panels with a
free iAqualink slot.

## Phase 3: setpoint control

> **DONE (Session 9 Phase 2, 2026-06-03), origin/master `a2039e2`.** Heater
> setpoints (pool + spa) ship via the value-set path, NOT the up/down menu-walk
> sketched below: nav HOME -> Other Devices -> DEVICES (0x36) -> press the heat
> item (Pool Heat `0x14` / Spa Heat `0x15`) -> SET_TEMP (`0x39`) -> `0x80` control
> request -> panel grants `0x31` CTRL_READY -> `0x24` value frame (ASCII degree
> digits, `num2iaqtRSset`) -> HOME. Gated by the master interlock + iAqualink
> presence; HA `number` entities Pool/Spa Heat Setpoint (clamps 45-90 / 80-104).
> Live-proven with the founder: the pool heater physically fired at target 90; the
> spa heated and the panel auto-offed spa heat at its 94 setpoint, well below the
> 104 ceiling (the proof, since the setpoint is unreadable). QUIRKS: SET_TEMP
> renders blind to our 0x33 emulation (no setpoint readback, no body-select
> buttons) so we write blind and confirm by heater behavior; the heat-item press
> opens SET_TEMP only ~50% of the time (auto-retry added in `a2039e2`: re-press the
> heat item on DEVICES if the SET_TEMP page-start does not arrive within ~4s); the
> panel commits the SET_TEMP page slowly (page-end ~11-27s after the start) so a
> write takes ~30s; the `*_heat_enabled` binary_sensors decode unreliably on this
> panel (trust the panel's own heat light, not the sensor). Heater hardware =
> Pentair MasterTemp commanded by the Jandy AquaLink; the AquaLink setpoint is the
> governing target, the MasterTemp's 104 is just its ceiling.

Once the menu-walk read loop is solid, extend it to change the pool and spa
heater setpoints: navigate to the setpoint menu, press up or down while reading
the displayed value back after each press, then commit. This is a write and must
stay behind an explicit, off-by-default config flag, with the same safety rules
as Phase 2.

## Phase 4: equipment status decode

Decode the binary `CMD_STATUS` (0x02) frames we already receive into named
equipment on/off states (pump, heater, aux circuits). The raw LED bitmap is
universal, but mapping each position to a named circuit is install-specific, so
this needs a small per-panel mapping config.

## Smaller improvements

- Make the acknowledgement type and the pending-key configurable (some panels or
  keypad types may want a different ack type than the AllButton `0x80`).
- Auto-detect a free keypad address by listening before answering.
- Desync and bus-error recovery hardening for long-term unattended operation.
- A debug build flag that re-enables raw per-frame logging for field diagnosis.

## Status + remaining sessions (updated 2026-05-31, Session 6)

SHIPPED and live-tested: keypad presence (v1), temps via 0x33 emulation, home
controls (filter pump, pool light, cleaner, air blower, pool mode), pump speed
READ (Session 4), SET (Session 6), and a timed auto-refresh schedule-watch
(Session 7). Pump SET was tuned live: the salt cell
flow floor is ~1850 RPM, so presets are Night 1100 / Low 2000 / Normal 2750 /
High 3200. Resting state safe (control interlock OFF, presence ON).

Each remaining session has a self-contained, paste-ready kickoff doc. Order is
deliberate (read before write, low-stakes before high-stakes); 7 and 8 are
independent and can swap.

- **Session 7** `SESSION-7-schedule-watch-kickoff.md` — SHIPPED 2026-05-31
  (commit `2fbe6c5`). Un-gated the view-only pump read from the control interlock
  (presence-only now) and added a "Pool Pump Auto-Refresh" switch + 15-min
  interval. The watch is running; next session reads the ~24h `Pool Pump Speed`
  history to map the panel's stored schedule, then makes the Session 10 decision.
- **Session 8** `SESSION-8-easy-toggles-kickoff.md` — SHIPPED 2026-06-02
  (commits `67c8541`..`675f586`). Gated Spa Light / Solar Light / Sprinklers
  DEVICES-page toggles, founder-watched live test. Discoveries: "Extra Aux" is the
  Solar Light (renamed, confirmed energizing live); Spa Light and Sprinklers send
  correctly on the bus but are physically disconnected (unplugged fixture / legacy
  removed controller). Page-guard + allowlist proven on harmless gear.
- **Session 9** `SESSION-9-heaters-kickoff.md` — heater ON/OFF (pool + spa)
  SHIPPED + founder-live-tested 2026-06-02 (commits `a024f55`..`b3547f2`). Both
  heaters proven; the spa heater physically fired. Also added a "Switch to Spa
  Mode" button (mirror of Switch to Pool Mode) and fixed a flaky spa-mode source
  (now gated on the reliable 0x08 `cs_spa_` bit). FINDING: the HOME heat button is
  a pure on/off toggle, not a setpoint opener, so the temperature SETPOINT is a
  Phase 2 build via the DEVICES route (DEVICES Pool Heat 0x14 / Spa Heat 0x15 ->
  SET_TEMP 0x39). Founder needs spa target 94F (not the 104F max), so the setpoint
  is concretely needed. Spec `2026-06-02-heaters-design.md`, plan `2026-06-02-heater-onoff-and-survey.md`.
- **Session 10** `SESSION-10-schedule-decision-kickoff.md` — deal with the
  panel's stored schedule, gated on Session 7. Likely solved by HA-as-scheduler
  (override / guard the salt floor) rather than editing the panel. The dead
  iAquaLink 2.0 is only a last-resort fallback (conflicts with our 0x33 seat).

Phase 4 (decode the CMD_STATUS LED bitmap into named circuit states) remains the
optional polish after control is complete.

## The HA pool brain (shipped 2026-06-04)

Home Assistant is now the pool's autonomous scheduler, driving the box's shipped
controls, with the panel's own stored schedule kept underneath as the hardware
failsafe.

The box gained a narrow "Pool Scheduler" permission (firmware `scheduler_armed`
switch, now `ALWAYS_OFF` so it never self-resumes after a power blip). When on, it
lets HA set pump speed and toggle the filter pump (`0x11`) and cleaner (`0x15`)
WITHOUT the master interlock, scoped per-key by `is_scheduler_safe_key`. Every other
write (heaters, spa, valves, blower, lights, DEVICES toggles) still requires the
master interlock, which still boots OFF. Proven live: under the scheduler the pump
set completes and the pump physically moves; the blower and heaters still refuse.

### Daily schedule (HA local time)

- 22:00 to 08:00 Quiet: pump Night 1100, cleaner off.
- 08:00 to 10:00 Morning clean: pump Normal 2750, cleaner on.
- 10:00 to 20:00 Day: pump Low 2000 (above the ~1850 salt floor), cleaner off.
- 20:00 to 22:00 Evening clean: pump Normal 2750, cleaner on.

### How it works

- A 2-minute "watch and correct" automation reads the pump and sets it back if the
  panel moved it off target, and enforces the cleaner state. It pauses during spa,
  a manual hold, bridge-offline, or scheduler-off.
- The pump is never commanded off (lowest is 1100), so the panel's own schedule keeps
  the water moving if HA or the box dies. Stagnation needs HA, the box, and the panel
  schedule to all fail at once.
- Spa: switching to spa stands the brain down (it touches nothing); switching back
  restores the filter pump (with a retry after the valves settle) and resumes.

### Controls (HA entities)

- `switch.pool_rs485_bridge_pool_scheduler`: the brain on/off. Turn OFF to pause the
  whole brain, and FIRST before bringing the iAqualink 2.0 online (the presence keeper
  is gated on this, so disarming fully stands the box down off the 0x33 seat).
- `switch.pool_rs485_bridge_pool_keypad_keypress_armed`: the master interlock for
  manual/risky writes. Unchanged; boots OFF.
- `input_button.pool_swim_boost`: bump to 2750 for ~2h or until the next phase.
- `input_number.pool_manual_speed_request`: hold a chosen speed for ~2h / next phase.
- `input_select.pool_phase`: shows the current phase (or "Spa (manual)").

### HA objects (created via the config API, not in this repo)

Helpers: `input_number.pool_target_rpm`, `input_boolean.pool_cleaner_should_run`,
`timer.pool_manual_hold`, `input_select.pool_phase`, `input_button.pool_swim_boost`,
`input_number.pool_manual_speed_request`.
Scripts: `pool_apply_pump`, `pool_apply_cleaner`, `pool_set_phase`,
`pool_evaluate_phase`.
Automations: `pool_phase_scheduler`, `pool_watch_and_correct`, `pool_startup_resync`,
`pool_spa_standdown`, `pool_restore_after_spa`, `pool_swim_boost`, `pool_manual_speed`,
`pool_presence_keeper`.

Spec `docs/superpowers/specs/2026-06-03-pool-ha-brain-design.md`, plan
`docs/superpowers/plans/2026-06-03-pool-ha-brain.md`.
