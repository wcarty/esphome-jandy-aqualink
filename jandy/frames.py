"""Jandy Aqualink RS-485 frame layer: extraction, de-stuffing, checksum.

Wire format:
    [optional 0x00 ...] 10 02 <dest> <cmd> <data...> <cksum> 10 03
    0x10 = DLE, 0x02 = STX, 0x03 = ETX.

Byte-stuffing: a 0x10 inside the payload is sent as "10 00". On receive, a 0x00
immediately following a non-marker 0x10 is dropped and the 0x10 kept as a
literal data byte.

Checksum: sum(logical_frame[:-3]) & 0xFF, i.e. every byte from the leading 0x10
up to but not including the checksum byte. Verified against 107/107 live frames.

FrameExtractor is a deliberately tiny per-byte state machine so it ports
straight to an ESP-IDF C ISR/task. feed() is incremental: state persists across
calls, so a frame split across TCP/UART reads still assembles cleanly.
"""

DLE = 0x10
STX = 0x02
ETX = 0x03
STUFF = 0x00

# Extractor states.
_SEARCH = 0   # outside a frame, hunting for DLE then STX
_DLE_OUT = 1  # saw DLE while searching, expecting STX next
_IN = 2       # inside a frame, collecting bytes
_DLE_IN = 3   # inside a frame, saw DLE, classifying the next byte


class Frame:
    """An un-stuffed logical frame: 10 02 dest cmd data... cksum 10 03."""

    __slots__ = ("raw",)

    def __init__(self, raw):
        self.raw = bytes(raw)

    @property
    def dest(self) -> int:
        return self.raw[2]

    @property
    def cmd(self) -> int:
        return self.raw[3]

    @property
    def data(self) -> bytes:
        return self.raw[4:-3]

    @property
    def checksum(self) -> int:
        return self.raw[-3]

    def checksum_valid(self) -> bool:
        return (sum(self.raw[:-3]) & 0xFF) == self.raw[-3]

    def __eq__(self, other):
        return isinstance(other, Frame) and other.raw == self.raw

    def __repr__(self):
        return f"Frame({self.raw.hex(' ')})"


class FrameExtractor:
    """Streaming extractor. feed(bytes) -> list[Frame] of complete frames."""

    def __init__(self):
        self._state = _SEARCH
        self._buf = bytearray()

    def feed(self, data) -> list:
        out = []
        for b in data:
            st = self._state
            if st == _IN:
                if b == DLE:
                    self._state = _DLE_IN
                else:
                    self._buf.append(b)
            elif st == _DLE_IN:
                if b == ETX:
                    self._buf.append(DLE)
                    self._buf.append(ETX)
                    out.append(Frame(self._buf))
                    self._buf = bytearray()
                    self._state = _SEARCH
                elif b == STUFF:
                    self._buf.append(DLE)  # de-stuff: literal data 0x10
                    self._state = _IN
                elif b == STX:
                    self._buf = bytearray((DLE, STX))  # fresh STX, resync
                    self._state = _IN
                else:
                    self._buf = bytearray()  # DLE + junk, abandon and resync
                    self._state = _SEARCH
            elif st == _SEARCH:
                if b == DLE:
                    self._state = _DLE_OUT
                # else: idle/lead byte (e.g. 0x00) or garbage -> ignore
            else:  # _DLE_OUT
                if b == STX:
                    self._buf = bytearray((DLE, STX))
                    self._state = _IN
                elif b == DLE:
                    self._state = _DLE_OUT  # consecutive DLEs, keep waiting
                else:
                    self._state = _SEARCH  # DLE not followed by STX
        return out


# --- Phase 2 keypress layer: the ACK that carries a button press ------------
#
# A registered AllButton keypad answers each poll with an ACK. The pending-key
# byte in that ACK is the press: 0x00 means "no key" (inert presence), and a
# keycode there tells the panel a button was pressed, which makes it redraw and
# push display text. ACK shape: 10 02 00 01 80 <key> <cksum> 10 03, where cmd
# 0x01 = CMD_ACK and ack_type 0x80 = ACK_ALLB_SIM (Jandy's AllButton simulator
# ack). Checksum is the usual sum over the logical frame up to the checksum.
ACK_ALLB_SIM = 0x80   # AllButton keypad simulator ack type
ACK_IAQ_TOUCH = 0x00  # iAqualink (Aqualink Touch) device ack type


def build_ack(ack_type: int, key: int) -> bytes:
    """Build a 9-byte ACK: 10 02 00 01 <ack_type> <key> <cksum> 10 03.

    cmd 0x01 = CMD_ACK. ack_type selects the emulated device family
    (ACK_ALLB_SIM 0x80 for AllButton, ACK_IAQ_TOUCH 0x00 for iAqualink). key is
    the pending button (0x00 = none). The result is checksum-valid.
    """
    body = bytes([DLE, STX, 0x00, 0x01, ack_type & 0xFF, key & 0xFF])
    cksum = sum(body) & 0xFF
    return body + bytes([cksum, DLE, ETX])


def build_ack_wire(ack_type: int, key: int) -> bytes:
    """Build a wire-ready ACK, stuffing DLE key and checksum bytes."""
    logical = build_ack(ack_type, key)
    out = bytearray(logical[:5])
    for byte in logical[5:7]:
        out.append(byte)
        if byte == DLE:
            out.append(STUFF)
    return bytes(out) + logical[7:]


# iAqualink Touch presence ACK (inert: ack_type 0x00, no key). Sending this in
# reply to every frame the panel addresses to the iAqualink device (0x33) makes
# the panel run its startup and push display pages, which carry the temperatures.
ACK_IAQ_PRESENCE = build_ack(ACK_IAQ_TOUCH, 0x00)  # 10 02 00 01 00 00 13 10 03

# iAqualink HOME-page equipment keycodes. These are home-button presses (the
# panel maps home button index N to KEY_IAQTCH_KEY0(N+1)). They are specific to
# THIS panel's home layout, confirmed from its captured 0x24 button frames:
# index 0 Filter Pump, 1 Spa (pool/spa valve toggle), 2 Pool Heat, 3 Spa Heat,
# 6 Pool Light.
KEY_IAQ_FILTER_PUMP = 0x11  # home button 0
KEY_IAQ_SPA = 0x12          # home button 1 (toggles spa mode / valves)
KEY_IAQ_CLEANER = 0x15      # home button 4
KEY_IAQ_AIR_BLOWER = 0x16   # home button 5
KEY_IAQ_POOL_LIGHT = 0x17   # home button 6

# Allowlist of iAqualink equipment keys this build will transmit. Deliberately
# EXCLUDES the heater buttons (Pool Heat 0x13, Spa Heat 0x14) and everything
# else, so a control press can never fire a heater.
_ALLOWED_IAQ_KEYS = frozenset(
    {KEY_IAQ_FILTER_PUMP, KEY_IAQ_SPA, KEY_IAQ_CLEANER, KEY_IAQ_AIR_BLOWER, KEY_IAQ_POOL_LIGHT}
)


def is_allowed_iaq_key(key: int) -> bool:
    """True only for the allowlisted iAqualink equipment keys (never a heater)."""
    return key in _ALLOWED_IAQ_KEYS


_SCHEDULER_SAFE_KEYS = frozenset({KEY_IAQ_FILTER_PUMP, KEY_IAQ_CLEANER})


def is_scheduler_safe_key(key: int) -> bool:
    """Keys the autonomous scheduler may send WITHOUT the master interlock: only
    Filter Pump (0x11) and Cleaner (0x15). These share the iaq_press allowlist with
    the spa toggle / blower / light, so the scheduler permission is scoped per-key,
    NOT by un-gating iaq_press wholesale."""
    return key in _SCHEDULER_SAFE_KEYS


# iAqualink global navigation keys (AqualinkD aq_serial.h KEY_IAQTCH_*). These are
# page-level keys that move the display and never actuate equipment, on ANY page:
# HOME 0x01, MENU 0x02, ONETOUCH 0x03, BACK 0x05, STATUS 0x06, PREV_PAGE 0x20,
# NEXT_PAGE 0x21. NOTE these values are iAqualink-protocol keycodes and are a
# different namespace from the AllButton KEY_* below (e.g. iAq 0x02 = MENU, while
# in the AllButton context 0x02 = pump). is_iaq_nav_key is only ever consulted on
# the iAqualink (0x33) path.
KEY_IAQT_HOME = 0x01
KEY_IAQT_MENU = 0x02
KEY_IAQT_ONETOUCH = 0x03
KEY_IAQT_BACK = 0x05
KEY_IAQT_STATUS = 0x06
KEY_IAQT_PREV_PAGE = 0x20
KEY_IAQT_NEXT_PAGE = 0x21
# Other Devices (home button 8). Only meaningful, and only safe, from the HOME
# page, so it is NOT in the nav set; the caller gates it to the HOME page.
KEY_IAQT_OTHER_DEVICES = 0x18

_IAQ_NAV_KEYS = frozenset(
    {
        KEY_IAQT_HOME,
        KEY_IAQT_MENU,
        KEY_IAQT_ONETOUCH,
        KEY_IAQT_BACK,
        KEY_IAQT_STATUS,
        KEY_IAQT_PREV_PAGE,
        KEY_IAQT_NEXT_PAGE,
    }
)


def is_iaq_nav_key(key: int) -> bool:
    """True only for global iAqualink navigation keys (safe on any page).

    Excludes 0x18 (Other Devices), which the caller gates to the HOME page, and
    every equipment/value keycode.
    """
    return key in _IAQ_NAV_KEYS

# Safe, display-only navigation keys (AqualinkD source/aq_serial.h). These move
# the menu/display and never actuate equipment, so they are the only keys this
# build will ever transmit. Equipment keys (pump 0x02, spa 0x01, pool heater
# 0x12, spa heater 0x17, aux* , override 0x1e, hold 0x19) are deliberately
# absent: there is no constant for them here and is_safe_nav_key refuses them.
KEY_MENU = 0x09
KEY_CANCEL = 0x0E
KEY_LEFT = 0x13
KEY_RIGHT = 0x18
KEY_ENTER = 0x1D

_SAFE_NAV_KEYS = frozenset({KEY_MENU, KEY_CANCEL, KEY_LEFT, KEY_RIGHT, KEY_ENTER})


def is_safe_nav_key(key: int) -> bool:
    """True only for display-only navigation keys (never equipment)."""
    return key in _SAFE_NAV_KEYS


def build_key_ack(key: int) -> bytes:
    """Build the 9-byte AllButton ACK carrying `key` in the pending-key slot.

    key=0x00 reproduces the proven inert presence ACK. The result is a
    self-consistent (checksum-valid) frame. For the allowlisted nav keys it
    contains no 0x10, so it can be written to the bus without byte-stuffing.
    """
    return build_ack(ACK_ALLB_SIM, key)


# --- Pump speed SET path ----------------------------------------------------
#
# Setting the VSP speed is a value-set command, not a keypress. The handshake
# (driven by the component state machine, confirmed against AqualinkD
# queue_iaqt_control_command): navigate to DEVICES, press the page-scoped
# VSP-adjust key (0x13), open SET_VSP, reply with the control-request ack
# (iaq_ctrl_ready_ack, key 0x80) on a poll, and when the panel sends
# CMD_IAQ_CTRL_READY (0x31) transmit the 0x24 value frame below.

# iAqualink page types we gate on (mirror of jandy/iaq.py and C++ jandy_proto.h).
IAQ_PAGE_HOME = 0x01
IAQ_PAGE_SET_VSP = 0x1E
IAQ_PAGE_STATUS2 = 0x2A
IAQ_PAGE_DEVICES = 0x36

# VSP-adjust keycode on the DEVICES page. Same byte as the home-page Pool Heat
# key (0x13): it means "VSP1 Spd ADJ" ONLY on DEVICES, so it is named separately
# to keep the page-gated intent explicit and never confused with a heater press.
KEY_IAQ_DEVICES_VSP_ADJ = 0x13

# Direct AllButton circuit keys, verified against AquaLinkD aq_serial.h. AUX6
# is a DLE byte and must be stuffed when serialized in an ACK.
KEY_AUX2 = 0x0A
KEY_AUX6 = 0x10


def is_direct_aux_key(key: int) -> bool:
    return key in {KEY_AUX2, KEY_AUX6}

# The pump's safe speed range (Pentair Intelliflo VS, RPM mode).
RPM_MIN = 600
RPM_MAX = 3450


def rpm_check(rpm: int) -> int:
    """Clamp to the pump's safe 600-3450 range and snap to the nearest 5 RPM."""
    rpm = max(RPM_MIN, min(RPM_MAX, int(rpm)))
    return ((rpm + 2) // 5) * 5


def num2iaqt_rpm(rpm: int) -> bytes:
    """ASCII digits of `rpm`, NUL-padded to a fixed 5-byte field (AqualinkD
    num2iaqtRSset, pad4unknownreason=True). Four-digit speeds get one trailing
    NUL, three-digit speeds get two."""
    digits = str(int(rpm)).encode("ascii")
    return digits + b"\x00" * (5 - len(digits))


_VSP_SET_CMD = 0x24          # value-set command frame cmd byte
_VSP_SET_SUBBYTE = 0x31      # literal sub-byte after the cmd
_VSP_SET_PAD = b"\xcd" * 11  # 0xcd padding out to logical index 18


def build_vsp_set_frame(rpm: int) -> bytes:
    """The 0x24 value frame that sets the VSP speed:
    10 02 00 24 31 <5-byte digit field> <eleven 0xcd> <cksum> 10 03.

    `rpm` is clamped and snapped first. No trailing NUL before the 0xcd run (the
    trailing-NUL form is a known VSP-drop footgun). For every valid speed the
    body contains no 0x10, so the logical frame is also the wire frame (no
    byte-stuffing needed), like the ACKs. Reproduces the captured frames exactly.
    """
    safe = rpm_check(rpm)
    body = bytes([DLE, STX, 0x00, _VSP_SET_CMD, _VSP_SET_SUBBYTE]) + num2iaqt_rpm(safe) + _VSP_SET_PAD
    cksum = sum(body) & 0xFF
    return body + bytes([cksum, DLE, ETX])


def iaq_ctrl_ready_ack() -> bytes:
    """The control-request reply, sent on an ordinary poll to ask the panel for
    the value-set control slot: an iAqualink ack carrying key 0x80
    (ACK_CMD_READY_CTRL), i.e. 10 02 00 01 00 80 93 10 03. The panel answers with
    CMD_IAQ_CTRL_READY (0x31), the cue to transmit build_vsp_set_frame."""
    return build_ack(ACK_IAQ_TOUCH, 0x80)


def vsp_adjust_allowed(current_page: int) -> bool:
    """True only on the DEVICES page (0x36). The VSP-adjust key (0x13) is Pool
    Heat on the HOME page, so sending it anywhere else could fire a heater. This
    is the central safety gate for the pump-set sequence."""
    return current_page == IAQ_PAGE_DEVICES


# --- DEVICES-page single-circuit toggles ------------------------------------
#
# Single on/off presses for the harmless DEVICES-page circuits (keycode =
# 0x11 + slot): slot 8 Spa Light, slot 12 Extra Aux, slot 13 Sprinklers. Like
# the VSP-adjust key these are page-scoped (the same byte means other equipment
# on HOME), so the caller must confirm page == DEVICES before pressing one.
KEY_IAQ_DEVICES_SPA_LIGHT = 0x19
KEY_IAQ_DEVICES_EXTRA_AUX = 0x1D
KEY_IAQ_DEVICES_SPRINKLERS = 0x1E

_DEVICE_TOGGLE_KEYS = frozenset(
    {KEY_IAQ_DEVICES_SPA_LIGHT, KEY_IAQ_DEVICES_EXTRA_AUX, KEY_IAQ_DEVICES_SPRINKLERS}
)


def is_device_toggle_allowed(key: int) -> bool:
    """True only for the allowlisted DEVICES-page toggle keys (Spa Light, Extra
    Aux, Sprinklers). Page-scoped: the caller must also confirm page == DEVICES."""
    return key in _DEVICE_TOGGLE_KEYS


# --- Heater on/off (HOME page) ----------------------------------------------
#
# Heaters are the highest-stakes control. HOME-page keycodes: Pool Heat 0x13,
# Spa Heat 0x14 (keycode 0x11 + home index 2/3). PAGE-SCOPED: 0x13 is the
# VSP-adjust on DEVICES and 0x14 is Pool Heat on DEVICES, so a heater key must
# ONLY ever be sent on HOME. Spa Heat additionally must never be enabled outside
# spa mode (water_mode 3). The panel runs the thermostat once a heater is enabled.
KEY_IAQ_HOME_POOL_HEAT = 0x13
KEY_IAQ_HOME_SPA_HEAT = 0x14  # 0x14 is page-scoped: Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU
WATER_MODE_SPA = 3

_HEATER_KEYS = frozenset({KEY_IAQ_HOME_POOL_HEAT, KEY_IAQ_HOME_SPA_HEAT})


def is_heater_key(key: int) -> bool:
    """True only for the two HOME-page heater keycodes (Pool Heat, Spa Heat)."""
    return key in _HEATER_KEYS


def heater_enable_allowed(key: int, current_page: int, water_mode: int) -> bool:
    """The full heater on/off safety gate. A heater key is honored ONLY on the HOME
    page (0x13/0x14 are other equipment elsewhere), and Spa Heat (0x14) ONLY while
    the panel is in spa mode (water_mode 3). Pool Heat is allowed on HOME in any mode."""
    if not is_heater_key(key):
        return False
    if current_page != IAQ_PAGE_HOME:
        return False
    if key == KEY_IAQ_HOME_SPA_HEAT and water_mode != WATER_MODE_SPA:
        return False
    return True


# --- Heater temperature setpoint (SET_TEMP page) ----------------------------
#
# Setting a heater target reuses the pump value-set handshake (the 0x80 control
# request then the 0x24 value frame), confirmed against AqualinkD
# queue_iaqt_control_command, which uses the SAME num2iaqtRSset encoder for both
# pump RPM and heater setpoint. The captured "Set Temp (pool)" frames in
# AqualinkD iaqtouch.h are the oracle (50F, 100F reproduce byte-for-byte).
#
# PAGE-SCOPED KEYCODES (the central safety trap): 0x14 is Spa Heat on HOME, Pool
# Heat on DEVICES, and Set Temp on MENU. Every keycode is gated to its page.
IAQ_PAGE_SET_TEMP = 0x39
IAQ_PAGE_MENU = 0x0F

# DEVICES-page heat items (keycode = 0x11 + slot): slot 3 Pool Heat, slot 4 Spa
# Heat. Pressed ONLY while page == DEVICES. The candidate "equipment route" onto
# SET_TEMP (confirmed by the survey).
KEY_IAQ_DEVICES_POOL_HEAT = 0x14  # 0x14 is page-scoped: Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU
KEY_IAQ_DEVICES_SPA_HEAT = 0x15
# MENU-page "Set Temp" key (AqualinkD KEY_IAQTCH_KEY04). Pressed ONLY while
# page == MENU. The AqualinkD route onto SET_TEMP (doubtful here; MENU is empty).
KEY_IAQT_SET_TEMP = 0x14  # 0x14 is page-scoped: Spa Heat on HOME, Pool Heat on DEVICES, Set Temp on MENU

# Heater target safe ranges (degrees F). Pool 45 (winter freeze low) to 90; spa
# 80 to 104 (heater hardware max). Refuse out of range by clamping.
POOL_TEMP_MIN, POOL_TEMP_MAX = 45, 90
SPA_TEMP_MIN, SPA_TEMP_MAX = 80, 104


def pool_setpoint_check(temp: int) -> int:
    """Clamp a pool target to the safe 45-90F range."""
    return max(POOL_TEMP_MIN, min(POOL_TEMP_MAX, int(temp)))


def spa_setpoint_check(temp: int) -> int:
    """Clamp a spa target to the safe 80-104F range."""
    return max(SPA_TEMP_MIN, min(SPA_TEMP_MAX, int(temp)))


def num2iaqt_temp(temp: int) -> bytes:
    """AqualinkD num2iaqtRSset for a temperature: ASCII digits, NUL-padded to a
    6-byte field, with a '0' (0x30) at index 4 for sub-1000 values. Reproduces
    the captured Set-Temp frames (50 -> 35 30 00 00 30 00, 100 -> 31 30 30 00 30 00)."""
    digits = str(int(temp)).encode("ascii")
    out = bytearray(6)
    out[: len(digits)] = digits
    for i in range(len(digits), 6):
        out[i] = 0x30 if (i == 4 and len(digits) <= 3) else 0x00
    return bytes(out)


_SETTEMP_PAD = b"\xcd" * 10  # 10 bytes (vs pump's 11) because the temp digit field is 6 bytes vs pump's 5, keeping the same 24-byte frame total


def build_settemp_frame(temp: int) -> bytes:
    """The 0x24 value frame that sets a heater target:
    10 02 00 24 31 <6-byte digit field> <ten 0xcd> <cksum> 10 03 (24 bytes).

    `temp` must already be clamped by the caller (pool vs spa ranges differ).
    Reproduces AqualinkD's captured Set-Temp(pool) frames for 50F and 100F."""
    body = bytes([DLE, STX, 0x00, _VSP_SET_CMD, _VSP_SET_SUBBYTE]) + num2iaqt_temp(temp) + _SETTEMP_PAD
    cksum = sum(body) & 0xFF
    return body + bytes([cksum, DLE, ETX])


def settemp_write_allowed(current_page: int) -> bool:
    """True only on the SET_TEMP page (0x39). The 0x24 value frame is transmitted
    only here; this is the central safety gate for the setpoint sequence."""
    return current_page == IAQ_PAGE_SET_TEMP
