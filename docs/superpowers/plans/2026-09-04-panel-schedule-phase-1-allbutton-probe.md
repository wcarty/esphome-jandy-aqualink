# Panel Schedule Access, Phase 1: AllButton Programming Probe

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Determine conclusively whether this panel will drive an emulated AllButton keypad's display into programming mode, because that single answer decides whether the panel's stored programs can be read with AqualinkD's proven menu walk or whether a real iAquaLink must be bought and sniffed.

**Architecture:** Add address-aware display decoding and a bus census to the Python reference library, mirror both into the C++ component, then add one gated firmware button that replays AqualinkD's documented programming-mode entry sequence on the AllButton address. Run it live, capture, and record a verdict. No schedule reading or writing is attempted in this phase.

**Tech Stack:** Python 3 with unittest for the reference implementation, C++ mirrored in `components/jandy_aqualink/jandy_proto.*` with an on-boot selftest, ESPHome YAML for the device config, Home Assistant for orchestration.

**Spec:** This document. Background and the decision it turns on are in the Background section below. Supporting evidence lives in `docs/MENU-WIPE-FEASIBILITY.md` and `docs/PANEL-CAPABILITY-MAP.md`, and the reference implementation being ported is `AqualinkD-ref/source/allbutton_aq_programmer.c`.

## Global Constraints

- The repo is PUBLIC. No secrets in any tracked file. All credentials stay `!secret`.
- The master keypress interlock (`switch.pool_rs485_bridge_pool_keypad_keypress_armed`) and the per-key `scheduler_armed` allowlist must not be weakened, bypassed, or widened. The probe added here is a NEW gated control, not a relaxation of an existing one.
- The C++ in `components/jandy_aqualink/jandy_proto.*` mirrors the Python in `jandy/` and self-tests the same vectors on boot. Any decoder added to one gets added to the other, with the same test vector.
- On-device selftest must report PASS and `checksum_errors` must stay 0 after any flash. Do not actuate equipment on a FAIL.
- Bus timing is hard real time. The keypad reply must land inside the panel's 20 to 40 ms poll window. The current implementation replies in about 110 us. Nothing in this plan may add work to the reply path.
- Home Assistant is the live pool scheduler and is running the pool. `automation.pool_watch_and_correct` fires every 2 minutes. Do not leave the bridge offline or the scheduler disarmed at the end of a session.
- Prose in docs and commit messages: no em dashes, no en dashes, no curly quotes. Ordinary hyphens in compound words are fine.

---

## Background, and why this phase exists

The founder wants two things: delete the panel's old stored programs, and understand the panel well enough to go past AqualinkD on cheap hardware.

Three facts, all verified on 2026-09-04:

1. **AqualinkD reads panel programs but cannot write them.** `AQ_GET_PROGRAMS` in `AqualinkD-ref/source/aq_programmer.h:61` is implemented by `get_allbutton_programs()` in `AqualinkD-ref/source/allbutton_aq_programmer.c:1251`. It walks `select_menu_item("REVIEW")` then `select_sub_menu_item("PROGRAMS")` then loops device keys. There is no matching set or write. `read_schedules()` and `write_schedules()` are declared and commented out in `AqualinkD-ref/source/aq_scheduler.h:63-64`. The project wiki tells users to clear panel schedules by hand instead.

2. **That entire read path is driven by matching LCD text.** `select_menu_item` and `waitForMessage` match literal display strings such as `"SELECT DEVICE TO REVIEW or PRESS ENTER TO END"`. Display text arrives as `CMD_MSG` and `CMD_MSG_LONG` and is handled at `AqualinkD-ref/source/allbutton.c:793`. No display text means no read path.

3. **On this panel, display text arrives at the iAquaLink address, not the AllButton address.** Every display fixture in `tests/fixtures.py` is addressed to `0x33`, for example `DISPLAY_AIR_LABEL = 10 02 33 25 ...`, and `fixtures.display_frame()` defaults `dest=0x33`. The docstring at the top of `jandy/display.py` calls 0x25 "the AllButton keypad" display, which is misleading and gets corrected in Task 1.

The open question is narrow and cheap to settle. Earlier sessions concluded that keypresses produced no display response on AllButton, but the box has only ever held **inert** presence at 0x08. AqualinkD does not just press keys. It performs a specific entry sequence to put the panel into programming mode, and its source carries a warning that two particular keys kill that mode. That exact sequence has never been replayed here.

If the panel answers it, reading programs becomes a port of working C rather than a reverse engineering project, and no hardware purchase is needed. If it does not, the iAquaLink capture route is justified and we stop guessing.

**Out of scope for this phase:** reading program contents, writing programs, deleting programs, and anything involving a physical iAquaLink. Those get their own plan once this one produces its verdict.

---

### Task 1: Address-aware display decoding and bus census

**Files:**
- Modify: `esp32-experiment/jandy/display.py:1-9` (docstring), and add `dest` to `DisplayLine`
- Create: `esp32-experiment/jandy/census.py`
- Test: `esp32-experiment/tests/test_census.py`
- Modify: `esp32-experiment/tests/test_display.py` (add dest assertions)

**Interfaces:**
- Consumes: `jandy.frames.Frame`, `jandy.frames.FrameExtractor`, `tests.fixtures.display_frame`
- Produces: `DisplayLine.dest` (int), and `census.BusCensus` with `feed(frame) -> None`, `counts -> dict[tuple[int,int], int]`, `dests_for_cmd(cmd) -> set[int]`, `saw_display_at(dest) -> bool`

- [ ] **Step 1: Write the failing tests**

Create `esp32-experiment/tests/test_census.py`:

```python
"""Bus census: count frames by (dest, cmd) so we can prove which addresses the
panel actually talks to, and specifically whether display text ever reaches an
AllButton keypad address (0x08-0x0B) rather than only iAqualink (0x33)."""

import unittest

from jandy.frames import FrameExtractor
from jandy.census import BusCensus
from jandy.display import decode_display, CMD_DISPLAY
from tests import fixtures as fx


def frame(raw):
    return FrameExtractor().feed(raw)[0]


class TestBusCensus(unittest.TestCase):
    def test_counts_by_dest_and_cmd(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        c.feed(frame(fx.DISPLAY_AIR_VALUE))
        c.feed(frame(fx.POLL_PUMP))
        self.assertEqual(c.counts[(0x33, CMD_DISPLAY)], 2)
        self.assertEqual(c.counts[(0x60, 0x00)], 1)

    def test_dests_for_cmd(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(c.dests_for_cmd(CMD_DISPLAY), {0x33})

    def test_saw_display_at_is_false_for_allbutton_when_only_iaqualink_talks(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        self.assertTrue(c.saw_display_at(0x33))
        self.assertFalse(c.saw_display_at(0x08))

    def test_saw_display_at_true_when_allbutton_gets_display(self):
        c = BusCensus()
        c.feed(frame(fx.display_frame(0x05, "REVIEW", dest=0x08)))
        self.assertTrue(c.saw_display_at(0x08))


class TestDisplayCarriesDest(unittest.TestCase):
    def test_decode_display_reports_dest(self):
        line = decode_display(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(line.dest, 0x33)

    def test_decode_display_reports_allbutton_dest(self):
        line = decode_display(frame(fx.display_frame(0x05, "REVIEW", dest=0x08)))
        self.assertEqual(line.dest, 0x08)
```

- [ ] **Step 2: Run the tests to verify they fail**

Run from `esp32-experiment/`:

```bash
python -m pytest tests/test_census.py -v
```

Expected: FAIL with `ModuleNotFoundError: No module named 'jandy.census'` and, for the two display tests, `AttributeError: 'DisplayLine' object has no attribute 'dest'`.

- [ ] **Step 3: Add `dest` to DisplayLine**

In `esp32-experiment/jandy/display.py`, replace the `DisplayLine` class and the tail of `decode_display`:

```python
class DisplayLine:
    __slots__ = ("line", "text", "dest")

    def __init__(self, line: int, text: str, dest: int = 0x33):
        self.line = line
        self.text = text
        self.dest = dest

    def __repr__(self):
        return (
            f"DisplayLine(dest=0x{self.dest:02X}, "
            f"line=0x{self.line:02X}, text={self.text!r})"
        )
```

and in `decode_display`, change the return line to:

```python
    return DisplayLine(frame.data[0], text, frame.dest)
```

- [ ] **Step 4: Correct the misleading module docstring**

Replace lines 1 to 9 of `esp32-experiment/jandy/display.py` with:

```python
"""Display-text layer for cmd 0x25.

On this panel every captured display frame is addressed to the iAqualink slot
0x33, not to an AllButton keypad at 0x08-0x0B. That distinction decides whether
AqualinkD's menu-walk program reader can be ported here at all, so DisplayLine
carries the destination address and callers must not assume it.

The panel writes a line at a time: a label ("Air Temp") then a value ("167").
We decode each line to clean ASCII and pair a label with the value line that
immediately follows it. The bus interleaves device polls between those two
writes, so the pairer must ignore non-display frames without losing the pending
label.
"""
```

- [ ] **Step 5: Write the census module**

Create `esp32-experiment/jandy/census.py`:

```python
"""Bus census: which addresses does the panel actually talk to, and with what.

Counting (dest, cmd) pairs is how we answer "does display text ever reach an
AllButton keypad address" without eyeballing a log. Cheap enough to run on
every frame.
"""

from .frames import Frame
from .display import CMD_DISPLAY

ALLBUTTON_ADDRESSES = (0x08, 0x09, 0x0A, 0x0B)


class BusCensus:
    """Counts frames by (dest, cmd)."""

    def __init__(self):
        self.counts = {}

    def feed(self, frame: Frame) -> None:
        key = (frame.dest, frame.cmd)
        self.counts[key] = self.counts.get(key, 0) + 1

    def dests_for_cmd(self, cmd: int) -> set:
        return {dest for (dest, c) in self.counts if c == cmd}

    def saw_display_at(self, dest: int) -> bool:
        return self.counts.get((dest, CMD_DISPLAY), 0) > 0

    def saw_display_at_allbutton(self) -> bool:
        return any(self.saw_display_at(a) for a in ALLBUTTON_ADDRESSES)

    def summary(self) -> str:
        lines = ["dest cmd  count"]
        for (dest, cmd), n in sorted(self.counts.items()):
            lines.append(f"0x{dest:02X} 0x{cmd:02X} {n:6d}")
        return "\n".join(lines)
```

- [ ] **Step 6: Run the tests to verify they pass**

```bash
python -m pytest tests/test_census.py tests/test_display.py -v
```

Expected: PASS, all tests.

- [ ] **Step 7: Run the whole suite for no regressions**

```bash
python -m pytest tests/ -q
```

Expected: PASS, no failures. The suite was **111 passed** on 2026-09-04 before this task, so expect 111 plus the 6 new tests from Step 1, which is 117.

- [ ] **Step 8: Commit**

```bash
git add jandy/census.py jandy/display.py tests/test_census.py tests/test_display.py
git commit -m "feat(census): count frames by dest+cmd and carry dest on DisplayLine

The AllButton program reader in AqualinkD is driven entirely by LCD text. On
this panel every captured display frame is addressed to 0x33, not to an
AllButton keypad. Make that measurable rather than assumed.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 2: Mirror the census into C++ with a selftest vector

**Files:**
- Modify: `esp32-experiment/components/jandy_aqualink/jandy_proto.h`
- Modify: `esp32-experiment/components/jandy_aqualink/jandy_proto.cpp` (selftest at line 345)

**Interfaces:**
- Consumes: the existing `Frame` representation and `selftest(std::string &detail)` in `jandy_proto.cpp:345`
- Produces: `struct BusCensus` with `void feed(uint8_t dest, uint8_t cmd)`, `uint32_t count(uint8_t dest, uint8_t cmd) const`, `bool saw_display_at(uint8_t dest) const`

- [ ] **Step 1: Add the failing selftest assertion**

In `jandy_proto.cpp`, inside `selftest()`, add before the final return:

```cpp
  {
    BusCensus census;
    census.feed(0x33, CMD_DISPLAY);
    census.feed(0x33, CMD_DISPLAY);
    census.feed(0x60, 0x00);
    if (census.count(0x33, CMD_DISPLAY) != 2) {
      detail = "census: 0x33/0x25 count != 2";
      return false;
    }
    if (!census.saw_display_at(0x33)) {
      detail = "census: expected display at 0x33";
      return false;
    }
    if (census.saw_display_at(0x08)) {
      detail = "census: unexpected display at 0x08";
      return false;
    }
  }
```

- [ ] **Step 2: Verify it fails to compile**

```bash
pwsh -File esphome_ws.ps1 compile
```

Expected: compile error, `'BusCensus' was not declared in this scope`. Do not upload.

- [ ] **Step 3: Declare BusCensus in the header**

In `jandy_proto.h`, add near the other protocol types, inside `namespace jandy` (opens at line 15, closes at line 281).

**Do not redefine `CMD_DISPLAY`.** It already exists at `jandy_proto.h:18` as part of
`static constexpr uint8_t CMD_POLL = 0x00, CMD_ACK = 0x01, CMD_STATUS = 0x02, CMD_DISPLAY = 0x25;`
Adding it again is a redefinition error.

```cpp
struct BusCensus {
  static const uint8_t MAX_ENTRIES = 64;
  struct Entry {
    uint8_t dest;
    uint8_t cmd;
    uint32_t count;
  };
  Entry entries[MAX_ENTRIES];
  uint8_t used = 0;

  void feed(uint8_t dest, uint8_t cmd);
  uint32_t count(uint8_t dest, uint8_t cmd) const;
  bool saw_display_at(uint8_t dest) const;
};
```

- [ ] **Step 4: Implement it**

In `jandy_proto.cpp`:

```cpp
void BusCensus::feed(uint8_t dest, uint8_t cmd) {
  for (uint8_t i = 0; i < used; i++) {
    if (entries[i].dest == dest && entries[i].cmd == cmd) {
      entries[i].count++;
      return;
    }
  }
  if (used < MAX_ENTRIES) {
    entries[used].dest = dest;
    entries[used].cmd = cmd;
    entries[used].count = 1;
    used++;
  }
}

uint32_t BusCensus::count(uint8_t dest, uint8_t cmd) const {
  for (uint8_t i = 0; i < used; i++) {
    if (entries[i].dest == dest && entries[i].cmd == cmd) {
      return entries[i].count;
    }
  }
  return 0;
}

bool BusCensus::saw_display_at(uint8_t dest) const {
  return count(dest, CMD_DISPLAY) > 0;
}
```

- [ ] **Step 5: Compile and confirm the selftest count went up**

```bash
pwsh -File esphome_ws.ps1 compile
```

Expected: clean compile. Do not upload yet, Task 3 adds the probe and they flash together.

- [ ] **Step 6: Commit**

```bash
git add components/jandy_aqualink/jandy_proto.h components/jandy_aqualink/jandy_proto.cpp
git commit -m "feat(census): mirror BusCensus into C++ with selftest vectors

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 3: Wire the census into the live frame path behind a switch

**Files:**
- Modify: `esp32-experiment/components/jandy_aqualink/jandy_aqualink.h`
- Modify: `esp32-experiment/components/jandy_aqualink/jandy_aqualink.cpp` (`observe_frame`, called at `:100` and `:289`)
- Modify: `esp32-experiment/firmware/pool-bridge.yaml`

**Interfaces:**
- Consumes: `BusCensus` from Task 2, and the existing `observe_frame(const Frame &f)` hook
- Produces: `void set_census(bool on)` on `JandyAqualink`, and an ESPHome switch named "Pool Bus Census"

**Do not build a key sequencer.** The keypress path already exists and is already gated: `arm_key(uint8_t)` at `jandy_aqualink.cpp:318` refuses when `interlock_` is false, and `firmware/pool-bridge.yaml:181-190` already exposes "Pool Keypad Press MENU" (`arm_key(0x09)`) and "Pool Keypad Press RIGHT" (`arm_key(0x18)`). Task 4 drives those by hand. The only thing missing is a way to measure what comes back, which is all this task adds.

- [ ] **Step 1: Declare the census members**

In `jandy_aqualink.h`, add to the class alongside the other state:

```cpp
  // Counts (dest, cmd) while enabled, so we can prove whether display text
  // (cmd 0x25) ever reaches an AllButton keypad address (0x08) rather than
  // only the iAqualink slot (0x33). Read-only: never sends anything.
  void set_census(bool on);
  bool census_on_{false};
  jandy::BusCensus census_;
```

Note the `jandy::` qualification. `observe_frame` is declared at `jandy_aqualink.h:132` as taking a `const jandy::Frame &`, and `BusCensus` lives in the same namespace.

- [ ] **Step 2: Implement the setter**

In `jandy_aqualink.cpp`, next to `set_sniff_all`:

```cpp
void JandyAqualink::set_census(bool on) {
  if (on) {
    this->census_ = BusCensus();
    this->census_on_ = true;
    ESP_LOGI(TAG, "bus census STARTED (counters reset)");
    return;
  }
  this->census_on_ = false;
  ESP_LOGI(TAG, "bus census STOPPED");
  ESP_LOGI(TAG, "census: display at 0x08 = %s",
           this->census_.saw_display_at(0x08) ? "YES" : "no");
  ESP_LOGI(TAG, "census: display at 0x33 = %s",
           this->census_.saw_display_at(0x33) ? "YES" : "no");
  for (uint8_t i = 0; i < this->census_.used; i++) {
    ESP_LOGI(TAG, "census: dest 0x%02X cmd 0x%02X count %u",
             this->census_.entries[i].dest, this->census_.entries[i].cmd,
             (unsigned) this->census_.entries[i].count);
  }
}
```

- [ ] **Step 3: Feed the census from observe_frame**

`observe_frame` is the single frame-observation hook and is already called after the reply on both paths (`jandy_aqualink.cpp:100` with the comment "after the reply, never delays it", and `:289`). Add as the FIRST statement in the body of `observe_frame`:

```cpp
  if (this->census_on_) {
    this->census_.feed(f.dest(), f.cmd());
  }
```

This is two comparisons and an increment on an array of at most 64 entries. It runs after the reply is already on the wire, so it cannot affect the 20 to 40 ms poll window.

- [ ] **Step 4: Expose it as a switch**

In `firmware/pool-bridge.yaml`, in the `switch:` block next to "Pool Bus Sniff" at line 154, add. Note the component id is `jandy_comp`:

```yaml
  - platform: template
    name: "Pool Bus Census"
    icon: mdi:counter
    optimistic: true
    restore_mode: ALWAYS_OFF
    turn_on_action:
      - lambda: "id(jandy_comp).set_census(true);"
    turn_off_action:
      - lambda: "id(jandy_comp).set_census(false);"
```

- [ ] **Step 5: Compile, verify selftest, then upload**

```bash
pwsh -File esphome_ws.ps1 compile
pwsh -File esphome_ws.ps1 upload
```

Watch the boot log. Expected: `selftest PASS` with a count one higher than the Task 2 baseline, and `checksum_errors 0`. **If the selftest fails, do not proceed to Task 4.**

- [ ] **Step 6: Commit**

```bash
git add components/jandy_aqualink/ firmware/pool-bridge.yaml
git commit -m "feat(census): count bus traffic by dest and cmd behind a switch

Read-only. Runs inside observe_frame, which is already called after the reply
is on the wire, so it cannot affect the poll window. Exists to answer one
question: does display text ever reach 0x08, or only 0x33.

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

### Task 4: Live run and verdict

**Files:**
- Create: `esp32-experiment/docs/ALLBUTTON-PROGRAM-PROBE-RESULT.md`
- Modify: `esp32-experiment/docs/ROADMAP.md`

**Interfaces:**
- Consumes: the "Pool Bus Census" switch from Task 3, and the existing "Pool Keypad Press MENU" and "Pool Keypad Press RIGHT" buttons
- Produces: a written verdict that selects the Phase 2 route

- [ ] **Step 1: Pause the pool brain so it is not pressing keys underneath you**

Start the manual hold so `automation.pool_watch_and_correct` skips its runs:

```
timer.start on timer.pool_manual_hold, duration 00:30:00
```

Confirm `timer.pool_manual_hold` is no longer `idle` before continuing. The watchdog presses buttons every 2 minutes and would otherwise contaminate the capture.

- [ ] **Step 2: Establish the idle baseline**

With the keypress interlock still OFF, turn ON "Pool Bus Census", wait 2 minutes, turn it OFF, and read the log summary.

Expected: display frames at 0x33 and none at 0x08. This is the control. If 0x08 already shows cmd 0x25 at idle, stop and record that, because the premise of this whole phase is wrong in your favour and Phase 2 is Route A.

- [ ] **Step 3: Run the menu walk**

Turn ON "Pool Bus Census". Arm `switch.pool_rs485_bridge_pool_keypad_keypress_armed`. Then press, waiting about 5 seconds between each:

1. "Pool Keypad Press MENU" (`arm_key(0x09)`)
2. "Pool Keypad Press RIGHT" (`arm_key(0x18)`)
3. "Pool Keypad Press RIGHT" again
4. "Pool Keypad Press MENU" again

Turn the interlock OFF, then turn "Pool Bus Census" OFF and read the summary.

**Never press AUX6 or AUX7.** `AqualinkD-ref/source/allbutton_aq_programmer.c:1255` records that those two kill programming mode:

```c
char keys[10] = {KEY_PUMP, KEY_SPA, KEY_AUX1, KEY_AUX2, KEY_AUX3, KEY_AUX4, KEY_AUX5}; // KEY_AUX6 & KEY_AUX7 kill programming mode
```

- [ ] **Step 4: Read the verdict off the census**

The log line to look for is `census: display at 0x08 = YES` or `= no`.

- **YES** means the panel renders menus to an emulated AllButton keypad. Phase 2 is a port of `get_allbutton_programs()` and no hardware purchase is needed.
- **no**, with 0x33 showing YES in the same run, means the panel will not drive an AllButton display, AqualinkD's read path cannot be ported here, and Phase 2 is the real iAquaLink capture route.
- **no** at both addresses means the capture itself failed. Check that the bridge was online and that `checksum_errors` is still 0, then re-run before drawing any conclusion.

- [ ] **Step 5: Disarm and restore**

Confirm `switch.pool_rs485_bridge_pool_keypad_keypress_armed` is OFF and "Pool Bus Census" is OFF. Cancel `timer.pool_manual_hold` so the pool brain resumes. Confirm the timer reads `idle` and that the next `pool_watch_and_correct` run completes and leaves pump watts within a few percent of `sensor.pool_expected_pump_watts`.

- [ ] **Step 6: Write the verdict document**

Create `docs/ALLBUTTON-PROGRAM-PROBE-RESULT.md` recording: the date, the exact key sequence sent, the idle baseline summary from Step 2, the full census summary from Step 3, the `display at 0x08` result, and the Phase 2 route it selects. Record a negative result just as fully as a positive one. A documented dead end is the point of this phase.

- [ ] **Step 7: Update the roadmap and commit**

```bash
git add docs/ALLBUTTON-PROGRAM-PROBE-RESULT.md docs/ROADMAP.md
git commit -m "docs: AllButton programming probe result and Phase 2 route

Co-Authored-By: Claude Opus 5 <noreply@anthropic.com>"
```

---

## Phase 2 is deliberately not planned yet

Phase 2 is either a port of a known-good C function or a hardware-dependent capture and decode project, and Task 4 decides which. Writing both now would mean writing one plan that gets thrown away, and writing the capture plan in detail before knowing whether the frames are even reachable would fill it with the guesses this plan format forbids.

When Task 4 lands, the next plan is one of:

- **Route A, panel answers on AllButton.** Port `select_menu_item`, `select_sub_menu_item` and `waitForMessage` from `AqualinkD-ref/source/allbutton_aq_programmer.c` into `jandy/`, TDD against synthetic display sequences, then implement the REVIEW to PROGRAMS walk. Reading is then solved and the novel work is the write path, which AqualinkD never built.
- **Route B, panel stays silent on AllButton.** Buy a working iAquaLink, put it on the bus alongside the bridge at 0x08, and capture the 0x33 paged-menu frames (0x23 page start, 0x24 button, 0x25 content, 0x28 page end, 0x40 continuation) while performing known create and delete operations in the app. Diff the labeled captures, then decode.

Route B has one property worth stating in advance: the iAquaLink only has to work long enough to teach the bridge. It is a capture fixture, not a dependency.
