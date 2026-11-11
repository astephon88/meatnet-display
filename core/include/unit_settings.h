// Unit settings: the keys and defaults of config.py's DEFAULTS, plus the
// C++ field unit's radio settings. Loading/saving them (JSON in NVS or a
// file) is the platform's job; the Node and the rest of core read this.
#pragma once

#include <string>
#include <vector>

namespace core {

struct MeaterProbe {
    std::string addr;           // "AA:BB:CC:DD:EE:FF"
    int id = 8;                 // probe ID 1..8 shown in the app
    int color = 0;
};

struct Reading {
    std::string dev;            // "probe:1000ABCD", "node:G000001CA8", ...
    std::string field;          // "core", "surface", "ambient", "t1".., "temp", ...
    std::string label;
};

// Waveshare ESP32-S3-LCD-1.28 (non-touch) defaults.
struct Pins {
    int sck = 10, mosi = 11, dc = 8, cs = 9, rst = 12, bl = 40, spi = 2;
    int baud = 80000000;
    int rotation = 0;
};

struct Settings {
    std::string role = "standalone";    // "standalone", "app" or "field"
    std::string split_peer;             // field unit: app unit host/IP, "" = beacon
    std::string split_key;              // secret
    int split_port = 7389;
    std::string wifi_ssid;
    std::string wifi_password;          // secret
    std::string ap_password;            // secret; "" = open setup network
    std::string hostname = "meatnet";
    std::string serial;                 // "" = derived from the MAC
    std::string dis_model = "Timer REL-00001";
    std::string dis_firmware = "v2.6.0";
    std::string unit = "c";
    std::string layout = "rotate";      // "rotate" or "quad"
    int rotate_s = 5;
    int brightness = 80;
    bool screen = true;
    bool status_page = true;
    std::vector<Reading> readings;
    std::vector<std::string> linked_probes;     // probe serials (hex) to link to
    std::vector<std::string> upstream_node;     // node serials to link to
    std::string repeat_adverts = "all";         // "all", "linked" or "off"
    bool meatnet_peer = true;           // heartbeats, link tables, connect notices
    // Also push our status/heartbeats/syncs and association to the upstream
    // node (a Gauge drops nodes that do this).
    bool upstream_push = false;
    int max_conn = 4;
    int linked_scan_duty = 20;          // % of the time scanning while links are up
    int inbound_reserve = 1;            // connections kept free for phones
    bool meater_bridge = false;
    std::vector<MeaterProbe> meater_probes;
    // Also advertise bridged MEATERs as a direct (non-connectable) probe, so
    // apps discover them.
    bool meater_direct_adverts = true;
    bool standby = false;
    int auto_standby_min = 0;           // 0 = off
    int battery_pin = 1;                // ADC pin of the battery divider, -1 = none
    double battery_divider = 3.0;       // V(bat) / V(pin): 200k/100k on the Waveshare
    Pins pins;
    // C++ radio (firmware)
    int supervision_probe_ms = 6000;
    int supervision_node_ms = 20000;
    int peer_latency_max = 0;
    std::string coex = "bt";            // "bt", "balance" or "wifi"
};

}  // namespace core
