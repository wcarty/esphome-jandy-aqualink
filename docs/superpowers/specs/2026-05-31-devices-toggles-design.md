# 🧠 Session 8 design: gated DEVICES-page toggles (Spa Light, Extra Aux, Sprinklers)

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

- Date: 2026-05-31
- Status: Approved by founder 2026-05-31. Ready for implementation plan. BUILD / FLASH / LIVE TEST DEFERRED to a later session (see Sequencing).
- Repo: esphome-jandy-aqualink, branch `master`, base commit `47b93ea` (after Session 7).
- Device `192.168.4.51`, ESPHome node `pool-bridge.yaml`, dashboard `http://192.168.1.126:6052`.

## Context

Session 8 is the low-stakes confidence-builder the founder chose before heaters:
gated on/off toggles for harmless DEVICES-page circuits. It exercises the
page-context safety guard on harmless equipment before the higher-stakes heater
work (Session 9). Pump READ/SET (Sessions 4/6) and the schedule-watch auto-refresh
(Session 7) are shipped. Resting state: control interlock OFF, iAqualink presence ON.

## Scope (decided with founder 2026-05-31)

Three gated toggle buttons on the DEVICES page (id `0x36`); keycode = `0x11 + slot`:

- **Spa Light (`0x19`)** — known; the founder wants it (and is unsure the fixture
  actually works, which the live test will settle).
- **Extra Aux (`0x1d`)** — function unknown; "Extra Aux" is the panel's own generic
  label. Built as a toggle specifically so the live test DISCOVERS what it drives.
- **Sprinklers (`0x1e`)** — an irrigation circuit; the live test confirms/discovers
  it with a brief on/off.

Excluded: **High Speed (`0x1a`)** — redundant with the High pump preset (founder's
call). **Spa Mode (`0x1f`)** — a valve operation, higher stakes, grouped with future
spa/valve work, not here.

DISCOVERY FRAMING: Extra Aux and Sprinklers are built as toggles so the founder,
watching at the pad, can find out what they control. After the live test, each
button is RELABELED with its real function (a small follow-up edit to firmware +
dashboard yaml). Any circuit that turns out to be something the founder would
rather not expose (for example a cleaner booster pump or a gas fire feature) is
noted and the founder decides whether to keep its button.

## Design

### 1. The toggle path (C++)

New method `press_device_toggle(uint8_t keycode)` in
`components/jandy_aqualink/jandy_aqualink.cpp`:

- Gated EXACTLY like every write: refuse + log if the control interlock is OFF;
  refuse + log if iAqualink presence is OFF (same guards as `iaq_press` /
  `set_pump_rpm`).
- Allowlist guard: refuse + log any keycode not in `{0x19, 0x1d, 0x1e}` (new
  helper, e.g. `is_device_toggle_allowed(keycode)`).
- Runs a short, page-confirmed sequence reusing the navigate-to-DEVICES machinery
  already proven for the pump's value-set (`advance_set_sequence_` steps 1-3): go
  HOME, press Other Devices (`0x18`), and CONFIRM `current_page() == DEVICES (0x36)`
  before sending anything. If the page is not DEVICES, abort + log (send nothing).
- Terminal step: send the ONE allowlisted toggle keycode (a single press on the
  next poll), then arm HOME so temps and the Session 7 auto-refresh resume.
- Turning the interlock OFF mid-sequence aborts it (same hard-abort as the existing
  set sequence).

NO new value-set machinery (these are single presses, not value writes). Every
other DEVICES keycode (heaters `0x14`/`0x15`, VSP adjust `0x13`, Spa Mode `0x1f`,
pump, spa) stays unreachable through this path.

### 2. HA controls (yaml)

Three template buttons in `firmware/pool-bridge.yaml` and the live dashboard yaml,
each calling `press_device_toggle` with its keycode, mirroring the existing
equipment buttons (Pool Light, Cleaner, etc.):

- "Spa Light" -> `press_device_toggle(0x19)`
- "Extra Aux" -> `press_device_toggle(0x1d)` (relabel after discovery)
- "Sprinklers" -> `press_device_toggle(0x1e)` (relabel/confirm after discovery)

No new binary_sensor / state entity this session (YAGNI). The DEVICES page
re-enumeration already logs each circuit's on/off button state, which is enough to
confirm a command registered during the live test. A persistent HA state indicator
can be added later if the founder wants spa light state in the dashboard.

### 3. Safety model (unchanged)

- Every toggle is gated by the control interlock + presence. With the interlock OFF
  (resting state), pressing any of the three buttons logs REFUSED and sends nothing.
- The page guard prevents a DEVICES keycode from ever being sent while the panel is
  on another page (keycodes are page-scoped; the same byte means different equipment
  on HOME). This is the core safety property of the session.
- The allowlist confines this path to exactly the three intended circuits.

### 4. Testing (TDD)

- Python tests (repo root, `pytest`): the allowlist (`0x19`/`0x1d`/`0x1e` accepted;
  a sample of other DEVICES keycodes, e.g. heaters / Spa Mode / VSP, refused) and
  the page guard (toggle refused unless `page == DEVICES`). Mirror any new C++
  constant in the on-device selftest vectors.
- `pytest` green before any flash.

### 5. Live test (founder watching, DEFERRED to a later session)

- Confirm refusal: interlock OFF, press a toggle -> REFUSED in log, nothing actuates.
- Arm (interlock ON, presence ON). For each circuit: press the toggle, watch the
  panel's DEVICES button state flip in the log AND watch the physical equipment;
  note what energizes; press again to return to the original state.
  - Spa Light: distinguish three outcomes — state flips + light on (works); state
    flips + no light (command works, bulb/fixture dead); state does not flip (a
    real bug).
  - Sprinklers: brief on, confirm, off (do not run a watering cycle).
  - Extra Aux: discover what it drives; if it is something better left alone (a
    booster pump, a fire feature), note it and decide whether to keep the button.
- Disarm when done. Note any side effects (does toggling a circuit disturb the pump
  or temps).
- RELABEL each button (firmware + dashboard yaml) with its discovered function; drop
  any unwanted button. Commit.

## Sequencing (important)

BUILD, FLASH, and LIVE TEST are DEFERRED. The design, spec, and implementation plan
are written now; the device work happens in a later session AFTER the Session 7
schedule-watch has been read (~24h), because:

- Flashing reboots the device and restarts the 15-minute interval clock.
- The armed, equipment-toggling live test would inject changes into the very
  overnight history the schedule-watch is recording.

So this session ends at the written plan. The code itself may be written ahead, but
compile / flash / live test wait for a clean device window.

## Non-goals / out of scope

- High Speed (`0x1a`) and Spa Mode (`0x1f`): excluded (above).
- No persistent HA on/off state entity this session (YAGNI; revisit if wanted).
- Heaters (Session 9) and the schedule decision (Session 10) are later and unchanged.

## Watchpoints

- ESPHome object-id REST URLs are deprecated (removed in 2026.7.0); drive via HA
  service calls (`button.press`). Do not reintroduce object-id URLs.
- Keycode meaning is page-scoped: any DEVICES keycode MUST confirm
  `current_page() == 0x36` first. This is the core safety property of the session.
