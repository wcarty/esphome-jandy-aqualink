# 🛠️ Heater On/Off Enables + SET_TEMP Survey Implementation Plan (Phase 1 of heaters)

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: use superpowers:executing-plans (inline)
> or superpowers:subagent-driven-development. Steps use checkbox (`- [ ]`) syntax.
> **IMPORTANT:** Tasks 1-4 are desk work (no device contact). Task 5 (deploy +
> founder-watched live test + SET_TEMP survey) drives LIVE heater hardware and MUST
> have the founder watching at the pad/spa. Its captured findings feed Phase 2
> (the setpoint + heat-state sensors), which is written afterward.

**Goal:** Add gated Pool Heat and Spa Heat on/off enables (the panel maintains the
target once enabled), then live-test them and survey the unmapped temperature screen.

**Architecture:** A new `press_heater(keycode)` starts a short core-1 sequence that
ensures the panel is on HOME, re-checks the heater safety gate, sends ONE HOME-page
heater keycode (Pool Heat 0x13 / Spa Heat 0x14), and goes idle. Gated by the master
interlock + iAqualink presence + a HOME-page-only guard + (for Spa Heat) a spa-mode
guard. Mutually exclusive with the pump-set and device-toggle sequences.

**Tech Stack:** ESPHome external C++ component (ESP32, core-1 FreeRTOS task), Python
reference + pytest, ESPHome dashboard WebSocket build (`esphome_ws.ps1`), Home Assistant.

**Safety invariant:** with the interlock OFF, every heater press logs REFUSED and
sends nothing. A heater keycode is transmitted ONLY while the decoder confirms
`page == HOME (0x01)`, and Spa Heat (0x14) ONLY while `water_mode == spa (3)`. No key
outside `{0x13, 0x14}` is reachable through this path.

Repo: `C:\Users\Falcon\Documents\pool-controller\esp32-experiment` (`<repo>`). Base `b2d46ca`.

---

### Task 1: Heater on/off safety-gate helpers (Python + C++ mirror) — TDD

**Files:**
- Create: `tests/test_heater.py`
- Modify: `jandy/frames.py` (append after the device-toggle section, end of file)
- Modify: `components/jandy_aqualink/jandy_proto.h` (after the `is_device_toggle_allowed` block)

- [ ] **Step 1: Write the failing test** (`tests/test_heater.py`):

```python
"""Heater on/off is the highest-stakes control. is_heater_key allowlists ONLY the
two HOME-page heater keycodes; heater_enable_allowed is the full gate: a heater key
is honored only on the HOME page (0x13 is the VSP-adjust on DEVICES and 0x14 is Pool
Heat on DEVICES), and Spa Heat (0x14) only while the panel is in spa mode (water_mode
3), because spa heat must never be enabled outside spa mode."""

import unittest

from jandy.frames import (
    is_heater_key,
    heater_enable_allowed,
    KEY_IAQ_HOME_POOL_HEAT,
    KEY_IAQ_HOME_SPA_HEAT,
    IAQ_PAGE_HOME,
    IAQ_PAGE_DEVICES,
)

WATER_MODE_POOL = 2
WATER_MODE_SPA = 3


class TestIsHeaterKey(unittest.TestCase):
    def test_only_the_two_home_heater_keys(self):
        self.assertTrue(is_heater_key(KEY_IAQ_HOME_POOL_HEAT))  # 0x13
        self.assertTrue(is_heater_key(KEY_IAQ_HOME_SPA_HEAT))   # 0x14

    def test_rejects_everything_else(self):
        for k in (0x11, 0x12, 0x15, 0x16, 0x17, 0x18, 0x19, 0x1D, 0x1E, 0x01, 0x06):
            self.assertFalse(is_heater_key(k), f"0x{k:02X} must not be a heater key")


class TestHeaterEnableAllowed(unittest.TestCase):
    def test_pool_heat_allowed_on_home_any_mode(self):
        self.assertTrue(heater_enable_allowed(KEY_IAQ_HOME_POOL_HEAT, IAQ_PAGE_HOME, WATER_MODE_POOL))
        self.assertTrue(heater_enable_allowed(KEY_IAQ_HOME_POOL_HEAT, IAQ_PAGE_HOME, WATER_MODE_SPA))

    def test_spa_heat_allowed_on_home_only_in_spa_mode(self):
        self.assertTrue(heater_enable_allowed(KEY_IAQ_HOME_SPA_HEAT, IAQ_PAGE_HOME, WATER_MODE_SPA))
        self.assertFalse(heater_enable_allowed(KEY_IAQ_HOME_SPA_HEAT, IAQ_PAGE_HOME, WATER_MODE_POOL))

    def test_no_heater_off_home(self):
        # 0x13 on DEVICES = VSP-adjust, 0x14 on DEVICES = Pool Heat: never honor off HOME.
        self.assertFalse(heater_enable_allowed(KEY_IAQ_HOME_POOL_HEAT, IAQ_PAGE_DEVICES, WATER_MODE_POOL))
        self.assertFalse(heater_enable_allowed(KEY_IAQ_HOME_SPA_HEAT, IAQ_PAGE_DEVICES, WATER_MODE_SPA))

    def test_non_heater_key_refused_even_on_home(self):
        self.assertFalse(heater_enable_allowed(0x11, IAQ_PAGE_HOME, WATER_MODE_SPA))
        self.assertFalse(heater_enable_allowed(0x1D, IAQ_PAGE_HOME, WATER_MODE_SPA))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_heater.py -q`
Expected: FAIL (`ImportError: cannot import name 'is_heater_key'`).

- [ ] **Step 3: Implement the Python helpers** (append to the end of `jandy/frames.py`):

```python
# --- Heater on/off (HOME page) ----------------------------------------------
#
# Heaters are the highest-stakes control. HOME-page keycodes: Pool Heat 0x13,
# Spa Heat 0x14 (keycode 0x11 + home index 2/3). PAGE-SCOPED: 0x13 is the
# VSP-adjust on DEVICES and 0x14 is Pool Heat on DEVICES, so a heater key must
# ONLY ever be sent on HOME. Spa Heat additionally must never be enabled outside
# spa mode (water_mode 3). The panel runs the thermostat once a heater is enabled.
KEY_IAQ_HOME_POOL_HEAT = 0x13
KEY_IAQ_HOME_SPA_HEAT = 0x14
WATER_MODE_SPA = 3

_HEATER_KEYS = frozenset({KEY_IAQ_HOME_POOL_HEAT, KEY_IAQ_HOME_SPA_HEAT})


def is_heater_key(key: int) -> bool:
    """True only for the two HOME-page heater keycodes (Pool Heat, Spa Heat)."""
    return key in _HEATER_KEYS


def heater_enable_allowed(key: int, current_page: int, water_mode: int) -> bool:
    """The full heater on/off safety gate. A heater key is honored ONLY on the HOME
    page (0x13/0x14 are other equipment elsewhere), and Spa Heat (0x14) ONLY while
    the panel is in spa mode (water_mode 3). Pool Heat is allowed on HOME in any mode."""
    if not is_heater_key(key):
        return False
    if current_page != IAQ_PAGE_HOME:
        return False
    if key == KEY_IAQ_HOME_SPA_HEAT and water_mode != WATER_MODE_SPA:
        return False
    return True
```

- [ ] **Step 4: Mirror the C++ helpers** (`components/jandy_aqualink/jandy_proto.h`, right after the `is_device_toggle_allowed` block):

```cpp
// Heater on/off (HOME page). Pool Heat 0x13, Spa Heat 0x14 (page-scoped: 0x13 is
// the VSP-adjust on DEVICES, 0x14 is Pool Heat on DEVICES), so a heater key is only
// ever sent on HOME. Spa Heat must never be enabled outside spa mode (water_mode 3).
// is_heater_key is the allowlist; heater_enable_allowed is the full gate.
static constexpr uint8_t KEY_IAQ_HOME_POOL_HEAT = 0x13, KEY_IAQ_HOME_SPA_HEAT = 0x14;
static constexpr int WATER_MODE_SPA = 3;

inline bool is_heater_key(uint8_t key) {
  return key == KEY_IAQ_HOME_POOL_HEAT || key == KEY_IAQ_HOME_SPA_HEAT;
}

inline bool heater_enable_allowed(uint8_t key, int current_page, int water_mode) {
  if (!is_heater_key(key)) return false;
  if (current_page != IAQ_PAGE_HOME) return false;
  if (key == KEY_IAQ_HOME_SPA_HEAT && water_mode != WATER_MODE_SPA) return false;
  return true;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `python -m pytest tests/test_heater.py -q`
Expected: PASS (3 passed).

- [ ] **Step 6: Run the full suite** (no regressions): `python -m pytest -q` (expect all green).

- [ ] **Step 7: Commit** (`-m` BEFORE `--`; GitHub-noreply; Co-Authored-By; no em dashes):

```
git -C <repo> add -- tests/test_heater.py jandy/frames.py components/jandy_aqualink/jandy_proto.h
git -C <repo> commit -m "feat(session-9): heater on/off safety-gate helpers" -- tests/test_heater.py jandy/frames.py components/jandy_aqualink/jandy_proto.h
```

---

### Task 2: Gated heater on/off sequence (C++)

No unit test: this is a runtime core-1 state machine. The gate is covered by Task 1's
`heater_enable_allowed` tests; behavior is verified by the Task 5 live test.

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (one public decl, one protected decl, two fields)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`press_heater`, `advance_heater_sequence_`, task_loop dispatch, interlock-off abort, and extend the busy-guard in `set_pump_rpm` + `press_device_toggle`)

- [ ] **Step 1: Header public method decl** — after `void press_device_toggle(uint8_t keycode);`:

```cpp
  // Enable/disable a heater (Pool Heat 0x13 / Spa Heat 0x14) from HA. Gated by the
  // master interlock + iAqualink presence + a HOME-page-only guard; Spa Heat also
  // refuses unless the panel is in spa mode. Runs a short sequence (ensure HOME,
  // re-check the gate, send ONE keycode on HOME). Toggles the panel's heat enable;
  // the panel then runs the thermostat to the setpoint. One write-sequence at a time.
  void press_heater(uint8_t keycode);
```

- [ ] **Step 2: Header protected method decl** — after `void advance_toggle_sequence_();`:

```cpp
  void advance_heater_sequence_();   // core-1: drive the heater on/off sequence on each poll
```

- [ ] **Step 3: Header fields** — after `volatile int iaq_toggle_key_{-1};`:

```cpp
  // Heater on/off sequence (multi-step, HOME-page). 0 = idle, 1..2 = steps.
  // iaq_heater_key_ is the allowlisted heater keycode to send on HOME. Mutually
  // exclusive with the pump-set and device-toggle sequences. Written by press_heater
  // (core 0) under mux_ and by the core-1 task as it advances.
  volatile int iaq_heater_step_{0};
  volatile int iaq_heater_key_{-1};
```

- [ ] **Step 4: cpp `press_heater`** — add after `press_device_toggle`'s closing brace:

```cpp
void JandyAqualink::press_heater(uint8_t keycode) {
  if (!interlock_) {
    ESP_LOGW(TAG, "heater REFUSED: safety interlock is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "heater REFUSED: iAqualink presence is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!jandy::is_heater_key(keycode)) {
    ESP_LOGW(TAG, "heater REFUSED: key 0x%02X is not a heater key", keycode);
    return;
  }
  int wm;
  portENTER_CRITICAL(&mux_);
  wm = iaq_water_mode_;
  portEXIT_CRITICAL(&mux_);
  // Spa Heat must never be enabled outside spa mode (Pool Heat is allowed any mode).
  if (keycode == jandy::KEY_IAQ_HOME_SPA_HEAT && wm != jandy::WATER_MODE_SPA) {
    ESP_LOGW(TAG, "heater REFUSED: Spa Heat needs spa mode (water_mode=%d)", wm);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "heater REFUSED: another sequence is in progress (key=0x%02X)", keycode);
    return;
  }
  iaq_heater_key_ = keycode;
  iaq_heater_step_ = 1;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "heater: start sequence -> key 0x%02X", keycode);
}
```

- [ ] **Step 5: cpp `advance_heater_sequence_`** — add after `advance_toggle_sequence_`'s closing brace:

```cpp
// core-1: advance one step of the gated heater on/off sequence. Called from the iAq
// branch of task_loop on each poll (cmd 0x30) while iaq_heater_step_ != 0. SAFETY: the
// heater keycode is sent ONLY when the decoder confirms page == HOME (0x01) AND the
// full gate (heater_enable_allowed: HOME + spa-mode for Spa Heat) re-passes at the
// transmit point; 0x13/0x14 are other equipment on other pages. Interlock off aborts.
void JandyAqualink::advance_heater_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "heater aborted at step %d: interlock OFF", iaq_heater_step_);
    iaq_heater_step_ = 0;
    iaq_heater_key_ = -1;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  int wm = iaq_reader_.water_mode();
  switch (iaq_heater_step_) {
    case 1:  // ensure HOME, then send the heater key on HOME (re-checking the gate)
      if (page == jandy::IAQ_PAGE_HOME) {
        if (jandy::heater_enable_allowed(static_cast<uint8_t>(iaq_heater_key_), page, wm)) {
          send_iaq_ack_(static_cast<uint8_t>(iaq_heater_key_));
          ESP_LOGW(TAG, "heater: pressed 0x%02X on HOME", iaq_heater_key_);
          iaq_heater_step_ = 2;
        } else {
          ESP_LOGW(TAG, "heater aborted: gate failed at transmit (page=0x%02X wm=%d)", page, wm);
          iaq_heater_step_ = 0;
          iaq_heater_key_ = -1;
          send_iaq_ack_(0x00);
        }
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);  // not on HOME yet, navigate there
      }
      break;
    case 2:  // pressed -> done (we stay on HOME, temps keep reading)
      ESP_LOGW(TAG, "heater sequence complete (key 0x%02X)", iaq_heater_key_);
      iaq_heater_step_ = 0;
      iaq_heater_key_ = -1;
      send_iaq_ack_(0x00);
      break;
    default:
      iaq_heater_step_ = 0;
      iaq_heater_key_ = -1;
      send_iaq_ack_(0x00);
      break;
  }
}
```

- [ ] **Step 6: cpp dispatch** — in `task_loop`'s iAq branch, immediately AFTER the device-toggle dispatch block (the `if (toggle_step != 0) { ... continue; }`) and BEFORE the `// iAqualink: the panel sends...` comment, insert:

```cpp
        // The heater sequence owns the iAq reply while active (mutually exclusive
        // with the set + toggle sequences). Same shape; the key goes out on HOME.
        int heater_step;
        portENTER_CRITICAL(&mux_);
        heater_step = iaq_heater_step_;
        portEXIT_CRITICAL(&mux_);
        if (heater_step != 0) {
          if (f.cmd() == 0x30) {
            advance_heater_sequence_();
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          iaq_water_mode_ = iaq_reader_.water_mode();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the heater sequence
        }
```

- [ ] **Step 7: cpp interlock-off abort** — in `set_interlock`, in the `if (!on) { ... }` block, after the `iaq_toggle_key_ = -1;` line, add:

```cpp
    iaq_heater_step_ = 0;     // and any in-progress heater sequence
    iaq_heater_key_ = -1;
```

- [ ] **Step 8: cpp extend the mutual-exclusion busy-guards.** In `set_pump_rpm`, change the busy check from `if (iaq_toggle_step_ != 0) {` to `if (iaq_toggle_step_ != 0 || iaq_heater_step_ != 0) {`. In `press_device_toggle`, change `if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0) {` to `if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0) {`. This keeps "one write-sequence at a time" true from every entry point.

- [ ] **Step 9: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
git -C <repo> commit -m "feat(session-9): gated heater on/off sequence (HOME-page + spa-mode guard)" -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
```

---

### Task 3: HA buttons (firmware yaml)

**Files:**
- Modify: `firmware/pool-bridge.yaml` (2 template buttons)

- [ ] **Step 1: Add two buttons** after the Sprinklers button block (the last DEVICES toggle):

```yaml
  # Heaters (HIGHEST-STAKES; gated by interlock + presence + a HOME-page-only guard;
  # Spa Heat additionally refuses unless the panel is in spa mode). These are on/off
  # enables; once enabled the panel maintains the target. Temperature setpoints are a
  # follow-up build (Phase 2). The heater keycodes were excluded from every allowlist
  # until now.
  - platform: template
    name: "Pool Heat"
    icon: "mdi:radiator"
    on_press:
      - lambda: "id(jandy_comp).press_heater(0x13);"   # HOME slot 2 = Pool Heat
  - platform: template
    name: "Spa Heat"
    icon: "mdi:hot-tub"
    on_press:
      - lambda: "id(jandy_comp).press_heater(0x14);"   # HOME slot 3 = Spa Heat (spa mode only)
```

- [ ] **Step 2: Commit:**

```
git -C <repo> add -- firmware/pool-bridge.yaml
git -C <repo> commit -m "feat(session-9): Pool Heat / Spa Heat on/off buttons" -- firmware/pool-bridge.yaml
```

---

### Task 4: Regression gate (pytest)

- [ ] **Step 1: Run the full suite** from the repo root:

Run: `python -m pytest -q` (cwd = `<repo>`)
Expected: all green (the prior 89 + 3 new heater-gate tests = 92 passed). A failure means
something unrelated broke; STOP and investigate before any flash.

---

### Task 5: Deploy + founder-watched on/off live test + SET_TEMP survey (LIVE, founder at the pad/spa)

DO NOT start without the founder watching the equipment. This actuates real heaters.

- [ ] **Step 1: Push** so the dashboard compile pulls the new component:
`git -C <repo> push origin master`, then confirm `## master...origin/master` with no `[ahead]`.

- [ ] **Step 2: Patch the LIVE dashboard yaml** with the 2 heater buttons. GET the live config with
`(New-Object System.Net.WebClient).DownloadString($uri)` where `$uri = "http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml"`. Back it up to
`dashboard-pool-bridge.BACKUP-<stamp>.yaml`. Insert the same 2-button block after the Sprinklers
button. POST with `.UploadString($uri,"POST",$body)`. GET again and readback-verify byte-exact.

- [ ] **Step 3: Compile + flash** (from `C:\Users\Falcon\Documents\pool-controller`):
`.\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 560` (expect
`Successfully compiled program.` + `EXIT CODE 0`), then
`.\esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -TimeoutSec 240`
(expect `OTA successful` + `EXIT CODE 0`).

- [ ] **Step 4: Post-flash health** (background flushing log capture via `C:\Users\Falcon\poollog.ps1`,
shared-read open): confirm `selftest PASS` and `checksum_errors=0` over a >15s window. Confirm
`iAqualink Presence` ON (temps reading) and the interlock OFF. Do NOT actuate on a FAIL.

- [ ] **Step 5: Refusal checks** (interlock OFF): press Pool Heat via HA
(`button.pool_rs485_bridge_pool_heat`) -> expect `heater REFUSED: safety interlock is OFF`,
nothing actuates. With the panel in POOL mode and the interlock ON, press Spa Heat -> expect
`heater REFUSED: Spa Heat needs spa mode (water_mode=2)`, nothing actuates. (This proves both gates.)

- [ ] **Step 6: Pool Heat on/off + flow-interlock check** (founder watching). Arm the interlock. Press
Pool Heat -> confirm `heater: pressed 0x13 on HOME` + `heater sequence complete`. Confirm at the
equipment that the heater fires AND that it ONLY fires with the filter pump running (the panel's flow
interlock). If the pump is off, confirm the heater does NOT fire. Press Pool Heat again to disable.
**Capture for Phase 2:** with Pool Heat enabled, log the HOME-page heater button state line
(`IAQ B2 s<state> ... Pool Heat`) so Phase 2 knows the "enabled" state value for the heat-state sensor.

- [ ] **Step 7: Spa Heat on/off** (founder watching). Switch the panel to spa mode (Spa toggle), confirm
`water_mode == spa`. Press Spa Heat -> confirm it is accepted (`pressed 0x14 on HOME`) and the spa
heater fires. **Capture:** the `IAQ B3 s<state> ... Spa Heat` enabled-state line. Press Spa Heat to
disable.

- [ ] **Step 8: SET_TEMP survey** (founder watching, a heater enabled). With a heater on (the founder's
constraint), cycle `iAqualink Presence` OFF then ON to force a fresh full enumeration, then from HOME
attempt to open the heat setpoint: try the survey nav buttons and any heat-item press, watching the
compact decoder log for `IAQ PAGE ...(0x39)` (SET_TEMP). Record: the exact key + page that opens the
POOL setpoint, the same for the SPA setpoint, and capture at least one raw SET_TEMP value frame for
each (the digit/wire format) for Phase 2's TDD. Reference: AqualinkD `iaqtouch_aq_programmer.c`.

- [ ] **Step 9: Spa-mode coupling check.** Switch spa -> pool while Spa Heat is enabled and observe
whether the panel drops Spa Heat on its own (watch the HOME Spa Heat button state). Record the result:
it decides whether Phase 2 needs the HA spa auto-off automation or can rely on the panel.

- [ ] **Step 10: Restore + disarm.** Turn both heaters OFF, return the panel to its normal mode, disarm
the interlock, confirm the resting state (presence ON, interlock OFF). Note any side effects.

- [ ] **Step 11: Record findings + write Phase 2.** Update memory + ROADMAP with: shipped SHA, the
enabled-state values, the SET_TEMP nav path + captured frames, and the spa-coupling result. Then write
the Phase 2 plan (heat-state sensors + temperature setpoint + clamps 45-90 / 80-104 + the spa auto-off
decision) from these real captures.

---

## Self-review notes

- **Spec coverage:** on/off enables with the HOME-page guard + spa-mode guard (Tasks 1-2), the
  mutual-exclusion invariant extended to the heater sequence (Task 2 step 8), 2 HA buttons (Task 3),
  pytest gate (Task 4), and the deferred live test that ALSO runs the SET_TEMP survey + captures the
  heat-state bytes + the spa-coupling result (Task 5). The setpoint, the heat-state sensors, the
  clamps, and the spa auto-off are Phase 2, deliberately written from Task 5's captures (the spec's
  build order: on/off first, then survey, then setpoint).
- **No guessed bytes:** every TDD task here tests pure gate logic (allowlist + page + mode), whose
  expected values are known. The survey-dependent pieces (value frame format, enabled-state value) are
  captured in Task 5, not guessed.
- **Type/name consistency:** `is_heater_key`, `heater_enable_allowed`, `press_heater`,
  `advance_heater_sequence_`, `iaq_heater_step_`, `iaq_heater_key_`, `KEY_IAQ_HOME_POOL_HEAT`/
  `_SPA_HEAT`, and `WATER_MODE_SPA` are used identically across Python, C++, and the dispatch.
- **Page guard** uses `page == jandy::IAQ_PAGE_HOME (0x01)`, re-checked at the transmit point, the same
  shape proven for the pump (`vsp_adjust_allowed`) and the toggles.
