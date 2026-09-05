# 📚 Documentation guide

> **Start with the current reference pages.** Historical notes preserve the
> engineering record, but may describe features that have since changed.

## ✨ Current reference

| Document | Use it for |
| --- | --- |
| [🏠 Project README](../README.md) | Installation, wiring, safety, and first startup |
| [🔌 Tested hardware stack](../README.md#1-gather-hardware) | M5Stack Atom Lite and ATOM RS485 Base |
| [🧩 Pool Panel Capability Map](PANEL-CAPABILITY-MAP.md) | Supported readings, controls, and protocol limits |
| [🗺️ Roadmap](ROADMAP.md) | Current feature status and future ideas |
| [⚙️ Example configuration](../firmware/pool-bridge.yaml) | ESPHome device and Home Assistant entities |

## 🗃️ Historical engineering record

- 📌 `SESSION-*.md` files are point-in-time handoff notes.
- 🛠️ `superpowers/plans/` contains implementation plans.
- 🧠 `superpowers/specs/` contains design notes and decisions.
- 🚫 [MENU-WIPE-FEASIBILITY.md](MENU-WIPE-FEASIBILITY.md) records why editing
  the panel schedule through the iAqualink Touch menu is not supported.

> [!IMPORTANT]
> When a historical note conflicts with the README, capability map, or source
> code, treat the current reference and source code as authoritative.
