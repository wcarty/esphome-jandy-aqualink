# 🗃️ Session kickoff: finish the pool controller (no iAquaLink)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. Self-contained. Brainstorm before building.

## Paste this to start

> Let's finish the pool controller project, the no-iAquaLink path: make Home
> Assistant the smart brain that runs the pool through our ESP box, while the
> panel keeps its basic filtration schedule as a hardware failsafe. Read the
> memory topic project_pool_controller_phase2.md first, plus these repo docs in
> C:\Users\Falcon\Documents\pool-controller\esp32-experiment\docs: ROADMAP.md,
> PANEL-CAPABILITY-MAP.md, SESSION-8-easy-toggles-kickoff.md,
> SESSION-9-heaters-kickoff.md. Brainstorm with me first, then build with TDD and
> founder-watched live tests. I'm non-technical, so explain in plain English.

## Where we are (confirmed)

- The autonomous spa/blower events were a **degraded spa-side remote** sending
  phantom presses. It is now unplugged. An overnight test (2026-06-01 to 06-02)
  confirmed it: spa mode and the blower stayed off all night; only the intended
  ~5am cleaner schedule ran. So we are **not** wiping the panel schedule; we keep
  the panel's cleaner/filtration schedule as the hardware failsafe.
- The ESP box (device `192.168.4.51`, ESPHome dashboard `192.168.1.126:6052`,
  repo `esphome-jandy-aqualink`) already, all via the bus, no iAquaLink:
  - Reads air/pool/spa temps and pump RPM/watts.
  - Holds an inert AllButton keypad sniffer (0x08) publishing 4 equipment
    binary_sensors: spa_mode, air_blower, filter_pump, cleaner.
  - Controls (gated): filter pump, pump speed (+ presets), cleaner, pool light,
    air blower, and pool/spa mode.
  - Has a phone-alert automation (`automation.pool_blower_spa_alert`) and a
    read-only "Pool Bus Sniff" capture switch.
- Resting state: 0x08 sniffer ON, `iAqualink Presence` (0x33) OFF, control
  interlock OFF, panel in Auto / pool mode. Repo on `master`, clean, pushed.

## The plan to finish (no iAquaLink needed)

1. **Easy DEVICES toggles** (low-stakes, do first): spa light (0x19), extra aux
   (0x1d), sprinklers (0x1e) on the DEVICES page. Exercises the page-context
   guard on harmless equipment. See SESSION-8-easy-toggles-kickoff.md (a spec
   already exists: docs/superpowers/specs/2026-05-31-devices-toggles-design.md).
2. **Heaters** (highest-stakes, do last, founder-watched): pool + spa heat on/off
   plus setpoint, via the proven 0x24 value-set path (same machinery as pump RPM).
   Confirm the panel's flow interlock, gate carefully. See
   SESSION-9-heaters-kickoff.md.
3. **Home Assistant scheduler** (the payoff, a new build to brainstorm + spec):
   HA automations that run the pump, cleaner, and filtration on the founder's
   schedule through the box's controls. KEEP the panel's basic filtration schedule
   underneath as the failsafe, and design HA's logic so water never goes stagnant
   if HA or the box hiccups (e.g., a watchdog that ensures a daily minimum
   circulation regardless).

Optional bonus reads, also no iAquaLink: salt/SWG passive bus decode; decode all
circuit states (not just the 4 mapped).

## Approach + safety

- Brainstorm-first; TDD (Python suite mirrored by the on-device selftest); gated
  controls with the master interlock OFF by default; founder-watched live tests
  for any equipment actuation (heaters especially).
- Deploy: commit + push to origin/master (the dashboard pulls the component from
  GitHub), then `esphome_ws.ps1 -Action compile` then `-Action upload -Port
  192.168.4.51`; watch the boot log for `selftest ... PASS`, do not actuate on a
  FAIL. Patch the live dashboard yaml (DownloadString/UploadString on
  `:6052/edit?configuration=pool-bridge.yaml`) so new entities survive a recompile.
- Bit map (0x08 status frame, data = raw[4:-3]): air_blower=byte0 bit6,
  cleaner=byte1 bit0, spa_mode=byte1 bit2, filter_pump=byte1 bit4.
- No em dashes, no AI jargon, plain-English explanations alongside any code.

## Deferred bonus (anytime, NOT required to finish the project)

Re-online the iAquaLink 2.0 over Ethernet and crack the stored-schedule
read/delete over the bus + cloud API (the novel, never-been-done capability).
Kickoff: docs/SESSION-iaqualink-reonline-and-sniff.md. Tooling:
C:\Users\Falcon\Documents\pool-controller\cloud-recon\. Feasibility note (why the
local Touch MENU route is a dead end): docs/MENU-WIPE-FEASIBILITY.md.
