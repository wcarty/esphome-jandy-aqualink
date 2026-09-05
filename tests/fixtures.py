"""Test fixtures for the Jandy Aqualink RS decoder.

The CAPTURED_* frames are real bytes pulled off the live RS-485 bus via the
M5Stack TCP bridge (192.168.4.51:8888). Every one validated checksum-clean
(107/107 over a 30s capture during the Phase 2 research session).

Raw bytes here are what arrives on the wire, INCLUDING any byte-stuffing.
The LOGICAL_* values are the de-stuffed frames the extractor should emit.
None of the live captures happen to contain a stuffed 0x10, so the stuffing
edge cases below are synthetic frames built by hand to the documented rule.
"""


def h(s: str) -> bytes:
    """Hex string -> bytes. Whitespace is ignored, so groups read cleanly."""
    return bytes.fromhex(s)


# --- Captured live (de-stuffed == raw; none contain a stuffed 0x10) ---

# poll to device 0x60 (the Pentair IntelliFlo3 pump)
POLL_PUMP = h("10 02 60 00 72 10 03")
# poll to 0x58
POLL_58 = h("10 02 58 00 6A 10 03")
# display text "Air Temp" (dest 0x33, cmd 0x25); data[0]=0x05 line/attr byte
DISPLAY_AIR_LABEL = h("10 02 33 25 05 41 69 72 20 54 65 6D 70 00 41 10 03")
# display text "167" + degree symbol (the air temp value line); data[0]=0x01
DISPLAY_AIR_VALUE = h("10 02 33 25 01 31 36 37 C2 BA 00 85 10 03")
# status to 0x38 (cmd 0x0C); 0x5B = 91 decimal = pool water temp
STATUS_38_TEMP = h("10 02 38 0C 12 57 66 5B 80 10 03")
# binary status to 0x33 (cmd 0x28)
STATUS_33_BIN = h("10 02 33 28 05 1E 1A 0F 36 EF 10 03")
# AquaPure SWG: panel sets output to 85%, then polls cell 0x50. The cell's
# CMD_PPM reply reports 5600 ppm and status 0x00 (on).
SWG_PERCENT_85 = h("10 02 50 11 55 C8 10 03")
POLL_SWG = h("10 02 50 00 62 10 03")
SWG_PPM_ON = h("10 02 00 16 38 00 60 10 03")
SWG_PPM_NO_FLOW = h("10 02 00 16 38 01 61 10 03")

# Keypad equipment-LED status (dest 0x08, cmd 0x02): the de-stuffed LOGICAL frame
# as the device stores it in Frame.raw. Captured live 2026-06-01 in Service mode
# by toggling one circuit at a time at the panel and diffing data = raw[4:-3].
# Circuits occupy bits in the data: air_blower = byte 0 bit 6, cleaner = byte 1
# bit 0, spa_mode = byte 1 bit 2, filter_pump = byte 1 bit 4 (set at rest).
# NOTE: these carry a literal 0x10 data byte, so build Frame(...) directly rather
# than re-feeding the FrameExtractor (which would de-stuff that 0x10 again).
STATUS_08_BASELINE = h("10 02 08 02 00 10 00 00 00 2C 10 03")    # pool, blower+cleaner off, pump on
STATUS_08_SPA_ON = h("10 02 08 02 00 14 00 00 00 30 10 03")      # spa mode on
STATUS_08_BLOWER_ON = h("10 02 08 02 40 10 00 00 00 6C 10 03")   # air blower on
STATUS_08_CLEANER_ON = h("10 02 08 02 00 11 00 00 00 2D 10 03")  # cleaner on

ALL_CAPTURED = [
    POLL_PUMP,
    POLL_58,
    DISPLAY_AIR_LABEL,
    DISPLAY_AIR_VALUE,
    STATUS_38_TEMP,
    STATUS_33_BIN,
]

# --- Synthetic (wire bytes WITH stuffing) for edge cases ---

# A data byte equal to 0x10 (DLE) is stuffed as "10 00" on the wire.
#   logical: 10 02 33 25 [10] 41 BB 10 03   (checksum 0xBB over [10 02 33 25 10 41])
WIRE_STUFFED_DATA = h("10 02 33 25 10 00 41 BB 10 03")
LOGICAL_STUFFED_DATA = h("10 02 33 25 10 41 BB 10 03")

# The checksum byte itself can equal 0x10 and is then stuffed too.
#   logical: 10 02 00 00 FE [10] 10 03   (checksum 0x10 over [10 02 00 00 FE])
WIRE_STUFFED_CKSUM = h("10 02 00 00 FE 10 00 10 03")
LOGICAL_STUFFED_CKSUM = h("10 02 00 00 FE 10 10 03")


def display_frame(line_byte: int, text: str, degree: bool = False, dest: int = 0x33) -> bytes:
    """Build a logical cmd-0x25 display frame for the keypad address `dest`.

    ASCII text needs no byte-stuffing, so the logical frame equals the wire
    bytes and can be fed straight to the extractor. `degree` appends the same
    two-byte degree glyph (C2 BA) the real panel emits, before the NUL.
    Verified to reproduce DISPLAY_AIR_LABEL and DISPLAY_AIR_VALUE exactly.
    """
    inner = bytearray([0x10, 0x02, dest, 0x25, line_byte])
    inner += text.encode("ascii")
    if degree:
        inner += b"\xC2\xBA"
    inner += b"\x00"
    inner.append(sum(inner) & 0xFF)  # checksum over everything so far
    inner += b"\x10\x03"
    return bytes(inner)
