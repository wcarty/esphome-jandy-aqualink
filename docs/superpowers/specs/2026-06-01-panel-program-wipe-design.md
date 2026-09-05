# 🧠 Panel program wipe + handoff to Home Assistant: design

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

- Date: 2026-06-01
- Status: FALLBACK / safety net. After review, the founder PREFERS the bus
  reverse-engineering approach (our ESP box reaches the program editor and deletes
  programs directly, no working iAquaLink required, the way AqualinkD does it),
  queued as a dedicated NEW session. Use THIS iAquaLink-app method only if the
  reverse-engineering proves infeasible on this screenless panel, or a fast wipe is
  ever needed. No urgency: the panel's leftover programs are harmless.
- Repo: esphome-jandy-aqualink. ESP bridge device `192.168.4.51`.
- Panel: Jandy AquaLink RS power center (no LCD). Controller: iAquaLink 2.0 (dead WiFi,
  HAS an Ethernet jack, founder believes the unit itself still works).

## Goal

Strip the panel of ALL its stored scheduling and automation so it becomes a "dumb
receiver" that only acts when told, then make Home Assistant (through our ESP bridge)
the SOLE scheduler for the pool. This sharpens the old Session 10 framing from "work
around the panel's schedule" to "delete it and take over."

## Why this method, and why not the others

The panel's program editor is NOT reachable from our ESP device: the iAquaLink MENU
page renders BLANK to our 0x33 emulation (confirmed in the Session 4 survey), and the
AllButton keypad menu is a dead end on this screenless panel (no display-text channel,
confirmed Session 2). So we cannot delete the programs by reverse-engineering the bus.

But the founder originally programmed everything through the iAquaLink APP / WEBSITE,
which talks to Jandy's cloud, which talks to the registered iAquaLink 2.0 device, which
programs the panel. That device is physically present (only its WiFi is dead) and has
an Ethernet jack. So the clean path is to put the real iAquaLink back online over
Ethernet and use the app, the exact tool that CREATED the programs, to delete them.
This avoids all reverse-engineering.

Our ESP box cannot stand in for the app: it emulates the LOCAL RS-485 bus side only,
not Jandy's cloud/account side. Making the app drive our box would mean impersonating
the device to Jandy's encrypted, proprietary cloud, a massive out-of-scope effort.

## Diagnosis carried in

- The panel runs baked-in programs autonomously: the cleaner runs on its morning and
  afternoon schedule with the iAquaLink dead; the pump self-schedules (1700-2900 RPM
  overnight); spa mode engages on its own.
- The spa-on is NOT a deliberate schedule (the founder never automated spa). It is an
  inadvertent side effect inside some automation he set up (a scene / one-touch that
  bundles spa-on). Day Party is the prime suspect.
- Our ESP firmware was AUDITED for spa paths 2026-06-01: it NEVER sends the DEVICES
  "Spa Mode" key (0x1f), and its only spa command is the Spa toggle (0x12) via
  `request_pool_mode`, which is manual-only and gated to fire ONLY when already in spa
  (to switch out). Nothing autonomous touches spa; the only timer action is the
  view-only pump read (STATUS then HOME navigation). So our code does not ISSUE a
  spa-on command. A subtler "our presence on the bus provokes the panel" effect is not
  100% ruled out; the verify step is the definitive test.

## The procedure

**Step 0 - BACK UP FIRST (do not skip).** In the iAquaLink app, document every
schedule, timer, and one-touch automation before deleting anything: what runs, what
time, what speed, which equipment. This is the blueprint for rebuilding the wanted
behavior (cleaner mornings/afternoons, normal filtration) in Home Assistant. While
documenting, also watch for the automation that inadvertently includes spa-on
(Day Party suspect). Identifying it is a bonus; deleting all of them removes it anyway.

**Step 1 - get the iAquaLink online.** Wire its Ethernet jack to the UniFi travel
router; give the router internet; confirm the app connects to the device.

**Step 2 - keep our ESP box silent.** It already is (iAqualink Presence OFF + Pool Pump
Auto-Refresh OFF). Only ONE device may hold the iAquaLink bus seat (0x33) at a time, so
our box stays silent the entire time the real iAquaLink is plugged in. No conflict.

**Step 3 - delete everything that self-runs.** Remove every schedule and timer first
(cleaner times, spa, pump-speed program), then the one-touch automations (Day Party,
etc.). Goal: nothing left that can act without being commanded.

**Step 4 - VERIFY (our part), in two clean stages.**
- (a) Box silent: confirm the panel is truly empty, no spa, no unprompted cleaner or
  pump changes over ~24h. Founder observes physically; our box can also read-only watch.
- (b) Box back online (Presence ON, read-only, Auto-Refresh ON): watch again. If spa
  EVER engages now, with nothing left on the panel that could do it, it is our box,
  and we debug exactly what our presence does (a definitive repro). If it stays quiet,
  our box is clean too.
This two-stage watch is also the definitive test of the "our box trips spa" hypothesis.

**Step 5 - disconnect the iAquaLink for good.** Unplug it once the wipe is verified.
This also kills any automation that lived in Jandy's cloud rather than the panel.

**Step 6 (separate future project) - HA as scheduler + failsafe.** Using the Step 0
blueprint, rebuild the cleaner and filtration schedules in Home Assistant driving our
ESP bridge, plus a filtration FAILSAFE so the pump still runs if HA or the ESP hiccups
(the panel no longer provides a hardware fallback once wiped). Scoped after the wipe
verifies.

## Risks

- The old iAquaLink 2.0 must still power up AND still be supported by Jandy's cloud. If
  the unit is dead beyond its WiFi, or Jandy has sunset the model/cloud for it, this
  path fails and we fall back to the hard route (extend the iAquaLink Touch menu
  emulation, a Pi running full AqualinkD, or a Jandy service tech). Known within minutes
  of plugging it in.
- Wiping panel memory: delete via the app (the supported tool); AVOID the panel's blunt
  "Clear Memory" command (phantom-program hazard). The Step 0 backup is the recovery
  path (recreate in HA).
- Once the panel is dumb, pool filtration depends on HA/ESP uptime. The Step 6 failsafe
  is mandatory, not optional.

## Non-goals / out of scope

- Reverse-engineering the panel's program editor over the bus: AVOIDED by this method
  (kept only as the fallback if the iAquaLink path fails).
- Building the HA scheduler: a separate project (Step 6), scoped after the wipe verifies.
- Our ESP box impersonating Jandy's cloud to the app: not feasible, out of scope.

## Open question (resolve during execution)

- Whether to also delete the one-touch macros (Day Party etc.) or only the autonomous
  schedules. Recommendation: delete all for a clean slate. The macros do not
  self-trigger, but a clean slate matches the "dumb receiver" goal and removes the
  spa-on side effect wherever it hides.
