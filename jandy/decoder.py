"""Top-level Jandy decoder: feed raw bus bytes, read back unified state.

Combines the frame extractor, the keypad display reader, and the binary status
decoder. Checksum-failing frames are counted and dropped so they can never
corrupt state. Alongside the decoded `state`, it records which (dest, cmd)
message types appear and the last raw frame of each, which is what we use to
learn the live bus (and later to pick a keypad address no real device answers).
"""

from collections import Counter

from .frames import FrameExtractor
from .display import DisplayReader
from .status import decode_status, SwgReader


class PoolDecoder:
    def __init__(self):
        self._extractor = FrameExtractor()
        self._display = DisplayReader()
        self._swg = SwgReader()
        self.state = {}
        self.stats = {"bytes": 0, "frames": 0, "bad_checksum": 0}
        self.message_counts = Counter()
        self.last_frame_by_type = {}

    def feed(self, data) -> list:
        """Consume raw bytes; update state/stats; return the frames extracted."""
        self.stats["bytes"] += len(data)
        frames = self._extractor.feed(data)
        for frame in frames:
            self.stats["frames"] += 1
            if not frame.checksum_valid():
                self.stats["bad_checksum"] += 1
                continue
            key = (frame.dest, frame.cmd)
            self.message_counts[key] += 1
            self.last_frame_by_type[key] = frame

            self._display.feed(frame)
            self._swg.feed(frame)
            self.state.update(self._display.readings)
            self.state.update(decode_status(frame))
            self.state.update(self._swg.state)
        return frames
