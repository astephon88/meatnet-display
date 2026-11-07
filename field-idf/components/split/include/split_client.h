// WiFi session from the field unit to the app unit (C++ counterpart of the
// Python split.SplitClient + the reporting half of split.FieldRadio).
//
// Wire protocol (see split.py): newline-delimited JSON over TCP; binary as
// base64, keys/addresses as hex. The app unit is found via a configured peer
// or its UDP beacon. This milestone streams the scan tables and handles
// scan_active/standby; links (open/close/write, link/rx) come in milestone 3.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

struct cJSON;

namespace split {

struct Config {
    std::string peer;           // app unit host/IP; empty = UDP beacon
    std::string key;            // shared key
    uint16_t port = 7389;
    std::string serial;         // our serial (10 chars)
    int max_conn = 4;           // outbound connections we offer
    std::string mac;            // our BLE address, display order (6 bytes)
};

class Client {
public:
    explicit Client(const Config &cfg) : cfg_(cfg) {}
    void start();               // runs in its own FreeRTOS task

    bool connected() const { return connected_; }
    std::string peer() const { return peer_serial_; }
    int rtt_ms() const { return rtt_ms_; }

private:
    static void task(void *arg);
    void run();
    bool find_app(std::string &host);
    void session(const std::string &host);
    void handle(cJSON *msg, std::vector<std::string> &out);
    std::string scan_message(int64_t now);

    Config cfg_;
    bool connected_ = false;
    std::string peer_serial_;
    int rtt_ms_ = -1;
    bool warned_links_ = false;
    std::map<std::string, int64_t> sent_;   // table+key -> sighting time last sent
};

}  // namespace split
