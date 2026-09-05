# 🛠️ Pool HA Brain Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make Home Assistant the autonomous pump/cleaner/filtration scheduler for the pool, driving the ESP bridge box's shipped controls, with the panel's stored schedule kept as a hardware failsafe.

**Architecture:** One small firmware change adds a narrow "Pool Scheduler" permission so HA can set pump speed and toggle filter pump and cleaner unattended (everything else still needs the master interlock that boots OFF). Then a set of HA helpers, scripts, and automations run a four-phase daily schedule, watch the pump every 2 minutes and correct any panel drift, stand down during spa mode, and resume after restarts. The pump is never commanded off, so the panel's own schedule underneath is always a working safety net.

**Tech Stack:** ESPHome external component (C++), Python + pytest for frame-logic TDD, on-device selftest, Home Assistant helpers/scripts/automations built via the `mcp__Home_Assistant__*` config tools. Deploy via `esphome_ws.ps1` against the dashboard at 192.168.1.126:6052; device 192.168.4.51.

Spec: `docs/superpowers/specs/2026-06-03-pool-ha-brain-design.md`. Repo base HEAD `5f78d13`.

---

## File structure

Firmware (Phase A, in this repo):
- `jandy/frames.py` — add `is_scheduler_safe_key` (Python source of truth for the scoped key set).
- `tests/test_frames.py` — add its test.
- `components/jandy_aqualink/jandy_proto.h` — mirror `is_scheduler_safe_key` in C++.
- `components/jandy_aqualink/jandy_proto.cpp` — add a selftest assertion for it.
- `components/jandy_aqualink/jandy_aqualink.h` — add `scheduler_armed_` member + setter.
- `components/jandy_aqualink/jandy_aqualink.cpp` — add `set_scheduler_armed`, change two gates.
- `firmware/pool-bridge.yaml` — add the "Pool Scheduler" template switch.

Home Assistant (Phases B to D, created via MCP, not in this repo):
- Helpers: `input_number.pool_target_rpm`, `input_boolean.pool_cleaner_should_run`, `input_datetime.pool_manual_hold_until`, `input_select.pool_phase`, `input_button.pool_swim_boost`, `input_number.pool_manual_speed_request`.
- Scripts: `script.pool_apply_pump`, `script.pool_apply_cleaner`, `script.pool_evaluate_phase`.
- Automations: phase scheduler, watch-and-correct, restore-after-spa, swim boost, manual speed, presence keeper, startup resync, (optional) correction-rate watchdog.

---

## PHASE A: Firmware permission (the Pool Scheduler switch)

### Task A1: Add `is_scheduler_safe_key` to Python frames (TDD)

**Files:**
- Modify: `jandy/frames.py` (after `is_allowed_iaq_key`, around line 162)
- Test: `tests/test_frames.py`

- [ ] **Step 1: Write the failing test**

In `tests/test_frames.py` add:

```python
def test_is_scheduler_safe_key_allows_only_pump_and_cleaner():
    from jandy.frames import is_scheduler_safe_key
    assert is_scheduler_safe_key(0x11) is True   # Filter Pump
    assert is_scheduler_safe_key(0x15) is True   # Cleaner
    # everything else the scheduler must NOT be able to send unattended
    for key in (0x12, 0x16, 0x17, 0x13, 0x14, 0x00):
        assert is_scheduler_safe_key(key) is False
```

- [ ] **Step 2: Run it, verify it fails**

Run: `python -m pytest tests/test_frames.py::test_is_scheduler_safe_key_allows_only_pump_and_cleaner -v`
Expected: FAIL with `ImportError` / `cannot import name 'is_scheduler_safe_key'`.

- [ ] **Step 3: Implement**

In `jandy/frames.py`, immediately after the `is_allowed_iaq_key` function:

```python
_SCHEDULER_SAFE_KEYS = frozenset({KEY_IAQ_FILTER_PUMP, KEY_IAQ_CLEANER})


def is_scheduler_safe_key(key: int) -> bool:
    """Keys the autonomous scheduler may send WITHOUT the master interlock: only
    Filter Pump (0x11) and Cleaner (0x15). These share the iaq_press allowlist with
    the spa toggle / blower / light, so the scheduler permission is scoped per-key,
    NOT by un-gating iaq_press wholesale."""
    return key in _SCHEDULER_SAFE_KEYS
```

- [ ] **Step 4: Run it, verify it passes**

Run: `python -m pytest tests/test_frames.py -v`
Expected: PASS (all frames tests green).

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_frames.py
git commit -m "feat(brain): is_scheduler_safe_key allows only pump + cleaner (0x11/0x15)"
```

### Task A2: Mirror in C++ and add a selftest assertion

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h` (after `is_allowed_iaq_key`, around line 150)
- Modify: `components/jandy_aqualink/jandy_proto.cpp` (in `selftest`, after the `is_allowed_iaq_key` block around line 444)

- [ ] **Step 1: Add the C++ helper**

In `jandy_proto.h`, immediately after the `is_allowed_iaq_key` inline function:

```cpp
// Keys the autonomous scheduler may send WITHOUT the master interlock: only Filter
// Pump (0x11) and Cleaner (0x15). They share the iaq_press allowlist with the spa
// toggle / blower / light, so the scheduler permission is scoped per-key here, NOT
// by un-gating iaq_press wholesale. Mirror of jandy/frames.is_scheduler_safe_key.
inline bool is_scheduler_safe_key(uint8_t key) {
  return key == KEY_IAQ_FILTER_PUMP || key == KEY_IAQ_CLEANER;
}
```

- [ ] **Step 2: Add the selftest assertion**

In `jandy_proto.cpp` `selftest`, right after the existing `is_allowed_iaq_key` checks (the block ending near line 444), matching the surrounding style:

```cpp
    if (!is_scheduler_safe_key(0x11) || !is_scheduler_safe_key(0x15)) {
      detail = "scheduler-safe key set too narrow";
      return false;
    }
    if (is_scheduler_safe_key(0x12) || is_scheduler_safe_key(0x16) ||
        is_scheduler_safe_key(0x17) || is_scheduler_safe_key(0x13)) {
      detail = "scheduler-safe key set too broad";
      return false;
    }
```

(Read the exact lines around 441 to 445 first and match the existing `detail = ...; return false;` style; the selftest count goes up by these checks.)

- [ ] **Step 3: Commit** (C++ compiles at deploy; the selftest runs on-device after flashing)

```bash
git add components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
git commit -m "feat(brain): mirror is_scheduler_safe_key in C++ + selftest assertion"
```

### Task A3: Add the `scheduler_armed_` member and setter

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (setter near line 58, member near line 186)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (impl after `set_iaq_presence`, around line 345)

- [ ] **Step 1: Header, add the setter and accessor**

In `jandy_aqualink.h`, right after the `set_iaq_presence` / `iaq_presence()` pair (around line 58 to 59):

```cpp
  void set_scheduler_armed(bool on);
  bool scheduler_armed() const { return scheduler_armed_; }
```

- [ ] **Step 2: Header, add the member**

In `jandy_aqualink.h`, right after `volatile bool iaq_presence_{false};` (around line 186):

```cpp
  // Scheduler permission: when on, lets the autonomous HA brain send ONLY pump speed,
  // Filter Pump (0x11), and Cleaner (0x15) WITHOUT the master interlock. Restores
  // across reboots so the brain self-resumes. Every other write still needs interlock_.
  volatile bool scheduler_armed_{false};
```

- [ ] **Step 3: Implementation**

In `jandy_aqualink.cpp`, right after the `set_iaq_presence` function (around line 345):

```cpp
void JandyAqualink::set_scheduler_armed(bool on) {
  portENTER_CRITICAL(&mux_);
  scheduler_armed_ = on;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "scheduler armed %s (permits pump speed + filter pump + cleaner unattended)",
           on ? "ON" : "OFF");
}
```

- [ ] **Step 4: Commit**

```bash
git add components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
git commit -m "feat(brain): scheduler_armed_ member + set_scheduler_armed"
```

### Task A4: Change the two gates to honor `scheduler_armed_`

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`iaq_press` lines 348-351, `set_pump_rpm` lines 414-417)

- [ ] **Step 1: `iaq_press` gate**

In `iaq_press`, replace the opening interlock check (lines 348-351):

```cpp
  if (!interlock_) {
    ESP_LOGW(TAG, "iaq press REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
```

with:

```cpp
  bool scheduler_ok = scheduler_armed_ && jandy::is_scheduler_safe_key(key);
  if (!interlock_ && !scheduler_ok) {
    ESP_LOGW(TAG, "iaq press REFUSED: interlock OFF and key 0x%02X not scheduler-safe", key);
    return;
  }
```

(The presence check and the `is_allowed_iaq_key` allowlist check immediately below stay exactly as they are. A scheduler-safe key still must be presence-on and in the allowlist.)

- [ ] **Step 2: `set_pump_rpm` gate**

In `set_pump_rpm`, replace the opening interlock check (lines 414-417):

```cpp
  if (!interlock_) {
    ESP_LOGW(TAG, "set_pump_rpm REFUSED: safety interlock is OFF (rpm=%u)", rpm);
    return;
  }
```

with:

```cpp
  if (!interlock_ && !scheduler_armed_) {
    ESP_LOGW(TAG, "set_pump_rpm REFUSED: interlock OFF and scheduler not armed (rpm=%u)", rpm);
    return;
  }
```

- [ ] **Step 3: Confirm no other write path changed**

Grep to be sure only these two gates changed and `press_heater`, `press_device_toggle`, `set_heater_setpoint`, `request_pool_mode`, `request_spa_mode`, `iaq_nav`, and the survey path still start with `if (!interlock_)`:

Run: `git diff components/jandy_aqualink/jandy_aqualink.cpp`
Expected: only the `iaq_press` and `set_pump_rpm` opening checks differ.

- [ ] **Step 4: Commit**

```bash
git add components/jandy_aqualink/jandy_aqualink.cpp
git commit -m "feat(brain): gate pump speed + 0x11/0x15 on scheduler_armed OR interlock"
```

### Task A5: Add the "Pool Scheduler" switch to the firmware yaml

**Files:**
- Modify: `firmware/pool-bridge.yaml` (in the `switch:` block, after the `bus_sniff` switch, before `button:` at line 165)

- [ ] **Step 1: Add the switch**

```yaml
  # Scheduler permission: when on, the autonomous HA pool brain may set pump speed
  # and toggle Filter Pump (0x11) / Cleaner (0x15) WITHOUT the master interlock.
  # Scoped to exactly those writes in firmware (is_scheduler_safe_key); every other
  # write still needs the interlock. Restores across reboots so the brain self-resumes.
  - platform: template
    name: "Pool Scheduler"
    id: scheduler_armed
    icon: "mdi:robot"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF
    turn_on_action:
      - lambda: "id(jandy_comp).set_scheduler_armed(true);"
    turn_off_action:
      - lambda: "id(jandy_comp).set_scheduler_armed(false);"
```

This yields HA entity `switch.pool_rs485_bridge_pool_scheduler`.

- [ ] **Step 2: Commit**

```bash
git add firmware/pool-bridge.yaml
git commit -m "feat(brain): Pool Scheduler switch (restore-on, scoped scheduler permission)"
```

### Task A6: Deploy and verify

- [ ] **Step 1: Run the full Python suite**

Run: `python -m pytest -q`
Expected: all green (previous count plus the one new test).

- [ ] **Step 2: Push**

```bash
git push origin master
```

- [ ] **Step 3: Back up and patch the LIVE dashboard yaml**

The live dashboard config is separate from the repo yaml. Back it up, then add the same "Pool Scheduler" switch block via the dashboard edit endpoint (use `(New-Object System.Net.WebClient).DownloadString` to read and `.UploadString(...,"POST",$body)` to save, per the durable tooling note). Read it back and confirm the block is present byte-exact.

- [ ] **Step 4: Compile**

Run (from `C:\Users\Falcon\Documents\pool-controller`):
`./esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -DashHost 192.168.1.126 -DashPort 6052`
Expected: `Successfully compiled program.` and EXIT CODE 0 in the tail (trust the human-readable tail, not marker counts).

- [ ] **Step 5: Upload (OTA)**

Run: `./esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -DashHost 192.168.1.126 -DashPort 6052`
Expected: `Successfully uploaded program.` / `OTA successful`, EXIT CODE 0.

- [ ] **Step 6: Verify selftest + health**

Capture device logs (`poollog.ps1` or `esphome_ws.ps1 -Action logs -Port 192.168.4.51`). Confirm `selftest PASS` (new higher count), `checksum_errors=0`. Do NOT proceed to the live test on a FAIL.

### Task A7: Safety-gate live test (the critical proof)

This proves the scheduler permission is correctly narrow. Founder welcome to watch. Start state: interlock OFF, scheduler OFF, presence ON.

- [ ] **Step 1: Both permissions off, pump write refused**

Set `number.pool_rs485_bridge_pump_speed_set` to 2000 (or press a preset). Watch the log.
Expected: `set_pump_rpm REFUSED: interlock OFF and scheduler not armed`. Pump untouched.

- [ ] **Step 2: Arm the scheduler, pump write allowed**

Turn ON `switch.pool_rs485_bridge_pool_scheduler` (interlock still OFF). Set pump speed to 2000.
Expected: `set_pump_rpm: start sequence -> 2000 RPM`, sequence completes, read-back shows ~2000.

- [ ] **Step 2b: Cleaner allowed under scheduler**

Press `button.pool_rs485_bridge_cleaner`.
Expected: it arms and sends (cleaner status flips). This is key 0x15, scheduler-safe.

- [ ] **Step 3: Risky keys STILL refused under scheduler**

With scheduler ON and interlock still OFF, press `button.pool_rs485_bridge_air_blower`, then `button.pool_rs485_bridge_pool_heat`.
Expected for each: `iaq press REFUSED: interlock OFF and key 0x16 not scheduler-safe` (and 0x13 for heat) / `heater REFUSED: safety interlock is OFF`. Nothing actuates. This proves the per-key scoping.

- [ ] **Step 4: Reset to a known state**

Turn the scheduler OFF for now (Phase B builds the HA side before we arm for real). Leave interlock OFF, presence ON.

---

## PHASE B: Home Assistant core schedule

All HA objects are created via the `mcp__Home_Assistant__*` config tools (`ha_config_set_helper`, `ha_config_set_script`, `ha_config_set_automation`). Configs below are shown as Home Assistant YAML.

### Task B1: Create the helpers

- [ ] **Step 1: Create all helpers**

- `input_number.pool_target_rpm`: min 600, max 3450, step 50, unit "RPM", icon mdi:speedometer. The brain's desired pump speed for the active phase.
- `input_boolean.pool_cleaner_should_run`: the desired cleaner state for the active phase.
- `input_datetime.pool_manual_hold_until`: has_date true, has_time true, initial `2000-01-01 00:00:00` (a past time, so the hold is inactive by default and the watch loop never sees a null timestamp). While `now()` is before this, the watch loop does not correct (a manual change is in effect).
- `input_select.pool_phase`: options `["Quiet","Morning clean","Day","Evening clean","Spa (manual)"]`. For visibility.
- `input_button.pool_swim_boost`: the swim boost trigger.
- `input_number.pool_manual_speed_request`: min 600, max 3450, step 50, unit "RPM". Set this to force a manual speed that holds until the next phase.

- [ ] **Step 2: Verify**

Run a search: confirm all six entities exist with the expected domains.

### Task B2: Create the apply scripts

- [ ] **Step 1: `script.pool_apply_pump`**

```yaml
alias: Pool apply pump
sequence:
  - service: number.set_value
    target: { entity_id: number.pool_rs485_bridge_pump_speed_set }
    data: { value: "{{ states('input_number.pool_target_rpm') | int }}" }
mode: single
```

- [ ] **Step 2: `script.pool_apply_cleaner`**

```yaml
alias: Pool apply cleaner
sequence:
  - condition: template
    value_template: >
      {{ states('binary_sensor.pool_rs485_bridge_pool_cleaner_status') in ['on','off']
         and (is_state('input_boolean.pool_cleaner_should_run','on')
              != is_state('binary_sensor.pool_rs485_bridge_pool_cleaner_status','on')) }}
  - service: button.press
    target: { entity_id: button.pool_rs485_bridge_cleaner }
mode: single
```

This presses the cleaner toggle only when the actual state differs from desired and the status is known, so it never toggles blindly.

- [ ] **Step 3: Verify** by calling each script with the scheduler armed and watching the device respond (pump speed set; cleaner toggles only on mismatch).

### Task B3: Create `script.pool_evaluate_phase` (the time-of-day brain)

- [ ] **Step 1: Create the script**

```yaml
alias: Pool evaluate phase
sequence:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
  - condition: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    state: "off"
  - condition: state
    entity_id: switch.pool_rs485_bridge_iaqualink_presence
    state: "on"
  - variables:
      phase: >
        {% set t = now().hour*60 + now().minute %}
        {% if t >= 1320 or t < 480 %}Quiet
        {% elif t < 600 %}Morning clean
        {% elif t < 1200 %}Day
        {% else %}Evening clean{% endif %}
      rpm: >
        {% set t = now().hour*60 + now().minute %}
        {% if t >= 1320 or t < 480 %}1100
        {% elif t < 600 %}2750
        {% elif t < 1200 %}2000
        {% else %}2750{% endif %}
      cleaner_on: >
        {% set t = now().hour*60 + now().minute %}
        {% if (t >= 480 and t < 600) or (t >= 1200 and t < 1320) %}on{% else %}off{% endif %}
  - service: input_select.select_option
    target: { entity_id: input_select.pool_phase }
    data: { option: "{{ phase | trim }}" }
  - service: input_number.set_value
    target: { entity_id: input_number.pool_target_rpm }
    data: { value: "{{ rpm | int }}" }
  - service: "input_boolean.turn_{{ cleaner_on | trim }}"
    target: { entity_id: input_boolean.pool_cleaner_should_run }
  - service: input_datetime.set_datetime
    target: { entity_id: input_datetime.pool_manual_hold_until }
    data: { timestamp: "{{ now().timestamp() }}" }   # clear any manual hold
  - service: script.pool_apply_pump
  - service: script.pool_apply_cleaner
mode: single
```

Phase map (minutes of day): Quiet 1320 to 480 (10pm to 8am) 1100 RPM cleaner off; Morning clean 480 to 600 (8am to 10am) 2750 cleaner on; Day 600 to 1200 (10am to 8pm) 2000 cleaner off; Evening clean 1200 to 1320 (8pm to 10pm) 2750 cleaner on.

- [ ] **Step 2: Verify** by arming the scheduler and calling `script.pool_evaluate_phase`; confirm the helpers and the device match the current time of day.

### Task B4: Phase scheduler automation

- [ ] **Step 1: Create**

```yaml
alias: Pool phase scheduler
trigger:
  - platform: time
    at: ["08:00:00", "10:00:00", "20:00:00", "22:00:00"]
action:
  - service: script.pool_evaluate_phase
mode: single
```

- [ ] **Step 2: Verify** by temporarily adding a near-future time trigger (or calling the script) and confirming the phase flips.

### Task B5: Watch-and-correct automation (the 2-minute enforcer)

- [ ] **Step 1: Create**

```yaml
alias: Pool watch and correct
trigger:
  - platform: time_pattern
    minutes: "/2"
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
  - condition: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    state: "off"
  - condition: state
    entity_id: switch.pool_rs485_bridge_iaqualink_presence
    state: "on"
  - condition: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_bridge_status
    state: "on"
  - condition: template   # manual hold not active (float(0) handles an unset datetime)
    value_template: "{{ now().timestamp() >= (state_attr('input_datetime.pool_manual_hold_until','timestamp') | float(0)) }}"
action:
  - service: button.press
    target: { entity_id: button.pool_rs485_bridge_read_pump_speed }
  - delay: { seconds: 8 }
  - choose:
      - conditions:
          - condition: template
            value_template: >
              {{ states('sensor.pool_rs485_bridge_pool_pump_speed') | int(-1) >= 0
                 and ((states('sensor.pool_rs485_bridge_pool_pump_speed') | int)
                      - (states('input_number.pool_target_rpm') | int)) | abs > 150 }}
        sequence:
          - service: script.pool_apply_pump
  - service: script.pool_apply_cleaner
mode: single
max_exceeded: silent
```

It reads, waits 8s for the sensor to settle, corrects the pump only if drift exceeds 150 RPM and the read succeeded, then enforces the cleaner state. `mode: single` plus the 2-minute cadence means runs never stack.

- [ ] **Step 2: Verify** with the scheduler armed: manually set the device pump to a wrong speed (for example 1100 during Day), wait up to ~2 minutes, confirm the enforcer reads, detects drift, and restores 2000.

### Task B6: Startup resync automation

- [ ] **Step 1: Create**

```yaml
alias: Pool startup resync
trigger:
  - platform: homeassistant
    event: start
  - platform: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    to: "on"
  - platform: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_bridge_status
    to: "on"
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
action:
  - delay: { seconds: 15 }   # let presence and sensors settle
  - service: script.pool_evaluate_phase
mode: single
```

- [ ] **Step 2: Verify** by toggling the scheduler off then on; confirm the brain re-applies the correct phase within ~15s.

---

## PHASE C: Coexistence and manual control

### Task C1: Spa stand-down (visibility) + confirm conditions

- [ ] **Step 1: Create a visibility automation**

```yaml
alias: Pool spa standdown
trigger:
  - platform: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    to: "on"
action:
  - service: input_select.select_option
    target: { entity_id: input_select.pool_phase }
    data: { option: "Spa (manual)" }
mode: single
```

Actual stand-down is enforced by the `spa_mode == off` conditions already in `pool_evaluate_phase`, the phase scheduler (via the script), and the watch-and-correct automation. This automation only reflects it on the dashboard.

- [ ] **Step 2: Verify** by switching to spa mode and confirming the watch-and-correct automation stops acting (check its trace shows the condition blocking) and the phase shows "Spa (manual)".

### Task C2: Restore after spa exit

- [ ] **Step 1: Create**

```yaml
alias: Pool restore after spa
trigger:
  - platform: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    to: "off"
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
action:
  - if:
      - condition: state
        entity_id: binary_sensor.pool_rs485_bridge_pool_filter_pump_status
        state: "off"
    then:
      - service: button.press
        target: { entity_id: button.pool_rs485_bridge_filter_pump }
  - delay: { minutes: 2 }   # valves settle; first press after spa exit often does not take
  - if:
      - condition: state
        entity_id: binary_sensor.pool_rs485_bridge_pool_filter_pump_status
        state: "off"
    then:
      - service: button.press
        target: { entity_id: button.pool_rs485_bridge_filter_pump }
  - service: script.pool_evaluate_phase
mode: single
```

- [ ] **Step 2: Verify** (founder-coordinated, since it needs a real spa session): switch to spa, then back to pool; confirm the filter pump comes back on and the schedule resumes for the current time.

### Task C3: Swim boost

- [ ] **Step 1: Create**

```yaml
alias: Pool swim boost
trigger:
  - platform: state
    entity_id: input_button.pool_swim_boost
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
  - condition: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    state: "off"
action:
  - service: input_number.set_value
    target: { entity_id: input_number.pool_target_rpm }
    data: { value: 2750 }
  - service: input_datetime.set_datetime
    target: { entity_id: input_datetime.pool_manual_hold_until }
    data: { timestamp: "{{ now().timestamp() + 7200 }}" }   # hold ~2h or until the next phase clears it
  - service: script.pool_apply_pump
mode: single
```

- [ ] **Step 2: Verify** by pressing the boost; confirm the pump goes to 2750 and the watch loop does not pull it back (manual hold active) until ~2h or the next phase boundary.

### Task C4: Manual speed request

- [ ] **Step 1: Create**

```yaml
alias: Pool manual speed
trigger:
  - platform: state
    entity_id: input_number.pool_manual_speed_request
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
  - condition: state
    entity_id: binary_sensor.pool_rs485_bridge_pool_spa_mode
    state: "off"
action:
  - service: input_number.set_value
    target: { entity_id: input_number.pool_target_rpm }
    data: { value: "{{ states('input_number.pool_manual_speed_request') | int }}" }
  - service: input_datetime.set_datetime
    target: { entity_id: input_datetime.pool_manual_hold_until }
    data: { timestamp: "{{ now().timestamp() + 43200 }}" }   # hold until the next phase boundary clears it
  - service: script.pool_apply_pump
mode: single
```

- [ ] **Step 2: Verify** by setting the manual speed; confirm the pump holds it until the next phase boundary.

### Task C5: Presence keeper

- [ ] **Step 1: Create**

```yaml
alias: Pool presence keeper
trigger:
  - platform: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    to: "on"
  - platform: state
    entity_id: switch.pool_rs485_bridge_iaqualink_presence
    to: "off"
condition:
  - condition: state
    entity_id: switch.pool_rs485_bridge_pool_scheduler
    state: "on"
action:
  - delay: { seconds: 5 }
  - if:
      - condition: state
        entity_id: switch.pool_rs485_bridge_iaqualink_presence
        state: "off"
    then:
      - service: switch.turn_on
        target: { entity_id: switch.pool_rs485_bridge_iaqualink_presence }
mode: single
```

This re-enables presence only while the scheduler is armed. During an iAqualink 2.0 session the founder turns the Pool Scheduler OFF first, which disarms this, so it never fights for the 0x33 seat.

- [ ] **Step 2: Verify** by turning presence off while the scheduler is armed; confirm it comes back on within ~5s. Then confirm that with the scheduler OFF, presence stays off.

---

## PHASE D: Failsafe verification and polish

### Task D1: Confirm the never-off invariant

- [ ] **Step 1: Review** every script and automation created. Confirm none ever turns the filter pump OFF or commands RPM 0. The only filter-pump press is in "restore after spa," and it only ever presses to turn it ON (guarded by `filter_pump_status == off`). The lowest scheduled speed is 1100. Document the finding in the plan checkboxes.

### Task D2: Correction-rate watchdog (optional, lightweight)

- [ ] **Step 1: Helper** `counter.pool_corrections` (or `input_number.pool_corrections_hour`).
- [ ] **Step 2:** In the watch-and-correct automation, increment the counter whenever it calls `script.pool_apply_pump`.
- [ ] **Step 3:** Automation: every hour, if the count exceeds 6, send a notification to `notify.mobile_app_honor` and `notify.mobile_app_nikkis_iphone17` ("Pool brain is correcting the pump unusually often"), then reset the counter; otherwise just reset. This surfaces a runaway tug-of-war without blocking.

### Task D3: Whole-brain accelerated test

- [ ] **Step 1:** With the scheduler armed, walk each phase by calling `script.pool_evaluate_phase` at representative times (or temporarily shifting the phase boundaries) and confirm the pump and cleaner match the table.
- [ ] **Step 2:** Force pump drift mid-phase; confirm correction within ~2 minutes.
- [ ] **Step 3:** Exercise swim boost and manual speed; confirm holds and auto-resume.
- [ ] **Step 4:** Founder-coordinated: spa in and out; confirm stand-down and restore.

### Task D4: Document and set resting state

- [ ] **Step 1:** Add a short "Operating the pool brain" section to `docs/ROADMAP.md`: what the Pool Scheduler switch does, how to pause it (and that pausing it is the first step before bringing the iAqualink online), the swim boost, and the manual speed.
- [ ] **Step 2:** Decide the resting state with the founder: scheduler ARMED (brain live) once the live tests pass, interlock OFF, presence ON. Record it.
- [ ] **Step 3: Commit** the docs.

```bash
git add docs/ROADMAP.md
git commit -m "docs(brain): operating guide + resting state"
```

---

## Self-review notes

- Spec coverage: permission model (A1 to A7), schedule phases (B3), watch-and-correct at 2 min (B5), cleaner keeper (B2/B5), spa stand-down + restore (C1/C2), manual hold + swim boost + manual speed (C3/C4), presence keeper (C5), startup resync (B6), never-off failsafe (D1), correction cap (D2). All spec behaviors map to a task.
- Per-key safety: the scheduler permission is scoped by `is_scheduler_safe_key` (0x11, 0x15) in both Python and C++, proven by the A7 gate test (blower/heater still refuse under scheduler).
- Naming consistency: helpers, scripts, and entity IDs are used identically across tasks (`input_number.pool_target_rpm`, `script.pool_apply_pump`, `switch.pool_rs485_bridge_pool_scheduler`, `number.pool_rs485_bridge_pump_speed_set`, `sensor.pool_rs485_bridge_pool_pump_speed`).
- Failsafe: no task ever commands the filter pump off or RPM 0; lowest is 1100. The panel's own schedule (soon the one deliberate failsafe program) carries the pool if the brain or box is down.
