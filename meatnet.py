"""MeatNet node logic for the Display/repeater (MicroPython + CPython).

The Node sits between the BLE transport (ble.BleTransport, or a fake in the
host tests) and the rest of the app:

- decodes scan observations into a probe table and a device table;
- keeps outbound links to the probes selected in the web UI (and optionally
  to one upstream node), within the BLE connection budget;
- answers / routes MeatNet UART requests from phones and peer nodes:
  probe list, node list, topology, probe-addressed commands (translated to
  the probe's own UART format), log reads, silence alarms, link tables;
- floods network messages (0x45 status, heartbeats, link notifications)
  between sides with request-ID de-duplication and hop counting;
- builds the advertising rotation (own Display info + repeated probes);
- bridges selected MEATER Pro probes: holds a link to each and publishes
  it as a Combustion probe in synthesized repeated-probe advertisements.

Message sources/destinations are "ports": ("in", conn_handle) for inbound
connections and ("link", key) for our outbound node links.
"""

import struct

import meater
import protocol
from util import ticks_ms, ticks_diff, RecentIds

PROBE_STALE_MS = 30000        # forget a probe not heard for this long
NODE_STALE_MS = 60000
FRESH_MS = 5000               # a source younger than this is "live"
PENDING_TIMEOUT_MS = 6000     # probe command without a response
LOG_IDLE_MS = 5000            # log read with no record for this long is done
LOG_FIRST_MS = 20000          # ...but a probe takes ~10 s to start streaming
BUSY_TIMEOUT_MS = 20000       # other commands wait behind a log read this long
MAX_LOG_READS = 4             # concurrent log reads kept per probe
STRAY_LOG_MS = 30000          # rate limit for "no request waiting" messages
HEARTBEAT_MS = 5000
SYNC_LIST_MS = 30000
FWD_TIMEOUT_MS = 15000
TABLE_PULL_MS = 30 * 60000    # pull a peer node's link table at most this often
MEATER_FRESH_MS = 10000       # stop advertising a MEATER this long after
                              # its last temperature notification
MEATER_SAMPLE_MS = 5000       # log period of a bridged MEATER (like a probe)
MEATER_LOG_MAX = 4320         # records kept per MEATER (6 h at 5 s)
LOG_JOB_BURST = 20            # log responses queued per service() call
TXQ_HIGH = 150                # ...while the transport's queue is below this


def _unix_time():
    """Seconds since 1970 if the clock has been set (NTP), else 0 ("not
    available" in link-table timestamps)."""
    try:
        import time
        t = int(time.time())
        if time.gmtime(0)[0] == 2000:       # MicroPython ports with a 2000 epoch
            t += 946684800
        return t if t > 1600000000 else 0
    except Exception:
        return 0


class Node:
    def __init__(self, transport, cfg, serial, log=print, links=None,
                 clock=None):
        self.t = transport
        self.cfg = cfg
        self.serial = serial
        self.log = log
        self.links_table = links
        self.ids = RecentIds(192)
        self.probes = {}          # serial4 -> merged reading dict
        self.devices = {}         # serial10 -> decoded node advert dict
        self.link_status = {}     # serial4 -> (status bytes, ticks)
        self.via_node = {}        # serial4 -> (status, ticks, hops, port)
        self.slots = [None] * 10  # probe device numbers (index + 1)
        self.node_numbers = {}    # serial10 bytes -> device number (>= 21)
        self.pending = []         # probe commands awaiting a response
        self.fwd = {}             # request id -> (port, ticks) we forwarded
        self.my_reqs = {}         # request id -> (purpose, port, extra)
        self.peer_serial = {}     # port -> peer node serial10 (heartbeats)
        self.conn_buf = {}        # inbound conn -> partial frame bytes
        self.link_buf = {}        # link key -> partial frame bytes
        self.link_state = {}      # link key -> last reported state
        self._table_pulled = {}   # peer node key -> ticks of the last table pull
        self.meater_data = {}     # MEATER addr -> {"temps", "at", "battery"}
        self.meater_sess = {}     # MEATER addr -> session/log/status (below)
        self.log_jobs = []        # paced log-read replies for bridged MEATERs
        self.standby = False      # set by power.PowerManager
        self._hb_at = None
        self._hb_inbound = False
        self._sync_at = None
        self.stats = {"rx": 0, "tx": 0, "routed": 0, "status_sent": 0}
        # Traffic the Display relays between phones and node links, by
        # message type: shows whether the app reaches a node through us.
        self.via = {"app_to_node": {}, "node_to_app": {}}
        self._now = ticks_ms()    # time of the current tick()/service() pass

    # -- config helpers ---------------------------------------------------------

    def wanted_probes(self):
        out = []
        for s in self.cfg.get("linked_probes", []):
            try:
                out.append(protocol.probe_serial_bytes(s))
            except Exception:
                pass
        return out

    def upstream_serial(self):
        s = self.cfg.get("upstream_node") or ""
        return protocol.encode_serial(s) if s else None

    def peer_features(self):
        return self.cfg.get("meatnet_peer", True)

    def push_upstream(self):
        """Send our own network traffic (status, heartbeats, syncs, link
        notices, association) to the upstream node too. Off by default: a
        Giant Grill Gauge drops a node that writes to it unprompted, and the
        app gets the node's data without it."""
        return self.peer_features() and self.cfg.get("upstream_push", False)

    def meater_enabled(self):
        return bool(self.cfg.get("meater_bridge"))

    def meater_selected(self):
        """addr bytes -> settings dict for the MEATERs to bridge."""
        out = {}
        if not self.meater_enabled():
            return out
        for e in self.cfg.get("meater_probes", []):
            try:
                out[meater.addr_bytes(e["addr"])] = e
            except Exception:
                pass
        return out

    def link_budget(self):
        """Outbound probe + MEATER links we may hold."""
        n = self.t.max_conn
        if not getattr(self.t, "separate_radios", False):
            # One radio: keep connections free for phones.
            n -= int(self.cfg.get("inbound_reserve", 1))
        if self.upstream_serial():
            n -= 1
        return max(0, n)

    # -- ports ------------------------------------------------------------------

    def _node_links(self):
        return [(k, l) for k, l in self.t.links.items()
                if l.kind == "node" and l.ready]

    def _probe_link(self, serial4):
        link = self.t.links.get(bytes(serial4))
        if link is not None and link.kind == "probe" and link.ready:
            return link
        return None

    def _count_via(self, direction, frame, key=None):
        d = self.via[direction]
        mt = frame[4]
        name = "0x%02x%s" % (mt & 0x7F, " rsp" if mt & 0x80 else "")
        first = name not in d
        d[name] = d.get(name, 0) + 1
        if first and direction == "app_to_node":
            self.log("[route] app reached %s through the Display (0x%s)" % (
                protocol.serial10_str(key) if key else "a node", name[2:]))

    def _send(self, port, frame):
        if port[0] == "in":
            self.t.send(frame, (port[1],))
        else:
            link = self.t.links.get(port[1])
            if link is not None and link.ready:
                link.write(frame)
        self.stats["tx"] += 1

    def _flood(self, frame, origin=None, inbound=True, links=None):
        """Send to every port except the one the message came from.
        links=None: node links only when push_upstream(); True: always (a
        request a phone addressed to the network)."""
        if links is None:
            links = self.push_upstream()
        if inbound:
            targets = [c for c in self.t.inbound
                       if origin != ("in", c)]
            if targets:
                self.t.send(frame, targets)
                if origin is not None and origin[0] == "link":
                    self._count_via("node_to_app", frame)
        if links:
            for key, link in self._node_links():
                if origin != ("link", key):
                    link.write(frame)
                    if origin is not None and origin[0] == "in":
                        self._count_via("app_to_node", frame, key)

    # -- periodic work ------------------------------------------------------------

    def tick(self, now=None):
        """1 Hz housekeeping."""
        now = ticks_ms() if now is None else now
        self._now = now
        if hasattr(self.t, "set_scan_active"):
            self.t.set_scan_active(self.meater_enabled() and not self.standby)
        self._refresh(now)
        if self.standby:
            # Drop every link so probes' connection slots are free; keep the
            # tables fresh for the display/web UI and auto-wake.
            for key in list(self.t.links):
                self.t.close_link(key)
            self.pending = []
            return
        self._manage_links(now)
        self._update_adverts(now)
        self._expire(now)
        if self.peer_features():
            if self._hb_at is None or ticks_diff(now, self._hb_at) >= HEARTBEAT_MS:
                self._hb_at = now
                self._send_heartbeat()
            if (self.push_upstream() and self._node_links() and (self._sync_at is None or
                                        ticks_diff(now, self._sync_at) >= SYNC_LIST_MS)):
                self._sync_at = now
                self._flood(protocol.build_sync_thermometer_list(
                    self.t.mac, self.slots[:4]), inbound=False)

    def service(self, now=None):
        """Fast path (~10 Hz): incoming frames and link data."""
        now = ticks_ms() if now is None else now
        self._now = now
        for conn, data in self.t.poll_rx():
            buf = self.conn_buf.get(conn, b"") + bytes(data)
            frames, rest = protocol.split_frames(buf)
            self.conn_buf[conn] = rest[-512:]
            for f in frames:
                self.stats["rx"] += 1
                self._on_frame(("in", conn), f, now)
        for conn in list(self.conn_buf):
            if conn not in self.t.inbound:
                del self.conn_buf[conn]
                self.peer_serial.pop(("in", conn), None)
        for key, link in list(self.t.links.items()):
            self._service_link(key, link, now)
        if self.log_jobs:
            self._service_log_jobs()

    def _service_link(self, key, link, now):
        state = link.state
        if self.link_state.get(key) != state:
            prev = self.link_state.get(key)
            self.link_state[key] = state
            name = self._link_name(key, link)
            self.log("[link] %s %s: %s" % (link.kind, name, state))
            if link.kind == "meater" and self.peer_features():
                if state == "ready" or prev == "ready":
                    self._flood(protocol.build_device_connected(
                        state == "ready", protocol.PRODUCT_TYPE_PROBE,
                        meater.serial_for(key)))
            if link.kind == "probe" and self.peer_features():
                if state == "ready":
                    self._flood(protocol.build_device_connected(
                        True, protocol.PRODUCT_TYPE_PROBE, key))
                elif prev == "ready":
                    self._flood(protocol.build_device_connected(
                        False, protocol.PRODUCT_TYPE_PROBE, key))
            if link.kind == "node" and state == "ready":
                self._on_node_link_ready(key)
        if not link.ready:
            return
        if link.kind == "meater":
            temps = link.take("temp")
            if temps:
                t6 = meater.decode_temps(temps[-1])
                if t6:
                    d = self.meater_data.setdefault(key, {"battery": None})
                    d["temps"] = t6
                    d["at"] = now
            bat = link.take("battery")
            if bat and bat[-1]:
                self.meater_data.setdefault(key, {"battery": None})["battery"] = bat[-1][0]
            if temps:
                self._meater_update(key, now)
            return
        if link.kind == "probe":
            statuses = link.take("status")
            if statuses:
                status = statuses[-1]
                if len(status) >= 48:
                    self.link_status[key] = (status, now)
                    self._assign_slot(key)
                    frame = protocol.build_node_probe_status(key, status, 1)
                    f_id = struct.unpack_from("<I", frame, 5)[0]
                    self.ids.seen(f_id)
                    self._flood(frame)
                    self.stats["status_sent"] += 1
            chunks = link.take("uart")
            if chunks:
                buf = self.link_buf.get(key, b"") + b"".join(chunks)
                frames, rest = protocol.split_probe_frames(buf)
                self.link_buf[key] = rest[-256:]
                for mtype, success, payload in frames:
                    self._on_probe_response(key, mtype, success, payload, now)
        elif link.kind == "node":
            chunks = link.take("uart")
            if chunks:
                buf = self.link_buf.get(key, b"") + b"".join(chunks)
                frames, rest = protocol.split_frames(buf)
                self.link_buf[key] = rest[-512:]
                for f in frames:
                    self.stats["rx"] += 1
                    self._on_frame(("link", key), f, now)

    @staticmethod
    def _link_name(key, link):
        if link.kind == "probe":
            return protocol.probe_serial_str(key)
        if link.kind == "meater":
            return meater.addr_str(key)
        return protocol.serial10_str(key)

    def meater_reading(self, addr, now):
        """Synthesized Combustion view of a bridged MEATER, or None if its
        data is missing/stale. -> (serial4, msd advert, decoded dict, age)."""
        d = self.meater_data.get(addr)
        sel = self.meater_selected().get(addr)
        if not d or "temps" not in d or sel is None:
            return None
        age = ticks_diff(now, d["at"])
        if age > MEATER_FRESH_MS:
            return None
        t6 = d["temps"]
        serial4 = meater.serial_for(addr)
        bat = d.get("battery")
        msd, _core = meater.build_advert(
            serial4, meater.map_sensors(t6[:5], t6[5]),
            probe_id=int(sel.get("id", 8)),
            low_battery=bat is not None and bat < meater.LOW_BATTERY_PCT)
        decoded = protocol.parse_probe_advert(msd[2:])
        return serial4, msd, decoded, age

    # -- bridged MEATERs as this node's probes ---------------------------------------

    def _meater_fields(self, addr):
        d = self.meater_data[addr]
        sel = self.meater_selected().get(addr, {})
        t6 = d["temps"]
        bat = d.get("battery")
        return meater.encode_fields(
            meater.map_sensors(t6[:5], t6[5]), probe_id=int(sel.get("id", 8)),
            color=int(sel.get("color", 0)),
            low_battery=bat is not None and bat < meater.LOW_BATTERY_PCT)

    def _meater_direct(self, addr):
        d = self.meater_data[addr]
        sel = self.meater_selected().get(addr, {})
        t6 = d["temps"]
        bat = d.get("battery")
        return meater.build_direct_advert(
            meater.serial_for(addr), meater.map_sensors(t6[:5], t6[5]),
            probe_id=int(sel.get("id", 8)), color=int(sel.get("color", 0)),
            low_battery=bat is not None and bat < meater.LOW_BATTERY_PCT)

    def _meater_update(self, addr, now):
        """New MEATER reading: log it (every MEATER_SAMPLE_MS), rebuild its
        Probe Status and send it to the network as 0x45."""
        if addr not in self.meater_selected() or "temps" not in self.meater_data.get(addr, {}):
            return
        sess = self.meater_sess.get(addr)
        if sess is None:
            sess = self.meater_sess[addr] = {
                "serial": meater.serial_for(addr),
                "session": protocol.new_message_id() | 1,   # 0 = "not supplied"
                "base": 0, "log": [], "next": now, "status": None, "at": now}
            self._meater_link(sess["serial"])
        fields = self._meater_fields(addr)
        if ticks_diff(now, sess["next"]) >= 0:
            sess["log"].append(meater.log_record(fields))
            if len(sess["log"]) > MEATER_LOG_MAX:
                sess["log"].pop(0)
                sess["base"] += 1
            sess["next"] = now + MEATER_SAMPLE_MS
        log_max = sess["base"] + max(0, len(sess["log"]) - 1)
        sess["status"] = meater.build_status(fields, sess["base"], log_max,
                                             sess["session"], MEATER_SAMPLE_MS)
        sess["at"] = now
        serial4 = sess["serial"]
        self._assign_slot(serial4)
        frame = protocol.build_node_probe_status(serial4, sess["status"], 1)
        self.ids.seen(struct.unpack_from("<I", frame, 5)[0])
        self._flood(frame)
        self.stats["status_sent"] += 1

    def _meater_for_serial(self, serial4, now=None):
        """addr of the bridged MEATER published as serial4 (fresh data only)."""
        now = self._now if now is None else now
        for addr, sess in self.meater_sess.items():
            if (sess["serial"] == bytes(serial4) and addr in self.meater_selected()
                    and ticks_diff(now, sess["at"]) <= MEATER_FRESH_MS):
                return addr
        return None

    def _meater_request(self, port, f, addr, probe_payload, now):
        """Answer a probe-addressed request for a bridged MEATER ourselves."""
        sess = self.meater_sess[addr]
        serial4 = sess["serial"]
        mt = f.msg_type
        if mt == protocol.MSG_READ_SESSION_INFO:
            self._send(port, protocol.build_response(
                mt, f.request_id, True, serial4 + struct.pack(
                    "<IH", sess["session"], MEATER_SAMPLE_MS)))
        elif mt == protocol.MSG_READ_LOGS and len(probe_payload) >= 8:
            start, end = struct.unpack_from("<II", probe_payload, 0)
            first = max(start, sess["base"])
            last = min(end, sess["base"] + len(sess["log"]) - 1)
            self.log_jobs = [j for j in self.log_jobs
                             if not (j["addr"] == addr and j["port"] == port)]
            if first <= last:
                self.log_jobs.append({"addr": addr, "port": port, "req": f.request_id,
                                      "next": first, "end": last})
            else:
                self._send(port, protocol.build_response(mt, f.request_id, False, b""))
        elif mt == protocol.MSG_READ_OVER_TEMPERATURE:
            self._send(port, protocol.build_response(mt, f.request_id, True,
                                                     serial4 + b"\x00"))
        elif mt in (protocol.MSG_SET_PROBE_ID, protocol.MSG_SET_PROBE_COLOR) \
                and probe_payload:
            # Keep the app's choice in the bridge settings.
            key = "id" if mt == protocol.MSG_SET_PROBE_ID else "color"
            val = probe_payload[0] + (1 if key == "id" else 0)
            for e in self.cfg.get("meater_probes", []):
                if e.get("addr") == meater.addr_str(addr):
                    e[key] = val
            self._send(port, protocol.build_response(mt, f.request_id, True, b""))
            self._meater_update(addr, now)
        else:
            # Predictions, alarms, food safe, power mode, reset: a MEATER
            # can't do these through the Combustion protocol.
            self._send(port, protocol.build_response(mt, f.request_id, False, b""))
        self.stats["routed"] += 1

    _INFO_FIELDS = {protocol.MSG_PROBE_FIRMWARE_REVISION: ("firmware", 20),
                    protocol.MSG_PROBE_HARDWARE_REVISION: ("hardware", 16),
                    protocol.MSG_PROBE_MODEL_INFORMATION: ("model", 50)}

    def _probe_device_info(self, port, f):
        """Answer 0x46-0x48 for a probe we hold a link to, from the Device
        Information strings the link read when it connected."""
        serial4 = bytes(f.payload[0:4])
        link = self._probe_link(serial4)
        name, size = self._INFO_FIELDS[f.msg_type]
        text = (getattr(link, "info", None) or {}).get(name) if link else None
        if text is None:
            return False
        raw = text.encode()[:size]
        self._send(port, protocol.build_response(
            f.msg_type, f.request_id, True, serial4 + raw + b"\x00" * (size - len(raw))))
        self.stats["routed"] += 1
        return True

    def _meater_device_info(self, port, f, addr):
        """Firmware (0x46), hardware (0x47) and model info (0x48) for a
        bridged MEATER: serial + fixed-size, zero-padded string."""
        mt = f.msg_type
        if mt == protocol.MSG_PROBE_FIRMWARE_REVISION:
            text, size = meater.DIS_FIRMWARE, 20
        elif mt == protocol.MSG_PROBE_HARDWARE_REVISION:
            text, size = meater.DIS_HARDWARE, 16
        else:
            text, size = meater.DIS_MODEL, 50
        raw = text.encode()[:size]
        self._send(port, protocol.build_response(
            mt, f.request_id, True,
            self.meater_sess[addr]["serial"] + raw + b"\x00" * (size - len(raw))))
        self.stats["routed"] += 1

    def _meater_link(self, serial4):
        """Record a bridged MEATER as a linked MeatNet device and announce it
        (Device Linked, 0x77), the way a node links a probe."""
        if self.links_table is None:
            return
        entry = {"ptype": protocol.PRODUCT_TYPE_PROBE, "serial": bytes(serial4),
                 "ts": _unix_time(), "legacy": False}
        if self.links_table.link(entry):
            note = protocol.build_link_notification(True, entry)
            self.ids.seen(struct.unpack_from("<I", note, 5)[0])
            self._flood(note)
            self.log("[meater] %s linked to MeatNet" % protocol.probe_serial_str(serial4))

    def _service_log_jobs(self):
        budget = LOG_JOB_BURST
        for job in list(self.log_jobs):
            sess = self.meater_sess.get(job["addr"])
            if sess is None:
                self.log_jobs.remove(job)
                continue
            while budget > 0 and job["next"] <= job["end"]:
                if len(getattr(self.t, "txq", ())) >= TXQ_HIGH:
                    return
                i = job["next"] - sess["base"]
                if 0 <= i < len(sess["log"]):
                    self._send(job["port"], protocol.build_response(
                        protocol.MSG_READ_LOGS, job["req"], True,
                        sess["serial"] + struct.pack("<I", job["next"])
                        + sess["log"][i]))
                    budget -= 1
                job["next"] += 1
            if job["next"] > job["end"]:
                self.log_jobs.remove(job)
            if budget <= 0:
                return

    # -- scan data ------------------------------------------------------------------

    def _refresh(self, now):
        t = self.t
        for d in (t.direct, t.repeated, t.nodes):
            for k in list(d):
                if ticks_diff(now, d[k][1]) > (NODE_STALE_MS if d is t.nodes
                                                else PROBE_STALE_MS):
                    del d[k]
        for k in list(self.link_status):
            if ticks_diff(now, self.link_status[k][1]) > PROBE_STALE_MS:
                del self.link_status[k]
        for k in list(self.via_node):
            if ticks_diff(now, self.via_node[k][1]) > PROBE_STALE_MS:
                del self.via_node[k]
        for k in list(t.meaters):
            if ticks_diff(now, t.meaters[k][1]) > NODE_STALE_MS and k not in t.links:
                del t.meaters[k]
        for k in list(self.meater_data):
            if ticks_diff(now, self.meater_data[k].get("at", now)) > PROBE_STALE_MS:
                del self.meater_data[k]
        sel = self.meater_selected()
        for k in list(self.meater_sess):
            if k not in sel:          # un-bridged: forget its session and log
                del self.meater_sess[k]

        probes = {}

        def put(serial, d, ticks, src, rank, rssi=None):
            age = ticks_diff(now, ticks)
            cur = probes.get(serial)
            live = age < FRESH_MS
            if cur is not None:
                cur_live = cur["age_ms"] < FRESH_MS
                if cur_live and not live:
                    return
                if cur_live == live and (cur["_rank"] < rank or
                                         (cur["_rank"] == rank and cur["age_ms"] <= age)):
                    return
            d = dict(d)
            d["serial"] = protocol.probe_serial_str(serial)
            d["src"] = src
            d["age_ms"] = age
            d["_rank"] = rank
            if rssi is not None:
                d["rssi"] = rssi
            elif cur is not None and "rssi" in cur:
                d["rssi"] = cur["rssi"]
            probes[serial] = d

        for serial, (status, ticks) in list(self.link_status.items()):
            d = protocol.decode_probe_status(status)
            if d:
                d["hops"] = 0
                put(serial, d, ticks, "link", 0)
        for serial, (body, ticks, _at, _addr, rssi) in list(t.direct.items()):
            d = protocol.parse_probe_advert(body)
            if d:
                put(serial, d, ticks, "advert", 1, rssi)
        for serial, (status, ticks, hops, _port) in list(self.via_node.items()):
            d = protocol.decode_probe_status(status)
            if d:
                d["hops"] = hops
                put(serial, d, ticks, "node", 2)
        for serial, (body, ticks, rssi) in list(t.repeated.items()):
            d = protocol.parse_probe_advert(body)
            if d:
                put(serial, d, ticks, "repeated", 3)
        for addr in list(self.meater_data):
            r = self.meater_reading(addr, now)
            if r:
                serial4, _msd, d, _age = r
                d["hops"] = 0
                d["battery_pct"] = self.meater_data[addr].get("battery")
                obs = t.meaters.get(addr)
                put(serial4, d, self.meater_data[addr]["at"], "meater", 0,
                    obs[3] if obs else None)
        # Keep RSSI from the direct advert even when another source wins.
        for serial, (_b, _t, _at, _addr, rssi) in list(t.direct.items()):
            if serial in probes:
                probes[serial]["rssi"] = rssi
        for d in probes.values():
            d.pop("_rank", None)
            d.pop("serial4", None)
        self.probes = probes

        devices = {}
        for serial, (body, ticks, _at, _addr, rssi) in list(t.nodes.items()):
            d = protocol.parse_node_advert(body)
            if d is None:
                continue
            d = dict(d)
            d["serial"] = protocol.serial10_str(serial)
            d["name"] = protocol.PRODUCT_NAMES.get(d["ptype"], "Device")
            d["rssi"] = rssi
            d["age_ms"] = ticks_diff(now, ticks)
            devices[bytes(serial)] = d
        self.devices = devices

    def probe_in_network(self, serial4):
        return (serial4 in self.link_status and self._probe_link(serial4)) or \
            serial4 in self.via_node or \
            self._meater_for_serial(serial4) is not None

    def _assign_slot(self, serial4):
        serial4 = bytes(serial4)
        if serial4 in self.slots:
            return self.slots.index(serial4) + 1
        for i in range(10):
            if self.slots[i] is None:
                self.slots[i] = serial4
                return i + 1
        # Full: recycle a slot whose probe is no longer reachable.
        for i in range(10):
            if not self.probe_in_network(self.slots[i]):
                self.slots[i] = serial4
                return i + 1
        return None

    def _node_number(self, serial10):
        serial10 = bytes(serial10)
        if serial10 not in self.node_numbers:
            self.node_numbers[serial10] = 21 + len(self.node_numbers)
        return self.node_numbers[serial10]

    # -- links ----------------------------------------------------------------------

    def _manage_links(self, now):
        t = self.t
        wanted = self.wanted_probes()
        meaters = self.meater_selected()
        budget = self.link_budget()
        for key, link in list(t.links.items()):
            if ((link.kind == "probe" and key not in wanted)
                    or (link.kind == "meater" and key not in meaters)):
                t.close_link(key)
        # Probes and MEATERs share the outbound budget.
        active = [k for k, l in t.links.items()
                  if l.kind in ("probe", "meater") and l.enabled]
        for serial in wanted:
            if serial in active:
                continue
            if len(active) >= budget:
                break
            obs = t.direct.get(serial)
            if obs is None:
                continue          # need to hear it directly for its address
            t.open_link(serial, "probe", obs[2], obs[3])
            active.append(serial)
        for addr in meaters:
            if addr in active:
                continue
            if len(active) >= budget:
                break
            obs = t.meaters.get(addr)
            if obs is None:
                continue          # not heard yet (needs its address type)
            t.open_link(addr, "meater", obs[2], addr)
            active.append(addr)
        up = self.upstream_serial()
        for key, link in list(t.links.items()):
            if link.kind == "node" and key != up:
                t.close_link(key)
        if up and up not in t.links:
            obs = t.nodes.get(up)
            if obs is not None:
                t.open_link(up, "node", obs[2], obs[3])

    def _on_node_link_ready(self, key):
        port = ("link", key)
        self.peer_serial[port] = key
        if not self.push_upstream():
            return
        # Ask the node to associate with us, as a node joining another does.
        frame = protocol.build_request(protocol.MSG_ASSOCIATE_NODE, b"")
        rid = struct.unpack_from("<I", frame, 5)[0]
        self.ids.seen(rid)
        self.my_reqs[rid] = ("assoc", port, None, ticks_ms())
        self._send(port, frame)
        # Its link table rarely changes; don't re-pull it on every reconnect.
        last = self._table_pulled.get(key)
        if self.links_table is not None and (
                last is None or ticks_diff(ticks_ms(), last) > TABLE_PULL_MS):
            self._table_pulled[key] = ticks_ms()
            self._request_link_table(port, protocol.MSG_READ_LINKED_DEVICES, 0)
            self._request_link_table(port, protocol.MSG_READ_UNLINKED_DEVICES, 0)

    # -- advertising ------------------------------------------------------------------

    def _update_adverts(self, now):
        t = self.t
        payloads = [protocol.build_display_advertisement(self.serial)]
        fast = False
        mode = self.cfg.get("repeat_adverts", "all")
        wanted = self.wanted_probes()
        done = set()
        if mode != "off":
            for serial, (body, ticks, _at, _addr, _rssi) in list(t.direct.items()):
                if ticks_diff(now, ticks) > 10000:
                    continue
                if mode == "linked" and serial not in wanted:
                    continue
                payloads.append(protocol.repeat_probe_advert(body, 1))
                done.add(serial)
                if body[18] & 0x03 == protocol.PROBE_MODE_INSTANT_READ:
                    fast = True
            for serial, (status, ticks) in list(self.link_status.items()):
                if serial in done or ticks_diff(now, ticks) > 10000:
                    continue
                payloads.append(protocol.probe_advert_from_status(serial, status, 1))
                if status[21] & 0x03 == protocol.PROBE_MODE_INSTANT_READ:
                    fast = True
        # Bridged MEATERs, published as Combustion probes (always: bridging
        # is the point of selecting them).
        for addr in list(self.meater_data):
            r = self.meater_reading(addr, now)
            if r:
                payloads.append(r[1])
                if self.cfg.get("meater_direct_adverts", True):
                    payloads.append((self._meater_direct(addr), "probe"))
        t.set_advertising(payloads, fast)

    # -- timeouts ---------------------------------------------------------------------

    def _log_read_active(self, serial4):
        """A probe answers other commands only after streaming a log read."""
        for q in self.pending:
            if q["probe"] == serial4 and q["type"] == protocol.MSG_READ_LOGS:
                return True
        return False

    def _expire(self, now):
        keep = []
        for p in self.pending:
            age = ticks_diff(now, p["at"])
            if p["type"] == protocol.MSG_READ_LOGS:
                if age > (LOG_IDLE_MS if p.get("got") else LOG_FIRST_MS):
                    continue
            elif age > (BUSY_TIMEOUT_MS if self._log_read_active(p["probe"])
                        else PENDING_TIMEOUT_MS):
                if p["type"] != protocol.MSG_SILENCE_ALARMS:
                    self._send(p["port"], protocol.build_response(
                        p["type"], p["req"], False, b""))
                self.log("[route] 0x%02x to %s timed out" % (
                    p["type"], protocol.probe_serial_str(p["probe"])))
                continue
            keep.append(p)
        self.pending = keep
        for rid in list(self.fwd):
            if ticks_diff(now, self.fwd[rid][1]) > FWD_TIMEOUT_MS:
                del self.fwd[rid]
        for rid in list(self.my_reqs):
            if ticks_diff(now, self.my_reqs[rid][3]) > FWD_TIMEOUT_MS:
                del self.my_reqs[rid]

    # -- frame handling -----------------------------------------------------------------

    def _on_frame(self, port, f, now):
        if f.is_response:
            self._on_response(port, f, now)
            return
        if self.ids.seen(f.request_id):
            return
        mt = f.msg_type
        if mt == protocol.MSG_READ_PROBE_LIST:
            self._send(port, protocol.build_read_probe_list_response(
                f.request_id, self.network_slots()))
        elif mt == protocol.MSG_READ_NODE_LIST:
            page = f.payload[0] if f.payload else 0
            self._send(port, protocol.build_read_node_list_response(
                f.request_id, page, self.node_list()))
        elif mt == protocol.MSG_READ_NETWORK_TOPOLOGY:
            self._on_topology(port, f, now)
        elif mt in protocol.PROBE_ADDRESSED:
            self._route_probe_request(port, f, now)
        elif mt == protocol.MSG_SILENCE_ALARMS:
            self._on_silence(port, f, now)
        elif mt == protocol.MSG_PROBE_STATUS:
            self._on_remote_status(port, f, now)
        elif mt == protocol.MSG_HEARTBEAT:
            hb = protocol.parse_heartbeat(f.payload)
            if hb:
                self.peer_serial[port] = hb["serial"]
                if hb["ptype"] != protocol.PRODUCT_TYPE_UNKNOWN:
                    self._node_number(hb["serial"])
            fwd = protocol.bump_heartbeat(f)
            if fwd:
                self._flood(fwd, port)
        elif mt == protocol.MSG_SYNC_THERMOMETER_LIST:
            self._on_sync_list(f)
            self._flood(f.raw, port, inbound=False)
        elif mt == protocol.MSG_ASSOCIATE_NODE:
            self._send(port, protocol.build_response(mt, f.request_id, True, b""))
        elif mt in (protocol.MSG_LINK_DEVICE, protocol.MSG_UNLINK_DEVICE,
                    protocol.MSG_DEVICE_LINKED, protocol.MSG_DEVICE_UNLINKED,
                    protocol.MSG_READ_LINKED_DEVICES,
                    protocol.MSG_READ_UNLINKED_DEVICES,
                    protocol.MSG_SET_PROXIMITY_LINKING,
                    protocol.MSG_READ_PROXIMITY_LINKING,
                    protocol.MSG_SYNC_LINK_TABLE):
            self._on_link_msg(port, f, now)
        elif mt in (protocol.MSG_PROBE_FIRMWARE_REVISION,
                    protocol.MSG_PROBE_HARDWARE_REVISION,
                    protocol.MSG_PROBE_MODEL_INFORMATION):
            addr = self._meater_for_serial(f.payload[0:4], now)
            if addr is not None:
                self._meater_device_info(port, f, addr)
            elif self._probe_device_info(port, f):
                pass
            elif not self._forward_to_owner(port, f, f.payload[0:4], now):
                self.log("[route] 0x%02x for %s: no answer available" % (
                    mt, protocol.probe_serial_str(f.payload[0:4])))
        else:
            # Gauge/Engine status, device connected/disconnected, requests for
            # other nodes (e.g. Read Gauge Logs 0x62, Set Engine Set Point):
            # propagate across the network, remembering who asked so the
            # replies find their way back.
            self.fwd[f.request_id] = (port, now)
            self._flood(f.raw, port, links=port[0] == "in")

    def _on_response(self, port, f, now):
        if self.ids.seen(("r", f.response_id)):
            return
        mine = self.my_reqs.get(f.request_id)
        if mine is not None:
            self._on_my_response(port, f, mine)
            return
        fwd = self.fwd.get(f.request_id)
        if fwd is not None:
            self._send(fwd[0], f.raw)
            if port[0] == "link" and fwd[0][0] == "in":
                self._count_via("node_to_app", f.raw)
            # Log reads (probe 0x04, gauge 0x62) answer with many responses:
            # keep the route open until it goes quiet (FWD_TIMEOUT_MS).
            self.fwd[f.request_id] = (fwd[0], now)

    # -- probe routing --------------------------------------------------------------------

    def _forward_to_owner(self, port, f, serial4, now):
        """Pass a request on to the upstream node that has the probe."""
        via = self.via_node.get(bytes(serial4))
        if via is None or via[3] == port:
            return False
        self.fwd[f.request_id] = (port, now)
        self._send(via[3], f.raw)
        if port[0] == "in" and via[3][0] == "link":
            self._count_via("app_to_node", f.raw, via[3][1])
        self.stats["routed"] += 1
        return True

    def _route_probe_request(self, port, f, now):
        r = protocol.node_to_probe(f.msg_type, f.payload)
        if r is None:
            return
        serial4, probe_payload = r
        addr = self._meater_for_serial(serial4, now)
        if addr is not None:
            self._meater_request(port, f, addr, probe_payload, now)
            return
        link = self._probe_link(serial4)
        if link is not None:
            p = {"probe": serial4, "type": f.msg_type, "req": f.request_id,
                 "port": port, "at": now, "end": None}
            if f.msg_type == protocol.MSG_READ_LOGS and len(probe_payload) >= 8:
                p["start"], p["end"] = struct.unpack_from("<II", probe_payload, 0)
                p["next"] = p["start"]
                # The app overlaps log reads (e.g. re-requesting a range); keep
                # them all and match records by sequence number, dropping the
                # oldest beyond MAX_LOG_READS.
                reads = [q for q in self.pending if q["probe"] == serial4
                         and q["type"] == protocol.MSG_READ_LOGS]
                for q in reads[:max(0, len(reads) - MAX_LOG_READS + 1)]:
                    self.pending.remove(q)
            self.pending.append(p)
            link.write(protocol.build_probe_request(f.msg_type, probe_payload))
            self.stats["routed"] += 1
            return
        if not self._forward_to_owner(port, f, serial4, now):
            self.log("[route] 0x%02x for %s: probe not in network" % (
                f.msg_type, protocol.probe_serial_str(serial4)))

    def _match_log_record(self, serial4, seq):
        """The log read this record belongs to. The probe answers overlapping
        reads one after the other, so take the oldest read whose range holds
        seq and that hasn't had it yet; else the newest whose range holds it."""
        newest = None
        for p in self.pending:
            if (p["probe"] == serial4 and p["type"] == protocol.MSG_READ_LOGS
                    and p.get("start", 0) <= seq <= p["end"]):
                if seq >= p.get("next", 0):
                    return p
                newest = p
        return newest

    def _on_probe_response(self, serial4, mtype, success, payload, now):
        if mtype == protocol.MSG_READ_LOGS and success and len(payload) >= 4:
            # A log record: route it by sequence number. (Failed replies carry
            # no valid record; they end the oldest open read, below.)
            seq = struct.unpack_from("<I", payload, 0)[0]
            p = self._match_log_record(serial4, seq)
            if p is None:
                self._stray(serial4, mtype, now, "log record %d" % seq)
                return
            self._send(p["port"], protocol.build_response(
                mtype, p["req"], True,
                protocol.probe_to_node_payload(serial4, payload)))
            p["at"] = now
            p["got"] = True
            p["next"] = max(p.get("next", 0), seq + 1)
            if seq >= p["end"]:
                self.pending.remove(p)
            return
        for p in self.pending:
            if p["probe"] == serial4 and p["type"] == mtype:
                break
        else:
            if mtype == protocol.MSG_READ_LOGS and not success:
                # A probe ends/gaps a log read with a "failed" reply whose
                # payload is leftover buffer bytes; after the read is done
                # it's expected noise: count it, don't log it.
                self.stats["log_fail_late"] = self.stats.get("log_fail_late", 0) + 1
                return
            self._stray(serial4, mtype, now, "%s, %d bytes" % (
                "ok" if success else "failed", len(payload)))
            return
        if mtype == protocol.MSG_SILENCE_ALARMS:
            self.pending.remove(p)
            self._send(p["port"], protocol.build_silence_alarms_response(
                p["req"], protocol.PRODUCT_TYPE_PROBE, serial4))
            return
        frame = protocol.build_response(
            mtype, p["req"], success,
            protocol.probe_to_node_payload(serial4, payload))
        self._send(p["port"], frame)
        self.pending.remove(p)

    def _stray(self, serial4, mtype, now, what=None):
        """A probe response no request is waiting for (e.g. a log record
        after its read timed out): ignore it, logging at most every 30 s."""
        self.stats["stray"] = self.stats.get("stray", 0) + 1
        last = getattr(self, "_stray_at", None)
        if last is None or ticks_diff(now, last) >= STRAY_LOG_MS:
            self._stray_at = now
            self.log("[route] %s response 0x%02x%s with no request waiting "
                     "(%d so far)" % (protocol.probe_serial_str(serial4), mtype,
                                      " (%s)" % what if what else "",
                                      self.stats["stray"]))

    def _on_silence(self, port, f, now):
        glob, ptype, serial4, serial10 = protocol.parse_silence_alarms(f.payload)
        targets = []
        if glob:
            targets = [k for k, l in self.t.links.items()
                       if l.kind == "probe" and l.ready]
            self._flood(f.raw, port, links=port[0] == "in")
        elif ptype == protocol.PRODUCT_TYPE_PROBE:
            if self._probe_link(serial4):
                targets = [bytes(serial4)]
            else:
                self._forward_to_owner(port, f, serial4, now)
        for s in targets:
            self.pending.append({"probe": s, "type": protocol.MSG_SILENCE_ALARMS,
                                 "req": f.request_id, "port": port, "at": now,
                                 "end": None})
            self._probe_link(s).write(
                protocol.build_probe_request(protocol.MSG_SILENCE_ALARMS))
        if glob or (ptype != protocol.PRODUCT_TYPE_PROBE and
                    bytes(serial10) == protocol.encode_serial(self.serial)):
            self._send(port, protocol.build_silence_alarms_response(
                f.request_id, protocol.PRODUCT_TYPE_DISPLAY, self.serial))

    def _on_remote_status(self, port, f, now):
        """0x45 from another node: remember it (we can route to the probe via
        that port) and pass it on one hop further."""
        r = protocol.parse_node_probe_status(f.payload)
        if r is None:
            return
        serial4, status, hops = r
        if serial4 not in self.link_status:
            self.via_node[serial4] = (status, now, hops + 1, port)
            self._assign_slot(serial4)
        fwd = protocol.bump_node_probe_status(f)
        if fwd:
            self._flood(fwd, port)

    def _on_topology(self, port, f, now):
        want = bytes(f.payload[0:10])
        if want != protocol.encode_serial(self.serial):
            for key, _l in self._node_links():
                if ("link", key) != port:
                    self.fwd[f.request_id] = (port, now)
                    self._send(("link", key), f.raw)
            return
        inbound = []
        for p, s in self.peer_serial.items():
            if p[0] == "in":
                inbound.append((self._node_number(s), 0))
        outbound = []
        for key, link in self.t.links.items():
            if not link.ready:
                continue
            if link.kind == "probe":
                num = self._assign_slot(key)
                obs = self.t.direct.get(key)
                outbound.append((num or 0, obs[4] if obs else 0))
            elif link.kind == "meater":
                num = self._assign_slot(meater.serial_for(key))
                obs = self.t.meaters.get(key)
                outbound.append((num or 0, obs[3] if obs else 0))
            elif link.kind == "node":
                obs = self.t.nodes.get(key)
                outbound.append((self._node_number(key), obs[4] if obs else 0))
        self._send(port, protocol.build_topology_response(
            f.request_id, 20, protocol.PRODUCT_TYPE_DISPLAY, self.serial,
            inbound, outbound))

    def network_slots(self):
        """Probe List slots: only probes we can actually reach."""
        return [s if s is not None and self.probe_in_network(s) else None
                for s in self.slots]

    def node_list(self):
        nodes = [(20, protocol.PRODUCT_TYPE_DISPLAY, self.serial)]
        seen = set()
        for port, serial in self.peer_serial.items():
            serial = bytes(serial)
            if serial in seen:
                continue
            seen.add(serial)
            d = self.devices.get(serial)
            ptype = d["ptype"] if d else protocol.PRODUCT_TYPE_UNKNOWN
            nodes.append((self._node_number(serial), ptype, serial))
        return nodes

    # -- heartbeat / thermometer list ---------------------------------------------------

    def _send_heartbeat(self):
        self._hb_inbound = not self._hb_inbound
        records = []
        if self._hb_inbound:
            for port, serial in self.peer_serial.items():
                if port[0] == "in":
                    d = self.devices.get(bytes(serial))
                    records.append((d["ptype"] if d else protocol.PRODUCT_TYPE_DISPLAY,
                                    serial, d["rssi"] if d else 0))
        else:
            for key, link in self.t.links.items():
                if not link.ready:
                    continue
                if link.kind == "probe":
                    obs = self.t.direct.get(key)
                    records.append((protocol.PRODUCT_TYPE_PROBE, key,
                                    obs[4] if obs else 0))
                elif link.kind == "meater":
                    obs = self.t.meaters.get(key)
                    records.append((protocol.PRODUCT_TYPE_PROBE,
                                    meater.serial_for(key), obs[3] if obs else 0))
                elif link.kind == "node":
                    d = self.devices.get(key)
                    records.append((d["ptype"] if d else protocol.PRODUCT_TYPE_DISPLAY,
                                    key, d["rssi"] if d else 0))
        frame = protocol.build_heartbeat(self.serial, self.t.mac,
                                         protocol.PRODUCT_TYPE_DISPLAY,
                                         self._hb_inbound, records[:4])
        self.ids.seen(struct.unpack_from("<I", frame, 5)[0])
        self._flood(frame)

    def _on_sync_list(self, f):
        r = protocol.parse_sync_thermometer_list(f.payload)
        if r is None:
            return
        _mac, slots = r
        for i, serial in enumerate(slots):
            if serial is None or self.slots[i] == serial:
                continue
            if serial in self.slots:
                self.slots[self.slots.index(serial)] = None
            displaced = self.slots[i]
            self.slots[i] = serial
            if displaced is not None:
                self._assign_slot(displaced)

    # -- link tables --------------------------------------------------------------------

    def _request_link_table(self, port, msg_type, page):
        frame = protocol.build_request(msg_type, bytes((page,)))
        rid = struct.unpack_from("<I", frame, 5)[0]
        self.ids.seen(rid)
        self.my_reqs[rid] = ("table", port, (msg_type, page), ticks_ms())
        self._send(port, frame)

    def _on_my_response(self, port, f, mine):
        purpose, req_port, extra, _at = mine
        self.my_reqs.pop(f.request_id, None)
        if purpose == "assoc":
            self.log("[link] node %s %s association" % (
                protocol.serial10_str(req_port[1]) if req_port[0] == "link" else "?",
                "accepted" if f.success else "refused"))
            return
        if purpose != "table" or not f.success or self.links_table is None:
            return
        r = protocol.parse_link_entries_response(f.payload)
        if r is None:
            return
        page, total, entries = r
        msg_type = extra[0]
        self.links_table.merge(entries, msg_type == protocol.MSG_READ_LINKED_DEVICES,
                               self.serial, self.peer_serial.get(port))
        if page + 1 < total:
            self._request_link_table(port, msg_type, page + 1)

    def _on_link_msg(self, port, f, now):
        lt = self.links_table
        mt = f.msg_type
        if lt is None:
            return
        if mt in (protocol.MSG_READ_LINKED_DEVICES, protocol.MSG_READ_UNLINKED_DEVICES):
            page = f.payload[0] if f.payload else 0
            entries = lt.linked if mt == protocol.MSG_READ_LINKED_DEVICES else lt.unlinked
            self._send(port, protocol.build_link_entries_response(
                mt, f.request_id, page, entries))
        elif mt in (protocol.MSG_LINK_DEVICE, protocol.MSG_UNLINK_DEVICE):
            e = protocol.parse_link_device(f.payload)
            ok = e is not None
            if ok:
                linked = mt == protocol.MSG_LINK_DEVICE
                (lt.link if linked else lt.unlink)(e)
                note = protocol.build_link_notification(linked, e)
                self.ids.seen(struct.unpack_from("<I", note, 5)[0])
                self._flood(note)
            self._send(port, protocol.build_response(mt, f.request_id, ok, b""))
        elif mt in (protocol.MSG_DEVICE_LINKED, protocol.MSG_DEVICE_UNLINKED):
            e = protocol.parse_link_device(f.payload)
            if e is not None:
                lt.merge([e], mt == protocol.MSG_DEVICE_LINKED, self.serial,
                         self.peer_serial.get(port))
            self._flood(f.raw, port)
        elif mt == protocol.MSG_SET_PROXIMITY_LINKING:
            lt.proximity = bool(f.payload[0]) if f.payload else False
            lt.save()
            self._send(port, protocol.build_response(mt, f.request_id, True, b""))
        elif mt == protocol.MSG_READ_PROXIMITY_LINKING:
            self._send(port, protocol.build_response(
                mt, f.request_id, True, bytes((1 if lt.proximity else 0,))))
        elif mt == protocol.MSG_SYNC_LINK_TABLE:
            self._send(port, protocol.build_response(mt, f.request_id, True, b""))
            self._request_link_table(port, protocol.MSG_READ_LINKED_DEVICES, 0)
            self._request_link_table(port, protocol.MSG_READ_UNLINKED_DEVICES, 0)

    # -- web UI status ------------------------------------------------------------------

    def status(self):
        links = []
        for key, link in self.t.links.items():
            links.append({
                "kind": link.kind,
                "serial": self._link_name(key, link),
                "state": link.state, "count": link.count, "mtu": link.mtu})
        probes = []
        wanted = self.wanted_probes()
        for serial, d in self.probes.items():
            p = d.get("prediction")
            probes.append({
                "serial": d["serial"], "id": d["probe_id"], "color": d["color"],
                "mode": d["mode"], "core_c": d["core_c"],
                "surface_c": d["surface_c"], "ambient_c": d["ambient_c"],
                "instant_c": d["instant_c"], "temps": d["temps"],
                "low_battery": d["low_battery"], "src": d["src"],
                "hops": d.get("hops", 0), "rssi": d.get("rssi"),
                "age_s": d["age_ms"] // 1000,
                "prediction": p, "alarming": d.get("alarming", False),
                "linked": serial in wanted,
                "direct": serial in self.t.direct,
                "slot": (self.slots.index(serial) + 1) if serial in self.slots else None,
            })
        devices = []
        for serial, d in self.devices.items():
            dd = dict(d)
            dd.pop("serial", None)
            dd["serial"] = d["serial"]
            dd["age_s"] = d["age_ms"] // 1000
            dd.pop("age_ms", None)
            devices.append(dd)
        meaters = []
        sel = self.meater_selected()
        seen = set(self.t.meaters) | set(sel)
        for addr in seen:
            obs = self.t.meaters.get(addr)
            data = self.meater_data.get(addr, {})
            link = self.t.links.get(addr)
            t6 = data.get("temps")
            meaters.append({
                "addr": meater.addr_str(addr),
                "name": obs[0] if obs and obs[0] else "MEATER",
                "rssi": obs[3] if obs else None,
                "age_s": ticks_diff(ticks_ms(), obs[1]) // 1000 if obs else None,
                "bridged": addr in sel,
                "id": int(sel[addr].get("id", 8)) if addr in sel else 8,
                "serial": protocol.probe_serial_str(meater.serial_for(addr)),
                "state": link.state if link else None,
                "tips": t6[:5] if t6 else None,
                "ambient": t6[5] if t6 else None,
                "battery": data.get("battery"),
            })
        return {"probes": probes, "devices": devices, "links": links,
                "meaters": meaters, "meater_bridge": self.meater_enabled(),
                "budget": self.link_budget(),
                "inbound": len(self.t.inbound),
                "max_conn": self.t.max_conn,
                "pending": len(self.pending), "stats": self.stats,
                "tx_dropped": getattr(self.t, "tx_dropped", 0),
                "via": self.via}
