"""Binary status layer: extract values carried in non-text status frames.

Currently this is just the pool-water-temp byte in the cmd-0x0C status frame
to 0x38 (0x5B = 91 in the one captured sample). The byte offset is pinned to
that fixture and must be confirmed against the live bus (watch data[3] track
the real pool temp as it changes). Salt (SWG, device 0x50) has no fixture yet,
so it is intentionally not decoded here until a real frame is captured.
"""

import unittest

from jandy.frames import FrameExtractor, Frame
from jandy.status import decode_chemistry, decode_status, decode_keypad_status, SwgReader
from tests import fixtures as fx


def frame(raw):
    return FrameExtractor().feed(raw)[0]


def lframe(raw):
    """Build a Frame directly from a de-stuffed logical frame, bypassing the
    extractor, for keypad-status fixtures that carry a literal 0x10 data byte."""
    return Frame(raw)


class TestDecodeStatus(unittest.TestCase):
    def test_decodes_pool_temp_from_0x0c_status_frame(self):
        self.assertEqual(decode_status(frame(fx.STATUS_38_TEMP)), {"pool_temp": 91})

    def test_poll_frame_yields_nothing(self):
        self.assertEqual(decode_status(frame(fx.POLL_PUMP)), {})

    def test_display_frame_yields_nothing(self):
        self.assertEqual(decode_status(frame(fx.DISPLAY_AIR_LABEL)), {})

    def test_other_binary_status_yields_nothing(self):
        # cmd 0x28 to 0x33 is a different status message we don't decode yet
        self.assertEqual(decode_status(frame(fx.STATUS_33_BIN)), {})


class TestKeypadStatus(unittest.TestCase):
    def test_baseline_all_off_pump_on(self):
        s = decode_keypad_status(lframe(fx.STATUS_08_BASELINE))
        self.assertFalse(s["air_blower"])
        self.assertFalse(s["cleaner"])
        self.assertFalse(s["spa_mode"])
        self.assertTrue(s["filter_pump"])

    def test_spa_mode_sets_only_spa_bit(self):
        s = decode_keypad_status(lframe(fx.STATUS_08_SPA_ON))
        self.assertTrue(s["spa_mode"])
        self.assertFalse(s["air_blower"])
        self.assertFalse(s["cleaner"])

    def test_air_blower_sets_only_blower_bit(self):
        s = decode_keypad_status(lframe(fx.STATUS_08_BLOWER_ON))
        self.assertTrue(s["air_blower"])
        self.assertFalse(s["spa_mode"])
        self.assertFalse(s["cleaner"])

    def test_cleaner_sets_only_cleaner_bit(self):
        s = decode_keypad_status(lframe(fx.STATUS_08_CLEANER_ON))
        self.assertTrue(s["cleaner"])
        self.assertFalse(s["air_blower"])
        self.assertFalse(s["spa_mode"])

    def test_non_status_frame_returns_empty(self):
        self.assertEqual(decode_keypad_status(frame(fx.POLL_PUMP)), {})


class TestSwgReader(unittest.TestCase):
    def test_decodes_output_salt_and_generating_state(self):
        reader = SwgReader()
        for raw in (fx.SWG_PERCENT_85, fx.POLL_SWG, fx.SWG_PPM_ON):
            reader.feed(frame(raw))
        self.assertEqual(reader.state["salt_chlorinator_output"], 85)
        self.assertEqual(reader.state["salt_level"], 5600)
        self.assertEqual(reader.state["salt_chlorinator_status"], "on")
        self.assertTrue(reader.state["salt_chlorinator_generating"])

    def test_reports_fault_status_and_not_generating(self):
        reader = SwgReader()
        for raw in (fx.SWG_PERCENT_85, fx.POLL_SWG, fx.SWG_PPM_NO_FLOW):
            reader.feed(frame(raw))
        self.assertEqual(reader.state["salt_chlorinator_status"], "no_flow")
        self.assertFalse(reader.state["salt_chlorinator_generating"])


class TestChemistry(unittest.TestCase):
    def test_decodes_truesense_ph_and_orp(self):
        self.assertEqual(decode_chemistry(frame(fx.CHEMISTRY_PH_ORP)), {"orp": 650, "ph": 7.5})


if __name__ == "__main__":
    unittest.main()
