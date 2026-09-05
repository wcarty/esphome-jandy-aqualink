# 🧠 Set the filter pump speed (RPM) from Home Assistant

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

Date: 2026-05-31
Status: Approved direction (brainstorm complete), pre-implementation
Scope of this push: add the ability to CHANGE the filter pump's speed from Home
Assistant, as four preset buttons plus a slider, behind the existing master
interlock. This is the value-set path, the highest-risk pump feature, because it
actuates the live pump and uses a multi-step handshake plus a packet type the
firmware has never transmitted. Reading already shipped in Session 4.

## In plain English (founder summary)

You get four one-tap speed buttons in Home Assistant (Night, Low, Normal, High)
and a slider for anything in between. Nothing can move the pump unless you first
flip the "Pool Keypad Keypress Armed" switch on, exactly like today's controls.
When you press a speed, the firmware walks to the right panel screen, double
checks it is on that screen before sending the speed command (because that command
shares its code with the Pool Heat button on a different screen), sends your RPM,
reads it back so you can see it took, and returns to showing temperatures. We
build and prove all the fiddly parts with tests before anything touches the pump,
then test live with you watching at the pad. The first live change is a tiny 50
RPM nudge so we can confirm the whole path works before doing anything bigger.

## Goal

Finish pump control. Today we can read the pump's speed and watts on demand
(Session 4). This session adds setting the speed:

- Four preset buttons in Home Assistant: Night, Low, Normal, High.
- A slider for any value from 600 to 3450 RPM.
- Both run through one gated set path with a read-back to confirm.

The two low presets get their exact values dialed in live at the pad against the
salt cell's flow cutoff (see the live test procedure).

## Brainstorm decisions (locked with founder 2026-05-31)

1. Control surface: BOTH preset buttons and a slider. The buttons are the safe
   everyday control; the slider covers the occasional specific RPM. Both call the
   same gated set method, so the safety checks are identical.
2. Presets: FOUR speeds.

   | Button | RPM (initial) | Purpose |
   |---|---|---|
   | Night | ~1100 (set at pad) | quiet overnight, below the salt cell's flow cutoff, no chlorine made |
   | Low | ~1600 (set at pad) | efficient daytime, still above the cutoff so the salt cell keeps generating |
   | Normal | 2750 | standard filtering (the speed observed during the survey) |
   | High | 3200 | cleaning, skimming, a boost |

   Rationale researched and recorded: a well-run salt pool does not need 24 hour
   chlorination; it banks chlorine during the daytime run and coasts overnight.
   The salt cell cares about water flow, not RPM directly, and shuts off below a
   flow-switch threshold (real-world minimums commonly 1200 to 1400 RPM, so the
   founder's observed ~1450 is normal). Method: find the lowest RPM that keeps the
   flow switch happy, add a ~150 RPM cushion for a dirtying filter, set Low there;
   set Night below the cutoff for true quiet running.
3. Reading stays ON-DEMAND. No periodic auto-refresh (the panel shows our device
   temps OR pump speed, never both, so a timed refresh would blink temps out). The
   set path still reads the new RPM back once automatically after each change.
4. First live write: a small ~50 RPM nudge off the current speed, to prove the
   path end to end before anything larger.

## What already exists (Session 4, deployed firmware `8e9d31b`, device 192.168.4.51)

The `jandy_aqualink` ESPHome component emulates the iAqualink controller at
address 0x33, with a core-1 pinned task answering polls in ~110 microseconds:

- Inert presence ACK gated by the "iAqualink Presence" switch (RESTORE_DEFAULT_OFF)
  makes the panel push pages; `IaqReader` decodes HOME-page temps.
- View-only navigation: `is_iaq_nav_key` (global keys) plus `iaq_nav(key)`, which
  accepts Other Devices (0x18) only when `current_page() == HOME (0x01)`. Seven
  gated HA nav buttons.
- Gated equipment toggles via `iaq_press` (heaters excluded from the allowlist).
- Pump speed reading: a gated "Read Pump Speed" button navigates to STATUS2,
  decodes the "RPM:" and "Watts:" text lines, publishes `sensor.pool_pump_speed`
  and `pool_pump_watts`, then the core-1 task auto-arms HOME so temps resume.
  Validated live (2750 RPM / 1263 W).
- Master interlock "Pool Keypad Keypress Armed" (ALWAYS_OFF, boots off) gates ALL
  key transmission. Turning it off is the hard abort.

The full panel map is in `docs/PANEL-CAPABILITY-MAP.md`. AqualinkD reference
source is cloned at `C:\Users\Falcon\Documents\pool-controller\AqualinkD-ref`.

## Reference protocol facts (from the survey and AqualinkD source)

The pump is a Pentair Intelliflo VS in RPM mode (devices label "VSP1 Spd"). Safe
range 600 to 3450 RPM.

The set sequence:

1. Navigate to DEVICES: press Home (0x01), confirm `current_page == HOME (0x01)`,
   press Other Devices (0x18), confirm `current_page == DEVICES (0x36)`. The
   existing `iaq_nav` already gates 0x18 on HOME.
2. Press the VSP speed-adjust button: on DEVICES it is slot 2, keycode 0x13.
   CRITICAL: 0x13 = Pool Heat on the HOME page; it only means "VSP1 Spd ADJ" on
   DEVICES. The firmware MUST confirm `current_page == DEVICES (0x36)` before
   sending 0x13, or it would fire a heater. After the press, confirm the panel
   opens SET_VSP (page 0x1e).
3. Value-set handshake (new machinery):
   - Reply ACK_CMD_READY_CTRL (ack_type 0x80) to an ORDINARY poll to request the
     control slot. On the wire: `10 02 00 01 00 80 93 10 03`.
   - The panel then addresses 0x33 with CMD_IAQ_CTRL_READY (cmd 0x31).
   - Reply to the 0x31 with the 0x24 value frame (NOT another 0x80). The 0x80 is
     the earlier request sent on an ordinary poll; the 0x24 is the reply to the
     0x31. Do NOT reply 0x80 to the 0x31.
4. The 0x24 value frame: dest 0x00, cmd 0x24, sub-byte 0x31 (literal), then ASCII
   RPM digits, then 0xcd padding out to logical index 18, then checksum + 10 03.
   On the wire: `10 02 00 24 31 <digits> <0xcd ...> <ck> 10 03`. No trailing NUL
   (the default `send_jandy_command` form; the trailing-NUL variant is a known
   VSP-drop footgun).
5. Digit encoding (`num2iaqtRSset`, `iaqtouch.c` ~line 240): each decimal digit to
   ASCII (digit + 0x30), most significant first, no leading zeros, then padding out
   to logical index 18 with 0xcd. The exact padding between the digits and the 0xcd
   run (including the under-1000 case, which the kickoff notes is special) is
   whatever reproduces the captured frames below; the tests pin it byte for byte
   rather than trusting a prose rule. Captured frames (`iaqtouch.h` ~line 161 to 203):
   - 1600 -> `10 02 00 24 31 31 36 30 30 00 cd cd cd cd cd cd cd cd cd cd cd fd 10 03`
   - 2000 -> `... ck f8`, 3000 -> `... ck f9`, 1000 -> `... ck f7`
   - 600  -> `10 02 00 24 31 36 30 30 00 00 cd ... cc 10 03` (under-1000 quirk)
6. Clamp 600 to 3450, snap to a multiple of 5 (AqualinkD `RPM_check`). The panel
   itself also rejects out-of-range with a cmd 0x2c error popup ("Speed must be
   600 - 3450 RPM").
7. Confirm the set took by reading STATUS2 back (reuse the Read Pump Speed path),
   then return Home so temps resume.

Authority note on exact bytes: the captured frames above and AqualinkD source are
the source of truth, not this spec's prose. Two details to pin during TDD rather
than trust by description: (a) the request ACK is written `10 02 00 01 00 80 93 10
03` in the kickoff but labeled "ack_type 0x80", and those two readings of the
ack_type vs key byte disagree, so the encoder is built to match the captured byte
string and confirmed against `aq_serial.c`; (b) the under-1000 digit padding, per
the 600 capture. Where prose and a capture differ, the capture wins.

Source references: `AqualinkD-ref/source/iaqtouch_aq_programmer.c`
(`set_aqualink_iaqtouch_pump_rpm`, `queue_iaqt_control_command`), `iaqtouch.c`
(`num2iaqtRSset`), `iaqtouch.h` (captured VSP frames), `aq_serial.c`
(`send_jandy_command`, the 0x31 path, `generate_checksum`).

## Safety model (the crux: this actuates the live pump)

1. Master interlock unchanged. "Pool Keypad Keypress Armed" gates every transmitted
   byte, off by default, boots off. Turning it off is the hard abort and clears any
   in-progress set sequence, reverting to inert presence.
2. RPM clamp and snap before anything goes on the wire. `rpm_check(rpm)` clamps to
   600 to 3450 and snaps to a multiple of 5. Out-of-range never transmits.
3. Page-context confirm on 0x13. The set state machine refuses to send 0x13 unless
   `current_page() == DEVICES (0x36)` at that instant. This is the single most
   important guardrail: it is what prevents a speed command from ever firing a
   heater. Tested like the safety gate it is.
4. Handshake order is explicit state, not inferred. 0x80 is sent only on an
   ordinary poll to request control; the 0x24 digits frame is sent only in response
   to a received 0x31. The state machine cannot send 0x80 to a 0x31.
5. One command at a time. A set sequence runs to completion (or aborts) before any
   other key path can arm. No overlapping sequences.
6. Every transmitted byte is logged, as today.
7. Heaters remain excluded and unreachable. No new equipment keycodes are added.
8. Defense in depth: the founder watches the pump at the pad throughout the live
   test.

## Architecture and components

One change set to the existing `jandy_aqualink` component, same core-1 task and
reply model. New pieces:

1. RPM helpers (firmware, pure logic, TDD now):
   - `jandy::rpm_check(rpm)`: clamp 600 to 3450, snap to multiple of 5.
   - `jandy::encode_iaqt_rpm(rpm, out)`: the `num2iaqtRSset` equivalent. Produces
     the ASCII digit bytes with width-6 padding and the under-1000 quirk.
   - `jandy::build_vsp_set_frame(rpm, out)`: assembles the full 0x24 frame
     (`10 02 00 24 31 <digits> <0xcd pad to index 18> <ck> 10 03`), 8-bit checksum
     summed from the leading 0x10 and masked to a byte, no trailing NUL.

2. New transmit path (firmware): a way to send the ~24-byte 0x24 command frame on
   the bus, distinct from the existing 9-byte ACK. It transmits in the same in-slot
   reply window the ACKs already use, from the core-1 task.

3. The set state machine (firmware), driven by one gated method
   `set_pump_rpm(rpm)`. Steps, each confirmed before the next:
   nav Home -> confirm HOME -> Other Devices (0x18) -> confirm DEVICES (0x36) ->
   send 0x13 (only if on DEVICES) -> confirm SET_VSP (0x1e) -> send 0x80 on the
   next ordinary poll -> on the received 0x31 send the 0x24 digits frame ->
   read STATUS2 back -> arm HOME. Models on the existing Read Pump Speed
   auto-return, with the extra steps and the control-command path. Interlock-off at
   any step aborts and clears the sequence.

4. Home Assistant control surface (config), all gated by interlock + presence:
   - Four `button` entities, "Pump Speed: Night / Low / Normal / High", each
     calling `set_pump_rpm` with its preset literal in the button lambda. Storing
     the values in the dashboard yaml (not the firmware) means the two low presets
     can be tuned at the pad with a yaml edit, no recompile.
   - One `number` entity (slider), "Pump Speed Set", min 600, max 3450, step 5,
     calling `set_pump_rpm` on value. Inert unless armed, same as the buttons.
   - The existing "Read Pump Speed" button and the RPM/Watts sensors stay.

No periodic refresh component is added (decision 3). No presence or interlock
default changes (control stays off after a reboot).

## Testing (TDD before any hardware contact)

Same discipline as the shipped code: pure logic is TDD'd in Python and mirrored in
the C++ on-device selftest against the same frame vectors.

- `rpm_check`: clamps below 600 up to 600 and above 3450 down to 3450, snaps to 5,
  passes in-range values. Boundary cases at 600, 3450, and a non-multiple like 1623.
- `encode_iaqt_rpm`: the digit bytes for 1600, 2000, 3000, 1000, and the under-1000
  quirk at 600, matched to the captured frames.
- `build_vsp_set_frame`: the full byte sequence including the exact checksums
  (fd for 1600, f8 for 2000, f9 for 3000, f7 for 1000, cc for 600), and the
  no-trailing-NUL form.
- The page-confirm gate on 0x13: the set sequence refuses to emit 0x13 when the
  current page is anything other than DEVICES (0x36). Tested as a safety gate.

The live handshake and state machine are live-tested (not unit testable); every
pure piece feeding them is tested first.

## The live test procedure (founder at the pad, watching the pump)

1. Confirm baseline: presence on, healthy, HOME reading, current RPM known via the
   Read Pump Speed button.
2. Arm the interlock.
3. First write: a ~50 RPM nudge off the current speed (decision 4). Confirm the
   pump changes by that small amount and the read-back matches. This proves nav,
   page-confirm, the handshake, the 0x24 frame, and the read-back end to end.
4. Find the salt cell's flow cutoff: step the pump down gradually (toward ~1400,
   ~1300, and so on) while the founder watches the salt unit's own flow/generating
   light at the pad (Home Assistant does not read salt status yet). Record where it
   cuts out. Set Low = cutoff + ~150; set Night below the cutoff. Update the two
   button literals in the dashboard yaml.
5. Test each preset (Night, Low, Normal, High) and a couple of slider values,
   confirming the read-back each time.
6. Disarm the interlock. Return to the resting state (presence on, interlock off,
   HOME, temps reading).

Persistence note: a hand-set speed may hold only until the pump's own daily
schedule next changes it (the pump self-schedules; we saw 2750 drop to 1700
unprompted during the survey). We observe and note what this pump does. This is
expected behavior, not a fault.

## Risks and mitigations

- Wrong page when sending 0x13 fires a heater. Mitigated by the page-confirm gate
  (refuse 0x13 unless on DEVICES 0x36), tested as a safety gate, plus the founder
  watching.
- Replying 0x80 to the 0x31 instead of the 0x24 (protocol footgun). Mitigated by
  explicit handshake state: 0x80 only on an ordinary poll, 0x24 only in reply to a
  received 0x31.
- A trailing NUL on the 0x24 frame silently drops the set. Mitigated by the no-NUL
  send form, verified byte for byte against the captures.
- Out-of-range or fat-fingered RPM. Clamped and snapped before send; the panel also
  rejects out-of-range with a 0x2c popup; the read-back confirms what actually took.
- The pump self-schedules, so a read-back taken much later may differ from what we
  set. The immediate post-set read-back is the confirmation of record.
- Salt status is not readable in Home Assistant yet, so the cutoff search relies on
  the physical indicator at the pad. Documented; the founder is present for it.

## Out of scope this session (deferred)

- Periodic auto-refresh of the RPM/Watts reading (founder chose on-demand).
- SWG/salt reading (a separate passive bus-frame decode task, not page navigation).
- Other DEVICES toggles (Spa Light 0x19, Extra Aux 0x1d, Sprinklers 0x1e), valve
  modes (Spa Mode), and heaters. Heaters last, per the founder.
- Any HA dashboard visual redesign beyond exposing the new entities.

## Tooling (unchanged)

Edit the component, commit and push, build via the ESPHome dashboard at
192.168.1.126:6052 (`esphome_ws.ps1 -Action compile|upload|logs -Config
pool-bridge.yaml -Port 192.168.4.51`; compile THEN upload). Dashboard yaml edited
via GET/POST `http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml` (back
up first; the live config has the WiFi password inline, so never echo its
contents). Python tests: `python -m unittest discover -s tests -t .`. WATCHPOINT:
ESPHome object-id REST URLs (`/button/<id>/press`) are deprecated and removed in
2026.7.0; use entity-name URLs or HA service calls before upgrading past 2026.7.
