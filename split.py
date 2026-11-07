"""Two-unit mode: a phone-side "app" unit and a device-side "field" unit
coordinating over WiFi (MicroPython + CPython host tests).

The field unit is a remote radio. It scans and holds the outbound BLE links
(probes, MEATERs, an upstream Gauge/Engine node), and runs no MeatNet logic.
The app unit runs the unchanged meatnet.Node on a SplitTransport that
combines its own BLE peripheral (phones, advertising) with a RemoteRadio
mirroring the field unit's scan tables and links.

Wire protocol: TCP (app unit listens on `split_port`), newline-delimited
JSON, binary as base64, keys as hex. The field unit finds the app unit via
`split_peer` or the app unit's UDP beacon on the same port.

  both:  hello {role, serial, key, max_conn, mac}, ping {ts}, pong {ts}
  field: scan {d, r, n, m}          scan-table entries updated since last batch
         link {k, kind, state, desc, mtu, count}
         rx   {k, sub, data}         a link notification
  app:   open {k, kind, at, addr}, close {k}, write {k, data},
         scan_active {on}, standby {on}
"""

import asyncio
import binascii
import json
import socket

from util import ticks_ms, ticks_diff, ticks_add

VERSION = 1
PING_MS = 2000
DEAD_MS = 6000
SCAN_BATCH_MS = 250
FIELD_STALE_MS = 60000
LINK_SUBS = {"probe": ("status", "uart"), "node": ("uart",),
             "meater": ("temp", "battery")}
RX_KEEP = 128


def b64(data):
    return binascii.b2a_base64(bytes(data)).strip().decode()


def unb64(text):
    return binascii.a2b_base64(text)


def encode(msg):
    return (json.dumps(msg) + "\n").encode()


def decode(line):
    try:
        if isinstance(line, (bytes, bytearray)):
            line = bytes(line).decode()
        msg = json.loads(line)
    except (ValueError, TypeError, UnicodeError):
        return None
    return msg if isinstance(msg, dict) and "t" in msg else None


# ---------------------------------------------------------------------------
# Field side
# ---------------------------------------------------------------------------

class FieldRadio:
    """Wraps the field unit's BleTransport: turns its scan tables and links
    into messages for the app unit, and applies the app unit's commands."""

    def __init__(self, transport, log=print):
        self.t = transport
        self.log = log
        self.ignore_serial = None  # the app unit's Display serial
        self._sent = {}           # (table, key) -> ticks of the entry last sent
        self._links = {}          # key -> (state, desc, mtu) last reported
        self.stats = {"scan": 0, "rx": 0, "cmd": 0}

    def reset(self):
        """New connection: resend everything."""
        self._sent = {}
        self._links = {}

    def set_ignore(self, mac, serial):
        """Don't report the app unit's own advertisements (its Display info,
        the probes it repeats, its synthesized MEATERs) back to it."""
        import protocol
        self.ignore_serial = protocol.encode_serial(serial) if serial else None
        mac = bytes(mac or b"")
        t = self.t
        if mac and mac != b"\x00" * len(mac):
            t.ignore_addrs = set((mac,))
            for d in (t.direct, t.nodes):
                for k in list(d):
                    if bytes(d[k][3]) == mac:
                        del d[k]
            t.meaters.pop(mac, None)
            t.repeated.clear()        # can't tell whose: refills within seconds

    def drop_links(self):
        for key in list(self.t.links):
            self.t.close_link(key)

    def prune(self, now):
        for d in (self.t.direct, self.t.repeated, self.t.nodes, self.t.meaters):
            for k in list(d):
                if ticks_diff(now, d[k][1]) > FIELD_STALE_MS:
                    del d[k]
                    for tbl in "drnm":
                        self._sent.pop((tbl, bytes(k)), None)

    def scan_message(self, now):
        msg = {"t": "scan"}
        n = 0
        tables = (("d", self.t.direct), ("r", self.t.repeated),
                  ("n", self.t.nodes), ("m", self.t.meaters))
        for tbl, d in tables:
            out = []
            for k, v in list(d.items()):
                k = bytes(k)
                if tbl == "n" and k == self.ignore_serial:
                    continue
                if self._sent.get((tbl, k)) == v[1]:
                    continue
                self._sent[(tbl, k)] = v[1]
                age = ticks_diff(now, v[1])
                if tbl in ("d", "n"):
                    out.append([k.hex(), b64(v[0]), age, v[2], bytes(v[3]).hex(), v[4]])
                elif tbl == "r":
                    out.append([k.hex(), b64(v[0]), age, v[2]])
                else:
                    out.append([k.hex(), v[0], age, v[2], v[3]])
            if out:
                msg[tbl] = out
                n += len(out)
        if not n:
            return None
        self.stats["scan"] += n
        return msg

    def link_messages(self):
        out = []
        for key, link in list(self.t.links.items()):
            desc = link.describe() if link.ready and hasattr(link, "describe") else ""
            info = getattr(link, "info", None) or {}
            cur = (link.state, desc, link.mtu, tuple(sorted(info.items())))
            if self._links.get(key) != cur:
                self._links[key] = cur
                out.append({"t": "link", "k": bytes(key).hex(), "kind": link.kind,
                            "state": link.state, "desc": desc, "mtu": link.mtu,
                            "count": link.count, "info": info})
            if link.ready:
                for sub in LINK_SUBS.get(link.kind, ()):
                    for data in link.take(sub):
                        out.append({"t": "rx", "k": bytes(key).hex(), "sub": sub,
                                    "data": b64(data)})
                        self.stats["rx"] += 1
        for key in list(self._links):
            if key not in self.t.links:
                del self._links[key]
                out.append({"t": "link", "k": bytes(key).hex(), "kind": "",
                            "state": "closed", "desc": "", "mtu": 23, "count": 0})
        return out

    def apply(self, msg):
        t = msg.get("t")
        self.stats["cmd"] += 1
        if t == "open":
            self.t.open_link(bytes.fromhex(msg["k"]), msg["kind"], msg["at"],
                             bytes.fromhex(msg["addr"]))
        elif t == "close":
            self.t.close_link(bytes.fromhex(msg["k"]))
        elif t == "write":
            link = self.t.links.get(bytes.fromhex(msg["k"]))
            if link is not None and link.ready:
                link.write(unb64(msg["data"]))
        elif t == "scan_active":
            self.t.set_scan_active(bool(msg.get("on")))
        elif t == "standby":
            self.t.set_standby(bool(msg.get("on")))


# ---------------------------------------------------------------------------
# App side
# ---------------------------------------------------------------------------

class RemoteLink:
    """CentralLink-compatible proxy for a link held by the field unit."""

    def __init__(self, radio, key, kind):
        self.radio = radio
        self.key = key
        self.kind = kind
        self.state = "idle"
        self.enabled = True
        self.count = 0
        self.mtu = 23
        self.desc = ""
        self.info = {}
        self.rx = {}
        self.opened = False       # open command sent on the current connection

    @property
    def ready(self):
        return self.state == "ready"

    def take(self, name):
        q = self.rx.get(name)
        if not q:
            return []
        self.rx[name] = []
        return q

    def write(self, data):
        if not self.ready:
            return False
        self.radio.queue({"t": "write", "k": self.key.hex(), "data": b64(data)})
        return True

    def describe(self):
        return self.desc or self.state


class RemoteRadio:
    """The field unit's radio as seen from the app unit."""

    def __init__(self, log=print):
        self.log = log
        self.direct = {}
        self.repeated = {}
        self.nodes = {}
        self.meaters = {}
        self.links = {}
        self.max_conn = 4
        self.connected = False
        self.peer = None          # (serial, ip)
        self.rtt_ms = None
        self.outbox = []
        self._scan_active = False
        self._standby = False
        self._link_args = {}      # key -> open message (for reconnects)
        self.own_serial = None    # our Display serial: never mirror it

    def queue(self, msg):
        if self.connected:
            self.outbox.append(msg)

    def take_outbox(self):
        out, self.outbox = self.outbox, []
        return out

    # -- transport interface used by meatnet.Node --------------------------

    def open_link(self, key, kind, addr_type, addr):
        key = bytes(key)
        link = self.links.get(key)
        if link is None:
            link = self.links[key] = RemoteLink(self, key, kind)
            if not self.connected:
                link.state = "error: field unit offline"
        link.enabled = True
        self._link_args[key] = {"t": "open", "k": key.hex(), "kind": kind,
                                "at": addr_type, "addr": bytes(addr).hex()}
        if not link.opened and self.connected:
            self.queue(self._link_args[key])
            link.opened = True
        return link

    def close_link(self, key):
        key = bytes(key)
        self.links.pop(key, None)
        self._link_args.pop(key, None)
        self.queue({"t": "close", "k": key.hex()})

    def set_scan_active(self, on):
        if on != self._scan_active:
            self._scan_active = on
            self.queue({"t": "scan_active", "on": on})

    def set_standby(self, on):
        if on != self._standby:
            self._standby = on
            self.queue({"t": "standby", "on": on})

    # -- connection events -------------------------------------------------

    def on_connect(self, hello, ip=None):
        self.connected = True
        self.max_conn = int(hello.get("max_conn", 4))
        self.peer = (hello.get("serial", "?"), ip)
        self.outbox = []
        self.queue({"t": "scan_active", "on": self._scan_active})
        self.queue({"t": "standby", "on": self._standby})
        for key, link in self.links.items():
            link.opened = False
            link.state = "idle"
            if link.enabled and key in self._link_args:
                self.queue(self._link_args[key])
                link.opened = True
        self.log("[split] field unit %s connected%s" % (
            self.peer[0], " from %s" % ip if ip else ""))

    def on_disconnect(self):
        if self.connected:
            self.log("[split] field unit offline")
        self.connected = False
        self.rtt_ms = None
        self.outbox = []
        for link in self.links.values():
            link.state = "error: field unit offline"
            link.opened = False

    def apply(self, msg, now):
        t = msg.get("t")
        if t == "scan":
            for row in msg.get("d", ()):
                self.direct[bytes.fromhex(row[0])] = (
                    unb64(row[1]), ticks_add(now, -row[2]), row[3], bytes.fromhex(row[4]), row[5])
            for row in msg.get("r", ()):
                self.repeated[bytes.fromhex(row[0])] = (
                    unb64(row[1]), ticks_add(now, -row[2]), row[3])
            for row in msg.get("n", ()):
                if bytes.fromhex(row[0]) == self.own_serial:
                    continue
                self.nodes[bytes.fromhex(row[0])] = (
                    unb64(row[1]), ticks_add(now, -row[2]), row[3], bytes.fromhex(row[4]), row[5])
            for row in msg.get("m", ()):
                self.meaters[bytes.fromhex(row[0])] = (row[1], ticks_add(now, -row[2]), row[3], row[4])
        elif t == "link":
            link = self.links.get(bytes.fromhex(msg["k"]))
            if link is not None:
                state = msg["state"]
                link.state = "idle" if state == "closed" else state
                link.desc = msg.get("desc", "")
                link.mtu = msg.get("mtu", 23)
                link.count = msg.get("count", link.count)
                link.info = msg.get("info") or link.info
                if state == "closed":
                    link.opened = False     # reopen on the next open_link()
        elif t == "rx":
            link = self.links.get(bytes.fromhex(msg["k"]))
            if link is not None:
                q = link.rx.setdefault(msg["sub"], [])
                q.append(unb64(msg["data"]))
                link.count += 1
                if len(q) > RX_KEEP:
                    q.pop(0)

    def status(self):
        return {"connected": self.connected,
                "peer": self.peer[0] if self.peer else None,
                "ip": self.peer[1] if self.peer else None,
                "rtt_ms": self.rtt_ms, "max_conn": self.max_conn}


class SplitTransport:
    """The transport meatnet.Node runs on in the app unit: phones and
    advertising on the local radio, scanning and links on the field unit."""

    separate_radios = True

    def __init__(self, local, remote):
        self.local = local
        self.remote = remote
        local.scan_wanted = False
        self.direct = remote.direct
        self.repeated = remote.repeated
        self.nodes = remote.nodes
        self.meaters = remote.meaters
        self.links = remote.links

    # remote: scanning + outbound links
    @property
    def max_conn(self):
        return self.remote.max_conn

    def open_link(self, key, kind, addr_type, addr):
        return self.remote.open_link(key, kind, addr_type, addr)

    def close_link(self, key):
        self.remote.close_link(key)

    def set_scan_active(self, on):
        self.remote.set_scan_active(on)

    # local: phones + advertising
    @property
    def inbound(self):
        return self.local.inbound

    @property
    def mac(self):
        return self.local.mac

    @property
    def txq(self):
        return self.local.txq

    @property
    def tx_dropped(self):
        return self.local.tx_dropped

    def poll_rx(self):
        return self.local.poll_rx()

    def send(self, frame, conns=None):
        self.local.send(frame, conns)

    def set_advertising(self, payloads, fast=False):
        self.local.set_advertising(payloads, fast)

    def rotate_advertisement(self):
        self.local.rotate_advertisement()

    def set_firmware(self, firmware):
        self.local.set_firmware(firmware)

    def set_standby(self, on):
        self.local.set_standby(on)
        self.remote.set_standby(on)

    def service(self):
        self.local.service()


# ---------------------------------------------------------------------------
# Networking (asyncio streams + UDP beacon)
# ---------------------------------------------------------------------------

def _close(writer):
    try:
        writer.close()
    except Exception:
        pass


async def _reader_loop(reader, on_msg, state, log=print):
    while True:
        line = await reader.readline()
        if not line:
            return
        state["rx_at"] = ticks_ms()
        msg = decode(line)
        if msg is not None:
            try:
                on_msg(msg)
            except Exception as exc:      # one bad message mustn't drop the link
                log("[split] error handling %s: %r" % (msg.get("t"), exc))


def _hello(role, serial, key, max_conn=0, mac=b""):
    return {"t": "hello", "v": VERSION, "role": role, "serial": serial,
            "key": key, "max_conn": max_conn, "mac": bytes(mac).hex()}


class SplitServer:
    """App unit: accept the field unit's connection, pump RemoteRadio
    messages, and broadcast a UDP beacon so the field unit can find us."""

    def __init__(self, remote, cfg, serial, log=print, mac=b""):
        self.remote = remote
        self.mac = mac
        import protocol
        remote.own_serial = protocol.encode_serial(serial)
        self.cfg = cfg
        self.serial = serial
        self.log = log
        self.port = int(cfg.get("split_port", 7389))
        self._gen = 0
        self._writer = None

    async def run(self, host="0.0.0.0", beacon=True):
        await asyncio.start_server(self._client, host, self.port)
        if beacon:
            asyncio.create_task(self._beacon())
        while True:
            await asyncio.sleep(3600)

    async def _beacon(self):
        s = None
        while True:
            try:
                if s is None:
                    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                    try:
                        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
                    except Exception:
                        pass
                if not self.remote.connected:
                    s.sendto(encode({"t": "beacon", "serial": self.serial,
                                     "port": self.port}),
                             ("255.255.255.255", self.port))
            except Exception:
                if s is not None:
                    try:
                        s.close()
                    except Exception:
                        pass
                s = None
            await asyncio.sleep(2)

    async def _client(self, reader, writer):
        try:
            peer = writer.get_extra_info("peername")
            ip = peer[0] if peer else None
        except Exception:
            ip = None
        try:
            line = await asyncio.wait_for(reader.readline(), 5)
            hello = decode(line) if line else None
            if not hello or hello.get("t") != "hello" or hello.get("role") != "field":
                _close(writer)
                return
            if (hello.get("key") or "") != (self.cfg.get("split_key") or ""):
                self.log("[split] field unit %s rejected: wrong key" % hello.get("serial"))
                _close(writer)
                return
        except Exception:
            _close(writer)
            return
        if self._writer is not None:          # a newer connection replaces it
            _close(self._writer)
        self._gen += 1
        gen = self._gen
        self._writer = writer
        writer.write(encode(_hello("app", self.serial, self.cfg.get("split_key") or "",
                                   mac=self.mac)))
        await writer.drain()
        self.remote.on_connect(hello, ip)
        state = {"rx_at": ticks_ms()}

        def on_msg(msg):
            if msg["t"] == "ping":
                self.remote.queue({"t": "pong", "ts": msg.get("ts")})
            elif msg["t"] == "pong":
                self.remote.rtt_ms = ticks_diff(ticks_ms(), msg.get("ts") or 0)
            else:
                self.remote.apply(msg, ticks_ms())

        task = asyncio.create_task(_reader_loop(reader, on_msg, state, self.log))
        ping_at = ticks_ms()
        try:
            while gen == self._gen:
                now = ticks_ms()
                if ticks_diff(now, state["rx_at"]) > DEAD_MS:
                    self.log("[split] field unit timed out")
                    break
                if hasattr(task, "done") and task.done():
                    break                     # connection closed
                if ticks_diff(now, ping_at) >= PING_MS:
                    ping_at = now
                    self.remote.queue({"t": "ping", "ts": now})
                out = self.remote.take_outbox()
                if out:
                    writer.write(b"".join(encode(m) for m in out))
                    await writer.drain()
                await _sleep_ms(50)
        except Exception as exc:
            self.log("[split] connection error: %s" % exc)
        finally:
            try:
                task.cancel()
            except Exception:
                pass
            _close(writer)
            if gen == self._gen:
                self._writer = None
                self.remote.on_disconnect()


class SplitClient:
    """Field unit: find and connect to the app unit, then stream FieldRadio
    messages and apply its commands."""

    def __init__(self, fradio, cfg, serial, log=print, max_conn=4, mac=b""):
        self.fradio = fradio
        self.cfg = cfg
        self.serial = serial
        self.log = log
        self.port = int(cfg.get("split_port", 7389))
        self.max_conn = max_conn
        self.mac = mac
        self.connected = False
        self.peer = None
        self.rtt_ms = None
        self._outbox = []
        self._udp = None

    def status(self):
        return {"connected": self.connected,
                "peer": self.peer[0] if self.peer else None,
                "ip": self.peer[1] if self.peer else None, "rtt_ms": self.rtt_ms}

    async def _find_app(self, timeout_ms=5000):
        host = (self.cfg.get("split_peer") or "").strip()
        if host:
            return host
        if self._udp is None:
            try:
                s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
                s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                s.bind(("0.0.0.0", self.port))
                s.setblocking(False)
                self._udp = s
            except Exception as exc:
                self.log("[split] beacon listener failed: %s" % exc)
                return None
        t0 = ticks_ms()
        while ticks_diff(ticks_ms(), t0) < timeout_ms:
            try:
                data, addr = self._udp.recvfrom(256)
                msg = decode(data)
                if msg and msg.get("t") == "beacon":
                    self.port = int(msg.get("port", self.port))
                    return addr[0]
            except OSError:
                pass
            await _sleep_ms(200)
        return None

    async def run(self):
        backoff = 2
        while True:
            host = await self._find_app()
            if host:
                try:
                    await self._session(host)
                    backoff = 2
                except Exception as exc:
                    self.log("[split] connection to app unit failed: %s" % exc)
                self.fradio.drop_links()
                if self.connected:
                    self.log("[split] app unit offline")
                self.connected = False
                self.rtt_ms = None
            await asyncio.sleep(backoff)
            backoff = min(backoff * 2, 16)

    async def _session(self, host):
        reader, writer = await asyncio.open_connection(host, self.port)
        try:
            writer.write(encode(_hello("field", self.serial, self.cfg.get("split_key") or "",
                                       self.max_conn, self.mac)))
            await writer.drain()
            line = await asyncio.wait_for(reader.readline(), 5)
            hello = decode(line) if line else None
            if not hello or hello.get("t") != "hello":
                self.log("[split] app unit at %s refused the connection (key?)" % host)
                return
            self.connected = True
            self.peer = (hello.get("serial", "?"), host)
            self.fradio.reset()
            try:
                app_mac = bytes.fromhex(hello.get("mac") or "")
            except ValueError:
                app_mac = b""
            self.fradio.set_ignore(app_mac, hello.get("serial"))
            self.log("[split] connected to app unit %s at %s" % (self.peer[0], host))
            state = {"rx_at": ticks_ms()}

            def on_msg(msg):
                if msg["t"] == "ping":
                    self._outbox.append({"t": "pong", "ts": msg.get("ts")})
                elif msg["t"] == "pong":
                    self.rtt_ms = ticks_diff(ticks_ms(), msg.get("ts") or 0)
                else:
                    self.fradio.apply(msg)

            task = asyncio.create_task(_reader_loop(reader, on_msg, state, self.log))
            ping_at = scan_at = ticks_ms()
            try:
                while True:
                    now = ticks_ms()
                    if ticks_diff(now, state["rx_at"]) > DEAD_MS:
                        self.log("[split] app unit timed out")
                        return
                    out = self._outbox
                    self._outbox = []
                    out.extend(self.fradio.link_messages())
                    if ticks_diff(now, scan_at) >= SCAN_BATCH_MS:
                        scan_at = now
                        self.fradio.prune(now)
                        m = self.fradio.scan_message(now)
                        if m:
                            out.append(m)
                    if ticks_diff(now, ping_at) >= PING_MS:
                        ping_at = now
                        out.append({"t": "ping", "ts": now})
                    if out:
                        writer.write(b"".join(encode(m) for m in out))
                        await writer.drain()
                    if hasattr(task, "done") and task.done():
                        return
                    await _sleep_ms(100)
            finally:
                try:
                    task.cancel()
                except Exception:
                    pass
        finally:
            _close(writer)


async def _sleep_ms(ms):
    if hasattr(asyncio, "sleep_ms"):
        await asyncio.sleep_ms(ms)
    else:
        await asyncio.sleep(ms / 1000)


# ---------------------------------------------------------------------------
# Field unit status (web UI, screen, console)
# ---------------------------------------------------------------------------

class FieldView:
    """node.status()-shaped view of the field unit for web/screen/console."""

    def __init__(self, transport, client, fradio):
        self.t = transport
        self.client = client
        self.fradio = fradio

    def status(self):
        import protocol
        import meater
        now = ticks_ms()
        probes = []
        for serial, (body, ticks, _at, _addr, rssi) in list(self.t.direct.items()):
            d = protocol.parse_probe_advert(body)
            if not d:
                continue
            probes.append({
                "serial": protocol.probe_serial_str(serial), "id": d["probe_id"],
                "color": d["color"], "mode": d["mode"], "core_c": d["core_c"],
                "surface_c": d["surface_c"], "ambient_c": d["ambient_c"],
                "instant_c": d["instant_c"], "temps": d["temps"],
                "low_battery": d["low_battery"], "src": "advert", "hops": 0,
                "rssi": rssi, "age_s": ticks_diff(now, ticks) // 1000,
                "prediction": None, "alarming": False, "linked": False,
                "direct": True, "slot": None})
        devices = []
        for serial, (body, ticks, _at, _addr, rssi) in list(self.t.nodes.items()):
            d = protocol.parse_node_advert(body)
            if d is None:
                continue
            d = dict(d)
            d["serial"] = protocol.serial10_str(serial)
            d["name"] = protocol.PRODUCT_NAMES.get(d["ptype"], "Device")
            d["rssi"] = rssi
            d["age_s"] = ticks_diff(now, ticks) // 1000
            devices.append(d)
        meaters = []
        for addr, (name, ticks, _at, rssi) in list(self.t.meaters.items()):
            meaters.append({"addr": meater.addr_str(addr), "name": name or "MEATER",
                            "rssi": rssi, "age_s": ticks_diff(now, ticks) // 1000})
        links = []
        for key, link in self.t.links.items():
            if link.kind == "probe":
                name = protocol.probe_serial_str(key)
            elif link.kind == "meater":
                name = meater.addr_str(key)
            else:
                name = protocol.serial10_str(key)
            links.append({"kind": link.kind, "serial": name, "state": link.state,
                          "count": link.count, "mtu": link.mtu})
        sp = self.client.status()
        lines = ["app unit " + ("%s" % sp["peer"] if sp["connected"] else "offline"),
                 "links %d  probes %d" % (
                     sum(1 for l in links if l["state"] == "ready"), len(probes)),
                 "devices %d  MEATERs %d" % (len(devices), len(meaters))]
        return {"role": "field", "probes": probes, "devices": devices,
                "meaters": meaters, "links": links, "budget": 0,
                "inbound": 0, "max_conn": self.t.max_conn, "pending": 0,
                "stats": dict(self.fradio.stats, routed=0, status_sent=0),
                "tx_dropped": 0, "meater_bridge": False,
                "status_title": "Field unit", "status_lines": lines,
                "split": sp}
