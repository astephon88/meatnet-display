"""Turn configured readings + live MeatNet data into display items.

Pure Python (no hardware), shared by screen.py and the web UI, and tested on
the host. A reading is {"dev": "probe:<serial>" | "node:<serial>",
"field": <field>, "label": optional text}.
"""

PROBE_FIELDS = (
    ("core", "Core"), ("surface", "Surface"), ("ambient", "Ambient"),
    ("instant", "Instant"), ("eta", "Time left"),
    ("t1", "T1"), ("t2", "T2"), ("t3", "T3"), ("t4", "T4"),
    ("t5", "T5"), ("t6", "T6"), ("t7", "T7"), ("t8", "T8"),
)
NODE_FIELDS = {3: (("temp", "Temp"),), 6: (("setpoint", "Set point"),)}

STALE_S = 15

# Probe accent colours by probe ID 1-8 (RGB).
ACCENTS = ((255, 196, 0), (64, 160, 255), (80, 220, 100), (255, 96, 96),
           (200, 120, 255), (0, 210, 210), (255, 150, 40), (220, 220, 220))


def to_unit(c, unit):
    return c * 9 / 5 + 32 if unit == "f" else c


def fmt_temp(c, unit):
    """Big-value text: one decimal below 100, whole degrees above."""
    if c is None:
        return "--"
    v = to_unit(c, unit)
    if abs(v) >= 100:
        return "%d" % round(v)
    return "%.1f" % v


def fmt_short(c, unit):
    return "--" if c is None else "%d" % round(to_unit(c, unit))


def fmt_eta(seconds):
    if seconds is None:
        return "--"
    m = int(seconds) // 60
    if m >= 60:
        return "%dh%02d" % (m // 60, m % 60)
    return "%dm" % m


def field_label(field):
    for k, v in PROBE_FIELDS:
        if k == field:
            return v
    for fields in NODE_FIELDS.values():
        for k, v in fields:
            if k == field:
                return v
    return field


def default_readings(status):
    """One reading per probe heard, core temperature."""
    out = []
    for p in sorted(status["probes"], key=lambda p: (p["id"], p["serial"])):
        out.append({"dev": "probe:" + p["serial"], "field": "core", "label": ""})
    return out


def _find(status, dev):
    kind, _, serial = dev.partition(":")
    if kind == "probe":
        for p in status["probes"]:
            if p["serial"] == serial:
                return "probe", p
    else:
        for d in status["devices"]:
            if d["serial"] == serial:
                return "node", d
    return kind, None


def _predicting(p):
    pr = p.get("prediction")
    return pr if pr and pr.get("state") == 3 and pr.get("type") in (1, 2) else None


def resolve(reading, status, unit):
    """-> item dict: label, value, unit, subs [text], stale, accent, alarm."""
    kind, d = _find(status, reading.get("dev", ""))
    field = reading.get("field", "core")
    label = reading.get("label") or ""
    item = {"label": label, "value": "--", "unit": "", "subs": [],
            "stale": True, "accent": (200, 200, 200), "alarm": False,
            "short": "--"}
    u = "F" if unit == "f" else "C"
    deg = chr(176)
    if d is None:
        item["label"] = label or field_label(field)
        item["subs"] = ["not found"]
        return item
    item["stale"] = d.get("age_s", 0) > STALE_S
    if kind == "probe":
        pid = d.get("id", 1)
        item["accent"] = ACCENTS[(pid - 1) % 8]
        item["alarm"] = bool(d.get("alarming"))
        if not label:
            label = "P%d %s" % (pid, field_label(field))
        if field == "eta":
            pr = _predicting(d)
            item["value"] = fmt_eta(pr["seconds"]) if pr else "--"
            item["short"] = item["value"]
            if pr:
                item["subs"].append("to %s%s%s" % (fmt_short(pr["setpoint_c"], unit), deg, u))
        else:
            if field in ("core", "surface", "ambient", "instant"):
                c = d.get(field + "_c")
                if field == "core" and c is None and d.get("instant_c") is not None:
                    c = d["instant_c"]          # Instant Read: show it on core
                    label = reading.get("label") or "P%d Instant" % pid
            else:
                i = int(field[1:]) - 1
                temps = d.get("temps") or []
                c = temps[i] if 0 <= i < len(temps) else None
            item["value"] = fmt_temp(c, unit)
            item["short"] = fmt_short(c, unit)
            item["unit"] = deg + u if c is not None else ""
            if field == "core":
                if d.get("surface_c") is not None:
                    item["subs"].append("Surf %s%s  Amb %s%s" % (
                        fmt_short(d["surface_c"], unit), deg,
                        fmt_short(d.get("ambient_c"), unit), deg))
                pr = _predicting(d)
                if pr:
                    item["subs"].append("Ready in %s" % fmt_eta(pr["seconds"]))
        src = {"link": "linked", "advert": "direct", "node": "via node",
               "repeated": "repeated", "meater": "MEATER"}.get(
                   d.get("src"), d.get("src", ""))
        info = src
        if d.get("rssi") is not None and d.get("src") in ("link", "advert", "meater"):
            info += " %ddBm" % d["rssi"]
        if d.get("low_battery"):
            item["subs"].append("LOW BATTERY")
        item["subs"].append(info)
    else:
        name = d.get("name", "Node")
        if not label:
            label = "%s %s" % (name, field_label(field)) if field != "temp" else name
        if field == "temp":
            c = d.get("temp_c")
        elif field == "setpoint":
            c = d.get("setpoint_c")
        else:
            c = None
        item["value"] = fmt_temp(c, unit)
        item["short"] = fmt_short(c, unit)
        item["unit"] = deg + u if c is not None else ""
        item["subs"].append("%s %ddBm" % (d["serial"], d.get("rssi", 0)))
    if item["stale"]:
        item["subs"].append("stale %ds" % d.get("age_s", 0))
    item["label"] = label
    return item


def status_item(status, wifi_mode, ip, ap_ssid):
    subs = []
    if status.get("status_lines"):            # field unit of a two-unit setup
        subs = list(status["status_lines"])
        if ip and wifi_mode != "ap":
            subs.insert(0, ip)
        elif wifi_mode == "ap":
            subs[0:0] = ["Join WiFi " + ap_ssid, "http://%s/" % ip]
        return {"label": status.get("status_title", "MeatNet"), "value": "",
                "unit": "", "subs": subs, "stale": False,
                "accent": (120, 170, 255), "alarm": False, "short": "",
                "status": True}
    if wifi_mode == "ap":
        subs.append("Join WiFi " + ap_ssid)
        subs.append("http://%s/" % ip)
    elif ip:
        subs.append(ip)
    else:
        subs.append("WiFi " + (wifi_mode or "off"))
    links = [l for l in status["links"] if l["state"] == "ready"]
    subs.append("phones %d  links %d" % (status["inbound"], len(links)))
    subs.append("probes heard %d" % len(status["probes"]))
    bat = status.get("battery")
    if bat and bat.get("present"):
        subs.append("battery %.2fV %d%%" % (bat["v"], bat["pct"]))
    return {"label": "MeatNet", "value": "", "unit": "", "subs": subs,
            "stale": False, "accent": (120, 170, 255), "alarm": False,
            "short": "", "status": True}


def build_items(cfg, status, wifi_mode=None, ip=None, ap_ssid=""):
    readings = cfg.get("readings") or default_readings(status)
    unit = cfg.get("unit", "c")
    items = [resolve(r, status, unit) for r in readings]
    if wifi_mode == "ap" or cfg.get("status_page", True) or not items:
        st = status_item(status, wifi_mode, ip, ap_ssid)
        if wifi_mode == "ap":
            items.insert(0, st)
        else:
            items.append(st)
    return items


def pages(cfg, items):
    """Group items into pages for the configured layout."""
    if cfg.get("layout") == "quad":
        temps = [i for i in items if not i.get("status")]
        other = [i for i in items if i.get("status")]
        out = [temps[k:k + 4] for k in range(0, len(temps), 4)]
        return out + [[i] for i in other]
    return [[i] for i in items]
