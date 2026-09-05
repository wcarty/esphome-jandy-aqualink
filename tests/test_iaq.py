"""iAqualink (Aqualink Touch) home-page decoder.

When we emulate the iAqualink device at 0x33, the panel pushes its HOME page as a
run of CMD_IAQ_PAGE_MSG (0x25) lines bracketed by CMD_IAQ_PAGE_START (0x23) and
CMD_IAQ_PAGE_END (0x28). Each 0x25 line has an index byte then NUL-terminated
text. The temperatures are laid out so a value line sits 4 indices before its
label line (AqualinkD source/iaqtouch.c: air = value[1] for label "Air Temp" at
index 5; pool/spa = value[0] for label at index 4). These fixtures are the exact
frames captured from the real RS panel (spa mode on): Spa 88, Air 156.
"""

import unittest

from jandy.iaq import IaqReader, iaq_page_name
from tests import fixtures as fx


# Real HOME-page frames captured 2026-05-31 with the panel in spa mode.
IAQ_PAGE_START_HOME = fx.h("10 02 33 23 01 69 10 03")          # page type 0x01 = HOME
IAQ_MSG_I2_EMPTY = fx.h("10 02 33 25 02 00 6C 10 03")
IAQ_MSG_I3_EMPTY = fx.h("10 02 33 25 03 00 6D 10 03")
IAQ_MSG_I1_156 = fx.h("10 02 33 25 01 31 35 36 C2 BA 00 83 10 03")  # "156" + degree
IAQ_MSG_I5_AIR = fx.h("10 02 33 25 05 41 69 72 20 54 65 6D 70 00 41 10 03")  # "Air Temp"
IAQ_MSG_I4_SPA = fx.h("10 02 33 25 04 53 70 61 20 54 65 6D 70 00 48 10 03")  # "Spa Temp"
IAQ_MSG_I0_88 = fx.h("10 02 33 25 00 38 38 C2 BA 00 56 10 03")     # "88" + degree
IAQ_PAGE_END = fx.h("10 02 33 28 05 1F 1A 08 1D D0 10 03")

# A pool-mode variant: same layout but index 4 label is "Pool Temp", value 90.
IAQ_MSG_I4_POOL = fx.h("10 02 33 25 04 50 6F 6F 6C 20 54 65 6D 70 00 BE 10 03")  # "Pool Temp"
IAQ_MSG_I0_90 = fx.h("10 02 33 25 00 39 30 C2 BA 00 4F 10 03")  # "90" + degree


def feed_frames(reader, *wire_frames):
    from jandy.frames import FrameExtractor

    for w in wire_frames:
        for frame in FrameExtractor().feed(w):
            reader.feed(frame)


class TestIaqHomePage(unittest.TestCase):
    def test_decodes_spa_and_air_from_real_home_page(self):
        r = IaqReader()
        feed_frames(
            r,
            IAQ_PAGE_START_HOME,
            IAQ_MSG_I2_EMPTY,
            IAQ_MSG_I3_EMPTY,
            IAQ_MSG_I1_156,
            IAQ_MSG_I5_AIR,
            IAQ_MSG_I4_SPA,
            IAQ_MSG_I0_88,
            IAQ_PAGE_END,
        )
        self.assertTrue(r.has_spa)
        self.assertEqual(r.spa, 88)
        self.assertTrue(r.has_air)
        self.assertEqual(r.air, 156)
        # Spa mode home page shows no pool temp.
        self.assertFalse(r.has_pool)

    def test_decodes_pool_when_label_is_pool_temp(self):
        r = IaqReader()
        feed_frames(
            r,
            IAQ_PAGE_START_HOME,
            IAQ_MSG_I1_156,
            IAQ_MSG_I5_AIR,
            IAQ_MSG_I4_POOL,
            IAQ_MSG_I0_90,
            IAQ_PAGE_END,
        )
        self.assertTrue(r.has_pool)
        self.assertEqual(r.pool, 90)
        self.assertEqual(r.air, 156)
        self.assertFalse(r.has_spa)

    def test_no_decode_until_page_end(self):
        r = IaqReader()
        feed_frames(r, IAQ_PAGE_START_HOME, IAQ_MSG_I4_SPA, IAQ_MSG_I0_88)
        # Page not ended yet; nothing committed.
        self.assertFalse(r.has_spa)

    def test_water_mode_tracks_the_current_home_page_label(self):
        # The "Switch to Pool Mode" control is gated on this: it must only fire
        # while the panel is actually in spa mode (index-4 label is "Spa Temp").
        r = IaqReader()
        feed_frames(
            r, IAQ_PAGE_START_HOME, IAQ_MSG_I1_156, IAQ_MSG_I5_AIR, IAQ_MSG_I4_SPA, IAQ_MSG_I0_88,
            IAQ_PAGE_END,
        )
        self.assertEqual(r.water_mode, 3)  # 3 = spa
        feed_frames(
            r, IAQ_PAGE_START_HOME, IAQ_MSG_I1_156, IAQ_MSG_I5_AIR, IAQ_MSG_I4_POOL, IAQ_MSG_I0_90,
            IAQ_PAGE_END,
        )
        self.assertEqual(r.water_mode, 2)  # 2 = pool

    def test_water_mode_starts_unknown(self):
        self.assertEqual(IaqReader().water_mode, 0)

    def test_decodes_pool_light_button_state(self):
        r = IaqReader()
        feed_frames(
            r,
            IAQ_PAGE_START_HOME,
            fx.iaq_button_frame(6, 3),
            IAQ_PAGE_END,
        )
        self.assertTrue(r.has_pool_light)
        self.assertTrue(r.pool_light_on)

        feed_frames(
            r,
            IAQ_PAGE_START_HOME,
            fx.iaq_button_frame(6, 0),
            IAQ_PAGE_END,
        )
        self.assertTrue(r.has_pool_light)
        self.assertFalse(r.pool_light_on)

    def test_non_home_page_does_not_set_temps(self):
        r = IaqReader()
        # Page type 0x36 = DEVICES, not HOME.
        feed_frames(
            r,
            fx.h("10 02 33 23 36 9E 10 03"),
            IAQ_MSG_I4_SPA,
            IAQ_MSG_I0_88,
            IAQ_PAGE_END,
        )
        self.assertFalse(r.has_spa)
        self.assertFalse(r.has_air)


class TestIaqCurrentPage(unittest.TestCase):
    def test_current_page_starts_zero(self):
        self.assertEqual(IaqReader().current_page, 0)

    def test_current_page_set_after_home_page_end(self):
        r = IaqReader()
        feed_frames(r, IAQ_PAGE_START_HOME, IAQ_MSG_I4_SPA, IAQ_MSG_I0_88, IAQ_PAGE_END)
        self.assertEqual(r.current_page, 0x01)

    def test_current_page_tracks_devices_page(self):
        r = IaqReader()
        feed_frames(r, fx.h("10 02 33 23 36 9E 10 03"), IAQ_PAGE_END)
        self.assertEqual(r.current_page, 0x36)

    def test_page_name_known_and_unknown(self):
        self.assertEqual(iaq_page_name(0x01), "HOME")
        self.assertEqual(iaq_page_name(0x36), "DEVICES")
        self.assertEqual(iaq_page_name(0x5B), "STATUS")
        self.assertEqual(iaq_page_name(0x1E), "SET_VSP")
        self.assertTrue(iaq_page_name(0x77).startswith("0x"))


if __name__ == "__main__":
    unittest.main()
