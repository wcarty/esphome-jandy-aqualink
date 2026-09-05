"""Bus census: which addresses does the panel actually talk to, and with what.

Counting (dest, cmd) pairs is how we answer "does display text ever reach an
AllButton keypad address" without eyeballing a log. Cheap enough to run on
every frame.
"""

from .frames import Frame
from .display import CMD_DISPLAY

ALLBUTTON_ADDRESSES = (0x08, 0x09, 0x0A, 0x0B)


class BusCensus:
    """Counts frames by (dest, cmd)."""

    def __init__(self):
        self.counts = {}

    def feed(self, frame: Frame) -> None:
        key = (frame.dest, frame.cmd)
        self.counts[key] = self.counts.get(key, 0) + 1

    def dests_for_cmd(self, cmd: int) -> set:
        return {dest for (dest, c) in self.counts if c == cmd}

    def saw_display_at(self, dest: int) -> bool:
        return self.counts.get((dest, CMD_DISPLAY), 0) > 0

    def saw_display_at_allbutton(self) -> bool:
        return any(self.saw_display_at(a) for a in ALLBUTTON_ADDRESSES)

    def summary(self) -> str:
        lines = ["dest cmd  count"]
        for (dest, cmd), n in sorted(self.counts.items()):
            lines.append(f"0x{dest:02X} 0x{cmd:02X} {n:6d}")
        return "\n".join(lines)
