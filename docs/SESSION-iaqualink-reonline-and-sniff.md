# 🗃️ Session kickoff: re-online the iAquaLink, sniff the schedule, then wipe

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. Self-contained. We do this one TOGETHER
(founder at the pad + on the app; Claude drives the box, captures, and the cloud).

## Paste this to start

> Let's do the pool wipe session. I'm going to bring the old iAquaLink 2.0 back
> online over Ethernet so we can both (a) delete the schedules in the app and (b)
> reverse-engineer how the schedule is read/deleted, via the bus sniff and the
> cloud API. Read docs/SESSION-iaqualink-reonline-and-sniff.md and memory
> project_pool_controller_phase2.md first. Back up the schedules before deleting.

## The goal

The iAquaLink Touch MENU is genuinely empty over the bus (confirmed, see
`docs/MENU-WIPE-FEASIBILITY.md`), so the box-only menu route is dead. BUT the app
clearly shows the schedules, so the data is reachable. This session brings the
real iAquaLink 2.0 back online and uses it two ways at once:
1. RELIABLE WIPE: delete all schedules/automations in the Jandy app (after backup).
2. REVERSE-ENGINEER (bonus, could revive a self-contained solution): capture how
   the real iAquaLink reads/writes the schedule, on the bus AND via the cloud API.

## Where we are at session start

- Repo `esphome-jandy-aqualink`, device `192.168.4.51`, ESPHome dashboard
  `192.168.1.126:6052`, helper `C:\Users\Falcon\Documents\pool-controller\esphome_ws.ps1`.
- The ESP box runs the latest build with: the read-only equipment sniffer (4 HA
  binary_sensors), the phone-alert automation, and a NEW gated "Pool Bus Sniff"
  promiscuous capture switch. Resting state: 0x08 keypad sniffer ON (always),
  `iAqualink Presence` (0x33) OFF, interlock OFF, `Pool Bus Sniff` OFF.
- Panel was left in AUTO. Phone alerts armed (`automation.pool_blower_spa_alert`).
- Cloud recon tooling ready at `C:\Users\Falcon\Documents\pool-controller\cloud-recon\`
  (read-only `iaqualink_probe.py` + README; needs `credentials.json`).

## CRITICAL safety constraint

Only ONE device may hold the 0x33 iAquaLink seat at a time. While the real
iAquaLink 2.0 is online, our box MUST keep `iAqualink Presence` OFF (it does by
default). Our box stays PASSIVE: it listens (0x08 keypad + bus sniff) and never
transmits on 0x33. Do NOT turn on `iAqualink Presence` during this session.

## Plan

### Step 0 - Back up the schedules FIRST (founder)
Before deleting anything, record every schedule/automation in the iAquaLink app:
screenshots and/or written list (name, days, start, end, target, what it does).
This is the blueprint for rebuilding the wanted ones in Home Assistant later, and
the record of the stray spa-mode program we want gone.

### Step 1 - Re-online the iAquaLink 2.0 (founder)
Its WiFi is dead but it has an Ethernet jack. Bring it online via the UniFi travel
router (or any wired connection at the pad). Confirm in the app that the system is
online and the schedules are visible.

### Step 2 - Bus sniff (Claude + founder)
- Confirm `iAqualink Presence` is OFF (must be, with the real device on 0x33).
- Turn ON `switch.pool_rs485_bridge_pool_bus_sniff`.
- Start a background log capture (esphome_ws.ps1 logs -> file; shared-read it).
- Founder: in the app, OPEN the schedule screen, scroll through each program, then
  (after Step 4 backup is done) DELETE one program. Each action makes the iAquaLink
  read/write the schedule over the bus; the capture records it as `SNIFF d=.. c=..`
  raw frames.
- Turn the sniff switch OFF (it is verbose).
- Analyze the `SNIFF` lines: find the panel<->0x33 frames that carry schedule data
  (look for new command bytes beyond the known page frames; before/after a delete
  is the Rosetta stone).

### Step 3 - Cloud API (Claude)
- Fill `cloud-recon/credentials.json` (founder's iAquaLink email + password; never
  commit/share it) and run `python iaqualink_probe.py`. Inspect `out/` for the
  system + any SCHEDULE-LIKE responses.
- DECISIVE method: log into the iAquaLink WEBSITE (iaqualink.zodiacpoolsystems.com)
  in a browser and watch the network requests while opening the schedule screen and
  deleting a program. That captures the exact read + delete API calls in plain
  JSON. Extend `iaqualink_probe.py` to replay the read (and, deliberately, the
  delete) once the call is known.

### Step 4 - Wipe (founder, the reliable path)
After backup: delete every schedule/automation in the app, especially the stray
spa-mode program (Day Party is a prime suspect). This is the dependable wipe
regardless of whether the reverse-engineering pans out.

### Step 5 - Verify (Claude + founder)
- Box-silent check: leave our box passive; confirm via the sniffer's binary_sensors
  + HA history that the panel no longer self-runs spa/blower/cleaner.
- Definitive spa-cause test: with schedules gone, confirm the spa/blower no longer
  fire on their own.

### Step 6 - Disconnect + decide
- Disconnect the iAquaLink 2.0 for good (kills any cloud-side automation).
- If Step 2 or 3 cracked a self-contained read/delete, capture it for a future
  box-only tool. Otherwise the app wipe stands and we move to the HA-scheduler
  project (with a mandatory filtration failsafe, since the panel is no longer a
  hardware fallback once wiped).

## Reference

- Equipment bit map (0x08 CMD_STATUS frame, data = raw[4:-3]): air_blower = byte 0
  bit 6; cleaner = byte 1 bit 0; spa_mode = byte 1 bit 2; filter_pump = byte 1 bit 4.
- HA entities: `switch.pool_rs485_bridge_iaqualink_presence` (KEEP OFF),
  `switch.pool_rs485_bridge_pool_keypad_keypress_armed`,
  `switch.pool_rs485_bridge_pool_bus_sniff`,
  `binary_sensor.pool_rs485_bridge_pool_{spa_mode,air_blower_status,filter_pump_status,cleaner_status}`,
  `automation.pool_blower_spa_alert`.
- Cloud API: app key `EOOEMOW4YR6QNB07`; login `prod.zodiac-io.com/users/v1/login`;
  devices `r-api.iaqualink.net/devices.json`; session `p-api.iaqualink.net/v1/mobile/session.json`.
- Feasibility verdict: `docs/MENU-WIPE-FEASIBILITY.md`. App-wipe spec:
  `docs/superpowers/specs/2026-06-01-panel-program-wipe-design.md` (commit 8074f15).
- Deploy: push to origin/master (dashboard pulls the component), then esphome_ws.ps1
  `compile` then `upload -Port 192.168.4.51`; watch for `selftest PASS`. Patch the
  live dashboard yaml via DownloadString/UploadString on `:6052/edit?configuration=pool-bridge.yaml`.
