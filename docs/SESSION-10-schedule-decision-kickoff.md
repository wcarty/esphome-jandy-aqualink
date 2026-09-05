# 🗃️ Session 10 kickoff: deal with the panel's stored schedule

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief. Self-contained. This is the hardest and most uncertain
remaining item, and it is GATED on the Session 7 watch data. Do Session 7 first;
its findings decide which path here is even necessary.

## The problem, in plain terms

The AquaLink RS power center keeps its own timed programs in panel memory and
runs them on its own clock (cleaner switches on unprompted; the pump self-
scheduled 2750 -> 1700 during the Session 4 survey). The founder's concern is
that this stored schedule fights whatever Home Assistant does. The schedule
editor lives in a settings menu that renders EMPTY to our emulated 0x33 device,
so we cannot open it that way today.

## The reframe (decided with founder 2026-05-31): you may not need to wipe it

Now that reliable WRITE control is proven, the cleanest fix is usually to let HA
be the scheduler and let HA WIN, without ever editing the panel:

- **Full HA override**: HA runs the pool on your timers and reasserts the desired
  speed/heat whenever the panel changes something. The panel schedule becomes
  cosmetic.
- **Minimal version (often enough)**: just GUARD THE SALT FLOOR. If the pump
  drops below ~1850 during the day, HA bumps it back up. Ignore the rest.

Session 7's watch tells us which world we are in:
- SIMPLE schedule (few transitions) -> HA override handles it, never touch the
  panel. STOP HERE. This is the expected outcome.
- BUSY/annoying schedule -> consider an actual wipe (below).

## If a real wipe is wanted, the options (ranked)

1. **HA full override** (no panel edit). Most robust without touching the panel.
   Build HA automations that own all timing and reassert on every panel change.
   Start here even in the "busy" case.
2. **Alternate keypad menu walk.** Emulate the older AllButton keypad at address
   0x08 (we already proved presence there in v1) and walk ITS menu tree, where RS
   program editing normally lives (unlike the 0x33 iAqualink device's settings
   page, which renders empty to us). UNCERTAIN on this screenless panel: with no
   LCD, it is untested whether the panel streams CMD_MSG menu text to an emulated
   keypad. Reference: AqualinkD `allbutton_aq_programmer.c` /
   `onetouch_aq_programmer.c` (cloned at `..\AqualinkD-ref`). NOTE: 0x08 and 0x33
   are different bus seats, so this can run alongside the temp/control emulation,
   but do not have two of OUR devices answer the same address.
3. **iAquaLink 2.0 revival** (founder's hardware, in a drawer). FALLBACK ONLY.
   Research FIRST whether Jandy's cloud for that generation is still alive and
   what revival requires (this is a reading-only task, no hardware, can be done
   any time). Big caveats: it is maybe-dead hardware, and it CONFLICTS with our
   0x33 emulation for the same bus seat, so we would have to take our device off
   0x33 to use it. Only pursue if 1 and 2 fail and the founder wants it.
4. **Pi + AqualinkD** (the path shelved in Phase 2). Has schedule editing built
   in and proven, but reintroduces a separate box in the house. Heavy.
5. **Service mode at the panel** (founder, manual). The panel's Clear Memory
   command wipes EVERYTHING (not just the schedule), and these old RS panels are
   known for leaving "phantom" undeletable programs. True last resort.

## Recommended flow for this session

1. Confirm Session 7's watch results are in; classify the schedule simple vs busy.
2. If simple: build the HA override (or just the salt-floor guard) as HA
   automations. Done, no firmware, no panel edit.
3. If busy and the founder still wants the panel schedule gone: try option 2
   (alternate keypad menu walk) as a research/spike first, since it reuses
   hardware we have. Time-box it. If it cannot reach the editor, present options
   3-5 honestly and let the founder choose.

## Optional anytime task

Research iAquaLink 2.0 revivability for the founder's generation (cloud service
status, activation steps, cost). Reading only. Tells us whether option 3 is even
a real fallback before anyone touches the hardware.

## Constraints

Non-technical founder, plain English. No em dashes, no AI jargon, no self-
deprecation. HA access via the MCP tools (see the home-infrastructure memory).
Any HA automation should be built as HA config (helpers/automations), not raw
YAML the founder has to hand-edit.
