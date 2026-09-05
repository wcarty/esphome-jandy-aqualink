# 🚫 Bus-driven wipe feasibility: iAquaLink Touch MENU route

> [!NOTE]
> **Historical engineering record.** This feasibility investigation may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Date: 2026-06-01
Status: CONFIRMED INFEASIBLE via live experiment (see Confirmation). Pivot to the iAquaLink-app method.

## Confirmation (live experiment, 2026-06-01)

Ran the "try harder" experiment: added a catch-all that raw-dumps EVERY 0x33 frame
type we don't already decode (diagnostic commit `570ea80`), redeployed, and
re-opened the MENU on a fresh registration. **Positive control passed:** the
logger caught the panel's startup frame (`IAQ RAW cmd=0x29`) and its identity
frame (`cmd=0x2D` = "AQUALINK"), proving it surfaces undecoded frames. With that
proven-complete logger, opening the MENU produced `PAGE MENU(0x0F)` then `PAGE
END` with **zero frames of any type in between**, while the HOME page immediately
prior streamed full content. So the MENU is genuinely empty on this panel; it is
NOT a decode or handshake gap. The Touch-MENU bus-wipe route is conclusively dead.
(Captures: menu_capture2.log, menu_capture3.log.)

## Question

Can our ESP box reach the panel's stored program/schedule editor over the bus and
delete all scheduling, with no working iAquaLink (the founder's preferred
self-contained wipe)? This note scopes the firmware build that would be needed,
based on a live read-only menu test plus a source study of AqualinkD.

## What we tested live (read-only)

With a fresh 0x33 (iAquaLink Touch) registration and a capture already running, we
pressed the Menu key (0x02). Result:

```
SENT IAQ KEY 0x02
IAQ PAGE MENU(0x0F)     <- panel switched to the MENU page
IAQ PAGE END            <- ... immediately, with ZERO buttons/messages in between
```

The MENU page is **reachable** (navigation works, the panel acknowledges it) but
the panel sends it **empty** to our emulation. This is not a capture-timing
artifact (it reproduced the Session 4 result with correct timing + fresh
registration).

## Source findings (AqualinkD)

1. **Same navigation mechanism.** `goto_iaqt_page` reaches MENU (0x0F) by pressing
   key 0x02 then waiting for the page, identical to what we do. AqualinkD adds a
   page-load discipline (`set_iaq_cansend(false)` on PAGE_START, `true` on
   PAGE_END in `process_iaqtouch_packet`), i.e. it does not send *keys* mid-load.
   But during the load it still only ACKs inertly, which is exactly what our box
   did while the MENU loaded. So our menu-open behavior already matched
   AqualinkD's, and the page was still empty.

2. **AqualinkD does NOT edit panel schedules over iAquaLink Touch.**
   `KEY_IAQTCH_SCHEDULE` and `KEY_IAQTCH_PROGRAM_GROUP` are defined in
   `iaqtouch_aq_programmer.c` but never referenced anywhere. The iAquaLink Touch
   programmer only does device on/off, OneTouch, light color, pump RPM, VSP
   assignments, freeze protect, setpoints, aux labels, AquaPure/SWG. AqualinkD's
   own scheduling (`aq_scheduler.c`) is an external cron that fires its API on a
   timer (the "be the outside scheduler" model). The only place AqualinkD walks a
   panel's PROGRAM menu is the **PDA protocol** (`pda_aq_programmer.c`), a
   different remote generation.

3. **Our panel is iAquaLink Touch, not PDA.** It pushes Touch pages at 0x33. The
   PDA/AquaPalm device range is 0x60-0x63 (`DEV_PDA_JDA_MASK`), which also
   collides with the IntelliFlo pump at 0x60 on our bus. Emulating a PDA remote on
   this Touch panel is unlikely to work and is unverified.

## Verdict

A self-contained bus wipe via the iAquaLink Touch MENU is **low-probability and
high-effort, with no reference implementation for the delete**:

- The MENU renders empty to a faithful Touch client (our load behavior matched
  AqualinkD's). The most likely explanation is that this minimal screenless RS-8
  power center simply does not expose program/schedule content over the Touch
  menu; it was meant to be programmed by the (now dead) iAquaLink 2.0 cloud
  device, not a local menu.
- Even if a fuller handshake did populate the menu, AqualinkD provides **no**
  precedent for deleting schedules over iAquaLink Touch. We would be reverse-
  engineering an unproven capability from scratch.
- The PDA route, where program editing actually exists in AqualinkD, does not fit
  this panel.

## Recommendation

**Pivot the wipe to the iAquaLink-app method** (already spec'd at
`docs/superpowers/specs/2026-06-01-panel-program-wipe-design.md`, commit
`8074f15`): re-online the real iAquaLink 2.0 over Ethernet (its WiFi is dead but
it has an Ethernet jack) and delete every schedule/automation through the Jandy
app, the exact tool that created them. Reliable, low-effort, and it directly
removes the programs. There is no urgency: the leftover programs are harmless and
we now monitor + alert on them.

### Optional, before fully abandoning the bus route

A single time-boxed confirmatory experiment: implement AqualinkD's exact startup
sequence + page-load discipline faithfully and re-test the MENU once. If it is
still empty, the Touch-MENU route is conclusively dead. Expected value is low
given the findings above, but it would remove all doubt for the founder's
"self-contained, repeatable" goal.

## Keep regardless of the wipe path

- The read-only sniffer + phone alerts shipped today (monitoring is independent of
  how we eventually wipe).
- The post-wipe future project stands: Home Assistant as the sole scheduler, with
  a mandatory filtration failsafe (the panel is no longer a hardware fallback once
  its programs are gone).
