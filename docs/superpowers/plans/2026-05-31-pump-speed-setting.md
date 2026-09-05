# 🛠️ Pump Speed Setting Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Set the filter pump speed (RPM) from Home Assistant via four preset buttons plus a slider, behind the existing master interlock, with a read-back to confirm.

**Architecture:** Pure protocol logic (RPM clamp/snap, the ASCII digit encoder, the 0x24 value frame builder, the page-gate predicate) is written and TDD'd in Python (`jandy/frames.py`), then mirrored byte-for-byte in C++ (`jandy_proto.*`) and checked by the on-device selftest. A small multi-step, page-driven state machine in the component (`jandy_aqualink.*`) sequences the live handshake: navigate Home then Other Devices, confirm the DEVICES page before sending the page-scoped 0x13 key, open SET_VSP, request control, send the digits on the panel's 0x31 reply, then read STATUS back and return Home. Four template buttons and one template number in the YAML call one gated entry point, `set_pump_rpm`.

**Tech Stack:** Python 3 + `unittest` (host tests), C++ in an ESPHome external component (ESP-IDF UART on a core-1 FreeRTOS task), ESPHome YAML (template button + number), deployed via the ESPHome dashboard.

---

## Background the engineer needs (read before starting)

- **Spec:** `docs/superpowers/specs/2026-05-31-pump-speed-setting-design.md`. Read it first.
- **The pump** is a Pentair Intelliflo VS in RPM mode. Safe range 600 to 3450 RPM.
- **The wire format** (see the header of `jandy/frames.py`): `10 02 <dest> <cmd> <data...> <cksum> 10 03`. Checksum = sum of every byte before the checksum, masked to one byte. `build_frame(dest, cmd, data)` already does this.
- **The iAqualink reply convention** (confirmed in the on-device selftest ACK vectors): every reply the device sends the panel uses `build_ack(ACK_IAQ_TOUCH=0x00, key)`, i.e. `10 02 00 01 00 <key> <ck> 10 03`. The meaningful byte is the key (byte 5). `send_iaq_ack_(key)` in the component is exactly this.
- **The set handshake** (from the captured frames, which are the source of truth):
  1. Reach the DEVICES page (0x36). Press the VSP-adjust key, which is byte **0x13**. CRITICAL: 0x13 is Pool Heat on the HOME page and VSP-adjust only on DEVICES, so it must never be sent off the DEVICES page.
  2. The panel opens SET_VSP (0x1E).
  3. Send the control-request reply `send_iaq_ack_(0x80)` (the bytes `10 02 00 01 00 80 93 10 03`) on an ordinary poll.
  4. The panel addresses 0x33 with CMD_IAQ_CTRL_READY (cmd **0x31**).
  5. Reply to the 0x31 with the **0x24 value frame** carrying the ASCII RPM digits. NOT another 0x80.
  6. Read STATUS back to confirm, then return Home.
- **Captured 0x24 value frames** (verified by reconstructing each checksum; these are the test oracle):
  - 1600 -> `10 02 00 24 31 31 36 30 30 00` + eleven `cd` + `fd 10 03`
  - 2000 -> same shape, digits `32 30 30 30 00`, checksum `f8`
  - 3000 -> digits `33 30 30 30 00`, checksum `f9`
  - 1000 -> digits `31 30 30 30 00`, checksum `f7`
  - 600  -> digits `36 30 30 00 00` (the under-1000 case: three digits, two NULs), checksum `cc`
  - Structure: dest `00`, cmd `24`, sub-byte `31`, then a 5-byte digit field (ASCII digits, NUL-padded to width 5), then eleven `0xcd` padding bytes, then checksum and `10 03`. Total 24 bytes. No trailing NUL.
- **Run the host tests:** `python -m unittest discover -s tests -t .` (from the repo root).
- **The Python stubs already exist** in `jandy/frames.py` and currently `raise NotImplementedError`: `num2iaqt_rpm`, `iaq_ctrl_ready_ack`, `build_vsp_set_frame`, `vsp_adjust_allowed`. Tasks 1 to 5 fill them (plus one new `rpm_check`).

## File structure

- `jandy/frames.py` (modify): fill the four stubs, add `rpm_check`. Pure logic, the reference the C++ mirrors.
- `tests/test_pump_set.py` (create): unit tests for all of the above, including the captured-frame oracle.
- `components/jandy_aqualink/jandy_proto.h` (modify): declare the mirrored functions; add the two missing IAQ page constants if absent.
- `components/jandy_aqualink/jandy_proto.cpp` (modify): implement the mirrored functions; add VSP vectors to `selftest`.
- `components/jandy_aqualink/jandy_aqualink.h` (modify): declare `set_pump_rpm`, the sequence helpers, and the sequence state members.
- `components/jandy_aqualink/jandy_aqualink.cpp` (modify): implement `set_pump_rpm`, `advance_set_sequence_`, `send_vsp_set_`; wire them into `handle_iaq_frame_`; add the 0x31 command constant; clear the sequence in `set_interlock(false)`.
- `firmware/pool-bridge.yaml` (modify) and the live dashboard YAML: four preset buttons + one slider.
- `components/jandy_aqualink/__init__.py` (modify): add `number` to AUTO_LOAD.

---

### Task 1: Python `rpm_check` (clamp 600-3450, snap to 5)

**Files:**
- Modify: `jandy/frames.py`
- Test: `tests/test_pump_set.py`

- [ ] **Step 1: Write the failing test**

Create `tests/test_pump_set.py`:

```python
"""Pump speed SET path: RPM clamp/snap, digit encoder, 0x24 value frame,
control-request ACK, and the page gate for the 0x13 VSP-adjust key.

The captured 0x24 frames are the oracle from the Session 5 kickoff (iaqtouch.h):
each checksum was reconstructed by hand and matches."""

import unittest

from jandy import frames


class TestRpmCheck(unittest.TestCase):
    def test_in_range_multiple_of_five_unchanged(self):
        self.assertEqual(frames.rpm_check(1600), 1600)
        self.assertEqual(frames.rpm_check(2750), 2750)

    def test_clamps_low_and_high(self):
        self.assertEqual(frames.rpm_check(0), 600)
        self.assertEqual(frames.rpm_check(599), 600)
        self.assertEqual(frames.rpm_check(9000), 3450)
        self.assertEqual(frames.rpm_check(3451), 3450)

    def test_snaps_to_nearest_five(self):
        self.assertEqual(frames.rpm_check(1622), 1620)
        self.assertEqual(frames.rpm_check(1623), 1625)
        self.assertEqual(frames.rpm_check(601), 600)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_pump_set -v`
Expected: FAIL with `AttributeError: module 'jandy.frames' has no attribute 'rpm_check'`

- [ ] **Step 3: Write minimal implementation**

In `jandy/frames.py`, add (place it just above the `num2iaqt_rpm` stub):

```python
RPM_MIN = 600
RPM_MAX = 3450


def rpm_check(rpm: int) -> int:
    """Clamp to the pump's safe 600-3450 range and snap to the nearest 5 RPM."""
    rpm = max(RPM_MIN, min(RPM_MAX, int(rpm)))
    return ((rpm + 2) // 5) * 5
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest tests.test_pump_set -v`
Expected: PASS (TestRpmCheck)

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_pump_set.py
git commit -m "feat(pump-set): rpm_check clamp 600-3450 and snap to 5"
```

---

### Task 2: Python `num2iaqt_rpm` (the 5-byte ASCII digit field)

**Files:**
- Modify: `jandy/frames.py:114` (the `num2iaqt_rpm` stub)
- Test: `tests/test_pump_set.py`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_pump_set.py`:

```python
class TestNum2IaqtRpm(unittest.TestCase):
    def test_four_digit_field_one_trailing_nul(self):
        self.assertEqual(frames.num2iaqt_rpm(1600), bytes.fromhex("3136303000"))
        self.assertEqual(frames.num2iaqt_rpm(2750), bytes.fromhex("3237353000"))
        self.assertEqual(frames.num2iaqt_rpm(3200), bytes.fromhex("3332303000"))
        self.assertEqual(frames.num2iaqt_rpm(1100), bytes.fromhex("3131303000"))

    def test_three_digit_field_two_trailing_nuls(self):
        # The under-1000 case: "600" then two NULs, total field width 5.
        self.assertEqual(frames.num2iaqt_rpm(600), bytes.fromhex("3630300000"))

    def test_field_is_always_five_bytes(self):
        for r in (600, 999, 1000, 3450):
            self.assertEqual(len(frames.num2iaqt_rpm(r)), 5)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_pump_set.TestNum2IaqtRpm -v`
Expected: FAIL with `NotImplementedError`

- [ ] **Step 3: Write minimal implementation**

Replace the `num2iaqt_rpm` stub body in `jandy/frames.py`:

```python
def num2iaqt_rpm(rpm: int) -> bytes:
    """ASCII digits of `rpm`, NUL-padded to a fixed 5-byte field (AqualinkD
    num2iaqtRSset). Three-digit speeds get two trailing NULs, four-digit one."""
    digits = str(int(rpm)).encode("ascii")
    return digits + b"\x00" * (5 - len(digits))
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest tests.test_pump_set.TestNum2IaqtRpm -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_pump_set.py
git commit -m "feat(pump-set): num2iaqt_rpm ASCII digit field"
```

---

### Task 3: Python `build_vsp_set_frame` (the full 0x24 value frame)

**Files:**
- Modify: `jandy/frames.py:124` (the `build_vsp_set_frame` stub)
- Test: `tests/test_pump_set.py`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_pump_set.py`:

```python
class TestBuildVspSetFrame(unittest.TestCase):
    # Captured frames (iaqtouch.h). Each is dest 00, cmd 24, sub 31, the 5-byte
    # digit field, eleven 0xcd, checksum, 10 03.
    CAPTURES = {
        1600: "10020024" + "31" + "3136303000" + "cd" * 11 + "fd" + "1003",
        2000: "10020024" + "31" + "3230303000" + "cd" * 11 + "f8" + "1003",
        3000: "10020024" + "31" + "3330303000" + "cd" * 11 + "f9" + "1003",
        1000: "10020024" + "31" + "3130303000" + "cd" * 11 + "f7" + "1003",
        600:  "10020024" + "31" + "3630300000" + "cd" * 11 + "cc" + "1003",
    }

    def test_matches_captured_frames(self):
        for rpm, hexstr in self.CAPTURES.items():
            self.assertEqual(
                frames.build_vsp_set_frame(rpm), bytes.fromhex(hexstr),
                f"frame mismatch for {rpm} RPM",
            )

    def test_frame_is_24_bytes(self):
        self.assertEqual(len(frames.build_vsp_set_frame(2750)), 24)

    def test_out_of_range_is_clamped_then_encoded(self):
        # 5000 clamps to 3450; 100 clamps to 600.
        self.assertEqual(frames.build_vsp_set_frame(5000), frames.build_vsp_set_frame(3450))
        self.assertEqual(frames.build_vsp_set_frame(100), frames.build_vsp_set_frame(600))

    def test_no_trailing_nul_before_checksum(self):
        # The byte right before the checksum (index -3) must be 0xcd, not 0x00.
        frame = frames.build_vsp_set_frame(1600)
        self.assertEqual(frame[-3], 0xCD)
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_pump_set.TestBuildVspSetFrame -v`
Expected: FAIL with `NotImplementedError`

- [ ] **Step 3: Write minimal implementation**

Replace the `build_vsp_set_frame` stub body in `jandy/frames.py`:

```python
CMD_IAQ_PAGE_BUTTON = 0x24
_VSP_SET_SUBBYTE = 0x31
_VSP_SET_PAD = b"\xcd" * 11


def build_vsp_set_frame(rpm: int) -> bytes:
    """The 0x24 value frame that sets the VSP speed: 10 02 00 24 31 <5-byte
    digit field> <eleven 0xcd> <cksum> 10 03. `rpm` is clamped/snapped first.
    No trailing NUL (the trailing-NUL form is a known VSP-drop footgun)."""
    safe = rpm_check(rpm)
    data = bytes([_VSP_SET_SUBBYTE]) + num2iaqt_rpm(safe) + _VSP_SET_PAD
    return build_frame(0x00, CMD_IAQ_PAGE_BUTTON, data)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest tests.test_pump_set.TestBuildVspSetFrame -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_pump_set.py
git commit -m "feat(pump-set): build_vsp_set_frame matches captured 0x24 frames"
```

---

### Task 4: Python `iaq_ctrl_ready_ack` (the control-request reply)

**Files:**
- Modify: `jandy/frames.py:119` (the `iaq_ctrl_ready_ack` stub)
- Test: `tests/test_pump_set.py`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_pump_set.py`:

```python
class TestCtrlReadyAck(unittest.TestCase):
    def test_exact_bytes(self):
        # Control-request reply: iAqualink ack (ack_type 0x00) carrying key 0x80.
        # Same convention as every other iAqualink ack (meaningful byte = key).
        self.assertEqual(frames.iaq_ctrl_ready_ack(), bytes.fromhex("100200010080931003"))

    def test_is_a_valid_ack_of_key_0x80(self):
        self.assertEqual(frames.iaq_ctrl_ready_ack(), frames.build_ack(frames.ACK_IAQ_TOUCH, 0x80))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_pump_set.TestCtrlReadyAck -v`
Expected: FAIL with `NotImplementedError`

- [ ] **Step 3: Write minimal implementation**

Replace the `iaq_ctrl_ready_ack` stub body in `jandy/frames.py`:

```python
def iaq_ctrl_ready_ack() -> bytes:
    """The control-request reply sent on an ordinary poll to ask the panel for
    the value-set control slot: an iAqualink ack carrying key 0x80
    (10 02 00 01 00 80 93 10 03). The panel answers with CMD_IAQ_CTRL_READY."""
    return build_ack(ACK_IAQ_TOUCH, 0x80)
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest tests.test_pump_set.TestCtrlReadyAck -v`
Expected: PASS

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_pump_set.py
git commit -m "feat(pump-set): iaq_ctrl_ready_ack control-request reply"
```

---

### Task 5: Python `vsp_adjust_allowed` (the page-context safety gate)

**Files:**
- Modify: `jandy/frames.py:131` (the `vsp_adjust_allowed` stub)
- Test: `tests/test_pump_set.py`

- [ ] **Step 1: Write the failing test**

Append to `tests/test_pump_set.py`:

```python
class TestVspAdjustAllowed(unittest.TestCase):
    def test_true_only_on_devices_page(self):
        self.assertTrue(frames.vsp_adjust_allowed(frames.IAQ_PAGE_DEVICES))  # 0x36

    def test_false_on_home_page_where_0x13_is_pool_heat(self):
        # This is the safety crux: 0x13 means Pool Heat on HOME, so it must be
        # refused there.
        self.assertFalse(frames.vsp_adjust_allowed(frames.IAQ_PAGE_HOME))  # 0x01

    def test_false_on_other_pages(self):
        for page in (frames.IAQ_PAGE_SET_VSP, frames.IAQ_PAGE_STATUS2, 0x00, 0x2D):
            self.assertFalse(frames.vsp_adjust_allowed(page))
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_pump_set.TestVspAdjustAllowed -v`
Expected: FAIL with `NotImplementedError`

- [ ] **Step 3: Write minimal implementation**

Replace the `vsp_adjust_allowed` stub body in `jandy/frames.py`:

```python
# The VSP-adjust keycode on the DEVICES page. Same byte as KEY_IAQ_POOL_HEAT
# (0x13): it means "VSP1 Spd ADJ" ONLY on DEVICES and "Pool Heat" on HOME, so it
# is named separately to make the page-gated intent explicit at call sites.
KEY_IAQ_DEVICES_VSP_ADJ = 0x13


def vsp_adjust_allowed(current_page: int) -> bool:
    """True only on the DEVICES page (0x36). The VSP-adjust key (0x13) is Pool
    Heat on HOME, so sending it anywhere but DEVICES could fire a heater."""
    return current_page == IAQ_PAGE_DEVICES
```

- [ ] **Step 4: Run the full Python suite to verify everything passes**

Run: `python -m unittest discover -s tests -t .`
Expected: OK (all existing tests plus the new `test_pump_set` ones)

- [ ] **Step 5: Commit**

```bash
git add jandy/frames.py tests/test_pump_set.py
git commit -m "feat(pump-set): vsp_adjust_allowed page gate for the 0x13 key"
```

---

### Task 6: C++ proto mirror + selftest vectors

The C++ has no host test runner; its check is the on-device `selftest()` (it logs `selftest PASS` at boot). This task adds the mirrored functions and selftest vectors whose expected bytes equal the Python-verified captures, so parity is cross-checked against the Task 1-5 tests.

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h`
- Modify: `components/jandy_aqualink/jandy_proto.cpp`

- [ ] **Step 1: Add the page constants if missing**

In `jandy_proto.h`, confirm whether `IAQ_PAGE_DEVICES` and `IAQ_PAGE_SET_VSP` already exist in the `jandy` namespace:

Run: `grep -nE "IAQ_PAGE_(DEVICES|SET_VSP|HOME|STATUS2)" components/jandy_aqualink/jandy_proto.h`

For any that are missing, add them near the other `IAQ_*` constants (the component already references `jandy::IAQ_PAGE_HOME` and `jandy::IAQ_PAGE_STATUS2`, so those two exist):

```cpp
static constexpr uint8_t IAQ_PAGE_DEVICES = 0x36;  // full equipment list
static constexpr uint8_t IAQ_PAGE_SET_VSP = 0x1E;  // VSP speed entry page
```

- [ ] **Step 2: Declare the mirrored functions in `jandy_proto.h`**

Add to the `jandy` namespace (near `build_ack`), and the VSP-adjust keycode alias:

```cpp
// VSP-adjust keycode on the DEVICES page. Same byte as KEY_IAQ_POOL_HEAT (0x13);
// named separately because it means "VSP1 Spd ADJ" only on DEVICES.
static constexpr uint8_t KEY_IAQ_DEVICES_VSP_ADJ = 0x13;

// Pump speed SET helpers (mirror of jandy/frames.py). See that file and the
// captured frames for the exact byte layout.
uint16_t rpm_check(uint16_t rpm);                 // clamp 600-3450, snap to 5
void num2iaqt_rpm(uint16_t rpm, uint8_t out[5]);  // ASCII digits, NUL-padded to 5
// Build the 0x24 value frame into out (needs >= 24 bytes). Returns bytes written.
size_t build_vsp_set_frame(uint16_t rpm, uint8_t *out, size_t out_cap);
// True only on the DEVICES page: gate for the page-scoped 0x13 VSP-adjust key.
bool vsp_adjust_allowed(uint8_t current_page);
```

- [ ] **Step 3: Implement them in `jandy_proto.cpp`**

Add (above the `selftest` definition):

```cpp
uint16_t rpm_check(uint16_t rpm) {
  if (rpm < 600) rpm = 600;
  if (rpm > 3450) rpm = 3450;
  return static_cast<uint16_t>(((rpm + 2) / 5) * 5);
}

void num2iaqt_rpm(uint16_t rpm, uint8_t out[5]) {
  char digits[8];
  int dlen = snprintf(digits, sizeof(digits), "%u", static_cast<unsigned>(rpm));
  for (int i = 0; i < 5; ++i) out[i] = (i < dlen) ? static_cast<uint8_t>(digits[i]) : 0x00;
}

size_t build_vsp_set_frame(uint16_t rpm, uint8_t *out, size_t out_cap) {
  if (out_cap < 24) return 0;
  uint16_t safe = rpm_check(rpm);
  uint8_t body[21];               // 10 02 00 24 + sub + 5 digits + 11 cd = 21
  body[0] = DLE; body[1] = STX; body[2] = 0x00; body[3] = 0x24; body[4] = 0x31;
  num2iaqt_rpm(safe, &body[5]);   // 5 bytes -> body[5..9]
  for (int i = 0; i < 11; ++i) body[10 + i] = 0xcd;  // body[10..20]
  uint32_t s = 0;
  for (int i = 0; i < 21; ++i) s += body[i];
  size_t n = 0;
  for (int i = 0; i < 21; ++i) out[n++] = body[i];
  out[n++] = static_cast<uint8_t>(s & 0xFF);
  out[n++] = DLE;
  out[n++] = ETX;
  return n;  // 24
}

bool vsp_adjust_allowed(uint8_t current_page) { return current_page == IAQ_PAGE_DEVICES; }
```

- [ ] **Step 4: Add selftest vectors**

In `jandy_proto.cpp`, inside `selftest()`: add one ACK vector to the existing `acks[]` array, and a new VSP-frame block after the ACK loop.

Add to `acks[]`:

```cpp
      {"ctrl_ready_iaq", jandy::ACK_IAQ_TOUCH, 0x80, "100200010080931003"},
```

After the ACK loop (the `for (const auto &v : acks)` block), add:

```cpp
  // VSP value-frame vectors: (name, rpm, expected 24-byte frame hex).
  struct VVSP { const char *name; uint16_t rpm; const char *expect_hex; };
  static const VVSP vsps[] = {
      {"vsp_1600", 1600, "10020024313136303000cdcdcdcdcdcdcdcdcdcdcdfd1003"},
      {"vsp_2000", 2000, "10020024313230303000cdcdcdcdcdcdcdcdcdcdcdf81003"},
      {"vsp_3000", 3000, "10020024313330303000cdcdcdcdcdcdcdcdcdcdcdf91003"},
      {"vsp_1000", 1000, "10020024313130303000cdcdcdcdcdcdcdcdcdcdcdf71003"},
      {"vsp_600",   600, "10020024313630300000cdcdcdcdcdcdcdcdcdcdcdcc1003"},
  };
  for (const auto &v : vsps) {
    total++;
    uint8_t out[32];
    size_t n = jandy::build_vsp_set_frame(v.rpm, out, sizeof(out));
    char hex[80];
    to_hex(out, n, hex);
    if (std::string(hex) == v.expect_hex) ok++;
    else { detail += " VSP:"; detail += v.name; }
  }
```

- [ ] **Step 5: Verify parity against the Python oracle, then commit**

The five `expect_hex` strings above must equal the Python `TestBuildVspSetFrame.CAPTURES` values and the control-request `100200010080931003` must equal Task 4's expected bytes. Confirm by eye that they match (same captures). A full compile happens at deploy (Task 9); the selftest result (`selftest PASS`) is checked then.

```bash
git add components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
git commit -m "feat(pump-set): C++ proto mirror + VSP selftest vectors"
```

---

### Task 7: C++ component header (entry point + sequence state)

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h`

- [ ] **Step 1: Declare the public entry point**

In the `public:` section of `class JandyAqualink`, after `read_pump_speed()`:

```cpp
  // Set the filter pump speed (RPM) from HA. Gated by the master interlock +
  // iAqualink presence; the value is clamped to 600-3450 and snapped to 5. Starts
  // a multi-step, page-confirmed sequence run by the core-1 task. One at a time.
  void set_pump_rpm(uint16_t rpm);
```

- [ ] **Step 2: Declare the sequence helpers (protected)**

In the `protected:` section, near the other helpers:

```cpp
  void advance_set_sequence_();      // core-1: drive the SET sequence on each poll
  void send_vsp_set_(uint16_t rpm);  // core-1: transmit the 0x24 value frame
```

- [ ] **Step 3: Add the sequence state members (protected)**

Near the other `iaq_*` volatile members:

```cpp
  // Pump speed SET sequence (multi-step, page-driven). 0 = idle, 1..8 = steps.
  // iaq_set_rpm_ is the clamped target. Written by set_pump_rpm (core 0) under
  // mux_ and by the core-1 task as it advances.
  volatile int iaq_set_step_{0};
  volatile int iaq_set_rpm_{0};
```

- [ ] **Step 4: Verify it still describes a consistent class (no test yet)**

There is no header-only test; correctness is checked when the component compiles at deploy. Re-read the diffs to confirm the method names match Task 8 (`set_pump_rpm`, `advance_set_sequence_`, `send_vsp_set_`).

- [ ] **Step 5: Commit**

```bash
git add components/jandy_aqualink/jandy_aqualink.h
git commit -m "feat(pump-set): declare set_pump_rpm + sequence state in component header"
```

---

### Task 8: C++ component implementation (the gated state machine)

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp`

- [ ] **Step 1: Add the CMD_IAQ_CTRL_READY constant**

Near the other `CMD_IAQ_*` constants (around line 14-20):

```cpp
static constexpr uint8_t CMD_IAQ_CTRL_READY = 0x31;  // panel grants the value-set control slot
```

- [ ] **Step 2: Implement `set_pump_rpm`, `advance_set_sequence_`, `send_vsp_set_`**

Add these definitions (place them right after `read_pump_speed()`):

```cpp
void JandyAqualink::set_pump_rpm(uint16_t rpm) {
  if (!iaq_control_ok_("set_pump_rpm")) return;
  uint16_t clamped = jandy::rpm_check(rpm);
  portENTER_CRITICAL(&mux_);
  iaq_set_rpm_ = clamped;
  iaq_set_step_ = 1;  // kick off the sequence on the next poll
  portEXIT_CRITICAL(&mux_);
  ESP_LOGI(TAG, "set_pump_rpm: start -> %u RPM (requested %u)", clamped, rpm);
}

// Transmit the 0x24 value frame on the bus (core-1 only). Logs the byte count.
void JandyAqualink::send_vsp_set_(uint16_t rpm) {
  uint8_t out[32];
  size_t n = jandy::build_vsp_set_frame(rpm, out, sizeof(out));
  uart_write_bytes(UART, reinterpret_cast<const char *>(out), n);
  ESP_LOGI(TAG, "VSP value frame sent: %u RPM (%u bytes)", rpm, static_cast<unsigned>(n));
}

// Drive one step of the SET sequence. Called from the POLL branch of
// handle_iaq_frame_ while iaq_set_step_ != 0. Each step sends exactly one reply
// (a nav key, the control request, or inert presence) and advances on the page
// the decoder reports. The 0x24 value frame itself goes out on the 0x31, handled
// in handle_iaq_frame_. Turning the interlock off mid-sequence aborts.
void JandyAqualink::advance_set_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "set sequence aborted at step %d: interlock OFF", iaq_set_step_);
    iaq_set_step_ = 0;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_set_step_) {
    case 1:  // go HOME first (deterministic starting point)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_set_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) { send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES); iaq_set_step_ = 3; }
      else { send_iaq_ack_(jandy::KEY_IAQT_HOME); }
      break;
    case 3:  // on DEVICES -> press VSP adjust (0x13); SAFETY: only on DEVICES
      if (jandy::vsp_adjust_allowed(static_cast<uint8_t>(page))) {
        send_iaq_ack_(jandy::KEY_IAQ_DEVICES_VSP_ADJ);
        iaq_set_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4:  // on SET_VSP -> request the control slot (0x80)
      if (page == jandy::IAQ_PAGE_SET_VSP) { send_iaq_ack_(0x80); iaq_set_step_ = 5; }
      else { send_iaq_ack_(0x00); }
      break;
    case 5:  // wait for the panel's 0x31; the 0x24 goes out there, not on a poll
      send_iaq_ack_(0x00);
      break;
    case 6:  // value sent -> read it back via STATUS
      send_iaq_ack_(jandy::KEY_IAQT_STATUS);
      iaq_set_step_ = 7;
      break;
    case 7:  // on STATUS2 (rpm captured by the decoder) -> return HOME
      if (page == jandy::IAQ_PAGE_STATUS2) { send_iaq_ack_(jandy::KEY_IAQT_HOME); iaq_set_step_ = 8; }
      else { send_iaq_ack_(0x00); }
      break;
    case 8:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGI(TAG, "set_pump_rpm sequence complete (%d RPM)", iaq_set_rpm_);
        iaq_set_step_ = 0;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_set_step_ = 0;
      send_iaq_ack_(0x00);
      break;
  }
}
```

- [ ] **Step 3: Wire the sequence into `handle_iaq_frame_`**

In `handle_iaq_frame_`, at the very start of the `if (is_poll) {` block (before the existing armed-key logic), add:

```cpp
    // A SET sequence in progress owns this poll.
    if (iaq_set_step_ != 0) { advance_set_sequence_(); return; }
```

And add the 0x31 handler. After the `if (is_startup) { ... }` block and before the final inert `send_iaq_ack_(0x00);`, add:

```cpp
  // The panel grants the value-set control slot: send the 0x24 value frame now.
  if (cmd == CMD_IAQ_CTRL_READY) {
    if (iaq_set_step_ == 5) {
      send_vsp_set_(static_cast<uint16_t>(iaq_set_rpm_));
      iaq_set_step_ = 6;
      return;
    }
    send_iaq_ack_(0x00);
    return;
  }
```

- [ ] **Step 4: Clear the sequence in `set_interlock(false)`**

Find `void JandyAqualink::set_interlock(bool on)`. In the path taken when `on` is false (the hard abort), also reset the sequence so turning the switch off is a clean stop. Add, under `mux_` alongside the existing armed-key clear:

```cpp
    iaq_set_step_ = 0;   // hard-abort any in-progress pump-set sequence
```

(If `set_interlock` does not currently clear `iaq_armed_key_` on off, add `iaq_armed_key_ = NO_KEY;` next to it as well, under the existing `portENTER_CRITICAL(&mux_)` / `portEXIT_CRITICAL(&mux_)` guard.)

- [ ] **Step 5: Commit**

```bash
git add components/jandy_aqualink/jandy_aqualink.cpp
git commit -m "feat(pump-set): gated multi-step set_pump_rpm state machine"
```

---

### Task 9: YAML controls (four presets + slider) and AUTO_LOAD

**Files:**
- Modify: `components/jandy_aqualink/__init__.py:11`
- Modify: `firmware/pool-bridge.yaml`
- Modify: the live dashboard YAML (same content; see deploy notes)

- [ ] **Step 1: Add `number` to AUTO_LOAD**

In `components/jandy_aqualink/__init__.py`, change:

```python
AUTO_LOAD = ["sensor"]
```

to:

```python
AUTO_LOAD = ["sensor", "number"]
```

- [ ] **Step 2: Add the four preset buttons**

In `firmware/pool-bridge.yaml`, in the `button:` list (right after the "Read Pump Speed" button), add:

```yaml
  # --- Pump speed presets (gated by interlock + iAqualink presence). Each starts
  # the page-confirmed value-set sequence, then reads the new RPM back. The two
  # low values are tuned at the pad against the salt cell flow cutoff. ---
  - platform: template
    name: "Pump Speed: Night"
    icon: "mdi:weather-night"
    on_press:
      - lambda: "id(jandy_comp).set_pump_rpm(1100);"   # below the salt cutoff; tuned at the pad
  - platform: template
    name: "Pump Speed: Low"
    icon: "mdi:speedometer-slow"
    on_press:
      - lambda: "id(jandy_comp).set_pump_rpm(1600);"   # efficient, still chlorinating; tuned at the pad
  - platform: template
    name: "Pump Speed: Normal"
    icon: "mdi:speedometer-medium"
    on_press:
      - lambda: "id(jandy_comp).set_pump_rpm(2750);"
  - platform: template
    name: "Pump Speed: High"
    icon: "mdi:speedometer"
    on_press:
      - lambda: "id(jandy_comp).set_pump_rpm(3200);"
```

- [ ] **Step 3: Add the slider (template number)**

In `firmware/pool-bridge.yaml`, add a top-level `number:` block (place it just before the `sensor:` block):

```yaml
# Pump speed slider. Inert unless the interlock is armed (set_pump_rpm refuses
# otherwise), same as the buttons. Snaps to 5 RPM; the component re-clamps to
# 600-3450 regardless.
number:
  - platform: template
    name: "Pump Speed Set"
    id: pump_speed_set
    icon: "mdi:speedometer"
    unit_of_measurement: "RPM"
    min_value: 600
    max_value: 3450
    step: 5
    mode: slider
    optimistic: true
    set_action:
      - lambda: "id(jandy_comp).set_pump_rpm((uint16_t) x);"
```

- [ ] **Step 4: Sanity-check the YAML locally (no device)**

There is no host test for the YAML; it is validated by the ESPHome compile at deploy (Task 10). Re-read the diff and confirm: four buttons present, one `number:` block, `AUTO_LOAD` includes `number`. Confirm the entity that gates everything (`Pool Keypad Keypress Armed`) is unchanged and still defaults OFF.

- [ ] **Step 5: Commit**

```bash
git add components/jandy_aqualink/__init__.py firmware/pool-bridge.yaml
git commit -m "feat(pump-set): HA preset buttons + speed slider"
```

---

### Task 10: Deploy and live test (founder at the pad)

This task has no host tests; it builds the firmware, runs the on-device selftest, and verifies behavior on the live pump with the founder watching. Do NOT start the live writes until the founder confirms they are at the pad.

- [ ] **Step 1: Push the component so the dashboard can pull it**

```bash
git push
```

- [ ] **Step 2: Compile, then upload, and watch the selftest**

```powershell
# from C:\Users\Falcon\Documents\pool-controller\esp32-experiment
./esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -Port 192.168.4.51
./esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51
./esphome_ws.ps1 -Action logs -Config pool-bridge.yaml -Port 192.168.4.51
```

Expected in the logs at boot: `selftest PASS`. If it says FAIL with a `VSP:` or `ACK:ctrl_ready_iaq` tag, the C++ proto mismatched the captures; fix Task 6 before going further. Do not proceed to live writes on a FAIL.

- [ ] **Step 3: Update the live dashboard YAML with the new controls**

The component code deploys via git + recompile, but the button/slider entities live in the dashboard YAML. Back it up, then add the same four buttons + `number:` block from Task 9. Never echo the dashboard file's contents (it has the WiFi password inline).

```powershell
# Back up first
Invoke-WebRequest "http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml" -OutFile "dashboard-pool-bridge.BACKUP-pre-pumpset.yaml"
```

Edit the fetched file to add the Task 9 button/number YAML, then POST it back (raw bytes) and recompile/upload via `esphome_ws.ps1`. Confirm the new entities appear in Home Assistant.

- [ ] **Step 4: Confirm the gate, with the interlock still OFF**

With "Pool Keypad Keypress Armed" OFF, press "Pump Speed: Normal". Expected in the logs: `set_pump_rpm refused: interlock OFF`. The pump must not change. This proves the gate.

- [ ] **Step 5: First live write (small nudge), interlock ON, founder watching**

Read the current speed (the "Read Pump Speed" button). Arm the interlock. Set the slider to roughly 50 RPM below the current reading. Watch the pump change by that small amount, and confirm the RPM reads back to the new value. Watch the logs for the sequence: nav to DEVICES, the `VSP value frame sent`, `set_pump_rpm sequence complete`.

- [ ] **Step 6: Find the salt cell cutoff, then lock the low presets**

Step the slider down gradually (toward ~1400, ~1300, ...) while the founder watches the salt unit's flow/generating light at the pad. Record the RPM where it stops generating. Set "Low" = that cutoff + ~150; set "Night" below it. Update the two button literals (`set_pump_rpm(...)`) in both `firmware/pool-bridge.yaml` and the dashboard YAML, then recompile/upload.

- [ ] **Step 7: Test each preset, then disarm and commit the tuned values**

Press Night, Low, Normal, High; confirm each reads back. Note whether a hand-set speed holds or reverts at the next scheduled change (persistence). Disarm the interlock. Return to the resting state (presence ON, interlock OFF, HOME).

```bash
git add firmware/pool-bridge.yaml
git commit -m "feat(pump-set): tune Night/Low presets to the measured salt-cell cutoff"
git push
```

---

## Self-review checklist (run after the live test)

- Every spec requirement maps to a task: control surface (Task 9), four presets (Task 9 + Task 10 tuning), on-demand read-back (Task 8 steps 6-8), clamp/snap (Tasks 1, 6), page-confirm on 0x13 (Tasks 5, 6, 8), one-at-a-time + interlock abort (Tasks 7, 8), TDD on the pure logic (Tasks 1-5), captures as oracle (Tasks 3, 6).
- Type/name consistency: `set_pump_rpm`, `advance_set_sequence_`, `send_vsp_set_`, `iaq_set_step_`, `iaq_set_rpm_`, `rpm_check`, `num2iaqt_rpm`, `build_vsp_set_frame`, `iaq_ctrl_ready_ack`, `vsp_adjust_allowed`, `KEY_IAQ_DEVICES_VSP_ADJ`, `IAQ_PAGE_DEVICES`, `IAQ_PAGE_SET_VSP`, `CMD_IAQ_CTRL_READY` are used identically across Python and C++.
- Heaters remain excluded: no heater keycode is added to any allowlist; 0x13 is sent only via the DEVICES-gated VSP path.

---

## Correction to Task 8 (the real integration point, confirmed against the source)

There is no `handle_iaq_frame_` function and no `send_iaq_ack_` helper. iAqualink frames are handled inline in `task_loop()` in the `else if (iaq_presence_ && f.dest() == iaq_addr_)` branch (`jandy_aqualink.cpp` ~lines 102-152), which replies to every 0x33 frame and arms a key only when `f.cmd() == 0x30`. AqualinkD `queue_iaqt_control_command` (iaqtouch_aq_programmer.c:290) confirms the 0x24 frame layout and that the control request is `ACK_CMD_READY_CTRL` = 0x80, sent as the iAq ack key. Use these corrected steps for Task 8:

1. Near line 14 of `jandy_aqualink.cpp`: `static constexpr uint8_t CMD_IAQ_CTRL_READY = 0x31;`
2. Add a helper (declare in the header protected section, `void send_iaq_ack_(uint8_t key);`):

```cpp
void JandyAqualink::send_iaq_ack_(uint8_t key) {
  uint8_t ack[jandy::ACK_PRESENCE_LEN];
  jandy::build_ack(jandy::ACK_IAQ_TOUCH, key, ack);
  uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(ack), jandy::ACK_PRESENCE_LEN);
}
```

3. `send_vsp_set_` writes the 0x24 frame to `JANDY_UART` (not `UART`); `set_pump_rpm` and `advance_set_sequence_` are as written in Task 8 Step 2, but every `send_iaq_ack_(...)` call now resolves to the helper above, and `advance_set_sequence_` reads `iaq_reader_.current_page()` directly (it runs on core 1).

4. Integration in the iAq branch of `task_loop` (insert at the TOP of the `else if (iaq_presence_ ...)` block, before the existing inert/armed-key ack code):

```cpp
        int set_step;
        portENTER_CRITICAL(&mux_);
        set_step = iaq_set_step_;
        portEXIT_CRITICAL(&mux_);
        if (set_step != 0) {
          if (f.cmd() == CMD_IAQ_CTRL_READY && set_step == 5) {
            send_vsp_set_(static_cast<uint16_t>(iaq_set_rpm_));
            portENTER_CRITICAL(&mux_); iaq_set_step_ = 6; portEXIT_CRITICAL(&mux_);
          } else if (f.cmd() == 0x30) {
            advance_set_sequence_();   // sends one ack (nav key / 0x80 / inert), advances
          } else {
            send_iaq_ack_(0x00);       // page frames during the sequence: stay inert
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          if (iaq_reader_.state.has_rpm) iaq_rpm_ = iaq_reader_.state.rpm;
          if (iaq_reader_.state.has_watts) iaq_watts_ = iaq_reader_.state.watts;
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the set sequence
        }
```

5. In `set_interlock(bool on)`, the `if (!on)` path must also clear the iAq state: `iaq_set_step_ = 0; iaq_armed_key_ = -1; iaq_return_home_ = false;` (under the existing `mux_` critical section).
