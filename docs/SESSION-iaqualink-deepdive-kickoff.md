# 🗃️ Session kickoff: reconnect the iAquaLink 2.0 + map what it is doing (cloud/API deep dive)

> [!NOTE]
> **Historical engineering record.** This point-in-time session handoff may not
> describe the current implementation. See the [📚 Documentation guide](README.md)
> and [🏠 current README](../README.md) for current references.

Paste-ready brief for a fresh session. Goal: bring the dead iAquaLink 2.0 back online,
deep-dive the Jandy cloud/API to understand what the panel and cloud are actually
holding (schedules, OneTouch macros, salt/SWG settings, anything that self-runs), then
trim the panel down to one deliberate failsafe program. Brainstorm first.

This is the optional "novel capability + clean up the panel" session deferred from the
HA-brain build. The HA pool brain is already LIVE and running the pool, so the headline
coordination is: stand the brain down off the bus BEFORE the iAquaLink goes online.

## Paste this to start

> **Reconnect the iAquaLink 2.0 and map what it is doing, then trim the panel to one
> failsafe program.** Brainstorm with me FIRST, then we work.
>
> Read first, in `C:\Users\Falcon\Documents\pool-controller\esp32-experiment`: the memory
> topic `project_pool_controller_phase2.md` (especially the Session 10 HA-brain entry and
> its iAquaLink coordination notes), `docs/SESSION-iaqualink-reonline-and-sniff.md`, the
> "Coordination with the iAquaLink re-online" section of
> `docs/superpowers/specs/2026-06-03-pool-ha-brain-design.md`, and
> `C:\Users\Falcon\Documents\pool-controller\cloud-recon\README` plus the probe there.
>
> Carry these in:
> - The HA pool brain is now LIVE and running the pool (armed, holding the schedule).
>   Before I connect the iAquaLink, the FIRST step is to turn the **Pool Scheduler** switch
>   OFF. That stands the brain fully down, including the presence keeper, so it cannot fight
>   the iAquaLink for the bus. Then turn **iAqualink Presence** OFF too. Only one device may
>   sit in the iAquaLink's 0x33 spot on the bus at a time. When we finish, disconnect the
>   iAquaLink, turn presence back on, and flip the Pool Scheduler back on.
> - The goal is to UNDERSTAND what the panel and the Jandy cloud are actually holding: the
>   schedules, the OneTouch macros (Clean Mode / Pool Mode / Day Party / All Off), the
>   salt/SWG settings, anything that self-runs. The most tractable path is the cloud/API:
>   log into prod.zodiac-io.com with the read-only `cloud-recon` probe, and watch the
>   iAquaLink website's network calls while I view a schedule, so we can read the schedule
>   data in plain JSON. Optionally, with the real iAquaLink online, flip the **Pool Bus
>   Sniff** switch to passively capture how it reads and writes the schedule over the bus
>   (our box stays silent, presence off, sniff is log-only).
> - After we understand it, BACK UP every schedule and macro first, then trim the panel to
>   ONE deliberate failsafe program through the iAquaLink app. Recommended: filter pump at
>   2000 RPM from 10am to 4pm daily. That agrees with the brain's Day phase when the brain
>   is up, and stands alone as the safety net if the brain is ever down. Set the failsafe
>   program BEFORE deleting the others, so there is never a moment with no net.
> - Hardware: the iAquaLink 2.0 has an Ethernet jack (only its WiFi is dead). Bring it
>   online over the UniFi travel router or a wired drop. Tooling already built: `cloud-recon/`
>   (read-only cloud probe, public app key, saves raw JSON to out/), the **Pool Bus Sniff**
>   firmware switch, and the kickoff doc above.
>
> I'm non-technical: plain English alongside any code, no em dashes, no AI jargon.
> Brainstorm with me before doing anything.

## Why the brain-down step matters (do not skip)

The HA brain holds the iAquaLink 0x33 bus seat through our ESP box whenever presence is on,
and the **Pool presence keeper** automation re-enables presence while the scheduler is
armed. So turning **Pool Scheduler** OFF is what truly stands the box down: it stops the
presence keeper, then turning presence OFF vacates 0x33 for the real iAquaLink. Two devices
on 0x33 at once corrupts the bus. The pool keeps circulating meanwhile on the panel's own
schedule (and, once trimmed, the one failsafe program).

## Recovery at the end

Disconnect the iAquaLink, turn **iAqualink Presence** back on, then turn **Pool Scheduler**
back on. The startup-resync re-applies the correct phase for the time of day, and the brain
resumes.
