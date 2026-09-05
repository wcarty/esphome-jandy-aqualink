# 🗃️ Session kickoff: reverse-engineer the panel program editor, then wipe all scheduling

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. Self-contained. BRAINSTORM with the founder
first, then a careful READ-ONLY spike before anything is deleted.

## The goal

Make the AquaLink RS panel a "dumb receiver" by deleting ALL of its stored
scheduling and automation using ONLY our ESP box, with no working iAquaLink
required, the way AqualinkD does it over the bus. End state: Home Assistant
becomes the sole pool scheduler. This is the founder's preferred, repeatable-by-
anyone solution (chosen over the quicker iAquaLink-app method, which is the
fallback). There is NO urgency: the panel's leftover programs are harmless.

## Where we are (start of this session)

- Repo HEAD `c028039` on origin/master, device `192.168.4.51` (ESP bridge),
  ESPHome dashboard `192.168.1.126:6052`. Panel = AquaLink RS power center, NO LCD.
- Our box is SILENT right now: iAqualink Presence OFF + Pool Pump Auto-Refresh OFF.
  Keep it silent until this session deliberately and carefully re-enables it.
- The panel runs harmless baked-in programs on its own (a morning/afternoon cleaner
  schedule, a stray spa-mode automation the founder never intentionally set, and a
  variable pump schedule). Founder uses panel SERVICE MODE as the off-switch if the
  spa/blower annoys him in the meantime.
- READ FOR FULL CONTEXT: memory topic `project_pool_controller_phase2.md`, especially
  the "STRATEGIC PIVOT", "TEST RESULT", "CODE AUDIT", and "PREFERENCE FLIP" notes.
- The FALLBACK method (re-online the real iAquaLink 2.0 over Ethernet, delete via the
  Jandy app) is spec'd at `docs/superpowers/specs/2026-06-01-panel-program-wipe-design.md`.
  Only reach for it if this reverse-engineering path proves infeasible.

## The central unknown to resolve FIRST

The panel's program/schedule editor lives in the MENU page, which rendered BLANK to
our PARTIAL 0x33 (iAquaLink Touch) emulation during the Session 4 survey (see
`docs/PANEL-CAPABILITY-MAP.md`: MENU key 0x02, page type 0x0F, "renders EMPTY to our
device"). The AllButton keypad (0x08) menu is also a dead end on this screenless
panel (no display-text channel, proven Session 2).

THE QUESTION: is that blank MENU a HARD limit of this screenless panel, or just our
emulation being incomplete versus AqualinkD's fuller iAquaLink Touch protocol?
AqualinkD reaches RS program editing over the same bus, so there is real reason to
believe the blank is on OUR side and a fuller handshake would open it.

## Approach (brainstorm-first, spike-first, read-only-first)

1. Study how AqualinkD walks, reads, edits, and deletes RS programs. Reference clone
   at `C:\Users\Falcon\Documents\pool-controller\AqualinkD-ref`: look at
   `iaqtouch_aq_programmer.c`, `allbutton_aq_programmer.c`, `onetouch_aq_programmer.c`,
   `aq_programmer.c`. Map its menu-navigation + the program read/delete sequences.
2. Diff against our current 0x33 emulation in `components/jandy_aqualink/` (jandy_proto
   + jandy_aqualink): what part of the iAquaLink Touch handshake do we NOT yet do that
   AqualinkD does, especially around reaching MENU (0x0F) and the program pages.
3. READ-ONLY SPIKE: extend the emulation just enough to try to OPEN the MENU and READ
   the program list. Confirm we can reach and read the programs, with the founder
   watching, gated, NO deletes. This answers the whole feasibility question.
4. If reachable: design the back-up (read every program out, the blueprint for the
   future HA scheduler) then the delete-all, then a founder-watched live wipe.
5. If NOT reachable: fall back to the iAquaLink-app spec.

## Safeguards

- Read-only until we have proven we can navigate the program editor reliably.
- Any delete stays gated behind the master interlock and is founder-watched.
- Back up (read out) every existing program BEFORE deleting anything.
- Only one device may hold the 0x33 iAquaLink seat at a time. Do not run our box and a
  real iAquaLink on the bus simultaneously.

## Bonus if it works

A fuller iAquaLink Touch implementation likely also unlocks the salt/SWG readings and
the rest of the menu, which have been out of reach (see PANEL-CAPABILITY-MAP notes).

## After the wipe (separate future project)

Home Assistant becomes the sole scheduler: rebuild the wanted cleaner/filtration
schedules from the backed-up blueprint, PLUS a mandatory filtration FAILSAFE so the
pump still runs if HA or the ESP hiccups (the panel is no longer a hardware fallback
once wiped).
