"""Small helpers shared by the MicroPython app and the CPython host tests."""

try:
    from time import ticks_ms, ticks_diff, ticks_add
except ImportError:          # CPython (host tests)
    import time as _time

    def ticks_ms():
        return int(_time.monotonic() * 1000)

    def ticks_diff(a, b):
        return a - b

    def ticks_add(a, b):
        return a + b


class RecentIds:
    """Bounded set of recently seen message IDs, for MeatNet de-duplication."""

    def __init__(self, size=128):
        self.size = size
        self._order = []
        self._set = set()

    def seen(self, msg_id):
        """True if msg_id was already recorded; records it otherwise."""
        if msg_id in self._set:
            return True
        self._set.add(msg_id)
        self._order.append(msg_id)
        if len(self._order) > self.size:
            self._set.discard(self._order.pop(0))
        return False
