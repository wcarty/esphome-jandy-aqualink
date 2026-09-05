# Documentation guide

This folder contains both the current reference material and the engineering
record that led to it. Start with the current pages below; the dated session,
plan, and design files are retained as historical context and may describe work
that is already complete.

## Current reference

| Document | Use it for |
| --- | --- |
| [README](../README.md) | Installing the component, wiring, safety, and first startup |
| [Pool Panel Capability Map](PANEL-CAPABILITY-MAP.md) | Supported readings and controls, and their protocol limits |
| [Roadmap](ROADMAP.md) | Current feature status and future ideas |
| [Example configuration](../firmware/pool-bridge.yaml) | Creating the ESPHome device and Home Assistant entities |

## Historical engineering record

- `SESSION-*.md` files are point-in-time handoff notes for completed or planned
  build sessions.
- `superpowers/plans/` contains implementation plans.
- `superpowers/specs/` contains design notes and decisions.
- [MENU-WIPE-FEASIBILITY.md](MENU-WIPE-FEASIBILITY.md) records why editing the
  panel schedule through the iAqualink Touch menu is not supported.

When a historical note conflicts with the README, capability map, or source
code, treat the current reference and source code as authoritative.
