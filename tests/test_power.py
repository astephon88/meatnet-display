import power as pw
from meatnet import Node
from tests.test_meatnet import FakeTransport, hear_probe, PROBE


class StandbyTransport(FakeTransport):
    standby = False

    def set_standby(self, on):
        self.standby = on


class FakeADC:
    def __init__(self, uv):
        self.uv = uv

    def read_uv(self):
        return self.uv


def make(cfg=None, uv=1_300_000):
    t = StandbyTransport()
    cfg = cfg or {}
    n = Node(t, cfg, "ME00000001", log=lambda *a: None)
    p = pw.PowerManager(cfg, t, n, log=lambda *a: None, adc=FakeADC(uv))
    return p, n, t


def test_battery_percent_curve():
    assert pw.battery_percent(4.25) == 100
    assert pw.battery_percent(3.80) == 52
    assert pw.battery_percent(3.75) == 44
    assert pw.battery_percent(3.0) == 0


def test_battery_reading_through_divider():
    p, _, _ = make(uv=1_300_000)          # 1.3 V at the pin -> 3.9 V cell
    b = p.read_battery()
    assert b["present"] and b["v"] == 3.9 and b["pct"] == 66
    p, _, _ = make(uv=20_000)
    assert not p.read_battery()["present"]


def test_manual_standby_drops_links_and_persists():
    cfg = {"linked_probes": ["04030201"]}
    p, n, t = make(cfg)
    hear_probe(t)
    n.tick(now=0)
    assert PROBE in t.links
    p.enter("manual", persist=False)
    assert t.standby and n.standby
    n.tick(now=1000)
    assert t.links == {}
    p.tick(now=2000)                      # probe still heard: manual stays
    assert p.standby
    p.exit("manual")
    assert not t.standby and not n.standby and not cfg.get("standby")


def test_auto_standby_and_wake_on_probe():
    p, n, t = make({"auto_standby_min": 1})
    p._last_probe = 0
    p.tick(now=30_000)
    assert not p.standby
    p.tick(now=61_000)
    assert p.standby and p.reason == "auto"
    hear_probe(t, now=70_000)
    p.tick(now=71_000)
    assert not p.standby
