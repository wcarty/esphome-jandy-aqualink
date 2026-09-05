"""Binary (non-text) status messages.

Only the water-temp byte in the cmd-0x0C status frame to 0x38 is decoded so
far. In the one captured sample, 10 02 38 0C 12 57 66 5B 80 10 03, the final
data byte 0x5B = 91 matches the real pool temp. The offset is pinned to that
sample and MUST be confirmed live (watch it track the pool as it changes; learn
what 0x12/0x57/0x66 are). AquaPure salt chlorinator (SWG) traffic is decoded
from its documented panel command and response pair.
"""

from .frames import Frame

_TEMP_STATUS_DEST = 0x38
_TEMP_STATUS_CMD = 0x0C
_POOL_TEMP_OFFSET = 3  # data[3] in the captured frame


def decode_status(frame: Frame) -> dict:
    """Return the values a status frame carries, or {} if it carries none we know."""
    if (
        frame.dest == _TEMP_STATUS_DEST
        and frame.cmd == _TEMP_STATUS_CMD
        and len(frame.data) > _POOL_TEMP_OFFSET
    ):
        return {"pool_temp": frame.data[_POOL_TEMP_OFFSET]}
    return {}


# --- Keypad equipment-LED status (CMD_STATUS 0x02 to the AllButton keypad) ---
#
# A registered AllButton keypad receives a steady stream of CMD_STATUS frames
# carrying the equipment LED bitmap. Each circuit LED occupies bits in the data
# payload (AqualinkD source/allbutton.c processLEDstate uses 2 bits per LED, an
# ON bit and an adjacent FLASH bit). The per-panel positions below were pinned
# live 2026-06-01 by toggling each circuit at the panel and diffing the frame.
# Only the ON bit is read (FLASH is not needed to tell whether a circuit is on).

KEYPAD_STATUS_CMD = 0x02

# AquaPure salt-water generator (SWG) protocol. The panel sends CMD_PERCENT to
# the cell address (0x50-0x53), then polls it. The reply to that poll has
# CMD_PPM; data[0] is salt in hundreds of ppm and data[1] is the cell status.
SWG_DEVICE_MIN = 0x50
SWG_DEVICE_MAX = 0x53
SWG_CMD_PERCENT = 0x11
SWG_CMD_PPM = 0x16
CHEMISTRY_CMD = 0x21

SWG_STATUSES = {
    0x00: "on",
    0x01: "no_flow",
    0x02: "low_salt",
    0x04: "high_salt",
    0x08: "clean_cell",
    0x09: "turning_off",
    0x10: "high_current",
    0x20: "low_voltage",
    0x40: "low_water_temp",
    0x80: "check_pcb",
    0xFD: "general_fault",
    0xFF: "off",
}


class SwgReader:
    """Decode the AquaPure command/poll/reply exchange."""

    def __init__(self):
        self.state = {}
        self._awaiting_reply = False

    def feed(self, frame: Frame) -> None:
        if SWG_DEVICE_MIN <= frame.dest <= SWG_DEVICE_MAX:
            if frame.cmd == SWG_CMD_PERCENT and frame.data and frame.data[0] <= 101:
                self.state["salt_chlorinator_output"] = frame.data[0]
            self._awaiting_reply = frame.cmd == 0x00
            return

        if not self._awaiting_reply:
            return
        self._awaiting_reply = False
        if frame.cmd != SWG_CMD_PPM or len(frame.data) < 2:
            return

        self.state["salt_level"] = frame.data[0] * 100
        status = frame.data[1]
        self.state["salt_chlorinator_status"] = SWG_STATUSES.get(status, f"unknown_0x{status:02x}")
        self.state["salt_chlorinator_generating"] = (
            status == 0x00 and self.state.get("salt_chlorinator_output", 0) > 0
        )


def decode_chemistry(frame: Frame) -> dict:
    """Decode Jandy TrueSense/ChemLink pH and ORP tag/value pairs."""
    if frame.cmd != CHEMISTRY_CMD:
        return {}
    out = {}
    data = frame.data
    for index in range(0, len(data) - 1, 2):
        tag, value = data[index], data[index + 1]
        if tag == 0x02 and 20 <= value <= 120:
            out["orp"] = value * 10
        elif tag == 0x03 and 50 <= value <= 100:
            out["ph"] = value / 10
    return out

# circuit name -> (data byte index, ON-bit index within that byte)
CIRCUIT_BITS = {
    "air_blower": (0, 6),
    "cleaner": (1, 0),
    "spa_mode": (1, 2),
    "filter_pump": (1, 4),
}


def decode_keypad_status(frame: Frame, bits: dict = CIRCUIT_BITS) -> dict:
    """Decode tracked circuit on/off states from a keypad CMD_STATUS frame.

    Returns {} for any non-status frame. Each circuit is the ON bit at its
    (byte, bit) position in frame.data; a byte past the end reads as off.
    """
    if frame.cmd != KEYPAD_STATUS_CMD:
        return {}
    data = frame.data
    out = {}
    for name, (byte_i, bit_i) in bits.items():
        out[name] = bool(byte_i < len(data) and (data[byte_i] >> bit_i) & 1)
    return out
