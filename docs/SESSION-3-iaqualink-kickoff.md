# 🗃️ Session 3 kickoff: read temperatures by emulating the iAqualink controller (0x33)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

## The goal
Read pool, spa, and air temperatures (and ideally setpoints) from the Jandy panel
and publish them to Home Assistant. Session 2 proved the AllButton keypress path
is a dead end on this specific panel; this session implements the path that
actually fits it.

## Why iAqualink, not AllButton keypresses (the Session 2 finding)
- This system has **no LCD/text keypad**. The panel is the bare power center with
  lit buttons; the only text controller it ever had was the dead wireless
  iAquaLink. So the panel emits **no `CMD_MSG` (0x03/0x04) display text** to read,
  no matter how many keys we press. Verified: keypresses transmit perfectly
  (`SENT KEY 0x09 ...`) but produce zero display frames.
- A full bus census (commit 7566d86) showed the panel emits 46 frame types, 45 of
  them empty polls to a wide address sweep. The **only** data frame is `08/02`
  (the equipment LED bitmap to our keypad). **No temperatures are on the bus**
  passively right now.
- Crucially, the panel polls **address 0x33 (the dead iAquaLink's slot) every
  cycle** (`33/00` in the census), waiting for that controller to answer. When a
  device answered in the iAqualink dialect during earlier research, the panel sent
  readable text ("Air Temp 167") as `CMD_IAQ_PAGE_MSG` (0x25). Our `jandy::Reader`
  already decodes 0x25 AIR/POOL/SPA TEMP label-value pairs.

So: emulate the iAqualink device at 0x33, complete its handshake, receive the
0x25 page messages, feed them to the existing Reader, publish temps.

## Starting state (do not re-derive)
- Repo HEAD `7566d86` (public github 4pBdhJoZ3.../esphome-jandy-aqualink). Device
  at 192.168.4.51 runs it: AllButton presence at 0x08 (read-only) + passive bus
  census/decoder + the gated keypress machinery, all inert by default (interlock
  OFF). Presence healthy, 0 checksum errors.
- The gated keypress framework from Session 2 is reusable for iAqualink navigation
  if needed: `build_key_ack` pattern, master interlock switch (off by default),
  five-key allowlist, on-device selftest, HA + web REST controls. See
  `project_pool_controller_phase2.md` for the full Session 2 record.

## Panel mode (operational, confirm before testing)
Test the iAqualink read with the panel in **Auto**, not Service. Auto is the
pool's normal running mode and is where the panel was observed polling the 0x33
iAqualink slot every cycle; Service mode is for local servicing and typically
locks out the RS485 remote controllers (the path we are using). Observed: Service
mode does NOT stop the panel polling our AllButton keypad at 0x08 (presence keeps
climbing), so the device is safe to leave on the bus overnight in either mode.
Founder may keep it in Service overnight and flip to Auto when ready to test.

## The build (incremental, validate each step)
1. Add iAqualink presence: answer polls to 0x33 with the iAqualink ACK (study
   AqualinkD `source/aq_serial.c` iAqualink path + `source/iaqualink_aq_programmer.c`;
   codes in `aq_serial.h`: CMD_IAQ_POLL 0x30, CMD_IAQ_STARTUP 0x29,
   CMD_IAQ_PAGE_START 0x23, CMD_IAQ_PAGE_MSG 0x25, CMD_IAQ_PAGE_END 0x28; ack types
   differ from AllButton's 0x80). Make the keypad address + protocol selectable so
   0x08 AllButton stays available.
2. Log every 0x33-addressed frame (the census already shows new types) to watch
   the panel start the page handshake once we answer. Confirm 0x23/0x25/0x28 pages
   begin to flow.
3. Extend/confirm the Reader for the AllButton-vs-iAqualink page framing, feed all
   pages, and decode air/pool/spa. TDD against the real captures already in
   `tests/fixtures.py` (`DISPLAY_AIR_LABEL`, `DISPLAY_AIR_VALUE`) plus any new
   captured page vectors.
4. Validate decoded values against reality (pool was ~91 F; confirm current
   readings with the founder), then publish pool/spa/air temperature sensors to HA.
5. Setpoints / control remain a later phase, behind the off-by-default interlock.

## Safety
- Emulating 0x33 to establish presence and **receive** pages is read-only. Any
  iAqualink navigation or control keycodes carry the same equipment risk as the
  AllButton keys and must stay behind the interlock, one key at a time, logged.
- Only one device may emulate each address. AqualinkD stays stopped. Heater OFF.

## Tooling (unchanged from Session 2)
- Edit component, commit+push to github, then build via the ESPHome dashboard
  (192.168.1.126:6052, no auth, pulls the component via external_components
  refresh:0s). Drive it with `esphome_ws.ps1 -Action compile|upload|logs -Config
  pool-bridge.yaml -Port 192.168.4.51`. `upload` does not recompile: `compile`
  then `upload`. Avoid `validate` (prints secrets).
- Dashboard yaml is edited via `GET/POST http://192.168.1.126:6052/edit?configuration=pool-bridge.yaml`
  (it returns/accepts raw bytes; back up first). Backups at
  `C:\Users\Falcon\Documents\pool-controller\dashboard-pool-bridge.BACKUP*.yaml`.
- Python tests: `python -m unittest discover -s tests -t .` from `esp32-experiment`.
