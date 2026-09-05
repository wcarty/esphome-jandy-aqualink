# 🧠 Panel capability survey and pump speed reading

> [!NOTE]
> **Historical engineering record.** This design specification may not describe
> the current implementation. See the [📚 Documentation guide](../../README.md)
> and [🏠 current README](../../../README.md) for current references.

Date: 2026-05-31
Status: Approved direction, pre-implementation
Scope of this push: a full read-only survey of every page the panel will draw, a
written Panel Capability Map, and pump speed (RPM) reading into Home Assistant.
No equipment, value, or commit buttons are pressed. No speed setting is built
this session (that is the next session, informed by this map).

## Goal

We have only ever seen the panel's home page, because that is the one page the
panel volunteers. The iAqualink protocol exposes many more pages we have never
looked at on this panel (a full devices list, a live status page, settings pages,
OneTouch macros, color lights). We do not know the full extent of what this panel
will let us read or control.

This session maps that surface. We extend the firmware with a safe, navigation
only path, walk the panel page by page while the founder watches the equipment,
log everything, and turn it into one reference document: the Panel Capability
Map. Pump speed reading falls out of the same Status and Devices pages, so we ship
a Pool Pump Speed sensor (and watts, if the panel exposes it) as the concrete
deliverable.

## Why this is worth doing now, and why it is safe

The panel enumerates its own buttons. When it draws a page, each button arrives
with its name and current state already filled in (for example "VSP1 Spd / ADJ",
"Pool Heat / ON", "Aux5 / OFF"). So we can inventory everything readable and
everything controllable purely by viewing pages. We never press an equipment
button to learn that it exists.

That makes the survey almost entirely safe navigation: send a page movement key,
read and log whatever the panel draws, move on. The only buttons with a side
effect (equipment, value, or commit) are simply never pressed during the survey.

## What already exists

The shipped firmware (repo HEAD `a6a7b59`, device 192.168.4.51) emulates the
iAqualink controller at address 0x33:

- An inert presence ACK (`10 02 00 01 00 00 13 10 03`) answered to every 0x33
  frame, gated by the "iAqualink Presence" switch, makes the panel push pages.
- `IaqReader` (mirrored in `jandy/iaq.py` and `jandy_proto.cpp`, TDD plus an
  on-device selftest) decodes the HOME page temperatures (`0x23` page start,
  `0x25` message lines, `0x28` page end).
- A gated one-shot keypress: a single allowlisted keycode placed in the ACK's
  pending-key byte, sent only on the `0x30` poll, behind a master interlock
  ("Pool Keypad Keypress Armed", default off) plus the presence switch plus an
  equipment allowlist that excludes the heaters.

Live state confirmed this session: 0 checksum errors, ~70 microsecond reply
latency, pool water 88F, presence holding. There is no pump speed reading today.

## Reference protocol facts (from AqualinkD source)

Page type constants (carried in the `0x23` page-start byte):

| Page | Type | Notes |
|---|---|---|
| HOME | 0x01 | what we read today |
| DEVICES | 0x36 | full equipment list with labels and states |
| DEVICES2 / DEVICES3 / REV | 0x35 / 0x51 / 0x0a | continuation variants on larger installs |
| STATUS | 0x5b (alt 0x2a) | live RPM, watts, SWG, salt |
| SET_TEMP | 0x39 | heat setpoint entry |
| SET_VSP | 0x1e | per-pump speed entry |
| SET_SWG | 0x30 | salt output entry |
| MENU | 0x0f | settings tree |
| ONETOUCH | 0x4d | macros |
| COLOR_LIGHT | 0x48 | light programs |
| VSP_SETUP | 0x2d | pump type and min/max config |

Global navigation keys (page level, never actuate equipment): HOME 0x01, MENU
0x02, ONETOUCH 0x03, HELP 0x04, BACK 0x05, STATUS 0x06, PREV_PAGE 0x20,
NEXT_PAGE 0x21. From the home page, "Other Devices" is the home button 0x18.

Button frame `0x24` layout: index, state, unknown, type, then NUL separated label
words and an "on/off/enabled" state token. Status page readings are text lines:
"RPM: 1350" with the integer at character offset 9, the pump name on the line
above; "GPM:" and "Watts:" follow the same shape.

Keycode meaning is page dependent. On the home page 0x11 to 0x17 are equipment;
on a grid page the same codes select grid tiles. This is why the survey restricts
itself to global keys plus the home gated 0x18, and never presses a numbered tile.

## Safety model (the crux)

1. Master interlock unchanged. The existing "Pool Keypad Keypress Armed" switch
   gates every transmitted key, navigation included. Off by default, boots off.
   Turning it off is the hard abort (clears any armed key, reverts to inert).
2. A separate navigation path that is physically incapable of sending an
   equipment, value, or commit key. New `is_iaq_nav_key` allowlist = the global
   keys {HOME 0x01, BACK 0x05, STATUS 0x06, MENU 0x02, ONETOUCH 0x03, PREV 0x20,
   NEXT 0x21}. The equipment path (`iaq_press`) and the navigation path
   (`iaq_nav`) share no keycodes.
3. Other Devices (0x18) is the one page entry key that is not a global. It is
   refused unless the decoder confirms the current page is HOME (0x01), so it can
   only ever mean "Other Devices" and never a grid tile on some other page.
4. View only. During the survey we read and log pages. We never press a numbered
   tile, an ADJ button, a value, or a commit/OK. We leave any page with BACK or
   HOME, which do not commit.
5. One key at a time. Same one-shot model as today: arm one key, it is sent on
   the next `0x30` poll, then cleared. We confirm the resulting page in the log
   before the next key.
6. Every transmitted byte is logged, as today.

Under these rules the survey cannot actuate equipment or change any setting. The
founder watches the pump and panel throughout as defense in depth.

## Architecture and components

One change set to the existing `jandy_aqualink` ESPHome component, same core-1
task and reply model. New pieces:

1. Navigation path (firmware):
   - `jandy::is_iaq_nav_key(key)`: pure allowlist of the global keys above.
   - `JandyAqualink::iaq_nav(key)`: gated by interlock + presence, then accepts
     the key only if `is_iaq_nav_key(key)` is true, OR the key is 0x18 (Other
     Devices) AND `iaq_reader_.current_page() == 0x01` (HOME). 0x18 is handled as
     this explicit special case; it is deliberately NOT in `is_iaq_nav_key`, so it
     can never pass on any page other than HOME. On acceptance it arms a one-shot
     key on the iAqualink path exactly like `iaq_press`.
   - Expose `current_page()` on `IaqReader` (it already tracks `page_type_`).

2. Richer page logging (firmware): for any page the panel draws, log the page
   type by name, and every `0x24` button as "index: label = state", and every
   `0x25` message line as text. A new `jandy::parse_iaq_button` extracts (index,
   state, type, label, state-token) from a `0x24` frame. This is the part we can
   build and TDD now against the reference `0x24` captures.

3. Passive pump traffic logging (firmware): also log the panel's frames to and
   from the pump address 0x60, so we can see whether the live RPM is sniffable
   with zero navigation (the cleanest possible read path).

4. Home Assistant navigation buttons (config), all behind the interlock: "iAq:
   Home", "iAq: Open Devices", "iAq: Open Status", "iAq: Back", "iAq: Menu",
   "iAq: OneTouch", "iAq: Next Page", "iAq: Prev Page". These are temporary
   survey instruments; we keep the useful ones and drop the rest afterward.

5. Pump speed reading (firmware + config, findings dependent): a decoder for
   wherever the survey finds the live RPM. If RPM is in the passive 0x60 traffic,
   decode it there (zero navigation). Otherwise decode the Status page "RPM:" and
   "Watts:" lines, and add a slow, gated status-page refresh so the value stays
   current. Publish `sensor` "Pool Pump Speed" (rpm) and, if available, "Pool
   Pump Watts".

6. Presence persistence (config, small extra): change the "iAqualink Presence"
   switch from `ALWAYS_OFF` to `RESTORE_DEFAULT_OFF` (restores its last state
   across reboots, off on first boot), so temperatures keep reading after a power
   blip. The control interlock stays `ALWAYS_OFF`, so control is still off after a
   reboot exactly as today.

## The live survey procedure

With the founder watching the pump and panel:

1. Confirm baseline: presence on, healthy, home page reading (it is now).
2. Open Devices (Home to confirm HOME, then Other Devices 0x18). Log the full
   button list with labels and states. Use Next/Prev to capture any continuation
   pages. This is the control inventory: every circuit, every VSP adjust button,
   heaters, aux, lights, and their current states.
3. Open Status (Status 0x06). Log every reading line: pump RPM and watts, SWG
   state and output and salt, temps, any freeze or service state. This is the
   read inventory, and the pump speed source.
4. View the remaining pages reachable by global keys (Menu, OneTouch, and from
   the devices page note which items carry an ADJ for settings), logging each.
   View only, no tile presses.
5. Return Home, leave the bus idle and healthy.
6. While there, determine RPM vs GPM mode for the IntelliFlo3 (the devices and
   status labels make this explicit) so a future "1600" is unambiguous.

## Deliverable: the Panel Capability Map

A document at `docs/PANEL-CAPABILITY-MAP.md` produced from the captured logs:

- Readable values: name, source page or passive frame, byte or text location,
  units, and current observed value.
- Controllable items: name, the page and button it lives on, its keycode in that
  page's context, its state token vocabulary, and the control mechanism (simple
  toggle in the ACK pending byte, vs the multi-step value-set handshake for VSP
  and setpoints).
- A build roadmap: a sane order to implement control, with the risk level of each
  (toggles low, valves and heaters higher), so future sessions wire up mapped
  items instead of discovering blind.

## Testing

Same discipline as the shipped code: pure logic is TDD'd in Python and mirrored
in the C++ on-device selftest against the same frame vectors.

- `is_iaq_nav_key` and the 0x18 home-gate: unit tests that the global keys pass,
  every equipment and value keycode is refused, and 0x18 is refused unless the
  current page is HOME. This is a safety gate, so it is tested like one.
- `parse_iaq_button`: tested against the reference `0x24` button captures
  (Filter Pump, VSP1 Spd ADJ, Pool Heat ON, Aux OFF, etc.), then against our
  panel's captured buttons once we have them.
- Pump speed decoder: tested against the captured Status frames (or 0x60 frames)
  from our panel, the same way the temperature decoder was built from captures.

The survey capture itself is live and not unit testable; the decoders built from
it are.

## Risks and mitigations

- Wrong key on an unknown page actuates equipment. Mitigated by the nav-only
  allowlist, the 0x18 home-gate, view-only discipline, one key at a time with
  log confirmation, and the founder watching.
- The panel may not expose VSP adjust or a status page at all over iAqualink on
  this install. That is itself a finding; the map records it and we adjust the
  roadmap. Reading still attempts the passive 0x60 path.
- RPM vs GPM ambiguity. Resolved during the survey by reading the explicit pump
  labels before trusting any magnitude.
- Holding the status page for continuous reading adds bus activity. If passive
  0x60 reading works we avoid it; otherwise the refresh is slow and gated.

## Out of scope this session (deferred, informed by the map)

- Setting pump speed (the multi-step value-set handshake: request control with
  0x80 on an ordinary poll, answer the panel's 0x31 with the 0x24 digits frame,
  clamp 600 to 3450 RPM, confirm by read-back). Next session.
- Any new equipment control (spa light, aux, valves, heaters).
- The HA dashboard integration of the new sensors (tracked separately).
