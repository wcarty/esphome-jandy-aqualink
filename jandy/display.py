"""Display-text layer for cmd 0x25.

On this panel every captured display frame is addressed to the iAqualink slot
0x33, not to an AllButton keypad at 0x08-0x0B. That distinction decides whether
AqualinkD's menu-walk program reader can be ported here at all, so DisplayLine
carries the destination address and callers must not assume it.

The panel writes a line at a time: a label ("Air Temp") then a value ("167").
We decode each line to clean ASCII and pair a label with the value line that
immediately follows it. The bus interleaves device polls between those two
writes, so the pairer must ignore non-display frames without losing the pending
label.
"""

import re

from .frames import Frame

CMD_DISPLAY = 0x25

# Keypad LCD label strings -> reading keys, normalized to UPPER + single-spaced.
# These three are the plausible temperature labels; live capture will confirm
# the exact strings and surface any aliases (e.g. "WATER TEMP").
_LABELS = {
    "AIR TEMP": "air_temp",
    "POOL TEMP": "pool_temp",
    "SPA TEMP": "spa_temp",
}

_LEADING_INT = re.compile(r"\s*(-?\d+)")


class DisplayLine:
    __slots__ = ("line", "text", "dest")

    def __init__(self, line: int, text: str, dest: int = 0x33):
        self.line = line
        self.text = text
        self.dest = dest

    def __repr__(self):
        return (
            f"DisplayLine(dest=0x{self.dest:02X}, "
            f"line=0x{self.line:02X}, text={self.text!r})"
        )


def decode_display(frame: Frame):
    """Decode a cmd-0x25 frame to a DisplayLine, or None if it is not one.

    data[0] is the LCD line/attribute byte; data[1:] is a NUL-terminated string.
    We keep only printable ASCII, dropping the NUL and the high-byte degree
    glyph, so the text is clean for label matching and number parsing.
    """
    if frame.cmd != CMD_DISPLAY or len(frame.data) < 1:
        return None
    text = "".join(chr(b) for b in frame.data[1:] if 0x20 <= b <= 0x7E)
    return DisplayLine(frame.data[0], text, frame.dest)


def _normalize(text: str) -> str:
    return " ".join(text.upper().split())


def parse_value(text: str):
    """Leading signed integer in `text`, or None. '167' -> 167, 'Air Temp' -> None."""
    m = _LEADING_INT.match(text)
    return int(m.group(1)) if m else None


class DisplayReader:
    """Pairs each known label with the value on the next display line."""

    def __init__(self):
        self.readings = {}
        self._pending = None

    def feed(self, frame: Frame):
        line = decode_display(frame)
        if line is None:
            return  # poll / binary status -> leave the pending label intact
        key = _LABELS.get(_normalize(line.text))
        if key is not None:
            self._pending = key
            return
        if self._pending is not None:
            value = parse_value(line.text)
            if value is not None:
                self.readings[self._pending] = value
            self._pending = None  # move off the screen either way; never mispair
