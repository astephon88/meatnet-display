import readings as r


def status():
    return {
        "probes": [{"serial": "1000ABCD", "id": 2, "color": 0, "mode": 0,
                    "core_c": 60.0, "surface_c": 70.0, "ambient_c": 120.0,
                    "instant_c": None, "temps": [60.0 + i for i in range(8)],
                    "low_battery": False, "src": "link", "hops": 0,
                    "rssi": -60, "age_s": 1, "alarming": False,
                    "prediction": {"state": 3, "type": 1, "seconds": 3900,
                                   "setpoint_c": 63.0}}],
        "devices": [{"serial": "GAUGE00001", "ptype": 3, "name": "Gauge",
                     "temp_c": 110.0, "rssi": -70, "age_s": 30}],
        "links": [], "inbound": 0,
    }


def test_formatting():
    assert r.fmt_temp(60.0, "c") == "60.0"
    assert r.fmt_temp(60.0, "f") == "140"
    assert r.fmt_temp(None, "c") == "--"
    assert r.fmt_eta(3900) == "1h05" and r.fmt_eta(300) == "5m"


def test_default_and_resolve_probe():
    s = status()
    rd = r.default_readings(s)
    assert rd == [{"dev": "probe:1000ABCD", "field": "core", "label": ""}]
    item = r.resolve(rd[0], s, "c")
    assert item["label"] == "P2 Core" and item["value"] == "60.0"
    assert item["unit"] == chr(176) + "C" and not item["stale"]
    assert any("Ready in 1h05" in x for x in item["subs"])
    eta = r.resolve({"dev": "probe:1000ABCD", "field": "eta"}, s, "c")
    assert eta["value"] == "1h05"
    t3 = r.resolve({"dev": "probe:1000ABCD", "field": "t3"}, s, "c")
    assert t3["value"] == "62.0"


def test_node_reading_and_stale():
    item = r.resolve({"dev": "node:GAUGE00001", "field": "temp"}, status(), "f")
    assert item["value"] == "230" and item["stale"] and item["label"] == "Gauge"


def test_missing_device():
    item = r.resolve({"dev": "probe:FFFFFFFF", "field": "core"}, status(), "c")
    assert item["value"] == "--" and item["subs"] == ["not found"]


def test_pages_layouts():
    s = status()
    cfg = {"readings": [{"dev": "probe:1000ABCD", "field": f} for f in
                        ("core", "surface", "ambient", "t1", "t2")]}
    items = r.build_items(cfg, s, "sta", "10.0.0.2")
    assert len(r.pages(cfg, items)) == 6
    cfg["layout"] = "quad"
    ps = r.pages(cfg, items)
    assert [len(p) for p in ps] == [4, 1, 1]
    ap = r.build_items({}, s, "ap", "192.168.4.1", "MeatNet-0001")
    assert ap[0].get("status") and "Join WiFi MeatNet-0001" in ap[0]["subs"]
