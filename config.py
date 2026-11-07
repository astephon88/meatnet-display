"""config.json: defaults, load/merge, save."""

import json

PATH = "config.json"

# Waveshare ESP32-S3-LCD-1.28 (non-touch). Other boards (incl. the Touch
# variant): check the schematic and set them in the web UI or config.json.
DEFAULT_PINS = {"sck": 10, "mosi": 11, "dc": 8, "cs": 9, "rst": 12, "bl": 40,
                "spi": 2, "baud": 80000000, "rotation": 0}

DEFAULTS = {
    "role": "standalone",       # "standalone", "app" (phone side) or "field"
                                # (device side) of a two-unit setup
    "split_peer": "",           # field unit: app unit host/IP, blank = beacon
    "split_key": "",            # shared key both units must have
    "split_port": 7389,
    "wifi_ssid": "",
    "wifi_password": "",
    "ap_password": "",          # blank = open setup access point
    "hostname": "meatnet",
    "serial": "",               # blank = derived from the chip MAC
    "dis_model": "Timer REL-00001",
    "dis_firmware": "v2.6.0",   # DIS firmware revision reported to the app
    "unit": "c",
    "layout": "rotate",         # "rotate" or "quad"
    "rotate_s": 5,
    "brightness": 80,
    "screen": True,             # drive the GC9A01A (off: unit runs headless)
    "status_page": True,
    "readings": [],             # [{"dev": "probe:1000ABCD", "field": "core", "label": ""}]
    "linked_probes": [],        # probe serials (hex) to hold an outbound link to
    "upstream_node": [],        # node serials to hold a link to (Gauge/Engine/...)
    "repeat_adverts": "all",    # "all", "linked" or "off"
    "meatnet_peer": True,       # heartbeats, link tables, device connected msgs
    "upstream_push": False,     # also push our status/heartbeats/syncs and
                                # association to the upstream node (a Gauge
                                # drops nodes that do this)
    "max_conn": 4,              # NimBLE connection limit of the firmware
    "linked_scan_duty": 20,     # % of the time scanning while links are up
    "inbound_reserve": 1,       # connections kept free for phones
    "meater_bridge": False,     # publish selected MEATER Pro probes on MeatNet
    "meater_probes": [],
    "meater_direct_adverts": True,  # also advertise bridged MEATERs as a direct
                                    # (non-connectable) probe so apps discover them        # [{"addr": "AA:BB:CC:DD:EE:FF", "id": 8}]
    "standby": False,           # manual standby (persisted across reboots)
    "auto_standby_min": 0,      # 0 = off; standby after N min without probes
    "battery_pin": 1,           # ADC pin of the battery divider, -1 = none
    "battery_divider": 3.0,     # V(bat) / V(pin): 200k/100k on the Waveshare
    "pins": DEFAULT_PINS,
}


def load(path=None):
    path = path or PATH
    cfg = {}
    for k, v in DEFAULTS.items():
        cfg[k] = dict(v) if isinstance(v, dict) else (list(v) if isinstance(v, list) else v)
    try:
        with open(path) as f:
            saved = json.load(f)
        for k, v in saved.items():
            if k == "pins" and isinstance(v, dict):
                cfg["pins"].update(v)
            elif k == "upstream_node" and isinstance(v, str):
                cfg[k] = [v] if v else []     # was a single serial
            else:
                cfg[k] = v
    except Exception:
        pass
    return cfg


def save(cfg, path=None):
    path = path or PATH       # looked up at call time: the host units set PATH
    try:
        with open(path, "w") as f:
            json.dump(cfg, f)
        return True
    except Exception as exc:
        print("[config] save failed:", exc)
        return False
