# 🛠️ Heater Temperature Setpoint (Phase 2 of heaters) Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.
> **IMPORTANT:** Tasks 1-8 are desk work (no device contact) and are fully specified now. Task 9 (deploy + founder-watched LIVE survey) and Task 11 (deploy + founder-watched LIVE setpoint test) drive REAL heater hardware and MUST have the founder watching at the pad/spa. Task 10 (the setpoint state machine) is written for the most-likely "equipment-page" route confirmed by the Task 9 survey; a short contingency note covers the MENU route.

**Goal:** Set the pool heater target to 85F and the spa heater target to 94F from Home Assistant (instead of the 104F max), with safe-range limits, two heater on/off status sensors, and a confirmed navigation route to the panel's temperature screen.

**Architecture:** The temperature WRITE reuses the proven pump value-set machinery: the panel's `0x80` control request then the `0x24` value frame carrying ASCII degree digits. The exact byte format is already resolved from AqualinkD's source and its captured "Set Temp (pool)" frames (verified by hand, checksum included), so the desk code is built and TDD'd against real reference bytes. The only genuinely unknown piece, the navigation route onto the SET_TEMP page on this screenless panel, is confirmed by a short founder-watched survey before the setpoint state machine is finalized.

**Tech Stack:** ESPHome external C++ component (ESP32, core-1 FreeRTOS task), Python reference + pytest, ESPHome dashboard WebSocket build (`esphome_ws.ps1`), Home Assistant.

**Safety invariant:** with the interlock OFF, every new control logs REFUSED and sends nothing. The `0x24` temperature value frame is transmitted ONLY while the decoder confirms `page == SET_TEMP (0x39)`. The page-scoped keycode `0x14` (Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU) is sent ONLY on the page that the caller confirms. One write-sequence at a time (setpoint is mutually exclusive with pump-set, device-toggle, heater-on/off, and the survey press). Interlock-off aborts mid-sequence.

Repo: `C:\Users\Falcon\Documents\pool-controller\esp32-experiment` (`<repo>`). Base `38ef035`.

---

## Scope note (read first)

The spec's "Fuller Phase 2" listed a water-mode reliability fix. While writing this plan I confirmed in the source that **the flaky spa-mode reading was already fixed in Session 9**: every spa-mode gate now reads the always-streaming `cs_spa_` (0x08 status) bit, the decoder's `water_mode_` already keeps its last value (it is never reset to 0), and the `iaq_water_mode_` mirror is written but never read (dead). So that piece reduces to a small regression test that locks the good behavior (Task 7). No functional change is needed there. Everything else in the spec is built.

## File Structure

- `jandy/frames.py` (modify, append): temperature clamps, `num2iaqt_temp`, `build_settemp_frame`, `settemp_write_allowed`, and the page/keycode constants. One responsibility: pure frame/byte logic.
- `jandy/iaq.py` (modify): extend `IaqReader` to decode the HOME-page heater button on/off states.
- `tests/test_heater_setpoint.py` (create): TDD the encoder/frame/clamps/gate against AqualinkD's captured frames.
- `tests/test_heater_status.py` (create): TDD the heat-enabled decode + the sticky-water-mode regression lock.
- `components/jandy_aqualink/jandy_proto.h` + `jandy_proto.cpp` (modify): C++ mirror of the above + on-device selftest vectors.
- `components/jandy_aqualink/jandy_aqualink.h` + `jandy_aqualink.cpp` (modify): the survey press, the setpoint state machine, the heat-enabled shared state + publish.
- `components/jandy_aqualink/__init__.py` (modify): codegen schema for the two heat-enabled binary_sensors.
- `firmware/pool-bridge.yaml` (modify): two setpoint `number` entities, two heat-enabled `binary_sensor`s, three survey buttons.

---

### Task 1: Temperature setpoint frame layer (Python) — TDD

**Files:**
- Create: `tests/test_heater_setpoint.py`
- Modify: `jandy/frames.py` (append at end of file)

- [ ] **Step 1: Write the failing test** (`tests/test_heater_setpoint.py`):

```python
"""Heater setpoint value frame. The format is AqualinkD's num2iaqtRSset, used for
BOTH pump RPM and heater setpoint. The captured "Set Temp (pool)" frames in
AqualinkD iaqtouch.h are the oracle: 50F and 100F reproduce byte-for-byte
(checksum included). 85/94/104 are then computed, not guessed."""

import unittest

from jandy import frames


class TestTempSetpointCheck(unittest.TestCase):
    def test_pool_clamps_45_to_90(self):
        self.assertEqual(frames.pool_setpoint_check(85), 85)
        self.assertEqual(frames.pool_setpoint_check(40), 45)
        self.assertEqual(frames.pool_setpoint_check(200), 90)

    def test_spa_clamps_80_to_104(self):
        self.assertEqual(frames.spa_setpoint_check(94), 94)
        self.assertEqual(frames.spa_setpoint_check(70), 80)
        self.assertEqual(frames.spa_setpoint_check(200), 104)


class TestNum2IaqtTemp(unittest.TestCase):
    # The digit field is 6 bytes: ASCII digits, then NUL pad, with a '0' (0x30) at
    # index 4 for sub-1000 values (the num2iaqtRSset quirk). Oracles: captured
    # "Set Temp (pool)" 50 and 100 from iaqtouch.h.
    def test_captured_oracles(self):
        self.assertEqual(frames.num2iaqt_temp(50), bytes.fromhex("353000003000"))
        self.assertEqual(frames.num2iaqt_temp(100), bytes.fromhex("313030003000"))

    def test_our_targets(self):
        self.assertEqual(frames.num2iaqt_temp(85), bytes.fromhex("383500003000"))
        self.assertEqual(frames.num2iaqt_temp(94), bytes.fromhex("393400003000"))
        self.assertEqual(frames.num2iaqt_temp(104), bytes.fromhex("313034003000"))

    def test_field_is_always_six_bytes(self):
        for t in (45, 80, 90, 104):
            self.assertEqual(len(frames.num2iaqt_temp(t)), 6)


class TestBuildSettempFrame(unittest.TestCase):
    # Full captured frames (iaqtouch.h "Set Temp (pool)"): dest 00, cmd 24, sub 31,
    # 6-byte digit field, ten 0xcd, checksum, 10 03. 24 bytes total.
    CAPTURES = {
        50:  "10020024" + "31" + "353000003000" + "cd" * 10 + "fe" + "1003",
        100: "10020024" + "31" + "313030003000" + "cd" * 10 + "2a" + "1003",
    }

    def test_matches_captured_frames(self):
        for temp, hexstr in self.CAPTURES.items():
            self.assertEqual(
                frames.build_settemp_frame(temp), bytes.fromhex(hexstr),
                f"frame mismatch for {temp}F",
            )

    def test_our_targets_checksums(self):
        self.assertEqual(frames.build_settemp_frame(85)[-3], 0x06)
        self.assertEqual(frames.build_settemp_frame(94)[-3], 0x06)
        self.assertEqual(frames.build_settemp_frame(104)[-3], 0x2E)

    def test_frame_is_24_bytes_and_checksum_valid(self):
        for temp in (45, 85, 94, 104):
            fr = frames.build_settemp_frame(temp)
            self.assertEqual(len(fr), 24)
            extracted = frames.FrameExtractor().feed(fr)
            self.assertEqual(len(extracted), 1)
            self.assertTrue(extracted[0].checksum_valid())
            self.assertEqual(extracted[0].cmd, 0x24)


class TestSettempWriteAllowed(unittest.TestCase):
    def test_true_only_on_set_temp_page(self):
        self.assertTrue(frames.settemp_write_allowed(frames.IAQ_PAGE_SET_TEMP))  # 0x39

    def test_false_elsewhere(self):
        for page in (frames.IAQ_PAGE_HOME, frames.IAQ_PAGE_DEVICES, frames.IAQ_PAGE_SET_VSP, 0x0F, 0x00):
            self.assertFalse(frames.settemp_write_allowed(page))


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_heater_setpoint.py -q`
Expected: FAIL (`AttributeError: module 'jandy.frames' has no attribute 'pool_setpoint_check'`).

- [ ] **Step 3: Implement the Python helpers** (append to the end of `jandy/frames.py`):

```python
# --- Heater temperature setpoint (SET_TEMP page) ----------------------------
#
# Setting a heater target reuses the pump value-set handshake (the 0x80 control
# request then the 0x24 value frame), confirmed against AqualinkD
# queue_iaqt_control_command, which uses the SAME num2iaqtRSset encoder for both
# pump RPM and heater setpoint. The captured "Set Temp (pool)" frames in
# AqualinkD iaqtouch.h are the oracle (50F, 100F reproduce byte-for-byte).
#
# PAGE-SCOPED KEYCODES (the central safety trap): 0x14 is Spa Heat on HOME, Pool
# Heat on DEVICES, and Set Temp on MENU. Every keycode is gated to its page.
IAQ_PAGE_SET_TEMP = 0x39
IAQ_PAGE_MENU = 0x0F

# DEVICES-page heat items (keycode = 0x11 + slot): slot 3 Pool Heat, slot 4 Spa
# Heat. Pressed ONLY while page == DEVICES. The candidate "equipment route" onto
# SET_TEMP (confirmed by the survey).
KEY_IAQ_DEVICES_POOL_HEAT = 0x14
KEY_IAQ_DEVICES_SPA_HEAT = 0x15
# MENU-page "Set Temp" key (AqualinkD KEY_IAQTCH_KEY04). Pressed ONLY while
# page == MENU. The AqualinkD route onto SET_TEMP (doubtful here; MENU is empty).
KEY_IAQT_SET_TEMP = 0x14

# Heater target safe ranges (degrees F). Pool 45 (winter freeze low) to 90; spa
# 80 to 104 (heater hardware max). Refuse out of range by clamping.
POOL_TEMP_MIN, POOL_TEMP_MAX = 45, 90
SPA_TEMP_MIN, SPA_TEMP_MAX = 80, 104


def pool_setpoint_check(temp: int) -> int:
    """Clamp a pool target to the safe 45-90F range."""
    return max(POOL_TEMP_MIN, min(POOL_TEMP_MAX, int(temp)))


def spa_setpoint_check(temp: int) -> int:
    """Clamp a spa target to the safe 80-104F range."""
    return max(SPA_TEMP_MIN, min(SPA_TEMP_MAX, int(temp)))


def num2iaqt_temp(temp: int) -> bytes:
    """AqualinkD num2iaqtRSset for a temperature: ASCII digits, NUL-padded to a
    6-byte field, with a '0' (0x30) at index 4 for sub-1000 values. Reproduces
    the captured Set-Temp frames (50 -> 35 30 00 00 30 00, 100 -> 31 30 30 00 30 00)."""
    digits = str(int(temp)).encode("ascii")
    out = bytearray(6)
    out[: len(digits)] = digits
    for i in range(len(digits), 6):
        out[i] = 0x30 if (i == 4 and len(digits) <= 3) else 0x00
    return bytes(out)


_SETTEMP_PAD = b"\xcd" * 10  # 0xcd padding out to logical index 18 (6-byte field)


def build_settemp_frame(temp: int) -> bytes:
    """The 0x24 value frame that sets a heater target:
    10 02 00 24 31 <6-byte digit field> <ten 0xcd> <cksum> 10 03 (24 bytes).

    `temp` must already be clamped by the caller (pool vs spa ranges differ).
    Reproduces AqualinkD's captured Set-Temp(pool) frames for 50F and 100F."""
    body = bytes([DLE, STX, 0x00, _VSP_SET_CMD, _VSP_SET_SUBBYTE]) + num2iaqt_temp(temp) + _SETTEMP_PAD
    cksum = sum(body) & 0xFF
    return body + bytes([cksum, DLE, ETX])


def settemp_write_allowed(current_page: int) -> bool:
    """True only on the SET_TEMP page (0x39). The 0x24 value frame is transmitted
    only here; this is the central safety gate for the setpoint sequence."""
    return current_page == IAQ_PAGE_SET_TEMP
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_heater_setpoint.py -q`
Expected: PASS.

- [ ] **Step 5: Commit:**

```
git -C <repo> add -- tests/test_heater_setpoint.py jandy/frames.py
git -C <repo> commit -m "feat(session-9): heater setpoint frame layer (Python, TDD vs AqualinkD captures)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- tests/test_heater_setpoint.py jandy/frames.py
```

---

### Task 2: Temperature setpoint frame layer (C++ mirror + selftest vectors)

No desk unit test runs C++; the Python suite (Task 1) is the logic oracle and the on-device `selftest()` mirrors it. Verified at the Task 9 deploy (compile + selftest PASS).

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h` (after the heater on/off block, ~line 76)
- Modify: `components/jandy_aqualink/jandy_proto.cpp` (after `vsp_adjust_allowed`, ~line 292; and add selftest vectors)

- [ ] **Step 1: Declare the constants + functions** in `jandy_proto.h`, right after the `heater_enable_allowed` block (line 76):

```cpp
// Heater temperature setpoint (SET_TEMP page 0x39). Reuses the pump value-set
// handshake; AqualinkD uses the SAME num2iaqtRSset encoder for RPM and setpoint.
// PAGE-SCOPED: 0x14 is Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU,
// so each keycode is gated to its page. The 0x24 value frame goes out ONLY on
// SET_TEMP. Captured oracles (iaqtouch.h Set Temp pool): 50F, 100F.
static constexpr uint8_t IAQ_PAGE_SET_TEMP = 0x39, IAQ_PAGE_MENU = 0x0F;
static constexpr uint8_t KEY_IAQ_DEVICES_POOL_HEAT = 0x14, KEY_IAQ_DEVICES_SPA_HEAT = 0x15;
static constexpr uint8_t KEY_IAQT_SET_TEMP = 0x14;  // MENU "Set Temp" (KEY04)
static constexpr int POOL_TEMP_MIN = 45, POOL_TEMP_MAX = 90, SPA_TEMP_MIN = 80, SPA_TEMP_MAX = 104;

inline int pool_setpoint_check(int t) { return t < POOL_TEMP_MIN ? POOL_TEMP_MIN : (t > POOL_TEMP_MAX ? POOL_TEMP_MAX : t); }
inline int spa_setpoint_check(int t) { return t < SPA_TEMP_MIN ? SPA_TEMP_MIN : (t > SPA_TEMP_MAX ? SPA_TEMP_MAX : t); }
inline bool settemp_write_allowed(uint8_t current_page) { return current_page == IAQ_PAGE_SET_TEMP; }

void num2iaqt_temp(uint16_t temp, uint8_t out[6]);                        // ASCII digits, 6-byte field
size_t build_settemp_frame(uint16_t temp, uint8_t *out, size_t out_cap);  // 0x24 frame; returns 24
```

- [ ] **Step 2: Implement them** in `jandy_proto.cpp`, right after `vsp_adjust_allowed` (line 292):

```cpp
void num2iaqt_temp(uint16_t temp, uint8_t out[6]) {
  // ASCII digits, then pad to 6: a '0' at index 4 for sub-1000 values, else NUL
  // (AqualinkD num2iaqtRSset). Reproduces the captured Set-Temp frames.
  char tmp[6];
  int d = 0;
  if (temp == 0) {
    tmp[d++] = '0';
  } else {
    while (temp > 0 && d < 6) { tmp[d++] = static_cast<char>('0' + (temp % 10)); temp /= 10; }
  }
  for (int i = 0; i < d; ++i) out[i] = static_cast<uint8_t>(tmp[d - 1 - i]);
  for (int i = d; i < 6; ++i) out[i] = (i == 4 && d <= 3) ? 0x30 : 0x00;
}

size_t build_settemp_frame(uint16_t temp, uint8_t *out, size_t out_cap) {
  if (out_cap < 24) return 0;
  uint8_t field[6];
  num2iaqt_temp(temp, field);
  size_t i = 0;
  out[i++] = DLE; out[i++] = STX; out[i++] = 0x00; out[i++] = 0x24; out[i++] = 0x31;
  for (int k = 0; k < 6; ++k) out[i++] = field[k];
  for (int k = 0; k < 10; ++k) out[i++] = 0xcd;
  uint32_t s = 0;
  for (size_t k = 0; k < i; ++k) s += out[k];
  out[i++] = static_cast<uint8_t>(s & 0xFF);
  out[i++] = DLE;
  out[i++] = ETX;
  return i;  // 24
}
```

- [ ] **Step 3: Add selftest vectors** in `jandy_proto.cpp`, inside `selftest()`, right after the pump-set `VSPGATE` block (after line 440, before the iAqualink HOME-page decode block):

```cpp
  // Heater setpoint: build_settemp_frame reproduces the captured Set-Temp(pool)
  // frames (50F, 100F), and settemp_write_allowed gates to SET_TEMP only.
  {
    struct TV { uint16_t t; uint8_t exp[24]; };
    static const TV tv[] = {
        {50,  {0x10,0x02,0x00,0x24,0x31,0x35,0x30,0x00,0x00,0x30,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xfe,0x10,0x03}},
        {100, {0x10,0x02,0x00,0x24,0x31,0x31,0x30,0x30,0x00,0x30,0x00,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0xcd,0x2a,0x10,0x03}},
    };
    for (const auto &v : tv) {
      total++;
      uint8_t out[32];
      size_t n = build_settemp_frame(v.t, out, sizeof(out));
      bool pass = (n == 24);
      for (int i = 0; pass && i < 24; ++i)
        if (out[i] != v.exp[i]) pass = false;
      if (pass) ok++;
      else detail += (v.t == 50 ? " TEMP50" : " TEMP100");
    }
    total++;
    if (settemp_write_allowed(IAQ_PAGE_SET_TEMP) && !settemp_write_allowed(IAQ_PAGE_DEVICES) &&
        !settemp_write_allowed(IAQ_PAGE_HOME))
      ok++;
    else detail += " TEMPGATE";
  }
```

- [ ] **Step 4: Brace-balance sanity check** (no desk compile available):

Run: `python -c "s=open(r'<repo>/components/jandy_aqualink/jandy_proto.cpp').read(); print('braces', s.count('{')-s.count('}'))"`
Expected: `braces 0`.

- [ ] **Step 5: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
git -C <repo> commit -m "feat(session-9): heater setpoint frame layer (C++ mirror + selftest vectors)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
```

---

### Task 3: Heater on/off status decode (Python IaqReader) — TDD

The HOME page enumerates each button as a `0x24` frame: `data[0]=index, data[1]=state`. Session 9 captured Pool Heat = button index 2, Spa Heat = index 3, with `state 3 = enabled`, `state 0 = off`. The reader currently ignores `0x24`; extend it to track these two.

**Files:**
- Create: `tests/test_heater_status.py`
- Modify: `jandy/iaq.py` (`IaqReader`)

- [ ] **Step 1: Write the failing test** (`tests/test_heater_status.py`):

```python
"""HOME-page heater on/off decode. Each HOME button arrives as a 0x24 frame
(data[0]=index, data[1]=state). Session 9 capture: Pool Heat = index 2, Spa Heat
= index 3, state 3 = enabled, state 0 = off. The reader commits these on HOME
page end and keeps the last value across a partial page."""

import unittest

from jandy.frames import DLE, STX, ETX
from jandy.iaq import IaqReader


def _frame(cmd, data):
    body = bytes([DLE, STX, 0x33, cmd]) + bytes(data)
    cksum = sum(body) & 0xFF
    return _Frame(body + bytes([cksum, DLE, ETX]))


class _Frame:
    def __init__(self, raw):
        self.raw = raw
        self.cmd = raw[3]
        self.data = raw[4:-3]


def _home(pool_state, spa_state):
    """A minimal HOME page: start, Pool Heat button (idx 2), Spa Heat button
    (idx 3), end. Button data layout: index, state, unknown, type, label..."""
    r = IaqReader()
    r.feed(_frame(0x23, [0x01]))  # page start, type HOME
    r.feed(_frame(0x24, [0x02, pool_state, 0x00, 0x0B] + list(b"Pool Heat")))
    r.feed(_frame(0x24, [0x03, spa_state, 0x00, 0x0B] + list(b"Spa Heat")))
    r.feed(_frame(0x28, [0x05]))  # page end
    return r


class TestHeaterStatusDecode(unittest.TestCase):
    def test_pool_on_spa_off(self):
        r = _home(pool_state=3, spa_state=0)
        self.assertTrue(r.has_pool_heat and r.pool_heat_enabled)
        self.assertTrue(r.has_spa_heat and not r.spa_heat_enabled)

    def test_both_on(self):
        r = _home(pool_state=3, spa_state=3)
        self.assertTrue(r.pool_heat_enabled and r.spa_heat_enabled)

    def test_both_off(self):
        r = _home(pool_state=0, spa_state=0)
        self.assertFalse(r.pool_heat_enabled or r.spa_heat_enabled)


class TestStickyWaterMode(unittest.TestCase):
    """Regression lock (Scope note): water_mode must persist across a partial HOME
    page so spa-mode is never spuriously 'unknown' once seen."""

    def test_water_mode_persists_across_partial_page(self):
        r = IaqReader()
        # Full spa HOME page: value line idx 0 = "88", label idx 4 = "Spa Temp".
        r.feed(_frame(0x23, [0x01]))
        r.feed(_frame(0x25, [0x00] + list(b"88")))
        r.feed(_frame(0x25, [0x04] + list(b"Spa Temp")))
        r.feed(_frame(0x28, [0x05]))
        self.assertEqual(r.water_mode, 3)
        # A later partial HOME page with no temp label must NOT reset water_mode.
        r.feed(_frame(0x23, [0x01]))
        r.feed(_frame(0x24, [0x02, 0x00, 0x00, 0x05] + list(b"Pool Heat")))
        r.feed(_frame(0x28, [0x05]))
        self.assertEqual(r.water_mode, 3)


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m pytest tests/test_heater_status.py -q`
Expected: FAIL (`AttributeError: 'IaqReader' object has no attribute 'has_pool_heat'`).

- [ ] **Step 3: Extend `IaqReader`** in `jandy/iaq.py`. In `__init__` (after `self.water_mode = 0`), add:

```python
        self.pool_heat_enabled = False
        self.spa_heat_enabled = False
        self.has_pool_heat = False
        self.has_spa_heat = False
        self._btn_state = {}  # index -> state, for the page being loaded
```

In `feed`, the `CMD_IAQ_PAGE_START` branch (where `self._lines = {}` is set) add a button-buffer reset:

```python
            self._btn_state = {}
```

In `feed`, add a new branch for the button frame (after the `CMD_IAQ_PAGE_MSG` branch, before `CMD_IAQ_PAGE_END`):

```python
        elif cmd == CMD_IAQ_PAGE_BUTTON:
            data = frame.data
            if len(data) >= 2:
                self._btn_state[data[0]] = data[1]
```

In `_commit_home` (at the end of the method), add the heater-button commit:

```python
        # HOME heater buttons: index 2 Pool Heat, index 3 Spa Heat; state 3 = on.
        if 2 in self._btn_state:
            self.pool_heat_enabled = self._btn_state[2] == 3
            self.has_pool_heat = True
        if 3 in self._btn_state:
            self.spa_heat_enabled = self._btn_state[3] == 3
            self.has_spa_heat = True
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m pytest tests/test_heater_status.py -q`
Expected: PASS.

- [ ] **Step 5: Run the full Python suite** (no regressions): `python -m pytest -q` (cwd `<repo>`). Expected: all green.

- [ ] **Step 6: Commit:**

```
git -C <repo> add -- tests/test_heater_status.py jandy/iaq.py
git -C <repo> commit -m "feat(session-9): decode HOME-page heater on/off state (Python) + sticky water_mode lock

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- tests/test_heater_status.py jandy/iaq.py
```

---

### Task 4: Heater on/off status decode (C++ IaqReader mirror + selftest vector)

**Files:**
- Modify: `components/jandy_aqualink/jandy_proto.h` (`IaqReader` class)
- Modify: `components/jandy_aqualink/jandy_proto.cpp` (`IaqReader::feed`, `commit_home`, selftest)

- [ ] **Step 1: Extend the `IaqReader` declaration** in `jandy_proto.h`. In the public section (after `int current_page() const`), add accessors:

```cpp
  bool has_pool_heat() const { return has_pool_heat_; }
  bool pool_heat_enabled() const { return pool_heat_enabled_; }
  bool has_spa_heat() const { return has_spa_heat_; }
  bool spa_heat_enabled() const { return spa_heat_enabled_; }
```

In the private section (after `int water_mode_ = 0;`), add:

```cpp
  bool pool_heat_enabled_ = false, spa_heat_enabled_ = false;
  bool has_pool_heat_ = false, has_spa_heat_ = false;
  uint8_t btn_state_[MAX_LINES] = {0};
  bool btn_present_[MAX_LINES] = {false};
```

- [ ] **Step 2: Handle the button frame** in `jandy_proto.cpp` `IaqReader::feed`. In the `cmd == 0x23` (page start) branch, after the `present_` reset loop, add a button-buffer reset:

```cpp
    for (int i = 0; i < MAX_LINES; ++i) btn_present_[i] = false;
```

Add a new `else if` branch for the button frame (after the `cmd == 0x25` branch, before `cmd == 0x28`):

```cpp
  } else if (cmd == 0x24) {  // CMD_IAQ_PAGE_BUTTON: data[0]=index, data[1]=state
    if (f.data_len() >= 2) {
      int idx = f.data()[0];
      if (idx >= 0 && idx < MAX_LINES) {
        btn_state_[idx] = f.data()[1];
        btn_present_[idx] = true;
      }
    }
```

- [ ] **Step 3: Commit the heater state** in `IaqReader::commit_home` (at the end of the method, after the temperature loop):

```cpp
  // HOME heater buttons: index 2 Pool Heat, index 3 Spa Heat; state 3 = on.
  if (btn_present_[2]) { pool_heat_enabled_ = (btn_state_[2] == 3); has_pool_heat_ = true; }
  if (btn_present_[3]) { spa_heat_enabled_ = (btn_state_[3] == 3); has_spa_heat_ = true; }
```

- [ ] **Step 4: Add a selftest vector** in `selftest()`, inside the existing "iAqualink HOME-page decode" block (the one that feeds spa=88/air=156). Add two button frames before the page-end frame, and extend the assert. Replace the page-end feed line and the final `if`:

Find:
```cpp
    feed_one({0x10, 0x02, 0x33, 0x25, 0x00, 0x38, 0x38, 0xC2, 0xBA, 0x00, 0x56, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x28, 0x05, 0x1F, 0x1A, 0x08, 0x1D, 0xD0, 0x10, 0x03});
    if (ir.state.has_spa && ir.state.spa == 88 && ir.state.has_air && ir.state.air == 156 &&
        !ir.state.has_pool && ir.water_mode() == 3 && ir.current_page() == 0x01)
      ok++;
```
Replace with:
```cpp
    feed_one({0x10, 0x02, 0x33, 0x25, 0x00, 0x38, 0x38, 0xC2, 0xBA, 0x00, 0x56, 0x10, 0x03});
    // Minimal HOME buttons (decode reads only data[0]=index, data[1]=state): Pool
    // Heat (idx 2) enabled (state 3); Spa Heat (idx 3) off (state 0). Checksums are
    // sum(bytes before cksum)&0xFF: 0x79 and 0x71 respectively.
    feed_one({0x10, 0x02, 0x33, 0x24, 0x02, 0x03, 0x00, 0x0B, 0x79, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x24, 0x03, 0x00, 0x00, 0x05, 0x71, 0x10, 0x03});
    feed_one({0x10, 0x02, 0x33, 0x28, 0x05, 0x1F, 0x1A, 0x08, 0x1D, 0xD0, 0x10, 0x03});
    if (ir.state.has_spa && ir.state.spa == 88 && ir.state.has_air && ir.state.air == 156 &&
        !ir.state.has_pool && ir.water_mode() == 3 && ir.current_page() == 0x01 &&
        ir.has_pool_heat() && ir.pool_heat_enabled() && ir.has_spa_heat() && !ir.spa_heat_enabled())
      ok++;
```
(The `0x79` / `0x71` checksums were computed as `sum(raw[:-3]) & 0xFF` over each frame's bytes.)

- [ ] **Step 5: Brace-balance check:** `python -c "s=open(r'<repo>/components/jandy_aqualink/jandy_proto.cpp').read(); print('braces', s.count('{')-s.count('}'))"` -> `braces 0`.

- [ ] **Step 6: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
git -C <repo> commit -m "feat(session-9): decode HOME-page heater on/off state (C++ mirror + selftest)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
```

---

### Task 5: Heat-enabled binary_sensors (firmware wiring)

Publish `pool_heat_enabled` / `spa_heat_enabled` to HA, mirroring the existing `cs_*` circuit binary_sensors. Core-1 writes shared state from the reader; `loop()` publishes on change.

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (setters, fields)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (mirror in dispatch, publish in `loop()`)
- Modify: `components/jandy_aqualink/__init__.py` (codegen)
- Modify: `firmware/pool-bridge.yaml` (sensor entries)

- [ ] **Step 1: Header setters + fields** (`jandy_aqualink.h`). After `set_cleaner_bs(...)` (line 42):

```cpp
  void set_pool_heat_bs(binary_sensor::BinarySensor *b) { pool_heat_bs_ = b; }
  void set_spa_heat_bs(binary_sensor::BinarySensor *b) { spa_heat_bs_ = b; }
```
After the `cleaner_bs_` field (line 141):

```cpp
  binary_sensor::BinarySensor *pool_heat_bs_{nullptr};
  binary_sensor::BinarySensor *spa_heat_bs_{nullptr};
```
After the `pub_cs_*` line (145), add shared + published heat-enable state (-1 unknown):

```cpp
  volatile int8_t he_pool_{-1}, he_spa_{-1};
  int8_t pub_he_pool_{-2}, pub_he_spa_{-2};
```

- [ ] **Step 2: Mirror the reader's heat state to core 0.** In `jandy_aqualink.cpp` `task_loop`, the normal iAq path (the block starting ~line 201 `iaq_reader_.feed(f);` ... after `iaq_current_page_ = iaq_reader_.current_page();` at line 210), add inside that same `portENTER_CRITICAL` region:

```cpp
        if (iaq_reader_.has_pool_heat()) he_pool_ = iaq_reader_.pool_heat_enabled() ? 1 : 0;
        if (iaq_reader_.has_spa_heat()) he_spa_ = iaq_reader_.spa_heat_enabled() ? 1 : 0;
```

- [ ] **Step 3: Publish in `loop()`.** In `jandy_aqualink.cpp`, inside the binary_sensor publish block (the `{ int8_t spa_s, ... }` block, after the `cleaner_bs_` publish at line 927, still inside the block's closing `}` at 928), add a snapshot + publish:

```cpp
    int8_t he_p, he_s;
    portENTER_CRITICAL(&mux_);
    he_p = he_pool_;
    he_s = he_spa_;
    portEXIT_CRITICAL(&mux_);
    if (pool_heat_bs_ && he_p >= 0 && he_p != pub_he_pool_) {
      pool_heat_bs_->publish_state(he_p != 0);
      pub_he_pool_ = he_p;
    }
    if (spa_heat_bs_ && he_s >= 0 && he_s != pub_he_spa_) {
      spa_heat_bs_->publish_state(he_s != 0);
      pub_he_spa_ = he_s;
    }
```

- [ ] **Step 4: Codegen** (`components/jandy_aqualink/__init__.py`). After `CONF_CLEANER_STATE = "cleaner_state"` (line 31):

```python
CONF_POOL_HEAT_ENABLED = "pool_heat_enabled"
CONF_SPA_HEAT_ENABLED = "spa_heat_enabled"
```
In `CONFIG_SCHEMA`, after the `CONF_CLEANER_STATE` entry (line 94-97):

```python
        cv.Optional(CONF_POOL_HEAT_ENABLED): binary_sensor.binary_sensor_schema(
            icon="mdi:radiator",
        ),
        cv.Optional(CONF_SPA_HEAT_ENABLED): binary_sensor.binary_sensor_schema(
            icon="mdi:hot-tub",
        ),
```
In `to_code`, after the `CONF_CLEANER_STATE` block (line 144-146):

```python
    if CONF_POOL_HEAT_ENABLED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_POOL_HEAT_ENABLED])
        cg.add(var.set_pool_heat_bs(b))
    if CONF_SPA_HEAT_ENABLED in config:
        b = await binary_sensor.new_binary_sensor(config[CONF_SPA_HEAT_ENABLED])
        cg.add(var.set_spa_heat_bs(b))
```

- [ ] **Step 5: Firmware entities** (`firmware/pool-bridge.yaml`). In the `jandy_aqualink:` block, after `cleaner_state:` (line 92-93):

```yaml
  # Heater on/off, decoded from the HOME-page heater button states (Session 9:
  # state 3 = enabled). Read-only; updates from the HOME enumeration plus deltas.
  pool_heat_enabled:
    name: Pool Heat (enabled)
  spa_heat_enabled:
    name: Spa Heat (enabled)
```

- [ ] **Step 6: Brace-balance check:** `python -c "s=open(r'<repo>/components/jandy_aqualink/jandy_aqualink.cpp').read(); print('braces', s.count('{')-s.count('}'))"` -> `braces 0`.

- [ ] **Step 7: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp components/jandy_aqualink/__init__.py firmware/pool-bridge.yaml
git -C <repo> commit -m "feat(session-9): Pool Heat / Spa Heat enabled binary_sensors

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp components/jandy_aqualink/__init__.py firmware/pool-bridge.yaml
```

---

### Task 6: Gated survey instrument (C++)

A one-shot, page-confirmed diagnostic press. The founder navigates to a page with the existing nav buttons, confirms the page in the log, then triggers a survey press that fires the keycode ONLY if the decoder confirms the expected page. This is how we test the equipment route (press 0x14/0x15 on DEVICES) and the MENU route (press 0x14 on MENU) without a value write.

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (decl + fields)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`survey_press`, dispatch block)
- Modify: `firmware/pool-bridge.yaml` (three survey buttons)

- [ ] **Step 1: Header** (`jandy_aqualink.h`). After `press_heater(...)` (line 111):

```cpp
  // SURVEY-ONLY (Phase 2 setpoint route discovery): send ONE keycode, but ONLY if
  // the decoder confirms the panel is on expect_page. Gated by interlock + presence
  // + one-at-a-time. The page-confirm means a page-scoped key (0x14 is Pool Heat on
  // DEVICES, Set Temp on MENU) can never fire on the wrong page. No value frame.
  void survey_press(uint8_t key, uint8_t expect_page);
```
After the heater sequence fields (line 205):

```cpp
  // Survey one-shot press: -1 idle, else the keycode armed for iaq_survey_page_.
  volatile int16_t iaq_survey_key_{-1};
  volatile int iaq_survey_page_{-1};
```

- [ ] **Step 2: `survey_press`** (`jandy_aqualink.cpp`), after `press_heater`'s closing brace (line 425):

```cpp
void JandyAqualink::survey_press(uint8_t key, uint8_t expect_page) {
  if (!interlock_) {
    ESP_LOGW(TAG, "survey REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "survey REFUSED: iAqualink presence is OFF (key=0x%02X)", key);
    return;
  }
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "survey REFUSED: another sequence is in progress (key=0x%02X)", key);
    return;
  }
  iaq_survey_key_ = key;
  iaq_survey_page_ = expect_page;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "survey: armed key 0x%02X for page 0x%02X (%s)", key, expect_page,
           jandy::iaq_page_name(expect_page));
}
```

- [ ] **Step 3: Dispatch block** (`jandy_aqualink.cpp` `task_loop`). Immediately after the heater-sequence dispatch block (the one ending `continue;  // this 0x33 frame fully handled by the heater sequence` at line 168), insert:

```cpp
        // SURVEY one-shot: send the armed key ONLY on the confirmed expected page.
        int sv_key, sv_page;
        portENTER_CRITICAL(&mux_);
        sv_key = iaq_survey_key_;
        sv_page = iaq_survey_page_;
        portEXIT_CRITICAL(&mux_);
        if (sv_key >= 0) {
          if (f.cmd() == 0x30) {
            int page = iaq_reader_.current_page();
            if (page == sv_page) {
              send_iaq_ack_(static_cast<uint8_t>(sv_key));
              ESP_LOGW(TAG, "survey: pressed 0x%02X on page 0x%02X (%s)", sv_key, page,
                       jandy::iaq_page_name(static_cast<uint8_t>(page)));
            } else {
              send_iaq_ack_(0x00);
              ESP_LOGW(TAG, "survey REFUSED at transmit: on 0x%02X (%s), expected 0x%02X", page,
                       jandy::iaq_page_name(static_cast<uint8_t>(page)), sv_page);
            }
            portENTER_CRITICAL(&mux_);
            iaq_survey_key_ = -1;
            iaq_survey_page_ = -1;
            portEXIT_CRITICAL(&mux_);
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the survey one-shot
        }
```

- [ ] **Step 4: Clear the survey one-shot on interlock-off.** In `set_interlock`, inside the `if (!on)` block (after `iaq_heater_key_ = -1;` at line 241):

```cpp
    iaq_survey_key_ = -1;    // and any armed survey press
    iaq_survey_page_ = -1;
```

- [ ] **Step 5: Survey buttons** (`firmware/pool-bridge.yaml`). After the heater buttons block (after line 263, the Spa Heat button), add:

```yaml
  # SURVEY (Phase 2 setpoint route discovery; gated + one-shot + page-confirmed).
  # Navigate first (Other Devices -> DEVICES, or Menu -> MENU), confirm the page in
  # the log, then press the matching survey button and watch for IAQ PAGE SET_TEMP(0x39).
  - platform: template
    name: "Survey: Pool Heat on DEVICES"
    icon: "mdi:magnify"
    on_press:
      - lambda: "id(jandy_comp).survey_press(0x14, 0x36);"   # DEVICES Pool Heat -> SET_TEMP?
  - platform: template
    name: "Survey: Spa Heat on DEVICES"
    icon: "mdi:magnify"
    on_press:
      - lambda: "id(jandy_comp).survey_press(0x15, 0x36);"   # DEVICES Spa Heat -> SET_TEMP?
  - platform: template
    name: "Survey: Set Temp on MENU"
    icon: "mdi:magnify"
    on_press:
      - lambda: "id(jandy_comp).survey_press(0x14, 0x0F);"   # MENU Set Temp (KEY04) -> SET_TEMP?
```

- [ ] **Step 6: Brace-balance check:** `python -c "s=open(r'<repo>/components/jandy_aqualink/jandy_aqualink.cpp').read(); print('braces', s.count('{')-s.count('}'))"` -> `braces 0`.

- [ ] **Step 7: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp firmware/pool-bridge.yaml
git -C <repo> commit -m "feat(session-9): gated one-shot survey press (page-confirmed) for SET_TEMP discovery

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp firmware/pool-bridge.yaml
```

---

### Task 7: Regression gate (pytest)

- [ ] **Step 1: Run the full suite** from `<repo>`:

Run: `python -m pytest -q`
Expected: all green (the prior heater suite + the new `test_heater_setpoint.py` and `test_heater_status.py`). A failure means something unrelated broke; STOP and investigate before any flash.

---

### Task 8: Deploy the desk build + post-flash health

DO NOT actuate anything yet. This flashes the survey + sensors + setpoint frame layer (the setpoint state machine is added in Task 10 after the survey). Heaters and the panel are untouched at boot (interlock OFF).

- [ ] **Step 1: Push** so the dashboard compile pulls the component:
`git -C <repo> push origin master`, then confirm `## master...origin/master` with no `[ahead]`.

- [ ] **Step 2: Patch the LIVE dashboard yaml** with the new entities (2 heat-enabled sensors, 3 survey buttons). GET the live config with `(New-Object System.Net.WebClient).DownloadString($uri)` where `$uri = "http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml"`. Back it up to `dashboard-pool-bridge.BACKUP-<stamp>.yaml` FIRST. Insert the same blocks (Task 5 step 5, Task 6 step 5). POST with `.UploadString($uri,"POST",$body)`. GET again and readback-verify byte-exact.

- [ ] **Step 3: Compile + flash** (from `C:\Users\Falcon\Documents\pool-controller`):
`.\esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -TimeoutSec 560` (expect `Successfully compiled program.` + `EXIT CODE 0`), then
`.\esphome_ws.ps1 -Action upload -Config pool-bridge.yaml -Port 192.168.4.51 -TimeoutSec 240` (expect `OTA successful` + `EXIT CODE 0`).

- [ ] **Step 4: Post-flash health** (background flushing capture via `C:\Users\Falcon\poollog.ps1`, shared-read open): confirm `selftest PASS` and the new count (prior total + 3 from Task 2 + 1 extended HOME-decode = verify the ratio is N/N, no FAIL tags) and `checksum_errors=0` over a >15s window. Confirm `iAqualink Presence` ON (temps reading) and the interlock OFF. Confirm the two new binary_sensors appear in HA (state will populate on the next full HOME enumeration). Do NOT proceed on a FAIL.

---

### Task 9: Founder-watched LIVE survey (confirm the route)

DO NOT start without the founder watching. This navigates the panel and presses page-scoped keys; it sends no value frame, but a heat-item press on DEVICES could conceivably toggle a heater, so the founder watches and we start from a safe state.

- [ ] **Step 1: Safe state + arm.** Confirm pool mode (`cs_spa_`/Pool Spa Mode sensor = off), filter pump running, presence ON. Start the background log capture. Arm the interlock. Because the founder reports the temperature screen needs a heater on, press **Pool Heat** (`button.pool_rs485_bridge_pool_heat`) and confirm `heater sequence complete` and the `Pool Heat (enabled)` sensor turns on.

- [ ] **Step 2: Equipment route.** Press **Other Devices** nav, confirm the log shows `IAQ PAGE DEVICES(0x36)`. Then press **Survey: Pool Heat on DEVICES**. Watch the log:
  - If it shows `IAQ PAGE SET_TEMP(0x39)` (possibly after the `survey: pressed 0x14 on page 0x36` line), the equipment route WORKS. Capture the full page enumeration: the `IAQ B<idx> s<state> t<type>: <label>` lines (note any "Pool Heat NN" / "Spa Heat NN" buttons and their indices = the body-select keycodes, and the NN = the current setpoints, so we read the spa's stored ~104 directly).
  - If the page stays DEVICES and `Pool Heat (enabled)` flips, the press only toggled the heater (re-enable it). Move to step 3.

- [ ] **Step 2b (if needed): Spa Heat on DEVICES.** Same as step 2 with **Survey: Spa Heat on DEVICES** (0x15), to see whether the spa setpoint screen opens the same way.

- [ ] **Step 3: MENU route (fallback).** Press **Menu** nav, confirm `IAQ PAGE MENU(0x0F)`. Press **Survey: Set Temp on MENU**. Watch for `IAQ PAGE SET_TEMP(0x39)`. (Expected doubtful given the empty MENU, but the key is fixed so it may still navigate.)

- [ ] **Step 4: Record the result.** Write down, for pool and spa: (a) the route that opened SET_TEMP and the exact nav keys, (b) whether SET_TEMP enumerates the pool/spa setpoint buttons and their keycodes/indices, (c) the current setpoints shown, (d) whether a separate body-select press was needed. If NEITHER route opened SET_TEMP, the setpoint defers: the heat-enabled sensors still ship; skip Tasks 10-11 and record the negative result (Task 12).

- [ ] **Step 5: Restore.** Turn Pool Heat back off (founder's resting preference until the setpoint exists), confirm `Pool Heat (enabled)` off, disarm the interlock.

---

### Task 10: Setpoint state machine + HA number entities (built from the survey)

Written for the **equipment-page route** (the most-likely path; Task 9 confirms it). SURVEY-CONFIRMED VALUES to verify before flashing: (a) the route is HOME -> Other Devices -> DEVICES -> press heat item (0x14 pool / 0x15 spa) -> SET_TEMP; (b) no separate body-select press is needed (the heat-item press lands on SET_TEMP with that body's field active). CONTINGENCY: if the survey found the MENU route, change step 2/3's nav to Menu (0x02) then Set Temp (0x14 on MENU); if SET_TEMP needs a body-select press first, insert a `case` that sends the captured body keycode while `page == SET_TEMP` before the 0x80. The safety invariant (0x24 only on SET_TEMP, heat item only on DEVICES) is unchanged either way.

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (decls, fields)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (`set_heater_setpoint`, `send_settemp_set_`, `advance_settemp_sequence_`, dispatch, interlock abort, busy-guards)
- Modify: `firmware/pool-bridge.yaml` (2 number entities)

- [ ] **Step 1: Header decls** (`jandy_aqualink.h`). After `survey_press(...)`:

```cpp
  // Set a heater target from HA. is_spa picks the body (clamp + DEVICES heat item):
  // pool 45-90 via 0x14, spa 80-104 via 0x15. Gated by interlock + presence; runs a
  // page-confirmed sequence (HOME -> DEVICES -> heat item -> SET_TEMP -> 0x80 ->
  // 0x24 value -> HOME). The 0x24 frame is sent ONLY on SET_TEMP. One at a time.
  void set_heater_setpoint(bool is_spa, uint16_t temp);
```
After `advance_heater_sequence_();` (protected, line 122):

```cpp
  void advance_settemp_sequence_();  // core-1: drive the setpoint sequence on each poll
  void send_settemp_set_(uint16_t temp);  // core-1: transmit the 0x24 setpoint value frame
```
After the survey fields:

```cpp
  // Setpoint sequence (multi-step, page-driven). 0 = idle, 1..7 = steps.
  // iaq_settemp_key_ is the DEVICES heat item (0x14 pool / 0x15 spa); iaq_settemp_val_
  // is the clamped target. Mutually exclusive with the other write-sequences.
  volatile int iaq_settemp_step_{0};
  volatile int iaq_settemp_key_{-1};
  volatile int iaq_settemp_val_{0};
```

- [ ] **Step 2: `set_heater_setpoint` + `send_settemp_set_`** (`jandy_aqualink.cpp`), after `send_vsp_set_` (line 440):

```cpp
void JandyAqualink::set_heater_setpoint(bool is_spa, uint16_t temp) {
  if (!interlock_) {
    ESP_LOGW(TAG, "setpoint REFUSED: safety interlock is OFF (%s %u)", is_spa ? "spa" : "pool", temp);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "setpoint REFUSED: iAqualink presence is OFF (%s %u)", is_spa ? "spa" : "pool", temp);
    return;
  }
  int clamped = is_spa ? jandy::spa_setpoint_check(temp) : jandy::pool_setpoint_check(temp);
  uint8_t key = is_spa ? jandy::KEY_IAQ_DEVICES_SPA_HEAT : jandy::KEY_IAQ_DEVICES_POOL_HEAT;
  portENTER_CRITICAL(&mux_);
  if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0) {
    portEXIT_CRITICAL(&mux_);
    ESP_LOGW(TAG, "setpoint REFUSED: another sequence is in progress (%s)", is_spa ? "spa" : "pool");
    return;
  }
  iaq_settemp_key_ = key;
  iaq_settemp_val_ = clamped;
  iaq_settemp_step_ = 1;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "setpoint: start sequence -> %s %dF (requested %u)", is_spa ? "spa" : "pool", clamped, temp);
}

// core-1: transmit the 0x24 setpoint value frame on the bus.
void JandyAqualink::send_settemp_set_(uint16_t temp) {
  uint8_t out[32];
  size_t n = jandy::build_settemp_frame(temp, out, sizeof(out));
  uart_write_bytes(JANDY_UART, reinterpret_cast<const char *>(out), n);
  ESP_LOGW(TAG, "setpoint value frame sent: %uF (%u bytes)", temp, static_cast<unsigned>(n));
}
```

- [ ] **Step 3: `advance_settemp_sequence_`** (`jandy_aqualink.cpp`), after `advance_heater_sequence_` (line 629):

```cpp
// core-1: advance one step of the gated setpoint sequence. Mirrors advance_set_sequence_
// (the pump value-set) but navigates to SET_TEMP via the DEVICES heat item and writes a
// temperature. SAFETY: the page-scoped heat item (0x14/0x15) is sent ONLY when page ==
// DEVICES, and the 0x24 value frame ONLY when page == SET_TEMP (settemp_write_allowed),
// both re-checked at the transmit point. Interlock off aborts. The 0x24 itself goes out
// on the panel's 0x31, handled in task_loop.
void JandyAqualink::advance_settemp_sequence_() {
  if (!interlock_) {
    ESP_LOGW(TAG, "setpoint aborted at step %d: interlock OFF", iaq_settemp_step_);
    iaq_settemp_step_ = 0;
    iaq_settemp_key_ = -1;
    send_iaq_ack_(0x00);
    return;
  }
  int page = iaq_reader_.current_page();
  switch (iaq_settemp_step_) {
    case 1:  // go HOME first (deterministic start)
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_settemp_step_ = 2;
      break;
    case 2:  // on HOME -> open Other Devices; else retry HOME
      if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);
        iaq_settemp_step_ = 3;
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    case 3:  // on DEVICES -> press the heat item. SAFETY: only on DEVICES (0x36).
      if (page == jandy::IAQ_PAGE_DEVICES) {
        send_iaq_ack_(static_cast<uint8_t>(iaq_settemp_key_));
        ESP_LOGW(TAG, "setpoint: pressed heat item 0x%02X on DEVICES", iaq_settemp_key_);
        iaq_settemp_step_ = 4;
      } else if (page == jandy::IAQ_PAGE_HOME) {
        send_iaq_ack_(jandy::KEY_IAQT_OTHER_DEVICES);  // not there yet, retry nav
      } else {
        send_iaq_ack_(0x00);  // wait for the panel to land on DEVICES
      }
      break;
    case 4:  // on SET_TEMP -> request the control slot (key 0x80). SAFETY: gate the page.
      if (jandy::settemp_write_allowed(static_cast<uint8_t>(page))) {
        send_iaq_ack_(0x80);
        iaq_settemp_step_ = 5;
      } else {
        send_iaq_ack_(0x00);  // wait for SET_TEMP (or the heat item only toggled; times out safe)
      }
      break;
    case 5:  // waiting for the panel's 0x31; the 0x24 goes out there, not on a poll
      send_iaq_ack_(0x00);
      break;
    case 6:  // value sent -> return HOME
      send_iaq_ack_(jandy::KEY_IAQT_HOME);
      iaq_settemp_step_ = 7;
      break;
    case 7:  // on HOME -> done
      if (page == jandy::IAQ_PAGE_HOME) {
        ESP_LOGW(TAG, "setpoint sequence complete (key 0x%02X -> %dF)", iaq_settemp_key_, iaq_settemp_val_);
        iaq_settemp_step_ = 0;
        iaq_settemp_key_ = -1;
        send_iaq_ack_(0x00);
      } else {
        send_iaq_ack_(jandy::KEY_IAQT_HOME);
      }
      break;
    default:
      iaq_settemp_step_ = 0;
      iaq_settemp_key_ = -1;
      send_iaq_ack_(0x00);
      break;
  }
}
```

- [ ] **Step 4: Dispatch block** (`task_loop`). Immediately after the survey one-shot block (Task 6 step 3) and before the normal armed-key path, insert (mirrors the pump-set dispatch, with the 0x24 sent on the panel's 0x31 at step 5):

```cpp
        // The setpoint sequence owns the iAq reply while active (mutually exclusive
        // with the other write-sequences). The 0x24 value frame goes out on 0x31.
        int settemp_step;
        portENTER_CRITICAL(&mux_);
        settemp_step = iaq_settemp_step_;
        portEXIT_CRITICAL(&mux_);
        if (settemp_step != 0) {
          if (f.cmd() == jandy::CMD_IAQ_CTRL_READY && settemp_step == 5) {
            send_settemp_set_(static_cast<uint16_t>(iaq_settemp_val_));
            portENTER_CRITICAL(&mux_);
            iaq_settemp_step_ = 6;
            portEXIT_CRITICAL(&mux_);
          } else if (f.cmd() == 0x30) {
            advance_settemp_sequence_();
          } else {
            send_iaq_ack_(0x00);
          }
          iaq_reader_.feed(f);
          portENTER_CRITICAL(&mux_);
          iaq_current_page_ = iaq_reader_.current_page();
          frames_++;
          portEXIT_CRITICAL(&mux_);
          continue;  // this 0x33 frame fully handled by the setpoint sequence
        }
```

- [ ] **Step 5: Interlock-off abort.** In `set_interlock`, inside `if (!on)` (after the survey clears from Task 6 step 4):

```cpp
    iaq_settemp_step_ = 0;   // and any in-progress setpoint sequence
    iaq_settemp_key_ = -1;
```

- [ ] **Step 6: Extend the busy-guards** so every write entry point refuses while the setpoint sequence runs. In `set_pump_rpm` change `if (iaq_toggle_step_ != 0 || iaq_heater_step_ != 0)` to `if (iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0)`. In `press_device_toggle`, `press_heater`, and `survey_press` change `if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0)` to `if (iaq_set_step_ != 0 || iaq_toggle_step_ != 0 || iaq_heater_step_ != 0 || iaq_settemp_step_ != 0)`.

  ALSO apply the deferred final-review robustness fix from Task 6 at the SAME guard sites: add `|| iaq_survey_key_ >= 0` to the busy checks in `set_pump_rpm`, `press_device_toggle`, and `press_heater` (so a new equipment sequence also refuses while a survey one-shot is armed, symmetric with the guard `survey_press` already has). Net: those three guards end up checking `iaq_set_step_`/`iaq_toggle_step_`/`iaq_heater_step_`/`iaq_settemp_step_` AND `iaq_survey_key_ >= 0` (set_pump_rpm omits its own `iaq_set_step_` term as before). This was deferred here because Task 10 already rewrites these exact lines.

- [ ] **Step 7: Number entities** (`firmware/pool-bridge.yaml`). After the `Pump Speed Set` number block (line 351), within the `number:` list:

```yaml
  - platform: template
    name: "Pool Heat Setpoint"
    id: pool_heat_setpoint
    icon: "mdi:radiator"
    unit_of_measurement: "°F"
    min_value: 45
    max_value: 90
    step: 1
    mode: box
    optimistic: true
    set_action:
      - lambda: "id(jandy_comp).set_heater_setpoint(false, (uint16_t) x);"
  - platform: template
    name: "Spa Heat Setpoint"
    id: spa_heat_setpoint
    icon: "mdi:hot-tub"
    unit_of_measurement: "°F"
    min_value: 80
    max_value: 104
    step: 1
    mode: box
    optimistic: true
    set_action:
      - lambda: "id(jandy_comp).set_heater_setpoint(true, (uint16_t) x);"
```

- [ ] **Step 8: Brace-balance check:** `python -c "s=open(r'<repo>/components/jandy_aqualink/jandy_aqualink.cpp').read(); print('braces', s.count('{')-s.count('}'))"` -> `braces 0`. Then `python -m pytest -q` (no Python changed, but confirm still green).

- [ ] **Step 9: Commit:**

```
git -C <repo> add -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp firmware/pool-bridge.yaml
git -C <repo> commit -m "feat(session-9): gated heater setpoint sequence (DEVICES -> SET_TEMP -> 0x24) + HA number entities

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>" -- components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp firmware/pool-bridge.yaml
```

---

### Task 11: Deploy + founder-watched LIVE setpoint test

DO NOT start without the founder watching the equipment.

- [ ] **Step 1: Push, patch the live dashboard yaml** (add the 2 number entities, back up first, readback-verify), **compile + flash** (same recipe as Task 8 steps 1-3).

- [ ] **Step 2: Post-flash health.** `selftest PASS`, `checksum_errors=0`, presence ON, interlock OFF. Do NOT actuate on a FAIL.

- [ ] **Step 3: Refusal check** (interlock OFF): set `Pool Heat Setpoint` to 85 in HA -> expect `setpoint REFUSED: safety interlock is OFF (pool 85)`, nothing actuates.

- [ ] **Step 4: Pool setpoint** (founder watching). Arm the interlock. Enable Pool Heat (so SET_TEMP is reachable). Set `Pool Heat Setpoint` = 85. Confirm the log: `setpoint: pressed heat item 0x14 on DEVICES`, `IAQ PAGE SET_TEMP(0x39)`, `setpoint value frame sent: 85F`, `setpoint sequence complete`. Re-open the temperature screen (or use the survey enumeration) to read the pool target back = 85.

- [ ] **Step 5: Spa setpoint** (founder watching). Switch to spa mode (Switch to Spa Mode). Set `Spa Heat Setpoint` = 94. Confirm the same log sequence with `0x15` and `94F`. Read back = 94. Enable Spa Heat and confirm at the spa that it heads toward 94, NOT 104.

- [ ] **Step 6: Spa auto-off observation.** With Spa Heat enabled, switch spa -> pool (re-press Filter Pump after the valves settle, per the durable note). Watch whether the panel drops Spa Heat on its own (the `Spa Heat (enabled)` sensor). Record: drops by itself (rely on the panel) or not (a future HA auto-off automation is needed).

- [ ] **Step 7: Resting state.** Leave **Pool Heat enabled at 85** (the panel holds the pool at 85 going forward; it is June and the pool is above 85, so it will not fire until it drops). Leave **spa heat off**. Confirm presence ON, interlock OFF. Note any side effects.

---

### Task 12: Record findings + finish the branch

- [ ] **Step 1: Update memory + ROADMAP + the spec** with: the shipped SHA, the confirmed SET_TEMP route + keys, whether SET_TEMP enumerates (and the captured setpoint-readback values), the spa-auto-off result, and the final resting state. If the survey found SET_TEMP unreachable, record that and that only on/off + sensors shipped.

- [ ] **Step 2: Confirm `git status` clean and pushed** (`## master...origin/master`, no `[ahead]`).

- [ ] **Step 3:** Use the `superpowers:finishing-a-development-branch` skill to decide integration (this repo works directly on `master`).

---

## Self-Review notes

- **Spec coverage:** survey route confirmation (Tasks 6, 9); setpoint value-set reusing the proven 0x24 path, format pinned to AqualinkD captures (Tasks 1-2, 10); clamps pool 45-90 / spa 80-104 (Task 1); HA number entities (Task 10); heat-enabled status sensors (Tasks 3-5); the water-mode reliability item (already fixed in Session 9, locked by a regression test, Task 3 + Scope note); live tests (Tasks 9, 11). The optional setpoint-readback bonus is captured in the Task 9 survey and Task 11 readback (publishing it as its own sensor is deferred unless the survey shows it is trivial).
- **No guessed bytes:** the value frame is TDD'd against AqualinkD's real captured Set-Temp(pool) frames (50F, 100F, checksums verified by hand). The only live-confirmed unknown is the navigation route (Task 9), which gates Task 10.
- **Type/name consistency:** `num2iaqt_temp`, `build_settemp_frame`, `settemp_write_allowed`, `pool_setpoint_check`, `spa_setpoint_check`, `set_heater_setpoint`, `advance_settemp_sequence_`, `send_settemp_set_`, `iaq_settemp_step_/key_/val_`, `KEY_IAQ_DEVICES_POOL_HEAT (0x14)`, `KEY_IAQ_DEVICES_SPA_HEAT (0x15)`, `IAQ_PAGE_SET_TEMP (0x39)`, `pool_heat_bs_/spa_heat_bs_`, `he_pool_/he_spa_`, `CONF_POOL_HEAT_ENABLED/CONF_SPA_HEAT_ENABLED` are used identically across Python, C++, codegen, and yaml.
- **Page guard:** the heat item (0x14/0x15) is sent only on DEVICES; the 0x24 value frame only on SET_TEMP (`settemp_write_allowed`), re-checked at the transmit point, mirroring the proven pump (`vsp_adjust_allowed`) pattern.
