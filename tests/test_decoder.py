"""Top-level decoder: bytes in, unified state out, plus discovery stats.

Wires the frame, display, and status layers together. Drops frames that fail
their checksum (so a corrupt frame never updates state) and counts the
(dest, cmd) message types it sees, which is how we'll learn the live bus and
later find an unused keypad address.
"""

import unittest

from jandy.decoder import PoolDecoder
from tests import fixtures as fx


class TestPoolDecoder(unittest.TestCase):
    def test_decodes_air_and_pool_temp_from_stream(self):
        d = PoolDecoder()
        d.feed(fx.DISPLAY_AIR_LABEL + fx.DISPLAY_AIR_VALUE + fx.STATUS_38_TEMP)
        self.assertEqual(d.state["air_temp"], 167)
        self.assertEqual(d.state["pool_temp"], 91)

    def test_counts_messages_by_type(self):
        d = PoolDecoder()
        d.feed(fx.POLL_PUMP + fx.POLL_PUMP + fx.POLL_58)
        self.assertEqual(d.message_counts[(0x60, 0x00)], 2)
        self.assertEqual(d.message_counts[(0x58, 0x00)], 1)

    def test_tracks_checksum_failures(self):
        d = PoolDecoder()
        bad = bytearray(fx.POLL_PUMP)
        bad[4] ^= 0xFF  # mangle checksum byte
        d.feed(bytes(bad) + fx.POLL_58)
        self.assertEqual(d.stats["frames"], 2)
        self.assertEqual(d.stats["bad_checksum"], 1)

    def test_bad_frame_does_not_update_state_or_counts(self):
        d = PoolDecoder()
        bad = bytearray(fx.STATUS_38_TEMP)
        bad[8] ^= 0xFF  # corrupt the checksum byte of the temp status frame
        d.feed(bytes(bad))
        self.assertNotIn("pool_temp", d.state)
        self.assertEqual(d.message_counts.get((0x38, 0x0C), 0), 0)

    def test_feed_returns_extracted_frames(self):
        d = PoolDecoder()
        frames = d.feed(fx.POLL_PUMP + fx.POLL_58)
        self.assertEqual(len(frames), 2)

    def test_keeps_last_raw_sample_per_type(self):
        d = PoolDecoder()
        d.feed(fx.STATUS_38_TEMP)
        self.assertEqual(d.last_frame_by_type[(0x38, 0x0C)].raw, fx.STATUS_38_TEMP)

    def test_decodes_salt_chlorinator_exchange(self):
        d = PoolDecoder()
        d.feed(fx.SWG_PERCENT_85 + fx.POLL_SWG + fx.SWG_PPM_ON)
        self.assertEqual(d.state["salt_chlorinator_output"], 85)
        self.assertEqual(d.state["salt_level"], 5600)
        self.assertTrue(d.state["salt_chlorinator_generating"])


if __name__ == "__main__":
    unittest.main()
