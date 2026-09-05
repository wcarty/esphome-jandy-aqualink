# 🛠️ Pump Auto-Refresh + Schedule Watch Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** This plan drives LIVE pool hardware over the network and is
> executed INLINE in this session (sequential build/flash/verify with live-log watching).
> It is not subagent-parallelizable. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let the view-only pump read run unattended on a 15-minute timer so Home Assistant
records the panel's self-driven pump schedule, without ever leaving control armed.

**Architecture:** One C++ change ungates `read_pump_speed()` from the control interlock
(keeping it behind iAqualink presence). Two firmware-yaml additions, an Auto-Refresh switch
(default OFF) and a 15-minute `interval:`, drive that read on a timer. The dashboard at
`192.168.1.126:6052` compiles `pool-bridge.yaml` and pulls the C++ component from GitHub
(`refresh: 0s`), so the C++ change must be pushed before compiling, and the yaml additions
must land in the live dashboard config (not only the repo reference copy).

**Tech Stack:** ESPHome external component (C++ on M5Stack Atom / ESP32), ESPHome dashboard
WebSocket build (`esphome_ws.ps1`), Home Assistant for control + history, Python pytest for
frame-logic regression.

**Safety invariant (must hold after every task):** with the control interlock OFF, every
WRITE/equipment path still logs REFUSED and transmits nothing. The only newly-unlocked action
is the view-only STATUS read.

---

### Task 1: Ungate the view-only read (C++)

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp:277-285`
- Modify: `components/jandy_aqualink/jandy_aqualink.h:73-74`

- [ ] **Step 1: Remove the interlock guard from `read_pump_speed()`**

Replace (cpp, the opening of the function):

```cpp
void JandyAqualink::read_pump_speed() {
  if (!interlock_) {
    ESP_LOGW(TAG, "read pump speed REFUSED: safety interlock is OFF");
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "read pump speed REFUSED: iAqualink presence is OFF");
    return;
  }
```

with:

```cpp
void JandyAqualink::read_pump_speed() {
  // View-only: gated by iAqualink presence ONLY, deliberately not the control
  // interlock, so an auto-refresh timer can map the panel's schedule without
  // leaving control armed unattended. This sends only view-only navigation
  // (STATUS 0x06, then HOME 0x01 via the core-1 auto-return) and never an
  // equipment key. Every WRITE path (set_pump_rpm, iaq_press, iaq_nav, arm_key,
  // request_pool_mode) keeps its interlock_ gate.
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "read pump speed REFUSED: iAqualink presence is OFF");
    return;
  }
```

- [ ] **Step 2: Update the header doc comment**

Replace (`.h`):

```cpp
  // Briefly view the STATUS page to read pump RPM/watts, then return to HOME so
  // temperatures keep updating. Gated by the master interlock + iAqualink presence.
```

with:

```cpp
  // Briefly view the STATUS page to read pump RPM/watts, then return to HOME so
  // temperatures keep updating. View-only: gated by iAqualink presence only, NOT
  // the control interlock, so an auto-refresh timer can run it unattended.
```

- [ ] **Step 3: Sanity-check nothing else references the removed log path**

Run (expect only the matches inside the other gated functions, NOT a second one in read_pump_speed):
`grep -n "read pump speed REFUSED" components/jandy_aqualink/jandy_aqualink.cpp`
Expected: exactly ONE remaining line (the presence refusal). The interlock refusal line is gone.

---

### Task 2: Add the Auto-Refresh switch + 15-min interval (repo reference yaml)

**Files:**
- Modify: `firmware/pool-bridge.yaml` (add a switch after "iAqualink Presence"; add a top-level `interval:`)

- [ ] **Step 1: Add the switch** immediately after the `iAqualink Presence` switch block (before `button:`):

```yaml
  # Auto-refresh driver: when on, a 15-min interval reads the pump speed so HA
  # history captures the panel's stored schedule. View-only; off by default.
  - platform: template
    name: "Pool Pump Auto-Refresh"
    id: pump_autorefresh
    icon: "mdi:timer-refresh"
    optimistic: true
    restore_mode: RESTORE_DEFAULT_OFF   # resumes the watch after an overnight reboot
```

- [ ] **Step 2: Add the interval** as a new top-level block (place after the `number:` block, before the `# Diagnostics` sensor block):

```yaml
# When Auto-Refresh is on AND iAqualink presence is on, read the pump speed every
# 15 minutes. read_pump_speed() is view-only (STATUS then auto-HOME); it actuates
# nothing. The presence check here only avoids a REFUSED log line when presence is off.
interval:
  - interval: 15min
    then:
      - lambda: |-
          if (id(pump_autorefresh).state && id(iaq_presence).state) {
            id(jandy_comp).read_pump_speed();
          }
```

- [ ] **Step 3: Confirm the edits are well-formed** (no compile yet; just eyeball the diff):
`git -C <repo> diff -- firmware/pool-bridge.yaml`
Expected: one new switch block, one new `interval:` block, nothing else changed.

---

### Task 3: Regression gate (pytest)

**Files:** none (runs the existing suite)

- [ ] **Step 1: Run the full suite from the repo root**

Run: `pytest -q` (in `C:\Users\Falcon\Documents\pool-controller\esp32-experiment`)
Expected: all green (82+ passed). This change touches no frame logic, so a failure here means
something unrelated broke and we STOP and investigate before flashing.

---

### Task 4: Commit and push (so the dashboard compile pulls the new C++)

**Files:** commits `jandy_aqualink.cpp`, `jandy_aqualink.h`, `firmware/pool-bridge.yaml`

- [ ] **Step 1: Stage only the three changed files**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.cpp components/jandy_aqualink/jandy_aqualink.h firmware/pool-bridge.yaml
```

- [ ] **Step 2: Commit** (message via here-string; `-m` before `--`; no em dashes; Co-Authored trailer):

```
feat(session-7): ungate view-only pump read + 15-min auto-refresh

read_pump_speed() now runs under iAqualink presence only (not the control
interlock), so a new Pool Pump Auto-Refresh switch + 15-min interval can map
the panel's stored schedule unattended. All WRITE paths stay interlock-gated.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>
```

- [ ] **Step 3: Push to origin/master** (the dashboard pulls the component from GitHub at compile):

```
git -C <repo> push origin master
```

Expected: push includes the spec commit `2aca8f6` and this feature commit. Confirm with
`git -C <repo> status -sb` showing `## master...origin/master` with no `[ahead]`.

---

### Task 5: Patch the LIVE dashboard config (back up first, verify state)

**Files:** the dashboard-hosted `pool-bridge.yaml` (edited via `http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml`), backup written to `C:\Users\Falcon\Documents\pool-controller\dashboard-pool-bridge.BACKUP-<stamp>.yaml`

- [ ] **Step 1: GET the current live config fresh** (do NOT trust the older backups):

`GET http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml`

- [ ] **Step 2: Write the fetched content to a timestamped backup** before any change.

- [ ] **Step 3: Verify current state in the fetched config and FLAG a discrepancy if present.**
Check the `Pump Speed: Low` preset value. The repo reference and Session 6 notes say **2000**
(salt floor ~1850 + cushion). If the live config still shows **1600**, STOP and flag to the
founder: running Low below ~1850 stops salt-cell chlorination, and it would mean the retune
never reached the live device. Do not silently change it; ask whether to align it to 2000 while
patching.

- [ ] **Step 4: Insert the same two additions** from Task 2 into the fetched yaml:
the `Pool Pump Auto-Refresh` switch (after the `iAqualink Presence` switch) and the `interval:`
block (top-level, e.g. appended at the end). Keep the live config's real secrets/wifi untouched.

- [ ] **Step 5: POST the patched config back**, then **GET again and readback-verify** the switch
and interval are present and the rest is byte-identical to the backup plus those two blocks.

---

### Task 6: Compile on the dashboard

- [ ] **Step 1: Compile** (from `C:\Users\Falcon\Documents\pool-controller`):

`.\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 600`

Expected: ends with `Successfully compiled program.` and `==> EXIT CODE 0`. Trust the
human-readable tail (the `.out` marker-count grep reads 0 even on success). If it pulls a stale
component, confirm Task 4 push landed on origin/master and re-run.

---

### Task 7: Upload / flash to the device

- [ ] **Step 1: Upload over the network**:

`.\esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -TimeoutSec 600`

Expected: `Successfully compiled/uploaded program.` and `==> EXIT CODE 0`. The device reboots
into the new firmware.

---

### Task 8: Post-flash health gate (do NOT actuate on any FAIL)

- [ ] **Step 1: Start a background log capture** to a file:
`.\esphome_ws.ps1 -Action logs -Config pool-bridge.yaml -Port 192.168.4.51 -TimeoutSec 120` (background),
writing to e.g. `docs/verify-boot.log`.

- [ ] **Step 2: After a >15s window, read the file with a shared-read open** (a plain read hits
EBUSY while the writer holds the file). Confirm:
- `selftest PASS -> 13/13`
- checksum errors 0 across the census burst
- the `iAqualink Presence` switch restored ON (temps reading). If it booted OFF, turn it ON via
  HA before the behavioral tests.

---

### Task 9: Behavioral verification (interlock OFF, presence ON)

**Drive controls via Home Assistant service calls** (`button.press`, `switch.turn_on`,
`number.set_value`), never object-id REST URLs (deprecated, removed in 2026.7.0).

- [ ] **Step 1: Resolve entity IDs** via HA (`ha_search_entities` on "Pool"):
the Read Pump Speed button, a pump preset button (use `Pump Speed: Normal` = 2750, which matches
the resting RPM so the refusal test carries zero actuation risk), the `Pool Pump Speed` sensor,
the `Pool Pump Auto-Refresh` switch, and confirm the interlock switch is OFF.

- [ ] **Step 2: Prove the read works ungated.** With interlock OFF + presence ON, press
`Read Pump Speed`. Expected: log line `read pump speed: viewing STATUS page, will return to HOME`,
and the `Pool Pump Speed` sensor refreshes. Wait for the confirmed sensor readback before asserting.

- [ ] **Step 3: Prove writes stay locked.** Press `Pump Speed: Normal`. Expected: log line
`set_pump_rpm REFUSED: safety interlock is OFF`, and the pump RPM is unchanged on the bus. The
REFUSED log line is the proof.

---

### Task 10: Start the watch and confirm the first auto-read

- [ ] **Step 1: Turn ON `Pool Pump Auto-Refresh`** via HA. Note the wall-clock time.

- [ ] **Step 2: Start a background log capture** (`-TimeoutSec 1080`, ~18 min) to a file.

- [ ] **Step 3: After ~15-16 min, shared-read the file** and confirm an UNPROMPTED
`read pump speed: viewing STATUS page` appears about 15 minutes after Step 1 (one that nobody
pressed). That confirms the timer fires. The watch then runs on the device overnight on its own.

---

### Task 11: Hand off to tomorrow

- [ ] **Step 1: Leave Auto-Refresh ON.** The device + HA record `Pool Pump Speed` history with no
session open.
- [ ] **Step 2: Update memory** (`project_pool_controller_phase2.md`) with the shipped commit SHA,
the verified behavior, and the Low-preset finding from Task 5.
- [ ] **Step 3: Tomorrow** (fresh short session): read `Pool Pump Speed` history/Logbook over ~24h,
map when/to-what the panel changes the pump, and feed the Session 10 simple-vs-busy decision.

---

## Self-review notes

- **Spec coverage:** code ungate (Task 1), switch+interval (Task 2, repo + Task 5, live),
  push-before-compile (Task 4/6), verification incl. write-still-refused (Task 9), 24h watch
  (Task 10/11), pytest + selftest health gates (Task 3/8). All spec sections map to a task.
- **No new unit test** is intentional and spec-consistent: gating is C++ runtime behavior, not
  frame logic; the selftest covers frame vectors only. The "tests" here are the on-device
  behavioral acceptance checks (Task 9) plus pytest/selftest as regression gates.
- **Type/name consistency:** switch `id: pump_autorefresh`, component `id: jandy_comp`, method
  `read_pump_speed()`, presence `id: iaq_presence` are used identically in Task 2 and the
  interval lambda.
