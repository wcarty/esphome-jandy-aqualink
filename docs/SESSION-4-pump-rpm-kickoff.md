# 🗃️ Session 4 kickoff: add filter pump RPM (speed) control via iAqualink

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

## The goal
Add control of the filter pump's speed (RPM) from Home Assistant, through the
iAqualink controller we emulate at 0x33. This is the founder's next priority
after Session 3 shipped temperature reading and equipment toggles.

## Where Session 3 left off (do not re-derive)
- Repo HEAD `a6a7b59` (public github 4pBdhJoZ3.../esphome-jandy-aqualink). Device
  192.168.4.51. Full detail in memory `project_pool_controller_phase2.md`
  (Session 3 outcome section). AqualinkD source cloned at
  `C:\Users\Falcon\Documents\pool-controller\AqualinkD-ref` for reference.
- WORKING: emulate iAqualink at 0x33 (inert ACK `10 02 00 01 00 00 13 10 03` to
  every 0x33 frame, gated by the "iAqualink Presence" switch) -> panel pushes the
  HOME page -> IaqReader decodes pool/spa/air temps to HA sensors. Gated control
  via `iaq_press(key)` sends one allowlisted home-button keycode in the ACK
  (`build_ack(0x00, key)`) only on the 0x30 poll. Home buttons: 0 Filter Pump
  0x11, 1 Spa 0x12, 2 Pool Heat 0x13, 3 Spa Heat 0x14, 4 Cleaner 0x15, 5 Air
  Blower 0x16, 6 Pool Light 0x17. Allowlist {0x11,0x12,0x15,0x16,0x17}; heaters
  excluded. Master interlock switch "Pool Keypad Keypress Armed" (default off).
- All toggles proven live (founder "worked perfectly"). Pump is Pentair
  IntelliFlo3; panel polls it at 0x60.

## Why RPM is different (and harder)
RPM is a "set a value" command, not a button toggle. On iAqualink it means:
navigate to the pump's VSP (variable-speed) adjust page, then send the RPM value
via the control-command path. From AqualinkD `aq_serial.c`: when the panel sends
`CMD_IAQ_CTRL_READY` (0x31) we reply `ACK_CMD_READY_CTRL` (0x80) then send a
`send_jandy_command` 0x24 packet carrying the value digits. See `iaqtouch.h`
worked examples (e.g. `10 02 00 24 31 36 30 30 00 ...` = 1600) and the VSP-set
logic in `iaqtouch_aq_programmer.c` (`num2iaqtRSset` encodes the digits).

## Suggested approach (incremental, validate each step, founder watching)
1. Capture the pump pages: with iAqualink presence on, navigate to the pump/VSP
   page (likely via a home button or the devices page) and log the frames to see
   the current RPM display and the page type. Add temporary logging of 0x31 / 0x24
   frames.
2. Work out the set sequence: respond to 0x31 with the 0x80 ack and send the
   value-encoded 0x24 command. TDD the value encoder (mirror `num2iaqtRSset`)
   against the `iaqtouch.h` examples.
3. Gate it hard: interlock + iAqualink presence + sane RPM bounds (e.g. clamp to
   the pump's min/max, refuse out-of-range). One command at a time, log every
   byte, founder watching the pump.
4. Expose an HA `number` (or a few preset buttons) for the RPM, validate the pump
   actually changes speed and the panel's RPM display matches, then confirm.

## Safety
Changing pump speed actuates the pump. Gate behind the interlock, clamp to safe
RPM bounds, send one command at a time, log everything, test with the founder
watching. Heater stays excluded. Only one device emulates 0x33; AqualinkD stays
stopped.

## After RPM (founder wants all of it; heater LAST)
Spa Light + Aux 5/6/7 (iAqualink "devices" page 0x36, needs page navigation),
then Spa Drain + Spa Fill (valve modes; investigate reachability), then Pool Heat
+ Spa Heat (add to allowlist) LAST.

## Tooling (unchanged)
Edit component, commit+push, build via ESPHome dashboard 192.168.1.126:6052
(`esphome_ws.ps1 -Action compile|upload|logs -Config pool-bridge.yaml -Port
192.168.4.51`; `compile` then `upload`). Dashboard yaml edited via GET/POST
`http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml` (raw bytes; back
up first). Drive controls via device web REST `http://192.168.4.51/...` or HA.
Python tests: `python -m unittest discover -s tests -t .` from esp32-experiment.

## Resting state note
The "iAqualink Presence" switch (temperature reading) is ON now but resets OFF on
reboot (restore_mode ALWAYS_OFF). A quick early win next session: decide whether
to make it persist so temps keep reading across reboots.
