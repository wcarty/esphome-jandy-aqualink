# 📚 Documentation guide

> **Start with the current reference pages.** Historical notes preserve the
> engineering record, but may describe features that have since changed.

> [🏠 Project README](../README.md) · [🧩 Capability map](PANEL-CAPABILITY-MAP.md) · [🗺️ Roadmap](ROADMAP.md)

## ✨ Current reference

| Document | Use it for |
| --- | --- |
| [🏠 Project README](../README.md) | Installation, wiring, safety, and first startup |
| [🔌 Tested hardware stack](../README.md#1-gather-hardware) | M5Stack Atom Lite and ATOM RS485 Base |
| [🧩 Pool Panel Capability Map](PANEL-CAPABILITY-MAP.md) | Supported readings, controls, and protocol limits |
| [🗺️ Roadmap](ROADMAP.md) | Current feature status and future ideas |
| [⚙️ Example configuration](../firmware/pool-bridge.yaml) | ESPHome device and Home Assistant entities |

## ✅ Current capability summary

| Area | Available now |
| --- | --- |
| 🌡️ Monitoring | Pool, spa, and air temperatures; pump RPM/watts; AquaPure output, salt, and status; TrueSense pH/ORP |
| 🎛️ Controls | State-aware pool/spa, pump, cleaner, blower, heater, and pool-light switches; AUX2 color wheel and AUX6 Stenner command switches |
| 🔥 Setpoints | Pump RPM, pool/spa heat setpoints, and AquaPure pool output when the iAqualink `0x33` slot is unused |
| 🖥️ Dashboard | Authenticated, locally hosted ESPHome dashboard with grouped controls |
| 🛡️ Safety | Boot-off interlock and scheduler permission, page-confirmed iAqualink writes, and one write sequence at a time |

> [!CAUTION]
> If a physical iAqualink controller uses address `0x33`, keep **iAqualink
> Presence** off. Use the physical controller for AquaPure output and Boost
> until the separate AllButton control path tracked in
> [issue #1](https://github.com/wcarty/esphome-jandy-aqualink/issues/1) is
> implemented.

## 🗃️ Historical engineering record

- 📌 `SESSION-*.md` files are point-in-time handoff notes.
- 🛠️ `superpowers/plans/` contains implementation plans.
- 🧠 `superpowers/specs/` contains design notes and decisions.
- 🚫 [MENU-WIPE-FEASIBILITY.md](MENU-WIPE-FEASIBILITY.md) records why editing
  the panel schedule through the iAqualink Touch menu is not supported.

> [!IMPORTANT]
> When a historical note conflicts with the README, capability map, or source
> code, treat the current reference and source code as authoritative.
