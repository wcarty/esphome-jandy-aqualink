# 🛠️ Blower / Spa-Mode Sniffer Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Decode the equipment-LED status the panel already streams to our inert AllButton keypad seat (0x08), so Home Assistant shows and charts exactly when spa mode and the air blower (plus filter pump and cleaner for context) switch on and off, with timestamps, confirming the panel is doing it on its own.

**Architecture:** The box already holds AllButton presence at 0x08 and receives a steady stream of `CMD_STATUS` (0x02) frames carrying a packed equipment-LED bitmap (2 bits per circuit: an ON bit and an adjacent FLASH bit, per AqualinkD `source/allbutton.c` `processLEDstate`). We add a pure decoder (Python reference + C++ mirror, TDD), wire it into the core-1 `observe_frame` path, store decoded states under the existing `mux_`, and publish four `binary_sensor` entities from core 0. Strictly read-only: no new bus transmissions, the control interlock and the 0x33 iAquaLink presence both stay OFF. The exact per-circuit bit positions are unknown until a live Service-mode toggle test (Discovery), so the plan front-loads a raw status-change logger, then Discovery pins the bits, then the decoder and sensors are built against the captured bytes.

**Tech Stack:** ESPHome external component (C++, Arduino framework, ESP32 / M5Stack Atom Lite); Python 3 reference mirror with `unittest` (run via `pytest`); deploy over the air through the ESPHome dashboard at `192.168.1.126:6052` driven by `C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1`.

---

## Deploy mechanics (read once, applies to every deploy task)

The component C++ is pulled by the dashboard via `external_components: github://...` (refresh 0s), so **component changes only reach the device after a `git push`**. The HA entity list lives in the dashboard's stored `pool-bridge.yaml`, which is separate from the repo's `firmware/pool-bridge.yaml`; entity additions must be patched into the live dashboard yaml (back up first, read back to verify).

Standard sequence for a component change:
1. Commit, then `git -C <repo> push` (dashboard fetches the pushed commit at compile time).
2. Compile: `powershell -File C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -DashHost 192.168.1.126 -DashPort 6052 -TimeoutSec 300` -> expect `Successfully compiled program.` and `EXIT CODE 0`. (Route output to a temp file and read that; the compile/upload `.out` files have a space-between-every-character artifact, so trust the human-readable tail line and `EXIT CODE 0`, not grep counts.)
3. Upload (OTA): `... -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 ...` -> expect `OTA successful` / `EXIT CODE 0`. `upload` flashes the already-compiled binary, so always `compile` then `upload`.
4. Logs: `... -Action logs -Port 192.168.4.51 ...` -> confirm `selftest ... PASS -> N/N`. **Do NOT trust or act on a build if the selftest logs FAIL.**

Live byte-log capture (used in Discovery and live verify): run a background PowerShell WebSocket capture of `ws://192.168.1.126:6052/logs` to a file, then read that file with a shared-read open (`[System.IO.File]::Open(path, Open, Read, ReadWrite)`) so the read never collides with the writer. Do not stack reads behind a long blocking capture call in the same message.

Safety invariant for every task: the master interlock stays OFF, `iAqualink Presence` stays OFF, and no key is ever armed or sent. This work only decodes inbound frames and publishes. If any step would transmit on the bus, stop.

---

## File structure

- `components/jandy_aqualink/jandy_proto.h` — add `CMD_STATUS = 0x02`; declare `KeypadStatus` struct and `decode_keypad_status(const Frame&)`.
- `components/jandy_aqualink/jandy_proto.cpp` — implement `decode_keypad_status` with the per-circuit bit table; add a `selftest()` vector block.
- `components/jandy_aqualink/jandy_aqualink.h` — add the raw-status-change buffer (Build 1); add `binary_sensor` members + setters and decoded-state volatiles (Build 2).
- `components/jandy_aqualink/jandy_aqualink.cpp` — `observe_frame`: log status payload on change (Build 1), then decode + store + log transitions (Build 2); `loop()`: publish the four binary_sensors (Build 2).
- `components/jandy_aqualink/__init__.py` — register four `binary_sensor` entities.
- `jandy/status.py` — add `KEYPAD_STATUS_CMD`, `CIRCUIT_BITS`, `decode_keypad_status`.
- `tests/fixtures.py` — add the captured `STATUS_08_*` frames (created in Discovery).
- `tests/test_status.py` — add decoder tests.
- `firmware/pool-bridge.yaml` — add the four binary_sensor entries (source of truth; live dashboard yaml patched at deploy).

---

## Part A — Build 1: raw status-change logger

### Task A1: Log the keypad status payload only when it changes

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h` (add `CMD_STATUS`)
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (add `last_status_raw_` member)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`observe_frame`)

- [ ] **Step 1: Add the command constant**

In `jandy_proto.h`, the line currently reads:
```cpp
static constexpr uint8_t CMD_POLL = 0x00, CMD_ACK = 0x01, CMD_DISPLAY = 0x25;
```
Change it to:
```cpp
static constexpr uint8_t CMD_POLL = 0x00, CMD_ACK = 0x01, CMD_STATUS = 0x02, CMD_DISPLAY = 0x25;
```

- [ ] **Step 2: Add the last-seen buffer member**

In `jandy_aqualink.h`, in the "Passive decode + bus census" block (near `std::vector<CensusEntry> census_;`), add:
```cpp
  // Last raw CMD_STATUS frame seen for our keypad address, so the status-change
  // logger fires only on a change (the panel streams these continuously).
  std::vector<uint8_t> last_status_raw_;
```

- [ ] **Step 3: Log on change in observe_frame**

In `jandy_aqualink.cpp`, inside `observe_frame`, after the census block and before the pump-0x60 logging block, add:
```cpp
  // Equipment LED bitmap to our keypad seat. Log the full frame only when it
  // changes (the stream is continuous). This is the Discovery capture tool and
  // the forensic record; decoding is added in Build 2.
  if (f.dest() == keypad_addr_ && f.cmd() == jandy::CMD_STATUS && f.raw != last_status_raw_) {
    last_status_raw_ = f.raw;
    char hex[3 * 32 + 1];
    static const char *const H = "0123456789ABCDEF";
    size_t n = f.raw.size() > 32 ? 32 : f.raw.size();
    size_t hp = 0;
    for (size_t i = 0; i < n; ++i) {
      uint8_t b = f.raw[i];
      hex[hp++] = H[b >> 4];
      hex[hp++] = H[b & 0x0F];
      hex[hp++] = ' ';
    }
    hex[hp] = '\0';
    ESP_LOGW(TAG, "STATUS08 change len=%u: %s", static_cast<unsigned>(f.raw.size()), hex);
  }
```

- [ ] **Step 4: Confirm the Python suite and brace balance still pass**

Run: `python -m pytest C:\Users\Falcon\Documents\pool-controller\esp32-experiment\tests -q`
Expected: all existing tests PASS (this change is C++ logging only; no Python behavior changed).

- [ ] **Step 5: Commit**

```bash
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment add components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment commit -m "feat(pool-sniffer): log keypad CMD_STATUS payload on change (Build 1)

Read-only Discovery tool: emit the full 0x08/0x02 equipment-LED frame to the
log only when it changes. No decoding, no new transmissions.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
```
Note: `-m` BEFORE `--` (committing with a pathspec after `--` parses the message as a pathspec).

### Task A2: Deploy Build 1 and confirm the logger is live

**Files:** none (deploy only)

- [ ] **Step 1: Push so the dashboard can pull the component**

Run: `git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment push`
Expected: push to `origin/master` succeeds.

- [ ] **Step 2: Compile**

Run: `powershell -File C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -DashHost 192.168.1.126 -DashPort 6052 -TimeoutSec 300`
Expected: `Successfully compiled program.` and `EXIT CODE 0`.

- [ ] **Step 3: Upload (OTA)**

Run: `powershell -File C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -DashHost 192.168.1.126 -DashPort 6052 -TimeoutSec 300`
Expected: `OTA successful` and `EXIT CODE 0`.

- [ ] **Step 4: Confirm selftest PASS and the logger emits**

Capture logs (background ws capture to file, then shared-read). Expected: `selftest ... PASS -> N/N`, `checksum_errors` 0, and at least one `STATUS08 change len=...` line within the first minute (the panel's state changes at least occasionally; if perfectly static you may see none until Discovery toggles something). Do not proceed on a selftest FAIL.

---

## Part B — Discovery: pin the bits (founder at the panel, Service mode)

### Task B1: Identify the spa-mode and blower bits, and add fixtures

**Files:**
- Modify: `tests/fixtures.py` (add captured frames)
- Modify: `jandy/status.py` (fill `CIRCUIT_BITS`)

This task is run live with the founder. The panel stays in Service mode (schedule paused; the founder operates equipment by hand). Our box only listens.

- [ ] **Step 1: Baseline**

With a background log capture running, record a baseline `STATUS08 change` line (the current resting bitmap). If none appears, ask the founder to toggle any circuit once to emit one.

- [ ] **Step 2: Isolate each circuit, one toggle at a time**

Ask the founder, one at a time, to operate at the panel and pause between each so a single `STATUS08 change` line is attributable:
  1. Air blower ON, then OFF.
  2. Pool -> Spa, then Spa -> Pool.
  3. (Optional context) Filter pump ON/OFF; Cleaner ON/OFF.
Record the `STATUS08` hex line that follows each single action.

- [ ] **Step 3: Diff to find each (byte, bit)**

For each circuit, XOR the before/after payloads (the `data` portion, i.e. bytes after `10 02 08 02` and before `cksum 10 03`). The changed bit is the circuit's ON bit. Record, per circuit, the `data` byte index and the bit index within that byte. Sanity-check against AqualinkD's 2-bits-per-LED, button-order layout, but trust the live diff.

- [ ] **Step 4: Add the captured frames to fixtures**

In `tests/fixtures.py`, after `STATUS_33_BIN`, add the real captured frames (paste the exact hex recorded above; keep the wire bytes including any stuffing):
```python
# Equipment LED bitmap to the AllButton keypad (0x08, cmd 0x02). Captured live
# 2026-06-01 in Service mode while toggling one circuit at a time.
STATUS_08_BLOWER_OFF = h("..")   # blower off sample
STATUS_08_BLOWER_ON  = h("..")   # blower on sample (only the blower bit differs)
STATUS_08_SPA_OFF    = h("..")   # pool mode sample
STATUS_08_SPA_ON     = h("..")   # spa mode sample
```

- [ ] **Step 5: Fill the bit table**

In `jandy/status.py`, add (using the indices found in Step 3):
```python
# Equipment LED bitmap carried in the CMD_STATUS (0x02) frame the panel sends an
# AllButton keypad. Each circuit LED uses 2 adjacent bits in `data`: bit b = ON,
# bit b+1 = FLASH (AqualinkD source/allbutton.c processLEDstate). Positions below
# are the live 2026-06-01 Discovery capture for THIS panel.
KEYPAD_STATUS_CMD = 0x02

# circuit -> (data byte index, ON-bit index within that byte)
CIRCUIT_BITS = {
    "spa_mode":    (0, 0),   # REPLACE with the Discovery values
    "air_blower":  (0, 0),
    "filter_pump": (0, 0),
    "cleaner":     (0, 0),
}
```

- [ ] **Step 6: Commit the captured data**

```bash
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment commit -m "test(pool-sniffer): capture live keypad status fixtures + bit map (Discovery)

Service-mode single-circuit toggles pin the spa-mode and blower bits in the
0x08/0x02 LED bitmap on this panel.

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- tests/fixtures.py jandy/status.py
```

---

## GATE before Part C

Proceed to Part C only if Discovery cleanly identified distinct, stable spa-mode and blower bits. If the bitmap does not expose them clearly (for example the blower never appears, or bits are ambiguous), STOP and reconvene with the founder: this is the spike answering "can we read it," and a negative answer routes to the fallback options in the spec rather than building sensors on shaky data.

---

## Part C — Build 2: decoder + Home Assistant sensors

### Task C1: Python decoder (TDD)

**Files:**
- Modify: `jandy/status.py` (add `decode_keypad_status`)
- Test: `tests/test_status.py`

- [ ] **Step 1: Write the failing tests**

Append to `tests/test_status.py` inside `TestDecodeStatus`:
```python
    def test_blower_bit_off_then_on(self):
        from jandy.status import decode_keypad_status
        off = decode_keypad_status(frame(fx.STATUS_08_BLOWER_OFF))
        on = decode_keypad_status(frame(fx.STATUS_08_BLOWER_ON))
        self.assertFalse(off["air_blower"])
        self.assertTrue(on["air_blower"])

    def test_spa_mode_bit_off_then_on(self):
        from jandy.status import decode_keypad_status
        off = decode_keypad_status(frame(fx.STATUS_08_SPA_OFF))
        on = decode_keypad_status(frame(fx.STATUS_08_SPA_ON))
        self.assertFalse(off["spa_mode"])
        self.assertTrue(on["spa_mode"])

    def test_non_status_frame_yields_empty(self):
        from jandy.status import decode_keypad_status
        self.assertEqual(decode_keypad_status(frame(fx.POLL_PUMP)), {})
```

- [ ] **Step 2: Run to confirm failure**

Run: `python -m pytest C:\Users\Falcon\Documents\pool-controller\esp32-experiment\tests\test_status.py -q`
Expected: FAIL (`decode_keypad_status` does not exist yet).

- [ ] **Step 3: Implement the decoder**

Append to `jandy/status.py`:
```python
def decode_keypad_status(frame, bits=CIRCUIT_BITS) -> dict:
    """Decode the circuits we track from a keypad CMD_STATUS (0x02) frame.

    Returns {} for any non-status frame. Each circuit is the ON bit at its
    (byte, bit) position in `frame.data`; absent bytes default the circuit off.
    """
    if frame.cmd != KEYPAD_STATUS_CMD:
        return {}
    out = {}
    for name, (byte_i, bit_i) in bits.items():
        out[name] = bool(byte_i < len(frame.data) and ((frame.data[byte_i] >> bit_i) & 1))
    return out
```

- [ ] **Step 4: Run to confirm pass**

Run: `python -m pytest C:\Users\Falcon\Documents\pool-controller\esp32-experiment\tests\test_status.py -q`
Expected: PASS. Then run the full suite: `python -m pytest C:\Users\Falcon\Documents\pool-controller\esp32-experiment\tests -q` -> all PASS.

- [ ] **Step 5: Commit**

```bash
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment commit -m "feat(pool-sniffer): decode keypad CMD_STATUS LED bits (Python)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- jandy/status.py tests/test_status.py
```

### Task C2: C++ decoder mirror + on-device selftest vector

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h`
- Modify: `components/jandy_aqualink/jandy_proto.cpp`

- [ ] **Step 1: Declare the struct and function**

In `jandy_proto.h`, after the `Decoded` struct, add:
```cpp
// Equipment LED states decoded from the CMD_STATUS (0x02) bitmap the panel sends
// an AllButton keypad. Per-panel (byte,bit) offsets are the live 2026-06-01
// Discovery capture (mirror of jandy/status.py CIRCUIT_BITS). The caller gates on
// dest == keypad address; this only checks cmd == CMD_STATUS.
struct KeypadStatus {
  bool valid = false;
  bool spa_mode = false, air_blower = false, filter_pump = false, cleaner = false;
};
KeypadStatus decode_keypad_status(const Frame &f);
```

- [ ] **Step 2: Implement, mirroring the Python bit table**

In `jandy_proto.cpp` (near the other free functions), add, using the SAME (byte,bit) values placed in `jandy/status.py` `CIRCUIT_BITS`:
```cpp
namespace {
struct CircuitBit { uint8_t byte_i, bit_i; };
inline bool bit_on(const Frame &f, CircuitBit cb) {
  return f.data_len() > cb.byte_i && ((f.data()[cb.byte_i] >> cb.bit_i) & 1u);
}
// (byte,bit) from the 2026-06-01 Discovery capture; keep in sync with status.py.
constexpr CircuitBit SPA_MODE_BIT    = {0, 0};   // REPLACE with Discovery values
constexpr CircuitBit AIR_BLOWER_BIT  = {0, 0};
constexpr CircuitBit FILTER_PUMP_BIT = {0, 0};
constexpr CircuitBit CLEANER_BIT     = {0, 0};
}  // namespace

KeypadStatus decode_keypad_status(const Frame &f) {
  KeypadStatus s;
  if (f.cmd() != CMD_STATUS) return s;
  s.valid = true;
  s.spa_mode = bit_on(f, SPA_MODE_BIT);
  s.air_blower = bit_on(f, AIR_BLOWER_BIT);
  s.filter_pump = bit_on(f, FILTER_PUMP_BIT);
  s.cleaner = bit_on(f, CLEANER_BIT);
  return s;
}
```

- [ ] **Step 3: Add a selftest vector block**

In `jandy_proto.cpp` `selftest()`, before the final `detail = ...` line, add (paste the SAME wire bytes used for `STATUS_08_BLOWER_OFF` / `_ON` in fixtures):
```cpp
  // Keypad CMD_STATUS LED decode: the blower bit reads off then on across the
  // two captured Service-mode samples. Same oracle as tests/test_status.py.
  {
    total++;
    auto decode_wire = [](const std::vector<uint8_t> &w) {
      FrameExtractor ex;
      std::vector<Frame> fr;
      ex.feed(w.data(), w.size(), fr);
      return fr.empty() ? KeypadStatus{} : decode_keypad_status(fr[0]);
    };
    const std::vector<uint8_t> off = { /* STATUS_08_BLOWER_OFF wire bytes */ };
    const std::vector<uint8_t> on  = { /* STATUS_08_BLOWER_ON  wire bytes */ };
    KeypadStatus a = decode_wire(off), b = decode_wire(on);
    if (a.valid && !a.air_blower && b.valid && b.air_blower) ok++;
    else detail += " STATUS08";
  }
```

- [ ] **Step 4: Commit (on-device verification happens at the Task C4 deploy)**

```bash
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment commit -m "feat(pool-sniffer): mirror keypad status decoder in C++ + selftest

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
```

### Task C3: Publish four binary_sensors and log transitions

**Files:**
- Modify: `components/jandy_aqualink/__init__.py`
- Modify: `components/jandy_aqualink/jandy_aqualink.h`
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp`
- Modify: `firmware/pool-bridge.yaml`

- [ ] **Step 1: Register the entities in codegen**

In `__init__.py`: add `binary_sensor` to the import and to `AUTO_LOAD`, define four CONF keys, four schema entries, and four `to_code` registrations. Specifically:

After `from esphome.components import sensor` add:
```python
from esphome.components import binary_sensor
```
Change `AUTO_LOAD = ["sensor", "number"]` to:
```python
AUTO_LOAD = ["sensor", "number", "binary_sensor"]
```
After the `CONF_PUMP_WATTS = "pump_watts"` line add:
```python
CONF_SPA_MODE = "spa_mode"
CONF_AIR_BLOWER = "air_blower"
CONF_FILTER_PUMP_STATE = "filter_pump_state"
CONF_CLEANER_STATE = "cleaner_state"
```
Inside `CONFIG_SCHEMA`, after the `CONF_PUMP_WATTS` entry add:
```python
        cv.Optional(CONF_SPA_MODE): binary_sensor.binary_sensor_schema(
            icon="mdi:hot-tub",
        ),
        cv.Optional(CONF_AIR_BLOWER): binary_sensor.binary_sensor_schema(
            icon="mdi:weather-windy",
        ),
        cv.Optional(CONF_FILTER_PUMP_STATE): binary_sensor.binary_sensor_schema(
            icon="mdi:pump",
        ),
        cv.Optional(CONF_CLEANER_STATE): binary_sensor.binary_sensor_schema(
            icon="mdi:robot-vacuum",
        ),
```
At the end of `to_code` add:
```python
    if CONF_SPA_MODE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_SPA_MODE])
        cg.add(var.set_spa_mode_bs(b))
    if CONF_AIR_BLOWER in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_AIR_BLOWER])
        cg.add(var.set_air_blower_bs(b))
    if CONF_FILTER_PUMP_STATE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_FILTER_PUMP_STATE])
        cg.add(var.set_filter_pump_bs(b))
    if CONF_CLEANER_STATE in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_CLEANER_STATE])
        cg.add(var.set_cleaner_bs(b))
```

- [ ] **Step 2: Add members, setters, and the include**

In `jandy_aqualink.h`, after `#include "esphome/components/sensor/sensor.h"` add:
```cpp
#include "esphome/components/binary_sensor/binary_sensor.h"
```
After `set_pump_watts_sensor(...)` add:
```cpp
  void set_spa_mode_bs(binary_sensor::BinarySensor *b) { spa_mode_bs_ = b; }
  void set_air_blower_bs(binary_sensor::BinarySensor *b) { air_blower_bs_ = b; }
  void set_filter_pump_bs(binary_sensor::BinarySensor *b) { filter_pump_bs_ = b; }
  void set_cleaner_bs(binary_sensor::BinarySensor *b) { cleaner_bs_ = b; }
```
After `sensor::Sensor *pump_watts_sensor_{nullptr};` add:
```cpp
  binary_sensor::BinarySensor *spa_mode_bs_{nullptr};
  binary_sensor::BinarySensor *air_blower_bs_{nullptr};
  binary_sensor::BinarySensor *filter_pump_bs_{nullptr};
  binary_sensor::BinarySensor *cleaner_bs_{nullptr};
  // Decoded keypad-status circuit states, written by core 1 under mux_, published
  // by core 0. -1 = not yet known, 0 = off, 1 = on.
  volatile int8_t cs_spa_{-1}, cs_blower_{-1}, cs_pump_{-1}, cs_cleaner_{-1};
  int8_t pub_cs_spa_{-2}, pub_cs_blower_{-2}, pub_cs_pump_{-2}, pub_cs_cleaner_{-2};
```

- [ ] **Step 3: Decode + store + log transitions in observe_frame**

In `jandy_aqualink.cpp` `observe_frame`, replace the Build 1 status block from Task A1 (the `if (f.dest() == keypad_addr_ && f.cmd() == jandy::CMD_STATUS && f.raw != last_status_raw_)` block) with a version that keeps the on-change raw log AND decodes:
```cpp
  if (f.dest() == keypad_addr_ && f.cmd() == jandy::CMD_STATUS) {
    jandy::KeypadStatus ks = jandy::decode_keypad_status(f);
    if (f.raw != last_status_raw_) {
      last_status_raw_ = f.raw;
      char hex[3 * 32 + 1];
      static const char *const H = "0123456789ABCDEF";
      size_t n = f.raw.size() > 32 ? 32 : f.raw.size();
      size_t hp = 0;
      for (size_t i = 0; i < n; ++i) {
        uint8_t b = f.raw[i];
        hex[hp++] = H[b >> 4];
        hex[hp++] = H[b & 0x0F];
        hex[hp++] = ' ';
      }
      hex[hp] = '\0';
      ESP_LOGW(TAG, "STATUS08 change len=%u: %s", static_cast<unsigned>(f.raw.size()), hex);
    }
    if (ks.valid) {
      int8_t spa = ks.spa_mode, blow = ks.air_blower, pump = ks.filter_pump, clean = ks.cleaner;
      portENTER_CRITICAL(&mux_);
      bool spa_ch = spa != cs_spa_, blow_ch = blow != cs_blower_,
           pump_ch = pump != cs_pump_, clean_ch = clean != cs_cleaner_;
      cs_spa_ = spa; cs_blower_ = blow; cs_pump_ = pump; cs_cleaner_ = clean;
      portEXIT_CRITICAL(&mux_);
      if (spa_ch) ESP_LOGW(TAG, "STATUS CHANGE: spa_mode -> %d", spa);
      if (blow_ch) ESP_LOGW(TAG, "STATUS CHANGE: air_blower -> %d", blow);
      if (pump_ch) ESP_LOGW(TAG, "STATUS CHANGE: filter_pump -> %d", pump);
      if (clean_ch) ESP_LOGW(TAG, "STATUS CHANGE: cleaner -> %d", clean);
    }
  }
```

- [ ] **Step 4: Publish on change in loop()**

In `jandy_aqualink.cpp` `loop()`, alongside the existing publish-on-change blocks, add:
```cpp
  {
    int8_t spa, blow, pump, clean;
    portENTER_CRITICAL(&mux_);
    spa = cs_spa_; blow = cs_blower_; pump = cs_pump_; clean = cs_cleaner_;
    portEXIT_CRITICAL(&mux_);
    if (spa_mode_bs_ && spa >= 0 && spa != pub_cs_spa_) { pub_cs_spa_ = spa; spa_mode_bs_->publish_state(spa != 0); }
    if (air_blower_bs_ && blow >= 0 && blow != pub_cs_blower_) { pub_cs_blower_ = blow; air_blower_bs_->publish_state(blow != 0); }
    if (filter_pump_bs_ && pump >= 0 && pump != pub_cs_pump_) { pub_cs_pump_ = pump; filter_pump_bs_->publish_state(pump != 0); }
    if (cleaner_bs_ && clean >= 0 && clean != pub_cs_cleaner_) { pub_cs_cleaner_ = clean; cleaner_bs_->publish_state(clean != 0); }
  }
```

- [ ] **Step 5: Add the entities to the repo yaml (source of truth)**

In `firmware/pool-bridge.yaml`, under the `jandy_aqualink:` component block (next to the temp/pump sensors), add:
```yaml
  spa_mode:
    name: Pool Spa Mode
  air_blower:
    name: Pool Air Blower (status)
  filter_pump_state:
    name: Pool Filter Pump (status)
  cleaner_state:
    name: Pool Cleaner (status)
```

- [ ] **Step 6: Build the Python suite (no C++ run here) and commit**

Run: `python -m pytest C:\Users\Falcon\Documents\pool-controller\esp32-experiment\tests -q`
Expected: all PASS (unchanged; this task is C++/codegen/yaml).
```bash
git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment commit -m "feat(pool-sniffer): publish spa/blower/pump/cleaner binary_sensors + transition logs

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/__init__.py components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp firmware/pool-bridge.yaml
```

### Task C4: Deploy Build 2 and patch the live dashboard yaml

**Files:** none in repo beyond the push; the live dashboard yaml is patched in place.

- [ ] **Step 1: Push, then back up the live dashboard yaml**

Run: `git -C C:/Users/Falcon/Documents/pool-controller/esp32-experiment push`
Then download the live dashboard config to a timestamped backup (string download, not byte): use `(New-Object System.Net.WebClient).DownloadString("http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml")` and save to `C:\Users\Falcon\Documents\pool-controller\dashboard-pool-bridge.BACKUP-<stamp>.yaml`.

- [ ] **Step 2: Add the four binary_sensor entries to the live yaml and POST it back**

Insert the same four-entry block from Task C3 Step 5 under the dashboard yaml's `jandy_aqualink:` block, then save with `(New-Object System.Net.WebClient).UploadString("http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml","POST",$body)`. Read it back with `DownloadString` and confirm the four names are present byte-exact.

- [ ] **Step 3: Compile, upload, verify**

Compile then upload per Deploy mechanics. In the logs confirm `selftest ... PASS -> N/N` (the new STATUS08 vector is included), `checksum_errors` 0, reply latency unchanged. Do not proceed on a FAIL.

### Task C5: Live verification, then the capture

**Files:** none

- [ ] **Step 1: Service-mode correctness check (founder)**

With the panel still in Service mode, the founder toggles the blower and pool/spa by hand. Confirm in Home Assistant that `Pool Air Blower (status)` and `Pool Spa Mode` flip to match every time, and the log shows the matching `STATUS CHANGE:` lines. If any sensor is inverted or stuck, fix the bit offset in both `jandy/status.py` and `jandy_proto.cpp`, re-run Task C1 Step 4 (Python green), redeploy (C4), and recheck.

- [ ] **Step 2: Start the capture (founder returns panel to Auto)**

The founder returns the panel to Auto. The monitor now records spa-mode and blower events in Home Assistant history with timestamps. Confirm the binary_sensors are populating and history is recording. The founder can flip back to Service mode for quiet at any time (coverage pauses during those stretches).

- [ ] **Step 3: Read the timeline**

After a capture window (a day, or whatever the founder allows), read the Home Assistant history for `Pool Spa Mode` and `Pool Air Blower (status)` and report the pattern of when they fire. This is the deliverable: a clear, charted record that the panel drives these on its own clock, and the timing data needed to choose the next step (HA-as-scheduler override, or the deeper menu-wipe).

---

## Self-review

- **Spec coverage:** Read-only monitor on the inert seat (Tasks A1-C5, no TX added); spa-mode + blower + pump + cleaner readouts (C3); timestamped transition logs (C3 Step 3); Service-mode bit identification (Part B); Auto-mode capture + timeline (C5); proof it is the panel not us (the inert-seat design plus C5 timeline). The "what it cannot do" (naming the program) is correctly left out of scope. Covered.
- **Placeholder scan:** The only deferred values are the four `(byte,bit)` offsets and the captured `STATUS_08_*` wire bytes. These are not lazy placeholders: they are the explicit deliverable of the Discovery task (B1 Steps 3-5), which writes them into `fixtures.py` and `status.py` before Part C consumes them. Every code step otherwise contains complete content. The `REPLACE with Discovery values` markers are paired with the task that produces them.
- **Type consistency:** `decode_keypad_status` returns a dict in Python and a `KeypadStatus` struct in C++ (intentional, mirrors the existing `decode_status` dict / `Reader` struct split). `CIRCUIT_BITS` (Python) and the `CircuitBit` constants (C++) must hold identical offsets; Task C2 Step 2 states this and Task C5 Step 1 re-checks both if a sensor is wrong. Setter names (`set_spa_mode_bs` etc.) match between `__init__.py` to_code and the header. `CMD_STATUS` is defined once in `jandy_proto.h` and used in both the component and the decoder.

## Execution handoff

Two execution options:

1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks. Note: subagents are currently overflowing on this session's large base context, so this may not be usable until that clears.
2. **Inline Execution** — execute tasks in this session with checkpoints for review. The natural checkpoints are: after Task A2 (logger live), Part B (founder at the panel, Service mode), the GATE, and Task C5 (founder returns to Auto).

Tasks A1-A2 (the logger) and C1-C2 (the decoder logic) need no founder presence. Part B and C5 need the founder at the panel. Recommended flow: build and deploy the logger (A1-A2) now, then pause for the founder to run Discovery (Part B) whenever he is ready to put the panel briefly back in Auto/Service for the toggle test.
