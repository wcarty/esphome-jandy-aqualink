# 🛠️ DEVICES-Page Toggles Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** Execute INLINE (the deferred Task 5 drives live hardware).
> Steps use checkbox (`- [ ]`) syntax. **IMPORTANT:** Tasks 1-4 are code/desk work with
> no device contact. Task 5 (compile / flash / live test) is DEFERRED until AFTER the
> Session 7 schedule-watch has been read (~24h), because flashing reboots the device
> (restarting the 15-min interval) and the armed live test would contaminate the
> overnight schedule history.

**Goal:** Add three gated, page-guarded on/off toggles (Spa Light, Extra Aux, Sprinklers)
for the panel's DEVICES-page circuits, with Extra Aux + Sprinklers built so their function
is discovered in a watched live test, then relabeled.

**Architecture:** A new `press_device_toggle(keycode)` starts a short core-1 state machine
that mirrors the proven pump-set navigation (go HOME, press Other Devices, confirm the panel
is on the DEVICES page) and then sends ONE allowlisted toggle keycode and returns HOME. It is
gated by the control interlock + iAqualink presence + a `{0x19,0x1d,0x1e}` allowlist, and the
keycode is sent only while the decoder confirms `page == DEVICES (0x36)`, because the same byte
means other equipment on HOME.

**Tech Stack:** ESPHome external C++ component (ESP32, core-1 FreeRTOS task), Python reference +
pytest, ESPHome dashboard WebSocket build (`esphome_ws.ps1`), Home Assistant for control + test.

**Safety invariant:** with the interlock OFF, every toggle logs REFUSED and sends nothing. No
keycode outside `{0x19,0x1d,0x1e}` is reachable through this path, and none is sent unless the
panel is confirmed on the DEVICES page.

Repo: `C:\Users\Falcon\Documents\pool-controller\esp32-experiment` (`<repo>` below). Base `47b93ea`.

---

### Task 1: Allowlist helper (Python + C++ mirror) — TDD

**Files:**
- Create: `tests/test_device_toggle.py`
- Modify: `jandy/frames.py` (after the `KEY_IAQ_DEVICES_VSP_ADJ` block, ~line 251)
- Modify: `components/jandy_aqualink/jandy_proto.h` (after the `KEY_IAQ_DEVICES_VSP_ADJ` constant, ~line 46)

- [ ] **Step 1: Write the failing test** (`tests/test_device_toggle.py`):

```python
"""The DEVICES-page toggle allowlist is a safety gate: it passes only the three
intended circuit keycodes (Spa Light 0x19, Extra Aux 0x1d, Sprinklers 0x1e) and
refuses every other DEVICES keycode, including the heaters and Spa Mode."""

import unittest

from jandy.frames import is_device_toggle_allowed


class TestDeviceToggleAllowlist(unittest.TestCase):
    def test_intended_toggles_allowed(self):
        for key in (0x19, 0x1D, 0x1E):  # spa light, extra aux, sprinklers
            self.assertTrue(is_device_toggle_allowed(key), f"0x{key:02X} should be allowed")

    def test_other_devices_keycodes_refused(self):
        # VSP adjust 0x13, Pool Heat 0x14, Spa Heat 0x15, High Speed 0x1a, Spa Mode
        # 0x1f, plus nav/value bytes: none may toggle through this path.
        for key in (0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1A, 0x1F, 0x01, 0x06, 0x18, 0x30, 0x31):
            self.assertFalse(is_device_toggle_allowed(key), f"0x{key:02X} must be refused")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_device_toggle.py -q`
Expected: FAIL (`ImportError: cannot import name 'is_device_toggle_allowed'`).

- [ ] **Step 3: Implement the Python helper** (`jandy/frames.py`, after the `KEY_IAQ_DEVICES_VSP_ADJ = 0x13` block ~line 251):

```python
# DEVICES-page toggle keycodes (page-scoped; keycode = 0x11 + slot): slot 8 Spa
# Light, slot 12 Extra Aux, slot 13 Sprinklers. Allowlisted for the device-toggle
# path only, which never presses unless the panel is confirmed on DEVICES.
KEY_IAQ_DEVICES_SPA_LIGHT = 0x19
KEY_IAQ_DEVICES_EXTRA_AUX = 0x1D
KEY_IAQ_DEVICES_SPRINKLERS = 0x1E

_DEVICE_TOGGLE_KEYS = frozenset(
    {KEY_IAQ_DEVICES_SPA_LIGHT, KEY_IAQ_DEVICES_EXTRA_AUX, KEY_IAQ_DEVICES_SPRINKLERS}
)


def is_device_toggle_allowed(key: int) -> bool:
    """True only for the allowlisted DEVICES-page toggle keys (Spa Light, Extra
    Aux, Sprinklers). Page-scoped: the caller must also confirm page == DEVICES."""
    return key in _DEVICE_TOGGLE_KEYS
```

- [ ] **Step 4: Mirror the C++ helper** (`components/jandy_aqualink/jandy_proto.h`, after the `KEY_IAQ_DEVICES_VSP_ADJ` constant ~line 46):

```cpp
// DEVICES-page toggle keycodes (page-scoped; keycode = 0x11 + slot): slot 8 Spa
// Light, slot 12 Extra Aux, slot 13 Sprinklers. Allowlisted for press_device_toggle
// only, which never presses unless the panel is confirmed on the DEVICES page.
static constexpr uint8_t KEY_IAQ_DEVICES_SPA_LIGHT = 0x19,
                         KEY_IAQ_DEVICES_EXTRA_AUX = 0x1D,
                         KEY_IAQ_DEVICES_SPRINKLERS = 0x1E;

inline bool is_device_toggle_allowed(uint8_t key) {
  return key == KEY_IAQ_DEVICES_SPA_LIGHT || key == KEY_IAQ_DEVICES_EXTRA_AUX ||
         key == KEY_IAQ_DEVICES_SPRINKLERS;
}
```

- [ ] **Step 5: Run to verify it passes**

Run: `python -m pytest tests/test_device_toggle.py -q`
Expected: PASS (2 passed).

- [ ] **Step 6: Commit** (message before `--`; GitHub-noreply identity; Co-Authored-By trailer; no em dashes):

```
git -C <repo> add -- tests/test_device_toggle.py jandy/frames.py components/jandy_aqualink/jandy_proto.h
git -C <repo> commit -m "feat(session-8): DEVICES-page toggle allowlist" -- tests/test_device_toggle.py jandy/frames.py components/jandy_aqualink/jandy_proto.h
```

---

### Task 2: The gated toggle sequence (C++)

No unit test: this is a runtime core-1 state machine. The allowlist gate is covered by Task 1;
the behavior is verified by the live test in Task 5.

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (one public decl, one protected decl, two fields)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`press_device_toggle`, `advance_toggle_sequence_`, core-1 dispatch, interlock-off abort)

- [ ] **Step 1: Header public method decl** — after `set_pump_rpm(uint16_t rpm);` (~line 81):

```cpp
  // Toggle one allowlisted DEVICES-page circuit (Spa Light 0x19, Extra Aux 0x1d,
  // Sprinklers 0x1e). Gated by the master interlock + iAqualink presence + the
  // device-toggle allowlist. Runs a short page-confirmed sequence (nav to DEVICES,
  // confirm the page, send ONE keycode, return HOME); the keycode is sent only
  // while the panel is confirmed on the DEVICES page. One sequence at a time.
  void press_device_toggle(uint8_t keycode);
```

- [ ] **Step 2: Header protected method decl** — after `void advance_set_sequence_();` (~line 90):

```cpp
  void advance_toggle_sequence_();   // core-1: drive the device-toggle sequence on each poll
```

- [ ] **Step 3: Header fields** — after `volatile int iaq_set_rpm_{0};` (~line 149):

```cpp
  // DEVICES-page toggle sequence (multi-step, page-driven). 0 = idle, 1..5 = steps.
  // iaq_toggle_key_ is the allowlisted keycode to send on DEVICES. Mutually
  // exclusive with the pump-set sequence (press_device_toggle refuses if either is
  // active). Written by press_device_toggle (core 0) under mux_ and by the core-1
  // task as it advances.
  volatile int iaq_toggle_step_{0};
  volatile int iaq_toggle_key_{-1};
```

- [ ] **Step 4: cpp `press_device_toggle`** — add after `set_pump_rpm` (after its closing brace, ~line 310):

```cpp
void JandyAqualink::press_device_toggle(uint8_t keycode) {
  if (!interlock_) {
    ESP_LOGW(TAG, "device toggle REFUSED: safety interlock is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "device toggle REFUSED: iAqualink presence is OFF (key=0x%02X)", keycode);
    return;
  }
  if (!jandy::is_device_toggle_allowed(keycode)) {
    ESP_LOGW(TAG, "device toggle REFUSED: key 0x%02X not in the DEVICES-toggle allowlist", keycode);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "device toggle REFUSED: another sequence is in progress");
    return;
  }
  iaq_toggle_key_ = keycode;
  iaq_toggle_step_ = 1;  // kick off the sequence on the next poll
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "device toggle: start sequence -> key 0x%02X", keycode);
}
```

- [ ] **Step 5: cpp `advance_toggle_sequence_`** — add after `advance_set_sequence_`'s closing brace (~line 405):

```cpp
// core-1: advance one step of the gated device-toggle sequence. Called from the
// iAq branch of task_loop on each poll (cmd 0x30) while iaq_toggle_step_ != 0.
// Mirrors the pump-set nav (HOME, Other Devices, confirm DEVICES) but the terminal
// action is a single toggle press, not a value-set. SAFETY: the toggle keycode is
// sent ONLY when the decoder confirms page == DEVICES (0x36); the same byte is other
// equipment on HOME. Turning the interlock off mid-sequence aborts.
void JandyAqualink::advance_toggle_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "device toggle aborted at step %d: interlock OFF", iaq_toggle_step_);
    iaq_toggle_step_ = 0;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_toggle_step_) {
    case 1:  // go HOME first (deterministic starting point)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_toggle_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);
        iaq_toggle_step_ = 3;
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    case 3:  // on DEVICES -> press the toggle. SAFETY: only on DEVICES (0x36)
      if (page == jandy::IAQ_PAGE_DEVICES) {
        send_iaq_ack_(static_cast<uint8_t>(iaq_toggle_key_));
        ESP_LOGW(TAG, "device toggle: pressed 0x%02X on DEVICES", iaq_toggle_key_);
        iaq_toggle_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4:  // pressed -> return HOME so temps + the Session 7 auto-refresh resume
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_toggle_step_ = 5;
      break;
    case 5:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGW(TAG, "device toggle sequence complete (key 0x%02X)", iaq_toggle_key_);
        iaq_toggle_step_ = 0;
        iaq_toggle_key_ = -1;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_toggle_step_ = 0;
      send_iaq_ack_(0x00);
      break;
  }
}
```

- [ ] **Step 6: cpp dispatch** — in `task_loop`'s iAq branch, immediately AFTER the existing pump-set block (the `if (set_step != 0) { ... continue; }` that ends ~line 126) and BEFORE the normal presence handling, insert:

```cpp
        // The device-toggle sequence owns the iAq reply while active (mutually
        // exclusive with the set sequence; press_device_toggle refuses if either
        // is running). Mirror the set-sequence handling, with no 0x24 value step.
        int toggle_step;
        portENTER_CRITICAL(&mux_);
        toggle_step = iaq_toggle_step_;
        portEXIT_CRITICAL(&mux_);
        if (toggle_step != 0) {
          if (f.cmd() == 0x30) {
            advance_toggle_sequence_();
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the toggle sequence
        }
```

- [ ] **Step 7: cpp interlock-off abort** — in `set_interlock`, in the `if (!on) { ... }` block alongside `iaq_set_step_ = 0;` (~line 195), add:

```cpp
    iaq_toggle_step_ = 0;     // also abort any in-progress device-toggle sequence
```

- [ ] **Step 8: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
git -C <repo> commit -m "feat(session-8): gated DEVICES-page toggle sequence" -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
```

---

### Task 3: HA buttons (firmware yaml)

**Files:**
- Modify: `firmware/pool-bridge.yaml` (3 template buttons)

- [ ] **Step 1: Add three buttons** after the "Air Blower" button (~line 182), alongside the other equipment buttons. Panel labels for now; relabel after discovery:

```yaml
  # DEVICES-page toggles (gated by interlock + presence + the device-toggle
  # allowlist; each confirms the panel is on DEVICES before pressing). Extra Aux
  # and Sprinklers are labeled per the panel; relabel after the live test reveals
  # what they actually drive.
  - platform: template
    name: "Spa Light"
    icon: "mdi:lightbulb-night"
    on_press:
      - lambda: "id(jandy_comp).press_device_toggle(0x19);"
  - platform: template
    name: "Extra Aux"
    icon: "mdi:toggle-switch"
    on_press:
      - lambda: "id(jandy_comp).press_device_toggle(0x1D);"
  - platform: template
    name: "Sprinklers"
    icon: "mdi:sprinkler"
    on_press:
      - lambda: "id(jandy_comp).press_device_toggle(0x1E);"
```

- [ ] **Step 2: Commit:**

```
git -C <repo> add -- firmware/pool-bridge.yaml
git -C <repo> commit -m "feat(session-8): Spa Light / Extra Aux / Sprinklers buttons" -- firmware/pool-bridge.yaml
```

---

### Task 4: Regression gate (pytest)

- [ ] **Step 1: Run the full suite** from the repo root:

Run: `python -m pytest -q` (cwd = repo)
Expected: all green (the prior 82 + 2 new device-toggle tests = 84 passed). A failure means
something unrelated broke; STOP and investigate before any flash.

---

### Task 5: DEFERRED deployment + discovery live test (TOMORROW)

DO NOT start until the Session 7 schedule-watch history has been read AND there is a clean device
window. Flashing reboots the device (restarting the 15-min interval); the armed live test would
inject changes into the overnight history.

- [ ] **Step 1: Push** so the dashboard compile pulls the new component:
`git -C <repo> push origin master`, then confirm `## master...origin/master` with no `[ahead]`.

- [ ] **Step 2: Patch the LIVE dashboard yaml** with the 3 buttons. GET the live config with
`(New-Object System.Net.WebClient).DownloadString($uri)` (NOT `Invoke-WebRequest`, whose byte-array
`.Content` stringifies to space-separated decimals). Back it up to
`dashboard-pool-bridge.BACKUP-<stamp>.yaml`. Insert the same 3-button block. POST with
`.UploadString($uri,"POST",$body)`. GET again and readback-verify byte-exact.

- [ ] **Step 3: Compile + flash** (from `C:\Users\Falcon\Documents\pool-controller`):
`.\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 580` (expect
`Successfully compiled program.` + `EXIT CODE 0`), then
`.\esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -TimeoutSec 580`
(expect `OTA successful` + `EXIT CODE 0`).

- [ ] **Step 4: Post-flash health** (background logs capture + shared-read): confirm
`selftest PASS -> 13/13` and `checksum_errors=0` over a >15s window. Confirm `Pool Pump Auto-Refresh`
restored ON (the Session 7 watch resumes) and the interlock OFF. Do NOT actuate on FAIL.

- [ ] **Step 5: Refusal check** (interlock OFF): press Spa Light via HA
(`button.pool_rs485_bridge_spa_light`) -> expect `device toggle REFUSED: safety interlock is OFF`,
nothing actuates.

- [ ] **Step 6: Armed discovery test** (founder at the pad). Arm: `switch.turn_on`
`switch.pool_rs485_bridge_pool_keypad_keypress_armed` (presence already ON). With a background log
capture running, for each of Spa Light, Extra Aux, Sprinklers:
  - Press the button via HA (`button.press`).
  - Confirm the log shows `device toggle: pressed 0xNN on DEVICES` then
    `device toggle sequence complete`, and the DEVICES re-enumeration shows that circuit's button
    state flip (`IAQ B<idx> s<state>`).
  - Note what physically energizes. Sprinklers: brief on, confirm, press again to turn off.
    Spa Light: note whether the light actually comes on (state flip + no light = dead bulb, not a
    bug; no state flip = a real bug).
  - Press again to return each circuit to its original state.
  Disarm (`switch.turn_off` the interlock) when done. Note any side effects (pump/temps disturbed).

- [ ] **Step 7: Relabel** each button (`firmware/pool-bridge.yaml` + the live dashboard yaml) with
its discovered function; drop any unwanted button; recompile/upload if firmware labels changed.
Commit.

- [ ] **Step 8: Update memory + ROADMAP** (`project_pool_controller_phase2.md`: shipped SHA, what
Extra Aux + Sprinklers turned out to be, side effects; ROADMAP: Session 8 shipped).

---

## Self-review notes

- **Spec coverage:** allowlist (Task 1), page guard (Task 2 step 5 case 3, `page == IAQ_PAGE_DEVICES`),
  gated sequence + interlock-off abort (Task 2), 3 buttons (Task 3), pytest (Task 4), deferred flash +
  discovery live test + relabel (Task 5). Every spec section maps to a task.
- **No new binary_sensor** (YAGNI, per spec); the live test reads the DEVICES button-state log line.
- **Type/name consistency:** `is_device_toggle_allowed`, `press_device_toggle`, `advance_toggle_sequence_`,
  `iaq_toggle_step_`, `iaq_toggle_key_`, and `KEY_IAQ_DEVICES_SPA_LIGHT`/`_EXTRA_AUX`/`_SPRINKLERS` are
  used identically across Python, C++, and the dispatch.
- **Page guard** uses `page == jandy::IAQ_PAGE_DEVICES (0x36)`, the same gate proven for the pump's
  VSP-adjust (`vsp_adjust_allowed`).
