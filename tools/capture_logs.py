"""Capture pool-bridge device logs without fighting Home Assistant for the API.

Why this exists
---------------
While Home Assistant holds an API client, the bridge will not give up a second
one. `esphome logs` reports "Successfully connected" repeatedly and streams zero
lines; this client fails earlier still, with SocketClosedAPIError during the
noise handshake. Every clean capture on 2026-09-04/05 happened while HA was down
for an update.

The device serves ONE API client at a time in practice, despite max_connections
defaulting to 5 on esp32. Home Assistant holds it.

Ruled out on 2026-09-05, each with evidence, so do not re-check these:

- Connection limit config. The default is 5, not 1.
- Log level / volume. Subscribing at WARN, which filters on the device and
  suppresses the ~46-line census burst, fails identically. The max_send_queue
  overflow theory is WRONG.
- Long uptime. A 13-minute-old device refuses a second client just the same.
- Stale sockets, and a bad key. A 45 s settle changed nothing; the PSK is a
  valid 44-char base64 string.
- Hammering. Repeated failed handshakes are harmless; HA stayed healthy through
  many of them.

*** DO NOT DISABLE THE BRIDGE'S HA INTEGRATION ENTRY TO FREE THE SLOT. ***
On 2026-09-05 that dropped HA's connection and the device never reclaimed the
slot. Nothing could reconnect, re-enabling and reload_config_entry both failed,
and recovery needed a physical power cycle at the pad with the pool brain down
for ~25 minutes.

The only known-good window is HA being FULLY STOPPED, which releases the slot
cleanly; that is how every successful capture on 2026-09-04/05 happened.

This is a firmware-level blocker for Phase 2, which needs many capture sessions.
Fix the device side first: a second log path that does not use the API (MQTT
logging, or serial on a spare UART, noting `logger: baud_rate: 0` currently
disables serial), or find why a second API client cannot be served.

The level argument is still worth having: WARN gives exactly the frames Phase 2
needs (`SNIFF d=0x.. c=0x..`, `SENT KEY`, `SENT IAQ KEY`, `STATUS CHANGE`)
without the census noise, which makes a long capture far easier to read.

No firmware change and no flash, which matters because the bridge is on ESPHome
2026.5.3 against a 2026.8.2 container and any OTA drags a three-release
framework jump onto the device that runs the pool.

Usage (inside the ESPHome container, which is where aioesphomeapi lives):

    docker exec ESPHome python /config/capture_logs.py --seconds 120
    docker exec ESPHome python /config/capture_logs.py --seconds 300 --level INFO

The API key is read from the ESPHome secrets file and is never printed.
"""

import argparse
import asyncio
import sys
from datetime import datetime

import yaml
from aioesphomeapi import APIClient, LogLevel

DEFAULT_SECRETS = "/config/secrets.yaml"
DEFAULT_SECRET_KEY = "pool_bridge_api_key"
DEFAULT_ADDRESS = "192.168.4.51"
API_PORT = 6053


def load_noise_psk(secrets_path: str, secret_key: str) -> str:
    """Read the API encryption key. Never log or echo the value."""
    with open(secrets_path, "r", encoding="utf-8") as fh:
        secrets = yaml.safe_load(fh) or {}
    if secret_key not in secrets:
        raise SystemExit(
            f"secret '{secret_key}' not found in {secrets_path}. "
            "Check the key name in the device yaml's api: block."
        )
    return secrets[secret_key]


async def capture(address: str, noise_psk: str, seconds: int, level: LogLevel) -> int:
    client = APIClient(address, API_PORT, None, noise_psk=noise_psk)
    await client.connect(login=True)

    count = 0

    def on_log(entry) -> None:
        nonlocal count
        count += 1
        text = entry.message
        if isinstance(text, (bytes, bytearray)):
            text = text.decode("utf-8", errors="replace")
        # The device already stamps its own time; prefix ours so a capture can be
        # correlated against Home Assistant history.
        print(f"{datetime.now().strftime('%H:%M:%S.%f')[:-3]} {text}", flush=True)

    client.subscribe_logs(on_log, log_level=level)

    try:
        await asyncio.sleep(seconds)
    finally:
        try:
            await client.disconnect()
        except Exception:
            pass

    return count


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--seconds", type=int, default=120, help="capture duration")
    ap.add_argument(
        "--level",
        default="WARN",
        choices=["ERROR", "WARN", "INFO", "CONFIG", "DEBUG"],
        help="device-side log level. WARN keeps the census burst off the wire.",
    )
    ap.add_argument("--address", default=DEFAULT_ADDRESS)
    ap.add_argument("--secrets", default=DEFAULT_SECRETS)
    ap.add_argument("--secret-key", default=DEFAULT_SECRET_KEY)
    args = ap.parse_args()

    level = getattr(LogLevel, f"LOG_LEVEL_{args.level}")
    noise_psk = load_noise_psk(args.secrets, args.secret_key)

    print(
        f"# capturing {args.seconds}s from {args.address} at {args.level}",
        file=sys.stderr,
        flush=True,
    )
    count = asyncio.run(capture(args.address, noise_psk, args.seconds, level))
    print(f"# {count} log lines captured", file=sys.stderr, flush=True)


if __name__ == "__main__":
    main()
