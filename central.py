"""BLE central-role link (MicroPython).

A CentralLink owns one outbound GATT connection to a peripheral (a probe or
another MeatNet node):

    connect -> discover services -> discover characteristics per wanted
            service -> find each notify char's CCCD -> enable notifications
            one by one -> exchange MTU -> ready

The on_* methods are called from the BLE IRQ and only record state; every
BLE stack call happens in service() on the main loop (calling gap/gattc APIs
from the IRQ panics the interrupt watchdog under WiFi coexistence).
"""

import bluetooth

from util import ticks_ms, ticks_diff

_CCCD = bluetooth.UUID(0x2902)
_BASE_SUFFIX = "-0000-1000-8000-00805F9B34FB"


def _uuid(text):
    """bluetooth.UUID for a UUID string. Standard-base UUIDs become their
    16-bit form: that's how the stack reports them (UUID(0x180A)), and a
    128-bit UUID never compares equal to it."""
    t = text.upper()
    if t.startswith("0000") and t.endswith(_BASE_SUFFIX) and len(t) == 36:
        return bluetooth.UUID(int(t[4:8], 16))
    return bluetooth.UUID(text)
CONNECT_TIMEOUT_MS = 12000   # abandon a connect that never completes
ERROR_BACKOFF_MS = 10000     # wait this long after an error before retrying
SETUP_TIMEOUT_MS = 20000     # connected but not ready after this long: give up
DISCONNECT_TIMEOUT_MS = 8000  # a disconnect that never completes: forget it
# Errors that mean "the stack is busy, try again" (newlib and MicroPython
# errno values, either sign): EBUSY, EALREADY.
_RETRY_ERRNOS = (16, -16, 120, -120, 114, -114)
WRITE_TIMEOUT_MS = 3000      # give up waiting for a write acknowledgement
_PROP_WRITE_NO_RESPONSE = 0x04
_PROP_WRITE = 0x08


class CentralLink:
    """subs: [(name, service uuid str, notify char uuid str, required)];
    a service of None means "look in every service" (characteristics found
    by UUID wherever they live).
    write: (service uuid str, write char uuid str) or None."""

    def __init__(self, ble, name, subs, write=None, keep=64, write_mode=0,
                 kind="probe", reads=None, backoff_ms=ERROR_BACKOFF_MS):
        self.ble = ble
        self.name = name
        self.kind = kind
        # 1 = write with response (paced by the ack), 0 = without response
        # (what Combustion's own apps use for the MeatNet UART). Replaced by
        # what the write characteristic actually supports once discovered.
        self.default_write_mode = write_mode
        self.write_mode = write_mode
        self.subs = [(n, _uuid(s) if s else None, _uuid(c), req)
                     for n, s, c, req in subs]
        self.any_service = any(s is None for _, s, _, _ in self.subs)
        self.write_spec = ((_uuid(write[0]), _uuid(write[1])) if write else None)
        # reads: [(name, service uuid, char uuid)] read once after the link is
        # ready (e.g. the probe's Device Information strings) into .info.
        self.read_spec = [(n, _uuid(s), _uuid(ch)) for n, s, ch in (reads or ())]
        self.info = {}
        self.keep = keep
        self.backoff_ms = backoff_ms
        self.enabled = False
        self.target = None       # (addr_type, addr bytes)
        self.key = None          # what we're connected to (serial bytes)
        self.count = 0
        self.sent = 0
        self.state = "idle"
        self.mtu = 23
        self.connected_at = None
        self._err_at = None
        self._reset()

    def _reset(self):
        self.conn = None
        self.svcs = {}           # service uuid -> (start, end)
        self.handles = {}        # sub name -> value handle
        self.cccds = {}          # sub name -> cccd handle
        self.write_h = None
        self.write_mode = getattr(self, "default_write_mode", 0)
        self.rx = {}             # sub name -> [notification bytes]
        self.action = None
        self.started = None
        self.tx_queue = []
        self.tx_busy = False
        self.tx_at = None
        self.disconnect_sent = False
        self._disc_at = None
        self._char_todo = []
        self._char_svc = None
        self._desc_todo = []
        self._desc_cur = None
        self._notify_todo = []
        self.read_h = {}         # read name -> value handle
        self._read_todo = []
        self._reading = None

    @property
    def ready(self):
        return self.state == "ready"

    def set_target(self, addr_type, addr, key=None):
        self.target = (addr_type, bytes(addr))
        self.key = key

    def _fail(self, why):
        # Keep the "error" state through the disconnect so the back-off applies.
        self.state = "error: " + why
        self._err_at = ticks_ms()
        if self.conn is None:
            self.started = None
        self.action = ("disconnect" if self.conn is not None
                       and not self.disconnect_sent else None)

    def _single_service(self):
        """The one service UUID every subscription and the write use, or
        None (several services, or "any service")."""
        if self.any_service:
            return None
        svcs = [s for _, s, _, _ in self.subs]
        if self.write_spec:
            svcs.append(self.write_spec[0])
        svcs += [s for _, s, _ in self.read_spec]
        first = svcs[0]
        for s in svcs[1:]:
            if s != first:
                return None
        return first

    def _svc_end(self, handle):
        for start, end in self.svcs.values():
            if start <= handle <= end:
                return end
        return 0xFFFF

    # -- IRQ side: record state only ----------------------------------------

    def on_connect(self, conn):
        self.conn = conn
        self.started = None
        self.connected_at = ticks_ms()
        self.state = "discovering"
        self.action = "discover_svc"

    def on_disconnect(self):
        self._reset()
        self.mtu = 23
        self.connected_at = None
        if not self.state.startswith("error"):
            self.state = "idle"

    def on_service(self, start, end, uuid):
        if self.any_service:
            self.svcs[_ukey(uuid)] = (start, end)
            return
        wanted = [s for _, s, _, _ in self.subs]
        if self.write_spec:
            wanted.append(self.write_spec[0])
        wanted += [s for _, s, _ in self.read_spec]
        for w in wanted:
            if uuid == w:
                self.svcs[_ukey(w)] = (start, end)

    def on_service_done(self):
        for n, s, _, req in self.subs:
            if req and s is not None and _ukey(s) not in self.svcs:
                self._fail("service not found")
                return
        self._char_todo = list(self.svcs.values())
        self._next_char_step()

    def _next_char_step(self):
        if self._char_todo:
            self._char_svc = self._char_todo.pop(0)
            self.action = "discover_char"
            return
        for n, _, _, req in self.subs:
            if req and n not in self.handles:
                self._fail("notify characteristic not found")
                return
        self._desc_todo = [n for n, _, _, _ in self.subs if n in self.handles]
        self._next_desc_step()

    def on_char(self, value_handle, uuid, properties=None):
        for n, _, c, _ in self.subs:
            if uuid == c:
                self.handles[n] = value_handle
        for n, _, c in self.read_spec:
            if uuid == c:
                self.read_h[n] = value_handle
        if self.write_spec and uuid == self.write_spec[1]:
            self.write_h = value_handle
            if properties is not None:
                if properties & _PROP_WRITE_NO_RESPONSE:
                    self.write_mode = self.default_write_mode
                elif properties & _PROP_WRITE:
                    self.write_mode = 1     # peer only accepts acked writes

    def on_char_done(self):
        self._next_char_step()

    def _next_desc_step(self):
        if self._desc_todo:
            self._desc_cur = self._desc_todo.pop(0)
            self.action = "discover_desc"
        else:
            self._notify_todo = [n for n in self.cccds]
            self.action = "enable_notify"

    def on_desc(self, handle, uuid):
        n = self._desc_cur
        if (n is not None and n not in self.cccds
                and handle > self.handles[n] and uuid == _CCCD):
            self.cccds[n] = handle

    def on_desc_done(self):
        self._next_desc_step()

    def on_notify(self, value_handle, data):
        for n, h in self.handles.items():
            if h == value_handle:
                q = self.rx.get(n)
                if q is None:
                    q = self.rx[n] = []
                q.append(bytes(data))
                self.count += 1
                if len(q) > self.keep:
                    q.pop(0)
                return

    def on_read_result(self, value_handle, data):
        if self._reading is not None:
            raw = bytes(data).split(b"\x00")[0]
            try:
                self.info[self._reading] = raw.decode()
            except Exception:
                self.info[self._reading] = ""

    def on_read_done(self):
        self._reading = None
        self.tx_busy = False

    def on_write_done(self):
        self.tx_busy = False

    def on_mtu(self, mtu):
        self.mtu = mtu
        self.tx_busy = False

    # -- main loop ----------------------------------------------------------

    def describe(self):
        """One-line summary of what the link set up (for the log)."""
        subs = ", ".join("%s notify %s" % (n, "on" if n in self.cccds else "OFF")
                         for n, _, _, _ in self.subs)
        if self.write_spec:
            subs += ", write %s (%s)" % (
                "h%d" % self.write_h if self.write_h else "MISSING",
                "with response" if self.write_mode else "no response")
        return subs

    def take(self, name):
        """Notifications received on the named subscription since last call."""
        q = self.rx.get(name)
        if not q:
            return []
        self.rx[name] = []
        return q

    def write(self, data):
        """Queue data for the write characteristic. It is chunked to fit the
        MTU when sent, one chunk per service() call."""
        if not self.ready or self.write_h is None:
            return False
        self.tx_queue.append(bytes(data))
        return True

    def service(self, may_connect):
        now = ticks_ms()
        # A connect that never completes (e.g. a stale link on the peer).
        if (self.state == "connecting" and self.started
                and ticks_diff(now, self.started) > CONNECT_TIMEOUT_MS):
            try:
                self.ble.gap_connect(None)
            except OSError:
                pass
            self._reset()
            self.state = "idle"
        # Connected but stuck in discovery/setup (e.g. a lost IRQ).
        if (self.conn is not None and self.state not in ("ready", "disconnecting")
                and not self.state.startswith("error")
                and self.connected_at is not None
                and ticks_diff(now, self.connected_at) > SETUP_TIMEOUT_MS):
            self._fail("setup timed out")
        # A disconnect whose event never arrives: treat the link as gone.
        if (self.conn is not None and self.disconnect_sent
                and self._disc_at is not None
                and ticks_diff(now, self._disc_at) > DISCONNECT_TIMEOUT_MS):
            self._reset()
            self.mtu = 23
            if not self.state.startswith("error"):
                self.state = "idle"
        # Recover from an error after a back-off.
        if (self.state.startswith("error") and self.conn is None
                and self._err_at is not None
                and ticks_diff(now, self._err_at) > self.backoff_ms):
            self.state = "idle"
        # Stop when disabled; otherwise start connecting when allowed.
        if not self.enabled:
            if self.state == "connecting":
                try:
                    self.ble.gap_connect(None)
                except OSError:
                    pass
                self._reset()
                self.state = "idle"
            elif self.conn is not None and not self.disconnect_sent:
                self.action = "disconnect"
        elif (self.state == "idle" and self.target and may_connect
              and self.action is None and self.conn is None):
            self.action = "connect"
        # One-off reads (Device Information) once ready, one at a time.
        if (self.ready and self._read_todo and not self.tx_busy
                and self._reading is None):
            name = self._read_todo.pop(0)
            try:
                self.ble.gattc_read(self.conn, self.read_h[name])
                self._reading = name
                self.tx_busy = True
                self.tx_at = now
            except OSError:
                pass
        if (self._reading is not None and self.tx_busy
                and ticks_diff(now, self.tx_at) > WRITE_TIMEOUT_MS):
            self._reading = None      # no answer: skip that string
            self.tx_busy = False
        # Drain queued writes, one outstanding at a time.
        if self.ready and self.tx_queue:
            if self.tx_busy and ticks_diff(now, self.tx_at) > WRITE_TIMEOUT_MS:
                self.tx_busy = False
            if not self.tx_busy:
                data = self.tx_queue[0]
                n = max(20, self.mtu - 3)
                try:
                    self.ble.gattc_write(self.conn, self.write_h, data[:n],
                                         self.write_mode)
                    self.sent += 1
                    if self.write_mode:
                        self.tx_busy = True
                        self.tx_at = now
                    if len(data) > n:
                        self.tx_queue[0] = data[n:]
                    else:
                        self.tx_queue.pop(0)
                except OSError:
                    pass       # retry next tick
        act = self.action
        if not act:
            return
        if act in ("mtu", "enable_notify") and self.tx_busy:
            # One ATT request at a time: let the previous CCCD write finish.
            if ticks_diff(now, self.tx_at) <= WRITE_TIMEOUT_MS:
                return
            self.tx_busy = False
        self.action = None
        try:
            if act == "connect":
                self.state = "connecting"
                self.started = now
                self.ble.gap_connect(self.target[0], self.target[1])
            elif act == "discover_svc":
                only = self._single_service()
                if only is not None:
                    # Fewer round trips over a weak link: ask for just the
                    # one service we need.
                    self.ble.gattc_discover_services(self.conn, only)
                else:
                    self.ble.gattc_discover_services(self.conn)
            elif act == "discover_char":
                self.ble.gattc_discover_characteristics(
                    self.conn, self._char_svc[0], self._char_svc[1])
            elif act == "discover_desc":
                h = self.handles[self._desc_cur]
                self.ble.gattc_discover_descriptors(self.conn, h,
                                                    self._svc_end(h))
            elif act == "enable_notify":
                if not self._notify_todo:
                    for n, _, _, req in self.subs:
                        if req and n not in self.cccds:
                            self._fail("no CCCD for notifications")
                            return
                    self.action = "mtu"
                    return
                n = self._notify_todo.pop(0)
                self.ble.gattc_write(self.conn, self.cccds[n], b"\x01\x00", 1)
                self.tx_busy = True
                self.tx_at = now
                self.action = "enable_notify"
            elif act == "mtu":
                # Exchange the MTU before reporting ready so the first frame
                # isn't split into 20-byte writes.
                try:
                    self.ble.gattc_exchange_mtu(self.conn)
                    self.tx_busy = True     # hold writes until it completes
                    self.tx_at = now
                except OSError:
                    pass                    # peer may have exchanged already
                self._read_todo = [n for n in self.read_h if n not in self.info]
                self.state = "ready"
            elif act == "disconnect":
                if self.conn is not None and not self.disconnect_sent:
                    self.disconnect_sent = True
                    self._disc_at = now
                    if not self.state.startswith("error"):
                        self.state = "disconnecting"
                    self.ble.gap_disconnect(self.conn)
        except OSError as exc:
            code = exc.args[0] if exc.args else None
            if code in _RETRY_ERRNOS:
                # Stack busy (another link's discovery, a pending connect):
                # try the same step again on the next tick.
                if act == "connect":
                    try:
                        self.ble.gap_connect(None)   # cancel a stale attempt
                    except OSError:
                        pass
                    self.state = "idle"
                    self.started = None
                elif act == "disconnect":
                    self.disconnect_sent = False
                    self.action = act
                else:
                    self.action = act
                return
            self._fail(_describe_error(exc))


def _describe_error(exc):
    """Readable text for the BLE stack errors seen in practice."""
    code = exc.args[0] if exc.args else None
    if code in (-128, 128):          # MP_ENOTCONN
        return "connection lost (out of range?)"
    if code in (-116, 116, -110, 110):  # ETIMEDOUT
        return "timed out"
    if code in (-12, 12):            # ENOMEM: too many connections
        return "no free connection (BLE limit)"
    return str(exc)


def _ukey(uuid):
    """Hashable key for a bluetooth.UUID."""
    return str(uuid)
