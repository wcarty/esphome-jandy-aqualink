# 🛠️ ESP32 Jandy keypad presence and reads: Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make the Aqualink RS-8 panel register an emulated keypad on the M5Stack Atom Lite, so it streams display and status, and decode those into Home Assistant. Reads only, no writes.

**Architecture:** An ESPHome external component (Arduino framework) owns the RS485 UART via the ESP-IDF driver and runs the time-critical poll-and-reply in a FreeRTOS task pinned to core 1. ESPHome on core 0 handles WiFi, the HA native API, OTA with rollback, and a raw-frame debug feed. The component is hosted in a git repo so the dashboard pulls it via `external_components`; that repo is also the publishable example.

**Tech Stack:** ESPHome (Arduino/ESP-IDF), C++17, FreeRTOS, ESP-IDF `driver/uart.h`, Home Assistant native API. Python 3 reference oracle already in `jandy/` (32 passing tests).

**Why no host unit tests:** this machine has no C++ compiler and no ESPHome CLI. The build/flash path is the ESPHome dashboard at `192.168.1.126:6052` (HTTP API, no auth). The C++ protocol port is validated by an on-device self-test that runs the same frame vectors as the Python suite and logs PASS/FAIL, watched over the dashboard log stream.

---

## Pre-flight decisions (confirm before Task 1)

1. **Deploy vehicle = a git repo the dashboard pulls.** Needs to be reachable by the dashboard at compile time. Public GitHub repo is simplest (no token). This is outward-facing, so confirm public-vs-private with the user (private needs a token wired into the dashboard's git).
2. **Keypad address.** Must be one no real keypad answers. The live capture showed zero display/status to any address, meaning nothing (real or emulated) is currently registered, so 0x08-0x0B are all free right now. Default `0x08`, make it a setting, retrieve AqualinkD's configured `device_id` if easily available, and watch for a collision after deploy.
3. **ACK shape confirmed** from the brief's AqualinkD constant `{00,10,02,00,01,00,00,13,10,03,00}`: dest `0x00`, cmd `0x01`, data `[ack_type=0x00, key=0x00]`, checksum `0x13`. Key byte stays `0x00` (no key) for the entire reads-only build.

## File structure

```
esp32-experiment/
  components/jandy_aqualink/
    __init__.py          # ESPHome config schema + codegen
    jandy_proto.h        # pure logic: consts, Frame, FrameExtractor, decoders, ACK, selftest
    jandy_proto.cpp      # pure logic impl, transliterated from jandy/*.py
    jandy_aqualink.h     # ESPHome component: owns UART, core-1 task, sensors, debug feed
    jandy_aqualink.cpp   # setup()/loop() glue
  firmware/
    pool-bridge.yaml             # new device config (deployed via dashboard)
    pool-bridge-original.yaml    # verbatim current bridge config, for instant rollback
```

`jandy_proto.*` has NO Arduino/ESPHome includes, so it is a faithful mirror of the proven Python and is self-test-validated. `jandy_aqualink.*` is the only hardware/ESPHome glue.

---

## Task 0: Save the current config and scaffold the deploy repo

**Files:**
- Create: `firmware/pool-bridge-original.yaml`
- Create: GitHub repo (deploy vehicle), pushed from `esp32-experiment/`

- [ ] **Step 1: Save the current working bridge config for rollback**

Fetch the live config and save it verbatim (redact nothing; it is local and gitignored if it carries the inline WiFi password). Use the dashboard API:
```
GET http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml   (-UseBasicParsing; decode bytes to text)
```
Write the result to `firmware/pool-bridge-original.yaml`. Add that path to `.gitignore` (it contains the inline WiFi password) so it stays local-only.

- [ ] **Step 2: Confirm public-vs-private repo with the user, then create and push**

```bash
gh repo create <name> --public --source=. --remote=origin --push   # or --private + token wiring
```
Expected: repo exists, `git push` succeeds, `external_components` can resolve `github://<user>/<name>`.

- [ ] **Step 3: Commit the scaffold**
```bash
git add firmware/.gitkeep .gitignore
git commit -m "Add deploy repo scaffold and saved rollback config reference"
```

## Task 1: Port the frame layer to C++ with an on-device self-test

**Files:**
- Create: `components/jandy_aqualink/jandy_proto.h`
- Create: `components/jandy_aqualink/jandy_proto.cpp`

The frame state machine mirrors `jandy/frames.py` exactly. Below is the full C++ (it is small and the core of everything):

- [ ] **Step 1: Write `jandy_proto.h` (frame layer + selftest decl)**

```cpp
#pragma once
#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace jandy {

static constexpr uint8_t DLE = 0x10, STX = 0x02, ETX = 0x03, STUFF = 0x00;

struct Frame {
  std::vector<uint8_t> raw;  // logical frame: 10 02 dest cmd data... cksum 10 03
  uint8_t dest() const { return raw[2]; }
  uint8_t cmd()  const { return raw[3]; }
  uint8_t checksum() const { return raw[raw.size() - 3]; }
  bool checksum_valid() const {
    uint32_t s = 0;
    for (size_t i = 0; i + 3 < raw.size(); ++i) s += raw[i];
    return (s & 0xFF) == raw[raw.size() - 3];
  }
};

class FrameExtractor {
 public:
  // Feed bytes; append complete logical frames to `out`.
  void feed(const uint8_t *data, size_t len, std::vector<Frame> &out);
 private:
  enum State { SEARCH, DLE_OUT, IN, DLE_IN } state_{SEARCH};
  std::vector<uint8_t> buf_;
};

// Returns true and fills `pass`/`detail` with self-test results over known vectors.
bool selftest(std::string &detail);

}  // namespace jandy
```

- [ ] **Step 2: Write the `FrameExtractor::feed` impl in `jandy_proto.cpp`**

Transliterate the Python state machine in `jandy/frames.py` (SEARCH/DLE_OUT/IN/DLE_IN). Same transitions: STUFF after in-frame DLE -> push literal DLE; ETX -> emit; STX mid-frame -> resync; DLE-not-STX while searching -> back to SEARCH.

- [ ] **Step 3: Write `selftest()` covering the captured + stuffing vectors**

```cpp
bool jandy::selftest(std::string &detail) {
  struct V { std::vector<uint8_t> wire; bool valid; uint8_t dest, cmd; };
  const std::vector<V> vectors = {
    {{0x10,0x02,0x60,0x00,0x72,0x10,0x03}, true, 0x60, 0x00},          // poll pump
    {{0x10,0x02,0x33,0x25,0x05,0x41,0x69,0x72,0x20,0x54,0x65,0x6D,0x70,0x00,0x41,0x10,0x03}, true, 0x33, 0x25}, // "Air Temp"
    {{0x10,0x02,0x38,0x0C,0x12,0x57,0x66,0x5B,0x80,0x10,0x03}, true, 0x38, 0x0C}, // pool temp 0x5B
    {{0x10,0x02,0x33,0x25,0x10,0x00,0x41,0xBB,0x10,0x03}, true, 0x33, 0x25},      // stuffed data 0x10
  };
  int ok = 0;
  for (auto &v : vectors) {
    jandy::FrameExtractor ex; std::vector<jandy::Frame> fr;
    ex.feed(v.wire.data(), v.wire.size(), fr);
    if (fr.size()==1 && fr[0].checksum_valid()==v.valid && fr[0].dest()==v.dest && fr[0].cmd()==v.cmd) ok++;
  }
  detail = "frames " + std::to_string(ok) + "/" + std::to_string(vectors.size());
  return ok == (int)vectors.size();
}
```

- [ ] **Step 4: Validation gate (deferred to Task 6 compile)** — selftest is compiled into the firmware and must log `frames 4/4` on boot.

## Task 2: Port poll-detection and the presence ACK

**Files:** Modify `jandy_proto.h`, `jandy_proto.cpp`

- [ ] **Step 1: Add to `jandy_proto.h`**

```cpp
inline bool is_poll_to(const Frame &f, uint8_t keypad_addr) {
  return f.cmd() == 0x00 && f.dest() == keypad_addr;
}
// Fixed inert presence ACK: dest 0x00, cmd 0x01, [ack_type=0x00, key=0x00], cksum 0x13.
static const uint8_t ACK_PRESENCE[9] = {0x10,0x02,0x00,0x01,0x00,0x00,0x13,0x10,0x03};
```

- [ ] **Step 2: Extend `selftest()`** to assert `is_poll_to(pollframe, 0x60)` is true, `is_poll_to(displayframe, 0x60)` is false, and that the checksum of `ACK_PRESENCE` validates (sum of first 6 bytes & 0xFF == 0x13). Append to detail: `ack ok`.

## Task 3: Port the display and status decoders

**Files:** Modify `jandy_proto.h`, `jandy_proto.cpp`

- [ ] **Step 1: Add decoder declarations**

```cpp
struct Decoded { bool has_air=false, has_pool=false, has_spa=false; int air=0, pool=0, spa=0; };
// Stateful reader: feed frames, it updates fields as label/value pairs and status frames arrive.
class Reader {
 public:
  void feed(const Frame &f);
  Decoded state;
 private:
  std::string pending_key_;  // "" none, else "air_temp"/"pool_temp"/"spa_temp"
};
```

- [ ] **Step 2: Implement `Reader::feed`** mirroring `jandy/display.py` (label map AIR/POOL/SPA TEMP, leading-int parse, strict adjacency pairing, ignore non-display frames) and `jandy/status.py` (dest 0x38 cmd 0x0C -> pool = data[3]).

- [ ] **Step 3: Extend `selftest()`**: feed the "Air Temp" then "167" vectors -> assert `state.air==167`; feed the 0x38/0x0C vector -> assert `state.pool==91`. Append `decode ok`.

## Task 4: ESPHome component glue (UART + core-1 task)

**Files:**
- Create: `components/jandy_aqualink/jandy_aqualink.h`
- Create: `components/jandy_aqualink/jandy_aqualink.cpp`

- [ ] **Step 1: Component header** with setters for pins, baud, keypad address, sensors, and a debug-feed port; a `Reader`; a FreeRTOS task handle; a mutex-guarded snapshot of `Decoded` + counters (frames, bad_cksum, acks_sent, last_reply_us).

- [ ] **Step 2: `setup()` installs the IDF UART driver and the pinned task**

```cpp
uart_config_t c{}; c.baud_rate=baud_; c.data_bits=UART_DATA_8_BITS;
c.parity=UART_PARITY_DISABLE; c.stop_bits=UART_STOP_BITS_1; c.flow_ctrl=UART_HW_FLOWCTRL_DISABLE;
uart_param_config(UART_NUM_1, &c);
uart_set_pin(UART_NUM_1, tx_pin_, rx_pin_, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
uart_driver_install(UART_NUM_1, 2048, 0, 0, nullptr, 0);
xTaskCreatePinnedToCore(&JandyAqualink::task_main, "jandy_bus", 4096, this, 20, &task_, 1);
```

- [ ] **Step 3: `task_main` loop** — `uart_read_bytes(UART_NUM_1, b, n, 5ms)` -> `extractor_.feed` -> for each frame: if `is_poll_to(f, keypad_addr_)` then timestamp, `uart_write_bytes(UART_NUM_1, ACK_PRESENCE, 9)`, record `last_reply_us`; else if `f.checksum_valid()` `reader_.feed(f)`; update counters under the mutex; copy raw bytes to the debug ring buffer.

- [ ] **Step 4: DE/auto-direction contingency (documented):** if presence does not register (Task 7), add `uart_set_mode(UART_NUM_1, UART_MODE_RS485_HALF_DUPLEX)` and a DE GPIO via `uart_set_pin` RTS. Only if needed.

## Task 5: Sensors, debug feed, and on-boot selftest log

**Files:** Modify `jandy_aqualink.cpp`, `__init__.py`

- [ ] **Step 1: `loop()`** publishes the mutex-guarded snapshot to the air/pool/spa temperature sensors (only on change) and exposes counters as diagnostic sensors.
- [ ] **Step 2:** run `jandy::selftest(detail)` once in `setup()` and `ESP_LOGI("jandy", "selftest %s -> %s", ok?"PASS":"FAIL", detail.c_str())`.
- [ ] **Step 3: Debug feed** — a tiny TCP server (port 8889) on core 0 that broadcasts each raw frame hex, plus a marker line when we send an ACK, so live behavior is watchable remotely with a tweak of `capture.py`.
- [ ] **Step 4: `__init__.py` codegen** — component class, config schema (tx/rx pins, baud default 9600, keypad_address default 0x08, debug_port default 8889, optional sensor blocks for air/pool/spa + counters), `to_code` wiring setters and `sensor.new_sensor`.

## Task 6: Compile-only via the dashboard, confirm selftest

**Files:** `firmware/pool-bridge.yaml`

- [ ] **Step 1: Write `pool-bridge.yaml`** — copy `pool-bridge-original.yaml`, keep `esphome/esp32/wifi/api/ota/logger/safe_mode/captive_portal` and the AP fallback unchanged; REMOVE `uart:` and `stream_server:`; set `external_components: source: github://<user>/<name>, refresh: 0s`; add the `jandy_aqualink:` block and its sensors.
- [ ] **Step 2: Push YAML to the dashboard** (`POST /edit?configuration=pool-bridge.yaml`) and trigger a compile-only run via the dashboard API. Expected: compile succeeds.
- [ ] **Step 3:** if compile fails, fix C++/codegen and repeat. Do not flash yet.

## Task 7: OTA deploy and live presence validation

- [ ] **Step 1: Confirm AqualinkD is stopped** (it is) so only one device answers the address.
- [ ] **Step 2: OTA via dashboard.** Watch logs: expect `selftest PASS -> frames 4/4 ack ok decode ok`.
- [ ] **Step 3: Watch the debug feed (port 8889).** Success: the panel keeps polling our address (does not drop us) AND begins sending display/status frames to it. This is the live proof of presence and of the auto-direction assumption.
- [ ] **Step 4: Measure reply latency** (`last_reply_us`) — confirm well under the 20-40 ms window.
- [ ] **Step 5: Collision watch** — bad-checksum rate must stay near 0. A spike means another device answers our address; revert (Task 9 rollback) and pick another address.

## Task 8: Confirm values in Home Assistant and refine

- [ ] **Step 1:** confirm air/pool/spa temperature sensors populate in HA with real values (pool about 91 F).
- [ ] **Step 2:** capture any newly-seen value frames (setpoint, salt, pump) from the debug feed; add them as vectors to the Python suite and the C++ selftest; extend decoders; redeploy.
- [ ] **Step 3: Commit** working firmware and updated decoders.

## Rollback plan

If the bus degrades or presence misbehaves: push `pool-bridge-original.yaml` back via the dashboard and OTA. That restores today's read-only bridge. The founder can restart AqualinkD from the Unraid Docker tab to return fully to the prior state.

## Out of scope

Writes/navigation (setpoint changes, equipment toggles), salt-via-menu, and the production Raspberry Pi track. Never run a second keypad emulator on the bus at the same time.
