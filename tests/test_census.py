"""Bus census: count frames by (dest, cmd) so we can prove which addresses the
panel actually talks to, and specifically whether display text ever reaches an
AllButton keypad address (0x08-0x0B) rather than only iAqualink (0x33)."""

import unittest

from jandy.frames import FrameExtractor
from jandy.census import BusCensus
from jandy.display import decode_display, CMD_DISPLAY
from tests import fixtures as fx


def frame(raw):
    return FrameExtractor().feed(raw)[0]


class TestBusCensus(unittest.TestCase):
    def test_counts_by_dest_and_cmd(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        c.feed(frame(fx.DISPLAY_AIR_VALUE))
        c.feed(frame(fx.POLL_PUMP))
        self.assertEqual(c.counts[(0x33, CMD_DISPLAY)], 2)
        self.assertEqual(c.counts[(0x60, 0x00)], 1)

    def test_dests_for_cmd(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(c.dests_for_cmd(CMD_DISPLAY), {0x33})

    def test_saw_display_at_is_false_for_allbutton_when_only_iaqualink_talks(self):
        c = BusCensus()
        c.feed(frame(fx.DISPLAY_AIR_LABEL))
        self.assertTrue(c.saw_display_at(0x33))
        self.assertFalse(c.saw_display_at(0x08))

    def test_saw_display_at_true_when_allbutton_gets_display(self):
        c = BusCensus()
        c.feed(frame(fx.display_frame(0x05, "REVIEW", dest=0x08)))
        self.assertTrue(c.saw_display_at(0x08))


class TestDisplayCarriesDest(unittest.TestCase):
    def test_decode_display_reports_dest(self):
        line = decode_display(frame(fx.DISPLAY_AIR_LABEL))
        self.assertEqual(line.dest, 0x33)

    def test_decode_display_reports_allbutton_dest(self):
        line = decode_display(frame(fx.display_frame(0x05, "REVIEW", dest=0x08)))
        self.assertEqual(line.dest, 0x08)
