# 🗃️ Session 5 kickoff: SET the filter pump speed (RPM) from Home Assistant

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

## The goal
Finish pump control: add the ability to CHANGE the filter pump's speed (RPM) from
Home Assistant. Reading shipped in Session 4 (Pool Pump Speed / Watts sensors +
the Read Pump Speed button). This is the deliberately deferred, highest-risk
piece: it actuates the live pump and uses a multi-step value-set handshake plus
packet types the firmware has never sent.

## Where things stand (do NOT re-derive)
- Repo HEAD `16f9c28` (deployed firmware `8e9d31b`), device 192.168.4.51, public
  github 4pBdhJoZ3.../esphome-jandy-aqualink. Full context in memory
  `project_pool_controller_phase2.md` (Session 4 outcome) and the survey results
  in `docs/PANEL-CAPABILITY-MAP.md`. AqualinkD reference source is cloned at
  `C:\Users\Falcon\Documents\pool-controller\AqualinkD-ref`.
- WORKING: iAqualink 0x33 emulation, temps + pump RPM/watts reading, gated
  equipment toggles + view-only navigation. Master interlock "Pool Keypad Keypress
  Armed" (default OFF) gates ALL key transmission. "iAqualink Presence"
  (RESTORE_DEFAULT_OFF) gates whether we answer 0x33.
- Pump = Pentair Intelliflo VS, RPM mode (devices label "VSP1 Spd"). Safe range
  600-3450 RPM.

## The exact set protocol (from the survey + AqualinkD source)
1. Navigate to the DEVICES page: arm interlock, press Home (0x01), confirm
   current_page == HOME (0x01), press Other Devices (0x18), confirm current_page
   == DEVICES (0x36). (iaq_nav already does the Home-gating for 0x18.)
2. Press the VSP speed-adjust button: on the DEVICES page it is slot 2, keycode
   **0x13**. CRITICAL SAFETY: 0x13 = Pool Heat on the HOME page; it only means
   "VSP1 Spd ADJ" on DEVICES. The firmware MUST confirm current_page == 0x36
   before sending 0x13, or it would fire a heater. After the press, confirm the
   panel opens SET_VSP (page 0x1e).
3. Value-set handshake (NEW machinery):
   - Reply ACK_CMD_READY_CTRL (ack_type 0x80) to an ORDINARY poll to REQUEST the
     control slot. That ack on the wire is `10 02 00 01 00 80 93 10 03`.
   - The panel then addresses 0x33 with CMD_IAQ_CTRL_READY (cmd 0x31).
   - Reply to the 0x31 with the 0x24 value frame (NOT another 0x80).
   - DANGER: do NOT reply 0x80 to the 0x31. The 0x80 is the earlier request sent
     on an ordinary poll; the 0x24 frame is the reply to the 0x31. The two 0x31s
     (the received command vs the literal payload sub-byte below) are unrelated.
4. The 0x24 value frame: dest 0x00, cmd 0x24, sub-byte 0x31 (literal), then ASCII
   RPM digits, then 0xcd padding out to logical index 18, then checksum + 10 03.
   On the wire: `10 02 00 24 31 <digits> <0xcd ...> <ck> 10 03`.
5. Digit encoding (num2iaqtRSset, AqualinkD iaqtouch.c ~line 240): each decimal
   digit -> ASCII (digit + 0x30), most-significant first, no leading zeros; field
   padded to width 6 (trailing NUL, except numbers < 1000 get a literal 0x30 at
   index 4). TDD against these captured frames (iaqtouch.h ~line 161-203):
   - 1600 -> `10 02 00 24 31 31 36 30 30 00 cd cd cd cd cd cd cd cd cd cd cd fd 10 03`
   - 2000 -> `... ck f8`, 3000 -> `... ck f9`, 1000 -> `... ck f7`
   - 600  -> `10 02 00 24 31 36 30 30 00 00 cd ... cc 10 03` (note the < 1000 quirk)
6. Clamp 600-3450, snap to multiple of 5 (AqualinkD RPM_check). The panel itself
   rejects out-of-range with an error popup (cmd 0x2c "Speed must be 600 - 3450 RPM").
7. Confirm the set took by reading STATUS2 back (reuse the Read Pump Speed path).

Reference: `AqualinkD-ref/source/iaqtouch_aq_programmer.c` (set_aqualink_iaqtouch_pump_rpm,
queue_iaqt_control_command), `iaqtouch.c` (num2iaqtRSset), `iaqtouch.h` (captured
VSP frames), `aq_serial.c` (send_jandy_command, the 0x31 path, generate_checksum).

## New firmware machinery needed (more than a keypress)
- A NEW transmit path for the 0x24 command frame (a ~24-byte packet on the bus,
  not the 9-byte ACK). Build frame + checksum (8-bit sum from the leading 0x10,
  & 0xFF, in the second-to-last byte). NO trailing NUL: the default
  send_jandy_command form; the trailing-NUL variant is a known VSP-drop footgun.
- A multi-step state machine in the core-1 task sequencing: nav to DEVICES ->
  confirm page -> press 0x13 -> confirm SET_VSP -> send 0x80 on the next poll ->
  on the 0x31 send the 0x24 frame -> confirm -> return Home. Like the
  read_pump_speed auto-return, but several steps plus the control-command path.
  Drive it from one gated method `set_pump_rpm(rpm)`.
- Gating: master interlock + presence + RPM clamp (refuse out-of-range before
  sending) + page-context confirm (refuse 0x13 unless current_page == DEVICES) +
  one command at a time + log every transmitted byte. Hard abort (interlock off)
  clears the whole sequence.

## Approach: brainstorm first, then TDD, then gated build, then live
1. BRAINSTORM the HA control surface + safe speeds with the founder BEFORE coding:
   - Slider (number entity, any 600-3450) vs preset buttons (a few named speeds)
     vs both. Presets are safer; a slider is more flexible.
   - The speeds the founder actually uses (normal filtering speed, a low
     circulation speed, a high/cleaner speed) -> preset values + a sane default.
   - Whether to also add the deferred pump-speed auto-refresh (keep the reading
     live) while in here.
2. TDD the digit encoder (num2iaqtRSset equivalent) against the captured frames;
   mirror in C++ + on-device selftest. TDD the 0x24 frame builder + checksum too.
3. Build the gated `set_pump_rpm` state machine: one command, page-confirmed, clamped.
4. Deploy, then live-test with the FOUNDER WATCHING the pump: start with a
   conservative in-band speed (e.g. its current filtering speed), confirm the pump
   changes and STATUS2 reads back the new RPM, then confirm. Heaters stay excluded.

## Safety (the highest-risk feature in this project)
Changing pump speed actuates the live pump. Clamp hard, confirm the page before
the 0x13 keycode (it is a heater on the home page), send one command at a time,
log every byte, founder watching. Heaters excluded. Only one device emulates
0x33; AqualinkD stays stopped.

## After speed setting
Easy DEVICES toggles (Spa Light 0x19, Extra Aux 0x1d, Sprinklers 0x1e), then
valve modes (Spa Mode), then heaters LAST per founder. Separately, the read-only
efforts: salt/SWG bus-frame decode and the equipment-status bitmap decode (both
need decoding raw bus frames, not page navigation).

## Tooling (unchanged)
Edit component, commit + push, build via ESPHome dashboard 192.168.1.126:6052
(`esphome_ws.ps1 -Action compile|upload|logs -Config pool-bridge.yaml -Port
192.168.4.51`; compile THEN upload). Dashboard yaml edited via GET/POST
`http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml` (returns/accepts
the file as raw bytes; back up first; the live config has the WiFi password
inline, so never echo its contents). Python tests:
`python -m unittest discover -s tests -t .`. WATCHPOINT: ESPHome object-id REST
URLs (`/button/<id>/press`) are deprecated and removed in 2026.7.0; use
entity-name URLs or HA service calls before upgrading past 2026.7.
