"""MeatNet link/unlink tables (messages 0x73-0x7B), persisted as JSON."""

import json

import protocol


def _key(e):
    return (e["ptype"] == protocol.PRODUCT_TYPE_PROBE, bytes(e["serial"]))


def _to_json(e):
    return {"t": e["ptype"], "s": bytes(e["serial"]).hex(), "ts": e.get("ts", 0),
            "l": 1 if e.get("legacy") else 0}


def _from_json(d):
    return {"ptype": d["t"], "serial": bytes.fromhex(d["s"]), "ts": d.get("ts", 0),
            "legacy": bool(d.get("l"))}


class LinkTable:
    def __init__(self, path="links.json"):
        self.path = path
        self.linked = []
        self.unlinked = []
        self.proximity = False
        self._load()

    def _load(self):
        if not self.path:
            return
        try:
            with open(self.path) as f:
                d = json.load(f)
            self.linked = [_from_json(e) for e in d.get("linked", [])]
            self.unlinked = [_from_json(e) for e in d.get("unlinked", [])]
            self.proximity = bool(d.get("proximity", False))
        except Exception:
            pass

    def save(self):
        if not self.path:
            return
        try:
            with open(self.path, "w") as f:
                json.dump({"linked": [_to_json(e) for e in self.linked],
                           "unlinked": [_to_json(e) for e in self.unlinked],
                           "proximity": self.proximity}, f)
        except Exception as exc:
            print("[links] save failed:", exc)

    @staticmethod
    def _remove(lst, entry):
        k = _key(entry)
        n = len(lst)
        lst[:] = [e for e in lst if _key(e) != k]
        return len(lst) != n

    def _put(self, lst, other, entry):
        """Insert/refresh entry in lst, remove it from other. Returns True if
        anything changed."""
        changed = self._remove(other, entry)
        k = _key(entry)
        for e in lst:
            if _key(e) == k:
                if e.get("ts", 0) != entry.get("ts", 0) and entry.get("ts", 0):
                    e["ts"] = entry["ts"]
                    changed = True
                if e.get("legacy") and not entry.get("legacy"):
                    e["legacy"] = False
                    changed = True
                return changed
        lst.append({"ptype": entry["ptype"], "serial": bytes(entry["serial"]),
                    "ts": entry.get("ts", 0), "legacy": bool(entry.get("legacy"))})
        return True

    def link(self, entry):
        changed = self._put(self.linked, self.unlinked, entry)
        if changed:
            self.save()
        return changed

    def unlink(self, entry):
        changed = self._put(self.unlinked, self.linked, entry)
        if changed:
            self.save()
        return changed

    def merge(self, entries, linked, own_serial10, sender_serial10):
        """Merge entries pulled from a peer. A node never stores its own
        serial: an entry naming us refers to the sending peer instead, and is
        dropped if the sender is unknown (spec: Sync Link Table receiver
        behaviour)."""
        own = protocol.encode_serial(own_serial10)
        changed = False
        for e in entries:
            if (e["ptype"] != protocol.PRODUCT_TYPE_PROBE
                    and protocol.encode_serial(e["serial"]) == own):
                if not sender_serial10:
                    continue
                e = dict(e)
                e["serial"] = protocol.encode_serial(sender_serial10)
            lst, other = ((self.linked, self.unlinked) if linked
                          else (self.unlinked, self.linked))
            if self._put(lst, other, e):
                changed = True
        if changed:
            self.save()
        return changed
