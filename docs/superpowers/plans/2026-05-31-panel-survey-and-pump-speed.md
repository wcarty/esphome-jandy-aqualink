# 🛠️ Panel Capability Survey and Pump Speed Reading Implementation Plan

> [!NOTE]
> **Historical engineering record.** This implementation plan may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a safe navigation-only path plus rich page logging to the iAqualink emulator, run a full read-only survey of the panel, produce a Panel Capability Map, and read live pump speed into Home Assistant.

**Architecture:** Extend the existing `jandy_aqualink` ESPHome component (core-1 reply task unchanged). Reuse the proven one-shot key-on-`0x30` mechanism, but draw keys from a separate navigation allowlist that cannot contain an equipment or value keycode. Pure protocol logic is TDD'd in Python under `jandy/` and mirrored in the C++ `selftest()` against the same frame vectors. Config changes go to both the repo reference yaml and the live ESPHome dashboard yaml.

**Tech Stack:** ESPHome (Arduino framework, ESP-IDF UART), C++ on ESP32, Python 3 `unittest` for the reference oracle, the ESPHome dashboard at 192.168.1.126:6052 (build/flash over its WebSocket via `esphome_ws.ps1`), device at 192.168.4.51.

**Execution note:** Tasks 1 to 5 are buildable now. Task 5 ends with a deploy. The CHECKPOINT is a live, founder-watched hardware survey (not unit-testable). Tasks 6 to 8 are finalized against the frames captured during the survey: the decoder code below follows the AqualinkD reference format, and the test fixtures are pinned from the real capture exactly as the temperature fixtures were. If the survey reveals a materially different structure, Tasks 7 to 8 get a short re-plan before implementation.

**Safety invariant (every task must preserve it):** the master interlock "Pool Keypad Keypress Armed" gates every transmitted key. The navigation path can emit only global navigation keys, plus `0x18` (Other Devices) and only while the panel is confirmed on the HOME page. No equipment, value, or commit button is ever pressed by any task in this plan.

---

## File Structure

- `jandy/frames.py` (modify): add the iAqualink navigation-key allowlist `is_iaq_nav_key` next to the existing `is_allowed_iaq_key`.
- `jandy/iaq.py` (modify): expose `IaqReader.current_page`, add `iaq_page_name`, add `parse_iaq_button`, and (Task 7) add the pump-speed decode.
- `components/jandy_aqualink/jandy_proto.h` / `.cpp` (modify): mirror `is_iaq_nav_key`, `IaqReader::current_page()`, `iaq_page_name`, the pump decode; extend `selftest()`.
- `components/jandy_aqualink/jandy_aqualink.h` / `.cpp` (modify): add `iaq_nav(key)`, enhanced survey logging (page name + frame role + the `0x60` pump traffic), and (Task 8) the pump-speed sensor wiring.
- `components/jandy_aqualink/__init__.py` (modify, Task 8): config keys + codegen for the new pump sensors.
- `firmware/pool-bridge.yaml` (modify): navigation buttons, presence `restore_mode`, pump sensors.
- `tests/test_iaq_nav.py` (create): navigation allowlist tests.
- `tests/test_iaq_button.py` (create): button-parser tests.
- `tests/test_iaq.py` (modify): `current_page` and `iaq_page_name` tests; (Task 7) pump-speed tests.
- `docs/PANEL-CAPABILITY-MAP.md` (create, Task 6): the survey deliverable.

Test command (from `esp32-experiment/`): `python -m unittest discover -s tests -t .`
C++ logic is verified by the on-device `selftest()` boot line after an OTA upload (no host C++ build exists).

---

## Task 1: iAqualink navigation-key allowlist

**Files:**
- Modify: `jandy/frames.py` (after `is_allowed_iaq_key`, ~line 162)
- Modify: `components/jandy_aqualink/jandy_proto.h` (after `is_allowed_iaq_key`, ~line 85)
- Modify: `components/jandy_aqualink/jandy_proto.cpp` (the `build_key_ack` selftest block, ~line 248)
- Test: `tests/test_iaq_nav.py` (create)

- [ ] **Step 1: Write the failing test**

Create `tests/test_iaq_nav.py`:

```python
"""The iAqualink navigation allowlist is a safety gate: it must pass the global
page-movement keys and refuse every equipment and value keycode. 0x18 (Other
Devices) is deliberately excluded here because it is only safe from the HOME
page and is gated by the caller, not by this set."""

import unittest

from jandy.frames import is_iaq_nav_key


class TestIaqNavAllowlist(unittest.TestCase):
    def test_global_nav_keys_allowed(self):
        for key in (0x01, 0x02, 0x03, 0x05, 0x06, 0x20, 0x21):  # home/menu/onetouch/back/status/prev/next
            self.assertTrue(is_iaq_nav_key(key), f"0x{key:02X} should be a nav key")

    def test_other_devices_is_not_a_global_nav_key(self):
        # 0x18 is page-context-dependent; the caller gates it to the HOME page.
        self.assertFalse(is_iaq_nav_key(0x18))

    def test_equipment_and_value_keys_refused(self):
        # Home/devices equipment tiles and the digit/value space.
        for key in (0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x19, 0x1A, 0x1D, 0x1E, 0x1F, 0x30, 0x31):
            self.assertFalse(is_iaq_nav_key(key), f"0x{key:02X} must be refused")


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run test to verify it fails**

Run: `python -m unittest tests.test_iaq_nav -v`
Expected: FAIL with `ImportError: cannot import name 'is_iaq_nav_key'`.

- [ ] **Step 3: Implement in Python**

In `jandy/frames.py`, after `is_allowed_iaq_key` (~line 162), add:

```python
# iAqualink global navigation keys (AqualinkD aq_serial.h KEY_IAQTCH_*). These are
# page-level keys that move the display and never actuate equipment, on ANY page:
# HOME 0x01, MENU 0x02, ONETOUCH 0x03, BACK 0x05, STATUS 0x06, PREV_PAGE 0x20,
# NEXT_PAGE 0x21. NOTE these values are iAqualink-protocol keycodes and are a
# different namespace from the AllButton KEY_* above (e.g. iAq 0x02 = MENU, while
# the AllButton comment notes 0x02 = pump). is_iaq_nav_key is only ever consulted
# on the iAqualink (0x33) path.
KEY_IAQT_HOME = 0x01
KEY_IAQT_MENU = 0x02
KEY_IAQT_ONETOUCH = 0x03
KEY_IAQT_BACK = 0x05
KEY_IAQT_STATUS = 0x06
KEY_IAQT_PREV_PAGE = 0x20
KEY_IAQT_NEXT_PAGE = 0x21
# Other Devices (home button 8). Only meaningful, and only safe, from the HOME
# page, so it is NOT in the nav set; the caller gates it to the HOME page.
KEY_IAQT_OTHER_DEVICES = 0x18

_IAQ_NAV_KEYS = frozenset(
    {
        KEY_IAQT_HOME,
        KEY_IAQT_MENU,
        KEY_IAQT_ONETOUCH,
        KEY_IAQT_BACK,
        KEY_IAQT_STATUS,
        KEY_IAQT_PREV_PAGE,
        KEY_IAQT_NEXT_PAGE,
    }
)


def is_iaq_nav_key(key: int) -> bool:
    """True only for global iAqualink navigation keys (safe on any page).

    Excludes 0x18 (Other Devices), which the caller gates to the HOME page, and
    every equipment/value keycode.
    """
    return key in _IAQ_NAV_KEYS
```

- [ ] **Step 4: Run test to verify it passes**

Run: `python -m unittest tests.test_iaq_nav -v`
Expected: PASS (3 tests).

- [ ] **Step 5: Mirror in C++**

In `jandy_proto.h`, after `is_allowed_iaq_key` (~line 85), add:

```cpp
// iAqualink global navigation keys (AqualinkD aq_serial.h KEY_IAQTCH_*): page
// movement only, never equipment, on any page. 0x18 (Other Devices) is NOT here
// because it is only safe from the HOME page; iaq_nav() gates it separately.
static constexpr uint8_t KEY_IAQT_HOME = 0x01, KEY_IAQT_MENU = 0x02, KEY_IAQT_ONETOUCH = 0x03,
                         KEY_IAQT_BACK = 0x05, KEY_IAQT_STATUS = 0x06, KEY_IAQT_PREV_PAGE = 0x20,
                         KEY_IAQT_NEXT_PAGE = 0x21, KEY_IAQT_OTHER_DEVICES = 0x18;

inline bool is_iaq_nav_key(uint8_t key) {
  return key == KEY_IAQT_HOME || key == KEY_IAQT_MENU || key == KEY_IAQT_ONETOUCH ||
         key == KEY_IAQT_BACK || key == KEY_IAQT_STATUS || key == KEY_IAQT_PREV_PAGE ||
         key == KEY_IAQT_NEXT_PAGE;
}
```

In `jandy_proto.cpp`, inside the `build_key_ack` selftest block (the `total++;` case ending ~line 293, just before `if (pass) ok++;`), add checks:

```cpp
    // iAqualink navigation allowlist: globals pass, 0x18 and equipment refused.
    const uint8_t nav_ok[] = {0x01, 0x02, 0x03, 0x05, 0x06, 0x20, 0x21};
    for (uint8_t k : nav_ok)
      if (!is_iaq_nav_key(k)) pass = false;
    const uint8_t nav_no[] = {0x18, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17, 0x1D, 0x31};
    for (uint8_t k : nav_no)
      if (is_iaq_nav_key(k)) pass = false;
```

- [ ] **Step 6: Commit**

```bash
git add jandy/frames.py components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp tests/test_iaq_nav.py
git commit -m "Add iAqualink navigation-key allowlist (global keys only)

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 2: IaqReader current page + page-name helper

**Files:**
- Modify: `jandy/iaq.py` (the `IaqReader` class and module top)
- Modify: `components/jandy_aqualink/jandy_proto.h` (the `IaqReader` class, ~line 140) and `.cpp` (the `IaqReader::feed` + selftest)
- Test: `tests/test_iaq.py` (add a class)

- [ ] **Step 1: Write the failing test**

Add to `tests/test_iaq.py`:

```python
from jandy.iaq import iaq_page_name


class TestIaqCurrentPage(unittest.TestCase):
    def test_current_page_starts_zero(self):
        self.assertEqual(IaqReader().current_page, 0)

    def test_current_page_set_after_home_page_end(self):
        r = IaqReader()
        feed_frames(r, IAQ_PAGE_START_HOME, IAQ_MSG_I4_SPA, IAQ_MSG_I0_88, IAQ_PAGE_END)
        self.assertEqual(r.current_page, 0x01)

    def test_current_page_tracks_devices_page(self):
        r = IaqReader()
        feed_frames(r, fx.h("10 02 33 23 36 9E 10 03"), IAQ_PAGE_END)
        self.assertEqual(r.current_page, 0x36)

    def test_page_name_known_and_unknown(self):
        self.assertEqual(iaq_page_name(0x01), "HOME")
        self.assertEqual(iaq_page_name(0x36), "DEVICES")
        self.assertEqual(iaq_page_name(0x5B), "STATUS")
        self.assertEqual(iaq_page_name(0x1E), "SET_VSP")
        self.assertTrue(iaq_page_name(0x77).startswith("0x"))
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m unittest tests.test_iaq -v`
Expected: FAIL (`ImportError: cannot import name 'iaq_page_name'` / `AttributeError: current_page`).

- [ ] **Step 3: Implement in Python**

In `jandy/iaq.py`, add at module level (after the page constants):

```python
IAQ_PAGE_DEVICES = 0x36
IAQ_PAGE_STATUS = 0x5B
IAQ_PAGE_SET_VSP = 0x1E

_PAGE_NAMES = {
    0x01: "HOME",
    0x0A: "DEVICES_REV",
    0x0F: "MENU",
    0x1D: "SET_BOOST",
    0x1E: "SET_VSP",
    0x2A: "STATUS2",
    0x2D: "VSP_SETUP",
    0x30: "SET_SWG",
    0x35: "DEVICES2",
    0x36: "DEVICES",
    0x39: "SET_TEMP",
    0x48: "COLOR_LIGHT",
    0x4B: "SET_TIME",
    0x4D: "ONETOUCH",
    0x51: "DEVICES3",
    0x5B: "STATUS",
}


def iaq_page_name(page_type: int) -> str:
    """Human name for an iAqualink page type, or 0xNN if unknown."""
    return _PAGE_NAMES.get(page_type, f"0x{page_type:02X}")
```

In `IaqReader.__init__`, add `self.current_page = 0`. In `IaqReader.feed`, in the `CMD_IAQ_PAGE_END` branch, set `self.current_page = self._page_type` before clearing lines:

```python
        elif cmd == CMD_IAQ_PAGE_END:
            self.current_page = self._page_type  # promote the displayed page
            if self._page_type == IAQ_PAGE_HOME:
                self._commit_home()
            self._lines = {}
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m unittest tests.test_iaq -v`
Expected: PASS (all existing + 4 new).

- [ ] **Step 5: Mirror in C++**

In `jandy_proto.h` `IaqReader` (public), add an accessor and a free function declaration:

```cpp
  int current_page() const { return current_page_; }
```
and in the private members add `uint8_t current_page_ = 0;`. Above the class, declare:
```cpp
const char *iaq_page_name(uint8_t page_type);  // human page name, or "" if unknown
```

In `jandy_proto.cpp`, in `IaqReader::feed` `cmd == 0x28` branch, set the page before commit:
```cpp
  } else if (cmd == 0x28) {  // CMD_IAQ_PAGE_END
    current_page_ = page_type_;
    if (page_type_ == 0x01) commit_home();
    for (int i = 0; i < MAX_LINES; ++i) present_[i] = false;
  }
```
Add the name function (only the names needed for logging):
```cpp
const char *iaq_page_name(uint8_t p) {
  switch (p) {
    case 0x01: return "HOME";
    case 0x0F: return "MENU";
    case 0x1E: return "SET_VSP";
    case 0x2A: return "STATUS2";
    case 0x2D: return "VSP_SETUP";
    case 0x30: return "SET_SWG";
    case 0x35: return "DEVICES2";
    case 0x36: return "DEVICES";
    case 0x39: return "SET_TEMP";
    case 0x4D: return "ONETOUCH";
    case 0x5B: return "STATUS";
    default: return "?";
  }
}
```
Extend `selftest()` (the iAqualink HOME-page decode block, ~line 314) to also assert `ir.current_page() == 0x01` after the page-end frame.

- [ ] **Step 6: Commit**

```bash
git add jandy/iaq.py components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp tests/test_iaq.py
git commit -m "Track current iAqualink page and add page-name helper

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 3: iAqualink button parser (Python, for capture analysis)

**Files:**
- Modify: `jandy/iaq.py`
- Test: `tests/test_iaq_button.py` (create)

The panel enumerates each page's buttons as `0x24` frames: data = [index, state, unknown, type, then NUL-separated printable segments]. We extract index/state/type and the text segments. The segments hold the label and a state token (ON/OFF/ENA/ADJ); the survey reads them by eye, so the parser returns them as a list and does not guess which is which.

- [ ] **Step 1: Write the failing test**

Create `tests/test_iaq_button.py`:

```python
"""Parse iAqualink 0x24 button frames into (index, state, type, segments).
Fixtures are real button frames from the AqualinkD reference capture
(source/iaqtouch.h), covering the home and devices page shapes."""

import unittest

from jandy.frames import FrameExtractor
from jandy.iaq import parse_iaq_button
from tests import fixtures as fx


def one(wire):
    frames = FrameExtractor().feed(fx.h(wire))
    assert len(frames) == 1
    return frames[0]


class TestParseIaqButton(unittest.TestCase):
    def test_home_filter_pump_two_word_label(self):
        b = parse_iaq_button(one("10 02 33 24 00 00 00 08 46 69 6C 74 65 72 00 50 75 6D 70 00 79 10 03"))
        self.assertEqual(b.index, 0)
        self.assertEqual(b.state, 0x00)
        self.assertEqual(b.type, 0x08)
        self.assertEqual(b.segments, ["Filter", "Pump"])

    def test_devices_filter_pump_on(self):
        b = parse_iaq_button(
            one("10 02 33 24 00 01 00 01 46 69 6C 74 65 72 20 50 75 6D 70 00 4F 4E 20 00 50 10 03")
        )
        self.assertEqual(b.index, 0)
        self.assertEqual(b.state, 0x01)
        self.assertEqual(b.segments, ["Filter Pump", "ON "])

    def test_devices_vsp1_spd_adj(self):
        b = parse_iaq_button(
            one("10 02 33 24 02 00 00 01 56 53 50 31 20 53 70 64 00 41 44 4A 00 AC 10 03")
        )
        self.assertEqual(b.index, 2)
        self.assertEqual(b.segments, ["VSP1 Spd", "ADJ"])

    def test_non_button_frame_returns_none(self):
        self.assertIsNone(parse_iaq_button(one("10 02 33 30 75 10 03")))  # a poll


if __name__ == "__main__":
    unittest.main()
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m unittest tests.test_iaq_button -v`
Expected: FAIL (`cannot import name 'parse_iaq_button'`).

- [ ] **Step 3: Implement**

In `jandy/iaq.py` add:

```python
CMD_IAQ_PAGE_BUTTON = 0x24


class IaqButton:
    __slots__ = ("index", "state", "type", "segments")

    def __init__(self, index, state, type_, segments):
        self.index = index
        self.state = state
        self.type = type_
        self.segments = segments

    def __repr__(self):
        return f"IaqButton(i={self.index} state=0x{self.state:02X} type=0x{self.type:02X} {self.segments})"


def parse_iaq_button(frame):
    """Parse a 0x24 button frame, or return None if it is not one.

    Layout after cmd: data[0]=index, [1]=state, [2]=unknown, [3]=type, then
    NUL-separated printable text segments (label words plus a state token).
    """
    if frame.cmd != CMD_IAQ_PAGE_BUTTON:
        return None
    d = frame.data
    if len(d) < 4:
        return None
    segments = []
    cur = []
    for b in d[4:]:
        if b == 0x00:
            if cur:
                segments.append("".join(cur))
                cur = []
        elif 0x20 <= b <= 0x7E:
            cur.append(chr(b))
    if cur:
        segments.append("".join(cur))
    return IaqButton(d[0], d[1], d[3], segments)
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m unittest tests.test_iaq_button -v`
Expected: PASS (4 tests).

- [ ] **Step 5: Full suite green**

Run: `python -m unittest discover -s tests -t .`
Expected: PASS (all tests).

- [ ] **Step 6: Commit**

```bash
git add jandy/iaq.py tests/test_iaq_button.py
git commit -m "Add iAqualink 0x24 button-frame parser for survey analysis

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 4: Navigation method + survey logging (firmware)

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.h` (declare `iaq_nav`)
- Modify: `components/jandy_aqualink/jandy_aqualink.cpp` (implement `iaq_nav`, enhance `log_iaq_frame`, log `0x60`)

This is component wiring on top of already-tested pure logic. There is no host C++ test build; the pure gate (`is_iaq_nav_key`) is covered by `selftest()` from Task 1, and the method gating mirrors the existing `iaq_press` exactly. Verification is on-device after Task 5's upload.

- [ ] **Step 1: Declare `iaq_nav` in the header**

In `jandy_aqualink.h`, after the `iaq_press` declaration (~line 62), add:

```cpp
  // Send one global navigation key on the iAqualink path to walk pages during a
  // read-only survey. Gated by the master interlock + iAqualink presence + the
  // navigation allowlist. The Other Devices key (0x18) is accepted only while the
  // decoder confirms we are on the HOME page, so it can never mean a grid tile on
  // another page. Sends exactly one key; never an equipment or value keycode.
  void iaq_nav(uint8_t key);
```

- [ ] **Step 2: Implement `iaq_nav`**

In `jandy_aqualink.cpp`, after `iaq_press` (~line 207), add:

```cpp
void JandyAqualink::iaq_nav(uint8_t key) {
  if (!interlock_) {
    ESP_LOGW(TAG, "iaq nav REFUSED: safety interlock is OFF (key=0x%02X)", key);
    return;
  }
  if (!iaq_presence_) {
    ESP_LOGW(TAG, "iaq nav REFUSED: iAqualink presence is OFF (key=0x%02X)", key);
    return;
  }
  bool ok = jandy::is_iaq_nav_key(key);
  if (!ok && key == jandy::KEY_IAQT_OTHER_DEVICES && iaq_reader_.current_page() == 0x01) {
    ok = true;  // Other Devices: only from the HOME page
  }
  if (!ok) {
    ESP_LOGW(TAG, "iaq nav REFUSED: key 0x%02X is not a nav key here (page=0x%02X)", key,
             iaq_reader_.current_page());
    return;
  }
  portENTER_CRITICAL(&mux_);
  iaq_armed_key_ = key;
  portEXIT_CRITICAL(&mux_);
  ESP_LOGW(TAG, "ARMED iAq NAV key 0x%02X -> sent on next iAqualink poll (one press)", key);
}
```

- [ ] **Step 3: Enhance survey logging**

In `jandy_aqualink.cpp`, replace the body of `log_iaq_frame` (~line 224) so each frame is prefixed with its role and, for page starts, the page name. Keep the existing hex+ascii dump after the prefix:

```cpp
void JandyAqualink::log_iaq_frame(const jandy::Frame &f) {
  uint8_t cmd = f.cmd();
  if (cmd == 0x30 || cmd == 0x00) return;  // skip bare poll / probe keepalives
  const char *role = "frame";
  char extra[24] = "";
  if (cmd == 0x23) {  // page start: data[0] = page type
    role = "PAGE START";
    uint8_t pt = f.data_len() >= 1 ? f.data()[0] : 0;
    snprintf(extra, sizeof(extra), " %s(0x%02X)", jandy::iaq_page_name(pt), pt);
  } else if (cmd == 0x24) {
    role = "BUTTON";
  } else if (cmd == 0x25) {
    role = "MSG";
  } else if (cmd == 0x28) {
    role = "PAGE END";
  }
  char hex[3 * 40 + 1];
  char asc[40 + 1];
  static const char *const H = "0123456789ABCDEF";
  size_t n = f.raw.size() > 40 ? 40 : f.raw.size();
  size_t hp = 0;
  for (size_t i = 0; i < n; ++i) {
    uint8_t b = f.raw[i];
    hex[hp++] = H[b >> 4];
    hex[hp++] = H[b & 0x0F];
    hex[hp++] = ' ';
    asc[i] = (b >= 0x20 && b <= 0x7E) ? static_cast<char>(b) : '.';
  }
  hex[hp] = '\0';
  asc[n] = '\0';
  ESP_LOGI(TAG, "IAQ %s%s | %s| %s", role, extra, hex, asc);
}
```

- [ ] **Step 4: Log the pump (0x60) traffic for a possible passive read**

In `observe_frame` (~line 249, the passive path for non-0x33 frames), after the census append, add a first-seen-and-changed log of any `0x60` frame so we can spot a passive RPM source during the survey:

```cpp
  if (f.dest() == 0x60) {
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
    ESP_LOGI(TAG, "PUMP 0x60 cmd=0x%02X len=%u: %s", f.cmd(), (unsigned) f.raw.size(), hex);
  }
```

- [ ] **Step 5: Commit (deploy happens in Task 5)**

```bash
git add components/jandy_aqualink/jandy_aqualink.h components/jandy_aqualink/jandy_aqualink.cpp
git commit -m "Add gated iAqualink navigation + survey page/pump logging

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 5: HA navigation buttons, presence persistence, and deploy

**Files:**
- Modify: `firmware/pool-bridge.yaml` (reference copy)
- Modify: the live dashboard config `pool-bridge.yaml` on 192.168.1.126:6052
- Push: the component repo to GitHub (the dashboard pulls the component from there)

- [ ] **Step 1: Update the reference yaml (navigation buttons)**

In `firmware/pool-bridge.yaml`, in the `button:` block (after the existing iAqualink toggles, ~line 175), add survey navigation buttons. These are temporary survey instruments:

```yaml
  # --- Survey navigation (read-only). Gated by the master interlock + iAqualink
  # presence. Each sends ONE global page-movement key; none can actuate equipment.
  - platform: template
    name: "iAq Nav Home"
    icon: "mdi:home"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x01);"
  - platform: template
    name: "iAq Nav Other Devices"
    icon: "mdi:format-list-bulleted"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x18);"   # only honored from the HOME page
  - platform: template
    name: "iAq Nav Status"
    icon: "mdi:gauge"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x06);"
  - platform: template
    name: "iAq Nav Menu"
    icon: "mdi:menu"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x02);"
  - platform: template
    name: "iAq Nav Back"
    icon: "mdi:arrow-left"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x05);"
  - platform: template
    name: "iAq Nav Next Page"
    icon: "mdi:chevron-right"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x21);"
  - platform: template
    name: "iAq Nav Prev Page"
    icon: "mdi:chevron-left"
    on_press:
      - lambda: "id(jandy_comp).iaq_nav(0x20);"
```

- [ ] **Step 2: Update the reference yaml (presence persistence)**

In `firmware/pool-bridge.yaml`, change the `iAqualink Presence` switch `restore_mode` from `ALWAYS_OFF` to `RESTORE_DEFAULT_OFF` (~line 111). Leave the `Pool Keypad Keypress Armed` interlock at `ALWAYS_OFF`.

```yaml
    restore_mode: RESTORE_DEFAULT_OFF   # was ALWAYS_OFF; temps keep reading after a reboot
```

- [ ] **Step 3: Commit the reference yaml and push the component**

```bash
git add firmware/pool-bridge.yaml
git commit -m "Add survey nav buttons; persist iAqualink presence across reboots

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push origin master
```

Expected: push succeeds (gh-authed). The dashboard's `external_components` (refresh: 0s) will pull this on the next compile.

- [ ] **Step 4: Mirror the config into the live dashboard yaml**

Back up first, then add the same `button:` entries and the `restore_mode` change to the live config. The live config has the real secrets and the real `external_components` URL, so edit it in place, do not overwrite it with the reference copy.

```powershell
$base = "http://192.168.1.126:6052"
$cfg  = "pool-bridge.yaml"
# Back up the current live config
$raw = Invoke-WebRequest -Uri "$base/edit?configuration=$cfg" -Method GET
$stamp = (Get-Item C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1).LastWriteTime.ToString("yyyyMMdd-HHmmss")
$bak = "C:\Users\Falcon\Documents\pool-controller\dashboard-pool-bridge.BACKUP-$stamp.yaml"
[IO.File]::WriteAllText($bak, $raw.Content)
Write-Host "backed up live config to $bak ($($raw.Content.Length) bytes)"
```

Then apply the same two edits (the seven nav buttons, and `restore_mode: RESTORE_DEFAULT_OFF` on the presence switch) to the backed-up content and POST it back:

```powershell
$content = [IO.File]::ReadAllText($bak)
# Edit $content in an editor or via string replace to add the nav buttons block
# and change the presence switch restore_mode, matching firmware/pool-bridge.yaml.
# Then:
Invoke-WebRequest -Uri "$base/edit?configuration=$cfg" -Method POST -Body $edited -ContentType "application/x-www-form-urlencoded"
```

(In practice: GET the live yaml, apply the same diff as Steps 1 to 2, POST it back. Keep the backup.)

- [ ] **Step 5: Compile and upload**

```powershell
cd C:\Users\Falcon\Documents\pool-controller
./esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -Port 192.168.4.51
./esphome_ws.ps1 -Action upload  -Config pool-bridge.yaml -Port 192.168.4.51
```

Expected: compile pulls the freshly pushed component, upload OTAs the device.

- [ ] **Step 6: Verify healthy on-device**

```powershell
./esphome_ws.ps1 -Action logs -Config pool-bridge.yaml -Port 192.168.4.51
```

Confirm in the log:
- `selftest PASS -> N/N` (N increased from the previous build; nav-key + current_page checks pass).
- Presence holds, checksum errors stay 0, reply latency stays double-digit microseconds.
- After a deliberate reboot, `iAqualink Presence` comes back on by itself and temps resume; `Pool Keypad Keypress Armed` stays off.

- [ ] **Step 7: Confirm the safety gate live (before any survey navigation)**

With the interlock OFF, press "iAq Nav Status" in HA and confirm the log shows `iaq nav REFUSED: safety interlock is OFF`. Turn the interlock ON and confirm a nav key now arms. This proves the gate before we navigate anywhere.

---

## CHECKPOINT: live panel survey (founder watching the pump and panel)

Not unit-testable. Drive one key at a time, read the log, confirm the page, then the next key. View only: never press a numbered tile, an ADJ button, a value, or a commit.

- [ ] Baseline: presence on, home page reading, healthy.
- [ ] Interlock ON. Press "iAq Nav Home"; confirm `PAGE START HOME(0x01)` in the log.
- [ ] Press "iAq Nav Other Devices"; confirm a non-HOME `PAGE START` and capture every `BUTTON` line (labels + state tokens). Use Next/Prev to capture any continuation pages. This is the full control inventory.
- [ ] Press "iAq Nav Status"; capture every `MSG` line (RPM, watts, SWG, salt, temps, service state). This is the read inventory and the pump-speed source.
- [ ] Note whether any `PUMP 0x60` line carries a decodable RPM (the zero-navigation read path).
- [ ] Determine RPM vs GPM from the explicit pump labels.
- [ ] View remaining global-key pages (Menu, OneTouch) for the map; view only.
- [ ] Press "iAq Nav Home" to return; interlock OFF; leave the bus idle and healthy.
- [ ] Save the captured frames as fixtures in `tests/fixtures.py` (the exact wire bytes, as the temp fixtures were), tagged with the page they came from.

---

## Task 6: write the Panel Capability Map

**Files:**
- Create: `docs/PANEL-CAPABILITY-MAP.md`

- [ ] **Step 1: Write the document** from the captured logs, with these sections:
  - Readable values: name, source (page or `0x60` frame), byte/text location, units, observed value.
  - Controllable items: name, page + button index, keycode-in-that-page-context, state-token vocabulary, and control mechanism (toggle-in-ACK vs the multi-step value-set handshake for VSP/setpoints).
  - Build roadmap: a safe order for future control, each with a risk level (toggles low; valves/heaters high), so future sessions wire up mapped items instead of discovering blind.
  - Explicit findings: does this panel expose VSP adjust over iAqualink, RPM vs GPM mode, and where the live RPM is readable.

- [ ] **Step 2: Commit**

```bash
git add docs/PANEL-CAPABILITY-MAP.md tests/fixtures.py
git commit -m "Add Panel Capability Map and captured survey fixtures

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 7: pump speed decoder (TDD against captured fixtures)

**Files:**
- Modify: `jandy/iaq.py` (or a new `jandy/pump.py` if the source is `0x60` rather than the status page)
- Modify: `components/jandy_aqualink/jandy_proto.{h,cpp}` (mirror + selftest)
- Test: `tests/test_pump.py` (create)

Branch decided at the checkpoint:
- **Finding A (preferred): RPM is in passive `0x60` traffic.** Decode it from the `0x60` frame (zero navigation, no home/status conflict). The decoder takes the captured `0x60` frame and returns rpm (and watts if present). Fixtures are the captured `0x60` frames.
- **Finding B: RPM only on the STATUS page text.** Decode the status `MSG` lines: a line containing `RPM:` yields the integer after the colon; `Watts:` likewise. This matches AqualinkD `passDeviceStatusPage`. Fixtures are the captured status-page `0x25` frames. Because holding the status page conflicts with reading temps on the home page, reading becomes on-demand in Task 8 (a button), not continuous.

The code below is the Finding B status-text decoder (the documented path); if Finding A holds, the decoder parses the `0x60` byte layout instead, TDD'd the same way against the captured `0x60` fixtures.

- [ ] **Step 1: Write the failing test** (fixtures pinned from the survey capture; the example below uses the AqualinkD reference status-line shape and is replaced with the real captured bytes):

```python
"""Decode live pump RPM (and watts) from the captured iAqualink status page.
Fixtures are the exact frames captured from the RS panel during the survey."""

import unittest

from jandy.frames import FrameExtractor
from jandy.iaq import IaqReader
from tests import fixtures as fx


def feed(reader, *wires):
    for w in wires:
        for fr in FrameExtractor().feed(w):
            reader.feed(fr)


class TestPumpSpeedFromStatus(unittest.TestCase):
    def test_reads_rpm_from_status_page(self):
        r = IaqReader()
        # REPLACE these with the captured STATUS-page frames from the survey:
        feed(
            r,
            fx.h("10 02 33 23 5B ?? 10 03"),                 # PAGE START STATUS
            fx.h("10 02 33 25 ?? <'Filter Pump' bytes> 00 ?? 10 03"),
            fx.h("10 02 33 25 ?? <'   RPM: 1350' bytes> 00 ?? 10 03"),
            fx.h("10 02 33 28 ?? 10 03"),                    # PAGE END
        )
        self.assertTrue(r.has_pump_rpm)
        self.assertEqual(r.pump_rpm, 1350)
```

- [ ] **Step 2: Run to verify it fails**

Run: `python -m unittest tests.test_pump -v`
Expected: FAIL (`AttributeError: pump_rpm`).

- [ ] **Step 3: Implement (Finding B status-text decoder)**

In `jandy/iaq.py`, extend `IaqReader`: add `self.pump_rpm = 0`, `self.has_pump_rpm = False`, `self.pump_watts = 0`, `self.has_pump_watts = False`. When a STATUS page (`current_page` becomes `0x5B`) ends, scan its lines for `RPM:` and `Watts:` and parse the trailing integer:

```python
        elif cmd == CMD_IAQ_PAGE_END:
            self.current_page = self._page_type
            if self._page_type == IAQ_PAGE_HOME:
                self._commit_home()
            elif self._page_type in (0x5B, 0x2A):  # STATUS
                self._commit_status()
            self._lines = {}
```
```python
    def _commit_status(self):
        for text in self._lines.values():
            up = text.upper()
            if "RPM:" in up:
                v = _leading_int(text.split(":", 1)[1])
                if v is not None:
                    self.pump_rpm = v
                    self.has_pump_rpm = True
            elif "WATTS:" in up:
                v = _leading_int(text.split(":", 1)[1])
                if v is not None:
                    self.pump_watts = v
                    self.has_pump_watts = True
```

- [ ] **Step 4: Run to verify it passes**

Run: `python -m unittest tests.test_pump -v`
Expected: PASS.

- [ ] **Step 5: Mirror in C++ + selftest**

Add `pump_rpm`/`has_pump_rpm` (and watts) to the C++ `Decoded` or to `IaqReader`, add `commit_status()` parsing the same way, and add a selftest block feeding the captured status fixtures and asserting the rpm.

- [ ] **Step 6: Full suite + commit**

```bash
python -m unittest discover -s tests -t .
git add jandy/iaq.py components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp tests/test_pump.py tests/fixtures.py
git commit -m "Decode pump RPM and watts from captured iAqualink frames

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
```

---

## Task 8: pump speed sensors, reading strategy, deploy, validate

**Files:**
- Modify: `components/jandy_aqualink/jandy_aqualink.{h,cpp}` (sensor pointers + publish-on-change + feed the decoder)
- Modify: `components/jandy_aqualink/__init__.py` (config keys + codegen)
- Modify: `firmware/pool-bridge.yaml` + the live dashboard yaml (sensor entries; on-demand "Read Pump Speed" button if Finding B)

- [ ] **Step 1: Add sensor pointers + publish** in `jandy_aqualink.h` (setters `set_pump_rpm_sensor`, `set_pump_watts_sensor`; members `pump_rpm_sensor_`, `pump_watts_sensor_`, volatile mirrors + `pub_` guards), and in `jandy_aqualink.cpp` feed them from the iAq path and publish-on-change in `loop()`, exactly like the temperature sensors (~lines 134 to 140 and 321 to 332).

- [ ] **Step 2: Reading strategy.**
  - Finding A (`0x60` passive): decode in `observe_frame` on each `0x60` frame; continuous, no extra navigation.
  - Finding B (status page): add a gated "Read Pump Speed" template button that arms `iaq_nav(0x06)` (Status), lets the decoder read it, then arms `iaq_nav(0x01)` (Home) to restore temp reading. On-demand only, so it never fights the home-page temp reading.

- [ ] **Step 3: Codegen** in `__init__.py`: add `CONF_PUMP_RPM = "pump_rpm"` and `CONF_PUMP_WATTS = "pump_watts"`, a `_rpm_sensor()` schema (unit `rpm`, `accuracy_decimals=0`, `state_class=STATE_CLASS_MEASUREMENT`, icon `mdi:pump`) and a watts schema (unit `W`, `device_class="power"`), and the matching `to_code` blocks calling `set_pump_rpm_sensor` / `set_pump_watts_sensor`, mirroring the temp-sensor pattern (lines 62 to 95).

- [ ] **Step 4: Config** in `firmware/pool-bridge.yaml` under `jandy_aqualink:` add `pump_rpm: { name: Pool Pump Speed }` and (if present) `pump_watts: { name: Pool Pump Watts }`; add the "Read Pump Speed" button if Finding B. Mirror into the live dashboard yaml (back up first).

- [ ] **Step 5: Deploy**

```bash
git add components/jandy_aqualink/ firmware/pool-bridge.yaml
git commit -m "Publish Pool Pump Speed (and watts) sensor to Home Assistant

Co-Authored-By: Claude Opus 4.8 (1M context) <noreply@anthropic.com>"
git push origin master
```
```powershell
cd C:\Users\Falcon\Documents\pool-controller
./esphome_ws.ps1 -Action compile -Config pool-bridge.yaml -Port 192.168.4.51
./esphome_ws.ps1 -Action upload  -Config pool-bridge.yaml -Port 192.168.4.51
./esphome_ws.ps1 -Action logs    -Config pool-bridge.yaml -Port 192.168.4.51
```

- [ ] **Step 6: Validate live**

Confirm `sensor.pool_rs485_bridge_pool_pump_speed` populates in HA and matches the speed shown by the panel/pump. Confirm temps still read (Finding B: confirm the on-demand read returns home and temps resume). Confirm `selftest PASS`, 0 checksum errors, presence holding.

---

## Self-review notes

- Spec coverage: navigation path (Task 1, 4), rich logging (Task 4), passive `0x60` check (Task 4, 7A), HA nav buttons (Task 5), presence persistence (Task 5), live survey (CHECKPOINT), capability map (Task 6), pump speed reading (Task 7, 8). All spec sections map to a task.
- The status-page-vs-`0x60` fork and the home-vs-status reading conflict are made explicit (Task 7 branches, Task 8 Step 2), resolved at the checkpoint from real captures.
- Out of scope (speed setting, other control) is not in any task, as intended.
- Type names are consistent: `is_iaq_nav_key`, `iaq_nav`, `current_page()`, `iaq_page_name`, `parse_iaq_button`/`IaqButton`, `pump_rpm`/`has_pump_rpm` used the same way across Python and C++ tasks.
