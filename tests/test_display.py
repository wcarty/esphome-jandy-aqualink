"""Display-text layer: decode cmd 0x25 lines and pair labels with values.

This mirrors how AqualinkD reads the AllButton keypad LCD: the panel writes a
label line ("Air Temp") then a value line ("167"), and we pair them. The bus
interleaves polls between those writes, so pairing must survive that.
"""

import unittest

from jandy.frames import FrameExtractor
from jandy.display import decode_display, DisplayReader
from tests import fixtures as fx


def frame(raw):
    return FrameExtractor().feed(raw)[0]


class TestFixtureBuilder(unittest.TestCase):
    """The synthetic builder must reproduce the real captures byte-for-byte."""

    def test_builder_reproduces_air_label(self):
        self.assertEqual(fx.display_frame(0x05, "Air Temp"), fx.DISPLAY_AIR_LABEL)

    def test_builder_reproduces_air_value(self):
        self.assertEqual(
            fx.display_frame(0x01, "167", degree=True), fx.DISPLAY_AIR_VALUE
        )


class TestDecodeDisplayLine(unittest.TestCase):
    def test_decodes_label_text(self):
        line = decode_display(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(line.line, 0x05)
        self.assertEqual(line.text, "Air Temp")

    def test_decodes_value_stripping_degree_and_nul(self):
        line = decode_display(frame(fx.DISPLAY_AIR_VALUE))
        self.assertEqual(line.line, 0x01)
        self.assertEqual(line.text, "167")

    def test_reports_dest_iaqualink(self):
        """Every captured display frame on this panel is addressed to 0x33.

        AqualinkD's program reader needs display text at an AllButton keypad
        address instead, so callers must be able to tell the two apart rather
        than assuming.
        """
        line = decode_display(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(line.dest, 0x33)

    def test_reports_dest_allbutton(self):
        line = decode_display(frame(fx.display_frame(0x05, "REVIEW", dest=0x08)))
        self.assertEqual(line.dest, 0x08)

    def test_non_display_frame_returns_none(self):
        self.assertIsNone(decode_display(frame(fx.POLL_PUMP)))  # cmd 0x00


class TestDisplayReaderPairing(unittest.TestCase):
    def setUp(self):
        self.r = DisplayReader()

    def feed(self, raw):
        self.r.feed(frame(raw))

    def test_pairs_air_temp_label_then_value(self):
        self.feed(fx.DISPLAY_AIR_LABEL)
        self.feed(fx.DISPLAY_AIR_VALUE)
        self.assertEqual(self.r.readings["air_temp"], 167)

    def test_pairs_pool_and_spa_temps(self):
        self.feed(fx.display_frame(0x05, "Pool Temp"))
        self.feed(fx.display_frame(0x01, "85", degree=True))
        self.feed(fx.display_frame(0x05, "Spa Temp"))
        self.feed(fx.display_frame(0x01, "102", degree=True))
        self.assertEqual(self.r.readings["pool_temp"], 85)
        self.assertEqual(self.r.readings["spa_temp"], 102)

    def test_pairs_across_interleaved_poll_frames(self):
        self.feed(fx.DISPLAY_AIR_LABEL)
        self.feed(fx.POLL_PUMP)  # the bus interleaves polls between LCD writes
        self.feed(fx.POLL_58)
        self.feed(fx.DISPLAY_AIR_VALUE)
        self.assertEqual(self.r.readings["air_temp"], 167)

    def test_value_before_any_label_is_ignored(self):
        self.feed(fx.DISPLAY_AIR_VALUE)
        self.assertNotIn("air_temp", self.r.readings)

    def test_new_label_replaces_unmatched_pending(self):
        self.feed(fx.display_frame(0x05, "Pool Temp"))  # no value follows
        self.feed(fx.display_frame(0x05, "Air Temp"))
        self.feed(fx.display_frame(0x01, "167", degree=True))
        self.assertEqual(self.r.readings.get("air_temp"), 167)
        self.assertNotIn("pool_temp", self.r.readings)

    def test_intervening_nonvalue_line_prevents_mispairing(self):
        # label, then an unrelated screen, then a number -> must NOT record
        self.feed(fx.display_frame(0x05, "Air Temp"))
        self.feed(fx.display_frame(0x01, "Filter"))
        self.feed(fx.display_frame(0x01, "42"))
        self.assertNotIn("air_temp", self.r.readings)

    def test_unknown_label_does_not_record(self):
        self.feed(fx.display_frame(0x05, "Main Menu"))
        self.feed(fx.display_frame(0x01, "42"))
        self.assertEqual(self.r.readings, {})


if __name__ == "__main__":
    unittest.main()
