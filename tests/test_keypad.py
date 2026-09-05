"""Phase 2 keypress layer: building the AllButton ACK that carries a key.

The panel pushes display text only in response to a keypad button press. A press
is delivered by putting a keycode in the pending-key byte of our ACK reply to a
poll, instead of the inert 0x00. These tests pin the exact wire bytes (a wrong
checksum or wrong key byte is an equipment-safety problem, since some keycodes
actuate the heater/pump/valves) and the safety allowlist that refuses any key
outside the display-only navigation set.

Expected ACK shape: 10 02 00 01 80 <key> <cksum> 10 03
  cmd 0x01 = CMD_ACK, ack_type 0x80 = ACK_ALLB_SIM, cksum = (0x93 + key) & 0xFF.
"""

import unittest

from jandy.frames import (
    FrameExtractor,
    build_ack,
    build_ack_wire,
    build_key_ack,
    is_direct_aux_key,
    is_safe_nav_key,
    ACK_IAQ_TOUCH,
    ACK_ALLB_SIM,
    KEY_MENU,
    KEY_CANCEL,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_ENTER,
    KEY_AUX2,
    KEY_AUX6,
)
from tests import fixtures as fx


class TestBuildKeyAck(unittest.TestCase):
    def test_menu_ack_exact_bytes(self):
        self.assertEqual(build_key_ack(KEY_MENU), fx.h("10 02 00 01 80 09 9C 10 03"))

    def test_each_safe_key_exact_bytes(self):
        self.assertEqual(build_key_ack(KEY_CANCEL), fx.h("10 02 00 01 80 0E A1 10 03"))
        self.assertEqual(build_key_ack(KEY_LEFT), fx.h("10 02 00 01 80 13 A6 10 03"))
        self.assertEqual(build_key_ack(KEY_RIGHT), fx.h("10 02 00 01 80 18 AB 10 03"))
        self.assertEqual(build_key_ack(KEY_ENTER), fx.h("10 02 00 01 80 1D B0 10 03"))

    def test_zero_key_reproduces_inert_presence_ack(self):
        # The proven v1 inert ACK; build_key_ack(0) must equal it exactly.
        self.assertEqual(build_key_ack(0x00), fx.h("10 02 00 01 80 00 93 10 03"))

    def test_key_ack_is_checksum_valid_and_well_formed(self):
        for key in (KEY_MENU, KEY_CANCEL, KEY_LEFT, KEY_RIGHT, KEY_ENTER):
            frame = FrameExtractor().feed(build_key_ack(key))[0]
            self.assertTrue(frame.checksum_valid(), f"key 0x{key:02X}")
            self.assertEqual(frame.dest, 0x00)
            self.assertEqual(frame.cmd, 0x01)

    def test_safe_key_acks_need_no_byte_stuffing(self):
        # No payload byte may equal 0x10 (DLE), or the raw write would corrupt
        # the frame (the task writes the ACK without stuffing).
        for key in (KEY_MENU, KEY_CANCEL, KEY_LEFT, KEY_RIGHT, KEY_ENTER):
            ack = build_key_ack(key)
            self.assertNotIn(0x10, ack[2:7], f"key 0x{key:02X} produces a 0x10 in payload")

    def test_direct_aux_keys_are_allowlisted_and_wire_encoded(self):
        self.assertTrue(is_direct_aux_key(KEY_AUX2))
        self.assertTrue(is_direct_aux_key(KEY_AUX6))
        self.assertFalse(is_direct_aux_key(0x05))
        self.assertEqual(
            build_ack_wire(ACK_ALLB_SIM, KEY_AUX2),
            fx.h("10 02 00 01 80 0A 9D 10 03"),
        )
        self.assertEqual(
            build_ack_wire(ACK_ALLB_SIM, KEY_AUX6),
            fx.h("10 02 00 01 80 10 00 A3 10 03"),
        )
        decoded = FrameExtractor().feed(build_ack_wire(ACK_ALLB_SIM, KEY_AUX6))
        self.assertEqual(len(decoded), 1)
        self.assertTrue(decoded[0].checksum_valid())


class TestBuildAck(unittest.TestCase):
    def test_iaqualink_inert_presence_ack(self):
        # iAqualink Touch presence ACK (AqualinkD ACK_IAQ_TOUCH): ack_type 0x00,
        # no key, checksum 0x13. This is what makes the panel push display pages.
        self.assertEqual(build_ack(ACK_IAQ_TOUCH, 0x00), fx.h("10 02 00 01 00 00 13 10 03"))

    def test_allbutton_inert_ack_via_build_ack(self):
        self.assertEqual(build_ack(ACK_ALLB_SIM, 0x00), fx.h("10 02 00 01 80 00 93 10 03"))

    def test_build_key_ack_is_allbutton_build_ack(self):
        # build_key_ack must be exactly build_ack with the AllButton ack type.
        self.assertEqual(build_key_ack(KEY_MENU), build_ack(ACK_ALLB_SIM, KEY_MENU))

    def test_ack_constants(self):
        self.assertEqual(ACK_IAQ_TOUCH, 0x00)
        self.assertEqual(ACK_ALLB_SIM, 0x80)


class TestIaqEquipmentAllowlist(unittest.TestCase):
    def test_allowed_iaq_equipment_keys(self):
        from jandy.frames import (
            is_allowed_iaq_key,
            KEY_IAQ_FILTER_PUMP,
            KEY_IAQ_SPA,
            KEY_IAQ_CLEANER,
            KEY_IAQ_AIR_BLOWER,
            KEY_IAQ_POOL_LIGHT,
        )

        for k in (KEY_IAQ_FILTER_PUMP, KEY_IAQ_SPA, KEY_IAQ_CLEANER, KEY_IAQ_AIR_BLOWER, KEY_IAQ_POOL_LIGHT):
            self.assertTrue(is_allowed_iaq_key(k), f"0x{k:02X} should be allowed")
        self.assertEqual(
            (KEY_IAQ_FILTER_PUMP, KEY_IAQ_SPA, KEY_IAQ_CLEANER, KEY_IAQ_AIR_BLOWER, KEY_IAQ_POOL_LIGHT),
            (0x11, 0x12, 0x15, 0x16, 0x17),
        )

    def test_iaq_heater_and_other_keys_refused(self):
        from jandy.frames import is_allowed_iaq_key

        # 0x13 = Pool Heat, 0x14 = Spa Heat (home buttons 2,3) MUST stay refused,
        # plus any non-allowlisted code.
        for k in (0x13, 0x14, 0x18, 0x00, 0x09, 0x1D):
            self.assertFalse(is_allowed_iaq_key(k), f"0x{k:02X} must not be allowed")

    def test_scheduler_safe_keys_are_only_pump_and_cleaner(self):
        from jandy.frames import is_scheduler_safe_key

        # The autonomous scheduler may send ONLY Filter Pump (0x11) and Cleaner (0x15)
        # without the master interlock; everything else stays interlock-only.
        self.assertTrue(is_scheduler_safe_key(0x11))
        self.assertTrue(is_scheduler_safe_key(0x15))
        for k in (0x12, 0x16, 0x17, 0x13, 0x14, 0x00):
            self.assertFalse(is_scheduler_safe_key(k), f"0x{k:02X} must not be scheduler-safe")


class TestSafeNavAllowlist(unittest.TestCase):
    def test_navigation_keys_are_allowed(self):
        for key in (KEY_MENU, KEY_CANCEL, KEY_LEFT, KEY_RIGHT, KEY_ENTER):
            self.assertTrue(is_safe_nav_key(key), f"key 0x{key:02X} should be safe")

    def test_equipment_keys_are_refused(self):
        # pump, spa, pool heater, spa heater, override, hold, aux1, aux2, aux3
        for key in (0x02, 0x01, 0x12, 0x17, 0x1E, 0x19, 0x05, 0x0A, 0x0F):
            self.assertFalse(is_safe_nav_key(key), f"key 0x{key:02X} must not be safe")


if __name__ == "__main__":
    unittest.main()
