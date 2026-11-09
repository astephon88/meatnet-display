# Plan: C++ field unit on ESP-IDF

## Why

In the two-unit setup the field unit does all the device-side Bluetooth work:
scanning, and holding links to probes, MEATERs and an upstream Gauge/Engine. It
is also where the remaining reliability problems are. In particular, the
Giant Grill Gauge link drops while the app browses gauge history, and during
start-up.

The MicroPython field unit runs the same NimBLE stack and radio firmware a C++
build would. What MicroPython hides is the control and the visibility we need
to diagnose and fix those drops:

- **Disconnect reason codes.** NimBLE reports why each connection ended:
  supervision timeout (radio/range), remote user terminated (the gauge hung
  up), failed to establish. MicroPython discards the reason.
- **Connection parameters.** That means the supervision timeout (NimBLE's
  default is about 2.5 s; phones allow 5 s or more) and the connection
  interval, set per link and renegotiable after connecting.
- **WiFi/Bluetooth coexistence.** Prefer Bluetooth on the shared radio.
  Stock MicroPython fixes this when the firmware is built.
- **Stack sizing:** connections, ACL buffers and mbufs, with no Python event
  queue that can overflow during log bursts.

The field unit is deliberately thin. All MeatNet logic stays on the app unit,
so only about 600 lines of Python need porting. The WiFi protocol (JSON over
TCP, `split.py`) doesn't change, so the C++ field unit works with the existing
Python app unit (ESP32 or the Linux host) as-is.

## Decisions

- **Framework:** ESP-IDF v5.x with the NimBLE host (central + observer roles
  only; the field unit never advertises). C++17 for the application, the C APIs
  of NimBLE/lwIP/esp_http_server, and cJSON (bundled with IDF) for JSON.
- **Targets:** ESP32-S3 (the Waveshare board) and plain ESP32. No PSRAM needed.
- **Configuration:** same experience as now. A setup access point
  (`MeatNet-XXXX`) with a captive page on first boot or when WiFi fails, then a
  small web page on the LAN. Settings are kept in NVS.
- **Screen:** headless in v1. A simple status screen is a later milestone.
- **Toolchain:** ESP-IDF installed on this host (`~/esp/esp-idf`,
  `~/.espressif`) when implementation starts. Builds and flashing run from
  here.
- **Location:** `field-idf/` in this repository. It's a standard ESP-IDF
  project and doesn't touch the MicroPython files.

## Compatibility contract (must match `split.py`)

The C++ unit is a drop-in replacement for `split.FieldRadio` +
`split.SplitClient` + the observer/central parts of `ble.BleTransport`.

**Connection**

- **Finding the app unit:** use the configured `split_peer` host/IP, else
  the first UDP beacon on `split_port` (default 7389):
  `{"t":"beacon","serial":…,"port":…}`.
- **Transport:** TCP to the app unit, newline-delimited JSON. Binary fields
  are base64 (mbedTLS `mbedtls_base64_*`); keys and addresses are lowercase hex.
- **Handshake:** send
  `{"t":"hello","v":1,"role":"field","serial":…,"key":…,"max_conn":N,"mac":hex}`,
  then expect the app's `hello` within 5 s. The app unit may close on a key
  mismatch. From the app's hello, remember `serial` and `mac`, and **drop
  every advertisement from the app unit's MAC**. Also drop Display adverts
  carrying its serial.
- **Optional fields:** extra fields are allowed and ignored by the app, e.g.
  `"impl":"idf","fw":"…"` in hello for display.
- **Liveness:** send `{"t":"ping","ts":ms}` every 2 s and answer pings with
  `{"t":"pong","ts":…}`. Treat 6 s of silence as dead. Reconnect with
  back-off 2→4→8→16 s.
- **On disconnect from the app unit:** close all links.

**Field → app**

- `scan`, every 250 ms. It contains only the entries updated since the last
  batch; everything is resent after each (re)connect:
  - `d` (direct probes, product type 1):
    `[serial4hex, body_b64, age_ms, addr_type, addrhex, rssi]`
  - `r` (repeated probe data, type 2, non-zero serial):
    `[serial4hex, body_b64, age_ms, rssi]`
  - `n` (nodes, type ≥ 3, keyed by 10-byte serial):
    `[serial10hex, body_b64, age_ms, addr_type, addrhex, rssi]`
  - `m` (MEATERs, matched by name prefix "MEATER" or service UUID
    `C9E2746C-…`/`DCBB67CA-…`): `[addrhex, name, age_ms, addr_type, rssi]`

  `body` is the manufacturer data after the company ID (0x09C7). Forget
  entries not heard for 60 s.
- `link` on any change of `(state, desc, mtu, info)`:
  `{"t":"link","k":hex,"kind":…,"state":…,"desc":…,"mtu":N,"count":N,"info":{…}}`.
  A removed link sends one final `state:"closed"`.
- `rx`, one per notification:
  `{"t":"rx","k":hex,"sub":name,"data":b64}`. Batch them into each 100 ms
  write.

**App → field**

- `open {k,kind,at,addr}`, `close {k}`, `write {k,data}`,
  `scan_active {on}`, `standby {on}`.

**Link kinds** (keys: probe = 4-byte serial, node = 10-byte serial,
MEATER = 6-byte address)

| kind | subscriptions (sub name → char) | write | reads |
|---|---|---|---|
| probe | `status` → Probe Status `00000101-CAAB-…` (required), `uart` → UART TX `6E400003-…` (optional) | UART RX `6E400002-…`, write type from char properties | DIS firmware/hardware/model (0x2A26/0x2A27/0x2A24) → `info` |
| node | `uart` → UART TX (required) | UART RX | – |
| meater | `temp` → `7EDDA774-…` (required), `battery` → `2ADB4877-…` (optional), found in **any** service | – | – |

**Link states** (strings the app unit logs and shows):

- the normal sequence: `idle`, `connecting`, `discovering`, `ready`,
  `disconnecting`;
- failures: `error: <text>`. The C++ unit adds the disconnect reason, e.g.
  `error: supervision timeout`, `error: gauge disconnected (remote
  terminated)`, `error: failed to establish`.

## Behaviour to port (lessons from the MicroPython unit)

- **Connections:**
  - one `ble_gap_connect` outstanding at a time;
  - a connection is claimed **only by exact address**, and a stray one is
    disconnected;
  - connect timeout 12 s; setup timeout 20 s (connected but not ready);
  - a disconnect that never completes is forgotten after 8 s;
  - error back-off 10 s for probes/MEATERs, 2 s for nodes;
  - `BLE_HS_EBUSY`/`EALREADY` are retried on the next tick, never treated
    as failures.
- **Setup sequence:** discover services, then characteristics, then CCCDs;
  enable notifications one at a time; exchange MTU; mark ready; then
  perform DIS reads one at a time. Standard 16-bit UUIDs are compared in
  their 16-bit form.
- **Scanning:**
  - passive, 50% duty while nothing is linked;
  - `linked_scan_duty` (default 20%) once any link exists;
  - 5% while any link receives more than 20 notifications/s, holding for 5 s
    after the burst ends;
  - about 3% in standby;
  - active scanning only while `scan_active` (MEATER discovery).
- **Buffers:** buffer at least 160 notifications per node link, so log
  reads can't overflow.

## New in the C++ unit

1. **Disconnect diagnostics.** Log and report every drop with its HCI reason,
   connection age and recent RX rate. Keep counters per link: connects,
   drops by reason. Show them on the field unit's page and include them in
   `link.desc`, so the app unit's log shows them too.
2. **Connection parameters.** Per kind, configurable:
   - supervision timeout: default 6 s (8 s for nodes);
   - connection interval: 30–50 ms (probes, nodes), 50–100 ms (MEATER);
   - slave latency 0.

   Applied at connect, and renegotiated with `ble_gap_update_params` if the
   peer changes them.
3. **Coexistence.** Prefer Bluetooth (`esp_coex_preference_set` /
   menuconfig coexistence options; check the current API in the chosen IDF
   version). Expose it as a setting, to compare against "balanced".
4. **Stack sizing** (sdkconfig):
   - `CONFIG_BT_NIMBLE_MAX_CONNECTIONS=6`;
   - more ACL buffers and mbufs;
   - NimBLE host task on core 0, application tasks on core 1 (S3);
   - peripheral and broadcaster roles disabled.

## Architecture

```
field-idf/
  CMakeLists.txt, sdkconfig.defaults, partitions.csv
  main/        app_main.cpp       NVS, config, start tasks, console
  components/
    config/    NVS-backed settings (+ JSON for the web page)
    wifi/      STA + setup-AP fallback, captive DNS, mDNS hostname, NTP
    radio/     NimBLE observer + central: Scanner, Link (state machine),
               LinkManager (one-connect-at-a-time, back-off, bursts, duty)
    split/     TCP client, UDP beacon listener, JSON codec (cJSON), base64,
               scan-delta tracking, link/rx message builders, command apply
    web/       esp_http_server: page, /api/state, /api/config, /api/wifi/scan,
               /api/reboot (same endpoints as web.py where it makes sense)
    proto/     pure helpers: advert parsing (manufacturer data, MEATER match),
               UUID handling. No IDF dependencies, so it's host-testable.
```

**Concurrency:**
- NimBLE callbacks only record events under a mutex. This is the same
  discipline as the MicroPython IRQ handler, minus the watchdog problem.
- A **radio task** (10 Hz) runs the link state machines and scan pacing.
- A **split task** owns the TCP session: it builds `scan` every 250 ms and
  `link`/`rx` every 100 ms from the shared state, and applies commands.
- `esp_http_server` runs its own task and reads the state under the same
  mutex.

## Testing

- **Host unit tests** for `proto/` and the split codec:
  - built either with ESP-IDF's `linux` target (Unity) or plain CMake;
  - covering advert parsing, MEATER matching, scan-delta encoding,
    base64/hex, and command parsing;
  - test vectors taken from the existing Python tests (e.g. the captured
    probe frame and MEATER layout).
- **Protocol compatibility test** (Python, in `tests/`): run the real
  `split.SplitServer` + `RemoteRadio` from this repo against a C++ unit on
  the LAN. Assert the hello exchange, scan rows, link lifecycle on
  `open`/`close`, `rx` delivery, `write` round-trip and liveness. This is
  the same harness style as `tests/test_split.py`'s TCP loopback.
- **On hardware:**
  - pair with the host app unit;
  - check probe, MEATER and gauge links come up and the app sees everything;
  - browse gauge history and compare the drop rate and **reasons** against
    the MicroPython unit;
  - try supervision timeout and coexistence settings;
  - run a multi-hour soak test (heap, reconnects).

## Milestones

1. **Toolchain and skeleton.** Install ESP-IDF v5.x. Create a project that
   builds for ESP32-S3 and ESP32 and boots with NVS, WiFi STA from compiled
   defaults, and NimBLE initialised. Flash the field board.
2. **Scanner + split session.** UDP beacon/peer, hello/ping, and `scan`
   batches with app-MAC filtering. Exit check: the app unit shows the
   devices heard and probes are re-advertised.
3. **Links (parity).** Probe, node and MEATER links with the full state
   machine, `rx`/`write`, DIS reads, back-off and timeouts, and the scan
   duty policy. Exit check: the app shows the probe, MEATER and gauge/Engine
   exactly as with the MicroPython unit.
4. **Diagnostics and tuning.** Disconnect reasons, counters, connection
   parameters, coexistence. Exit check: the gauge-history drop is
   explained, and fixed or reduced.
5. **Configuration UX.** Setup AP + captive page, settings page,
   NVS-backed settings, WiFi scan, reboot.
   - Step 1 (2026-11-08, working on the field board): `components/settings`
     (NVS namespace `meatnet`; Kconfig only supplies radio and port
     defaults, WiFi, the app unit address and the split key come from NVS
     only, and WiFi saved by older
     builds in the driver's storage is adopted once and cleared there) and
     `components/web` (page + `/api/state`, `/api/config`, `/api/reboot`;
     secrets are never sent back, only `*_set` flags). The open setup
     network `MeatNet-<serial>` comes up when no WiFi is set or none is
     reachable for 30 s.
   - Step 2 (2026-11-08, working with an Android phone): captive portal on
     the setup network: `components/captive` answers every DNS name with
     the unit, DHCP offers the unit as DNS and the page as RFC 8910
     captive-portal URI, and unknown paths arriving on the setup network
     redirect to the page (404 elsewhere). Optional WPA2 password for the
     setup network (`ap_password`, a secret like the others). While the
     setup network is up, WiFi is retried only every 60 s (none while a
     device is on it): each attempt scans all channels, and the setup
     network couldn't be joined meanwhile.
   - Step 3 (2026-11-08, working on the field board): WiFi scan on the
     page (`GET /api/wifi/scan`, same JSON as the MicroPython unit:
     strongest first, one entry per name, hidden networks left out). With
     the default scan (120 ms per channel, ~5 s) the split session timed
     out, which closes every device link; 20-60 ms per channel with 100 ms
     back home in between (and coex background scan) takes ~6.4 s and kept
     the session up through three scans in a row. An unconfigured unit runs
     the setup network next to an idle station, so the scan works there too
     (checked). The Scan button is disabled with a status line while a
     scan runs.
6. **Later: status screen** on the GC9A01A (small framebuffer driver, or
   LVGL): app unit link, devices heard, link states.
   - 2026-11-08 (working on the field board; previewed on the host first):
     `components/screen` drives the panel through esp_lcd's SPI panel IO
     with the MicroPython driver's init sequence, draws in 24-row bands
     (11.5 KB DMA buffer, no framebuffer), uses the MicroPython unit's
     `fonts/med.bin` and `small.bin`, and redraws only on change. Content:
     app unit (or the setup network to join), IP, devices heard, one line
     per link (`up`/`conn`/`down`), "+N more" when they don't fit. Pins and
     brightness in Kconfig (Waveshare defaults).

## Changes outside `field-idf/`

- `README.md`: a "C++ field unit" section (build, flash, settings).
- `tests/`: the protocol compatibility harness.
- No change to the app unit is required. Optionally it could show the field
  unit's implementation/version from the extra hello fields.

## Risks / open points

- **Coexistence API naming** differs between IDF versions; check against the
  installed version.
- **Gauge behaviour.** If the reason codes show the gauge itself terminating
  under load, no field-side setting will fully fix it. The diagnostics will
  say so, and fast recovery (milestone 3) remains the mitigation.
- **Behaviour parity.** The MicroPython unit's behaviour was tuned
  empirically. The compatibility test and a side-by-side run against the
  same devices guard against regressions.
