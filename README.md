# 🌊 ESPHome Jandy AquaLink

> **A local, ESP32-native bridge for monitoring and carefully controlling a
> Jandy AquaLink RS pool system from Home Assistant.**

[![ESPHome](https://img.shields.io/badge/ESPHome-external%20component-2CA3D5?logo=esphome&logoColor=white)](https://esphome.io/)
[![Platform](https://img.shields.io/badge/platform-ESP32-00979D?logo=espressif&logoColor=white)](https://www.espressif.com/)
[![Protocol](https://img.shields.io/badge/bus-RS485-0B7285)](docs/PANEL-CAPABILITY-MAP.md)

Connect an ESP32 directly to the pool panel's RS485 bus—no separate Linux box,
cloud service, or Wi-Fi bridge in the real-time path. The timing-critical bus
task runs on a dedicated ESP32 CPU core while Home Assistant and Wi-Fi run on
the other.

> [!WARNING]
> **This project can operate live pool equipment.** Start with control disabled,
> confirm monitoring data at the equipment pad, and test one circuit at a time.
> Never run another emulator at the same RS485 address.

## ✨ At a glance

| Capability | Status | What you get |
| --- | :---: | --- |
| 🌡️ Environment | ✅ | Pool, spa, and air temperature |
| ⚡ Equipment | ✅ | Filter pump, cleaner, blower, pool/spa mode, and heater state |
| 💧 Water chemistry | ✅ | AquaPure output, salt level, faults, TrueSense pH, and ORP |
| 🎛️ Everyday control | ✅ | State-aware equipment switches, pool light, color wheel, and Stenner pump |
| 🔥 Advanced control | ✅ | Pump RPM, heater setpoints, heater enable, and AquaPure output |
| 🖥️ Local dashboard | ✅ | Authenticated, responsive ESPHome web dashboard |
| 🛡️ Safety gates | ✅ | Boot-off master interlock, page checks, and narrow scheduler permission |

> [!TIP]
> The [complete example configuration](firmware/pool-bridge.yaml) creates every
> supported entity and is the recommended starting point.

## 🚀 Quick start

### 1. Gather hardware

<p align="center">
  <a href="https://docs.m5stack.com/en/core/ATOM%20Lite">
    <img src="https://raw.githubusercontent.com/m5stack/m5-docs/e5354c5dcc61d16fdc1dff9d92c2c760728d0b60/docs/assets/img/product_pics/core/minicore/atom/atom_lite_01.webp" alt="M5Stack Atom Lite ESP32 controller" width="260">
  </a>
  &nbsp;&nbsp;&nbsp;&nbsp;
  <a href="https://docs.m5stack.com/en/base/atom_rs485">
    <img src="https://raw.githubusercontent.com/m5stack/m5-docs/e5354c5dcc61d16fdc1dff9d92c2c760728d0b60/docs/assets/img/product_pics/atom_base/atomicRS485/atom485.webp" alt="M5Stack ATOM RS485 Base" width="260">
  </a>
</p>

| Hardware | Tested model | Role |
| --- | --- | --- |
| 🧠 ESP32 controller | [**M5Stack Atom Lite**](https://docs.m5stack.com/en/core/ATOM%20Lite) | Runs ESPHome and the RS485 protocol task |
| 🔌 RS485 interface | [**M5Stack ATOM RS485 Base**](https://docs.m5stack.com/en/base/atom_rs485) | Connects the Atom Lite to the AquaLink bus |
| 🏊 Pool controller | Jandy AquaLink RS panel | Provides RS485 A, B, and ground terminals |

```text
┌──────────────────────┐      ┌────────────────────────┐      ┌─────────────────────┐
│  M5Stack Atom Lite   │──────│  M5Stack ATOM RS485     │──────│  Jandy AquaLink RS  │
│  ESP32 + ESPHome     │      │  Base (SP3485)          │      │  RS485 A / B / GND  │
└──────────────────────┘      └────────────────────────┘      └─────────────────────┘
         GPIO19 TX                       RS485 bus
         GPIO22 RX
```

The tested ATOM RS485 base uses automatic direction control. Defaults are
**GPIO19 TX**, **GPIO22 RX**, **9600 baud**, 8N1.

### 2. Configure ESPHome

Copy [`firmware/pool-bridge.yaml`](firmware/pool-bridge.yaml), then add your
Wi-Fi, API encryption, OTA, and dashboard credentials to `secrets.yaml`.

```yaml
external_components:
  - source: github://wcarty/esphome-jandy-aqualink
    refresh: 0s
```

```yaml
jandy_aqualink:
  keypad_address: 0x08
  polls_answered:
    name: Jandy Keypad Polls Answered
  reply_latency:
    name: Jandy Keypad Reply Latency
  checksum_errors:
    name: Jandy Bus Checksum Errors
```

### 3. Flash and verify

1. ✅ Flash the ESP32.
2. ✅ Confirm **Polls Answered** climbs steadily.
3. ✅ Confirm **Checksum Errors** stays at zero.
4. ✅ Turn on **iAqualink Presence** to receive rich display-page readings.
5. ⚠️ Keep **Pool Keypad Keypress Armed** off until you are ready to test a
   control at the pool pad.

Open `http://<pool-bridge-ip>/` on your trusted local network for the
authenticated dashboard. The master interlock remains off after every restart.

## 🛡️ Built for safe control

| Guardrail | Why it matters |
| --- | --- |
| **Master interlock starts OFF** | The bridge begins as monitor-only after every restart. |
| **iAqualink page confirmation** | Page-scoped commands cannot be sent from the wrong screen. |
| **State-aware switches** | Confirmed circuits only press the panel when their requested state differs. |
| **One write sequence at a time** | Multi-step pump, heater, and chlorinator writes cannot overlap. |
| **Narrow scheduler permission** | Automation may operate only filter pump, cleaner, and pump speed. |
| **Local authenticated web UI** | Dashboard access requires secret-backed credentials. |

> [!IMPORTANT]
> The **Pool Color Wheel** and **Stenner Dosing Pump** use command-state
> switches because AUX2/AUX6 feedback is not decoded yet. They never toggle on
> boot, but their displayed state means “last command sent by this bridge,” not
> necessarily the relay's state after manual panel use.

## 🎚️ Controls and monitoring

### Everyday operation

- ✅ **Pool/spa, filter pump, cleaner, air blower, pool heat, spa heat**:
  panel-state-aware ON/OFF switches.
- ✅ **Pool light**: state-aware ON/OFF switch from the iAqualink HOME page.
- ✅ **Color wheel and Stenner pump**: boot-inert ON/OFF command switches.
- ✅ **Pump speed**: presets and a 600–3450 RPM slider.
- ✅ **AquaPure**: pool output control from 0–100%.
- ✅ **Heaters**: enable switches and pool/spa setpoint controls.

### Live monitoring

- 🌡️ Pool, spa, and air temperatures
- ⚡ Pump RPM and watts
- 🧂 Salt level, chlorinator output, status, and generating state
- 🧪 TrueSense pH and ORP
- 📊 Bus health, reply latency, Wi-Fi signal, and uptime

## 🔌 Choosing a keypad address

The panel supports AllButton keypads at `0x08` through `0x0B`. Choose an
address no physical keypad uses; two devices answering the same poll corrupt
the bus. `0x08` is a good first choice.

> [!CAUTION]
> If **Checksum Errors** rises, stop and select a different keypad address.

The richer display and control path uses the iAqualink Touch slot (`0x33`).
It must also be unused by another live iAqualink controller.

## 🧭 Dashboard

The local dashboard is organized into **Pool Overview**, **Water Chemistry**,
**Equipment Status**, **Automation & Controls**, and **Diagnostics**. It is
designed for a trusted local network only; ESPHome's web server is HTTP, not
HTTPS.

Add these secrets before compiling:

```yaml
pool_bridge_web_username: your_username
pool_bridge_web_password: a_strong_password
```

## ⚠️ Limits to know

- Heater and AquaPure setpoint writes are protocol-confirmed, but the panel
  does not echo the chosen numeric target back to this controller.
- AquaPure output requires both **iAqualink Presence** and the master interlock.
- Boost and other AquaPure settings are intentionally not changed.
- Trust the panel's physical heat indicator over any unexpected heater state.

## 📚 Documentation

| Start here | Purpose |
| --- | --- |
| [📖 Documentation guide](docs/README.md) | Current reference and historical notes |
| [🧩 Panel capability map](docs/PANEL-CAPABILITY-MAP.md) | Protocol support and control limits |
| [🗺️ Roadmap](docs/ROADMAP.md) | Current state and future ideas |
| [⚙️ Example configuration](firmware/pool-bridge.yaml) | Complete ESPHome device setup |

## 🗂️ Repository layout

| Path | Contents |
| --- | --- |
| `components/jandy_aqualink/` | ESPHome component, protocol decoder, and control state machines |
| `firmware/pool-bridge.yaml` | Complete example device configuration |
| `jandy/` and `tests/` | Python protocol reference and regression tests |
| `docs/` | Capability map, roadmap, and engineering history |

## 🙏 Credits

Built on and continuing the work of the original
[ESPHome Jandy AquaLink repository](https://github.com/4pBdhJoZ3Xy3reVvBoU9C3YPzyXDDU/esphome-jandy-aqualink).

Protocol understanding stands on the shoulders of
[AqualinkD](https://github.com/sfeakes/AqualinkD) and
[aquaweb](https://github.com/earlephilhower/aquaweb). This is an independent
ESP32-native reimplementation and is not affiliated with Jandy or Zodiac.
