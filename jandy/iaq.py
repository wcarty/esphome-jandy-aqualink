"""iAqualink (Aqualink Touch) page decoder.

The panel pushes display pages to the emulated iAqualink device (0x33) as a run
of page-message lines (CMD_IAQ_PAGE_MSG 0x25) bracketed by page start (0x23) and
page end (0x28). The page start carries the page type (0x01 = HOME). Each 0x25
line is: index byte, then NUL-terminated text (temperature values carry a
trailing degree glyph 0xC2 0xBA which we drop).

On the HOME page the temperatures sit at a fixed offset from their labels: the
value line is 4 indices before the label line. Confirmed against AqualinkD
source/iaqtouch.c (air = value[1] for "Air Temp" at index 5; pool/spa = value[0]
for the label at index 4) and against real frames from this RS panel.
"""

CMD_IAQ_PAGE_START = 0x23
CMD_IAQ_PAGE_BUTTON = 0x24
CMD_IAQ_PAGE_MSG = 0x25
CMD_IAQ_PAGE_END = 0x28
IAQ_PAGE_HOME = 0x01
IAQ_PAGE_DEVICES = 0x36
IAQ_PAGE_STATUS = 0x5B
IAQ_PAGE_SET_VSP = 0x1E

# A temperature value sits this many indices before its label line.
_VALUE_OFFSET = 4

# HOME-page heater button indices (this panel's home layout): Pool Heat, Spa Heat.
_BTN_POOL_HEAT = 2
_BTN_SPA_HEAT = 3
_BTN_POOL_LIGHT = 6

# Human names for iAqualink page types (AqualinkD aq_serial.h IAQ_PAGE_*), used
# for legible survey logging and for gating navigation by current page.
_PAGE_NAMES = {
    0x01: "HOME",
    0x0A: "DEVICES_REV",
    0x0F: "MENU",
    0x1D: "SET_BOOST",
    0x1E: "SET_VSP",
    0x2A: "STATUS2",
    0x2D: "VSP_SETUP",
    0x30: "SET_SWG",
    0x35: "DEVICES2",
    0x36: "DEVICES",
    0x39: "SET_TEMP",
    0x48: "COLOR_LIGHT",
    0x4B: "SET_TIME",
    0x4D: "ONETOUCH",
    0x51: "DEVICES3",
    0x5B: "STATUS",
}


def iaq_page_name(page_type: int) -> str:
    """Human name for an iAqualink page type, or 0xNN if unknown."""
    return _PAGE_NAMES.get(page_type, f"0x{page_type:02X}")


def _norm_label(text: str) -> str:
    """Upper-case, collapse internal whitespace to one space, trim ends."""
    out = []
    prev_space = True
    for ch in text:
        u = ch.upper()
        if u in (" ", "\t"):
            if not prev_space:
                out.append(" ")
                prev_space = True
        else:
            out.append(u)
            prev_space = False
    while out and out[-1] == " ":
        out.pop()
    return "".join(out)


def _leading_int(text: str):
    s = text.strip()
    i = 0
    neg = False
    if i < len(s) and s[i] == "-":
        neg = True
        i += 1
    j = i
    while j < len(s) and s[j].isdigit():
        j += 1
    if j == i:
        return None
    v = int(s[i:j])
    return -v if neg else v


class IaqReader:
    """Accumulates iAqualink HOME-page lines and decodes air/pool/spa temps."""

    def __init__(self):
        self.air = 0
        self.pool = 0
        self.spa = 0
        self.has_air = False
        self.has_pool = False
        self.has_spa = False
        self.water_mode = 0  # current home-page water label: 0 none, 2 pool, 3 spa
        self.pool_heat_enabled = False
        self.spa_heat_enabled = False
        self.has_pool_heat = False
        self.has_spa_heat = False
        self.pool_light_on = False
        self.has_pool_light = False
        self._btn_state = {}  # index -> state, for the page being loaded
        self.current_page = 0  # page type of the most recently completed page
        self.pump_rpm = 0
        self.has_pump_rpm = False
        self.pump_watts = 0
        self.has_pump_watts = False
        self._page_type = 0
        self._lines = {}  # index -> text

    def feed(self, frame):
        cmd = frame.cmd
        if cmd == CMD_IAQ_PAGE_START:
            data = frame.data
            self._page_type = data[0] if len(data) >= 1 else 0
            self._lines = {}
            self._btn_state = {}
        elif cmd == CMD_IAQ_PAGE_MSG:
            data = frame.data
            if len(data) < 1:
                return
            idx = data[0]
            text = "".join(chr(b) for b in data[1:] if 0x20 <= b <= 0x7E)
            self._lines[idx] = text
        elif cmd == CMD_IAQ_PAGE_BUTTON:
            data = frame.data
            if len(data) >= 2:
                self._btn_state[data[0]] = data[1]
        elif cmd == CMD_IAQ_PAGE_END:
            self.current_page = self._page_type  # promote the displayed page
            if self._page_type == IAQ_PAGE_HOME:
                self._commit_home()
            elif self._page_type in (0x2A, 0x5B):  # STATUS2 / STATUS
                self._commit_status()
            self._lines = {}
            self._btn_state = {}

    def _commit_home(self):
        for idx, text in self._lines.items():
            label = _norm_label(text)
            if label not in ("AIR TEMP", "POOL TEMP", "SPA TEMP", "WATER TEMP"):
                continue
            value_text = self._lines.get(idx - _VALUE_OFFSET)
            if value_text is None:
                continue
            val = _leading_int(value_text)
            if val is None:
                continue
            if label == "AIR TEMP":
                self.air = val
                self.has_air = True
            elif label in ("POOL TEMP", "WATER TEMP"):
                self.pool = val
                self.has_pool = True
                self.water_mode = 2
            elif label == "SPA TEMP":
                self.spa = val
                self.has_spa = True
                self.water_mode = 3
        # HOME heater buttons: state 3 = on.
        if _BTN_POOL_HEAT in self._btn_state:
            self.pool_heat_enabled = self._btn_state[_BTN_POOL_HEAT] == 3
            self.has_pool_heat = True
        if _BTN_SPA_HEAT in self._btn_state:
            self.spa_heat_enabled = self._btn_state[_BTN_SPA_HEAT] == 3
            self.has_spa_heat = True
        if _BTN_POOL_LIGHT in self._btn_state:
            self.pool_light_on = self._btn_state[_BTN_POOL_LIGHT] == 3
            self.has_pool_light = True

    def _commit_status(self):
        # The STATUS page lists the pump as text lines: "    RPM: 2750" and
        # "  Watts: 1263". Find each and parse the trailing integer.
        for text in self._lines.values():
            up = text.upper()
            if "RPM:" in up:
                v = _leading_int(text[up.index("RPM:") + 4:])
                if v is not None:
                    self.pump_rpm = v
                    self.has_pump_rpm = True
            if "WATTS:" in up:
                v = _leading_int(text[up.index("WATTS:") + 6:])
                if v is not None:
                    self.pump_watts = v
                    self.has_pump_watts = True


class IaqButton:
    """One enumerated button from an iAqualink page (a 0x24 frame)."""

    __slots__ = ("index", "state", "type", "segments")

    def __init__(self, index, state, type_, segments):
        self.index = index
        self.state = state
        self.type = type_
        self.segments = segments

    def __repr__(self):
        return f"IaqButton(i={self.index} state=0x{self.state:02X} type=0x{self.type:02X} {self.segments})"


def parse_iaq_button(frame):
    """Parse a 0x24 button frame, or return None if it is not one.

    Layout after cmd: data[0]=index, [1]=state, [2]=unknown, [3]=type, then
    NUL-separated printable text segments (label words plus a state token). The
    segments are returned as-is; the survey reads which is label vs state by eye.
    """
    if frame.cmd != CMD_IAQ_PAGE_BUTTON:
        return None
    d = frame.data
    if len(d) < 4:
        return None
    segments = []
    cur = []
    for b in d[4:]:
        if b == 0x00:
            if cur:
                segments.append("".join(cur))
                cur = []
        elif 0x20 <= b <= 0x7E:
            cur.append(chr(b))
    if cur:
        segments.append("".join(cur))
    return IaqButton(d[0], d[1], d[3], segments)
