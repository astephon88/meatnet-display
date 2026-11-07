#include "split_client.h"

#include <cstring>

#include "cJSON.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"
#include "mbedtls/base64.h"
#include "proto.h"
#include "radio.h"

namespace split {

static const char *TAG = "split";

static constexpr int kVersion = 1;
static constexpr int64_t kPingMs = 2000;
static constexpr int64_t kDeadMs = 6000;
static constexpr int64_t kScanBatchMs = 250;

// -- helpers ---------------------------------------------------------------------

static std::string b64(const std::string &bytes)
{
    size_t olen = 0;
    std::string out(4 * ((bytes.size() + 2) / 3) + 1, '\0');
    mbedtls_base64_encode(reinterpret_cast<unsigned char *>(&out[0]), out.size(), &olen,
                          reinterpret_cast<const unsigned char *>(bytes.data()), bytes.size());
    out.resize(olen);
    return out;
}

static std::string node_serial(const std::string &text)
{
    std::string s = text.substr(0, 10);
    s.resize(10, '\0');
    return s;
}

static std::string json_line(cJSON *obj)
{
    char *txt = cJSON_PrintUnformatted(obj);
    std::string line = txt ? std::string(txt) + "\n" : std::string();
    cJSON_free(txt);
    cJSON_Delete(obj);
    return line;
}

static const char *str_field(cJSON *msg, const char *name)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(msg, name);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

static bool send_all(int fd, const std::string &data)
{
    size_t off = 0;
    while (off < data.size()) {
        int n = send(fd, data.data() + off, data.size() - off, 0);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

// -- task ---------------------------------------------------------------------------

void Client::start()
{
    xTaskCreate(task, "split", 8192, this, 5, nullptr);
}

void Client::task(void *arg)
{
    static_cast<Client *>(arg)->run();
}

void Client::run()
{
    int backoff_s = 2;
    while (true) {
        std::string host;
        if (find_app(host)) {
            session(host);
            if (connected_) backoff_s = 2;
            if (connected_) ESP_LOGW(TAG, "app unit offline");
            connected_ = false;
            rtt_ms_ = -1;
        }
        vTaskDelay(pdMS_TO_TICKS(backoff_s * 1000));
        backoff_s = backoff_s * 2 > 16 ? 16 : backoff_s * 2;
    }
}

bool Client::find_app(std::string &host)
{
    if (!cfg_.peer.empty()) {
        host = cfg_.peer;
        return true;
    }
    // Listen for the app unit's beacon: {"t":"beacon","serial":...,"port":N}
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    bool found = false;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) == 0) {
        char buf[256];
        struct sockaddr_in from = {};
        socklen_t fl = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<struct sockaddr *>(&from), &fl);
        if (n > 0) {
            buf[n] = 0;
            cJSON *msg = cJSON_Parse(buf);
            const char *t = msg ? str_field(msg, "t") : nullptr;
            if (t && strcmp(t, "beacon") == 0) {
                cJSON *p = cJSON_GetObjectItemCaseSensitive(msg, "port");
                if (cJSON_IsNumber(p)) cfg_.port = static_cast<uint16_t>(p->valueint);
                char ip[16];
                inet_ntoa_r(from.sin_addr, ip, sizeof(ip));
                host = ip;
                found = true;
            }
            cJSON_Delete(msg);
        }
    }
    close(fd);
    return found;
}

void Client::session(const std::string &host)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", cfg_.port);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "can't resolve app unit '%s'", host.c_str());
        return;
    }
    int fd = socket(res->ai_family, res->ai_socktype, 0);
    if (fd < 0) {
        freeaddrinfo(res);
        return;
    }
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection to app unit %s:%s failed (errno %d)", host.c_str(), port, errno);
        close(fd);
        return;
    }

    // Handshake.
    radio::Radio &r = radio::Radio::get();
    cJSON *hello = cJSON_CreateObject();
    cJSON_AddStringToObject(hello, "t", "hello");
    cJSON_AddNumberToObject(hello, "v", kVersion);
    cJSON_AddStringToObject(hello, "role", "field");
    cJSON_AddStringToObject(hello, "serial", cfg_.serial.c_str());
    cJSON_AddStringToObject(hello, "key", cfg_.key.c_str());
    cJSON_AddNumberToObject(hello, "max_conn", cfg_.max_conn);
    cJSON_AddStringToObject(hello, "mac", proto::hex(cfg_.mac).c_str());
    cJSON_AddStringToObject(hello, "impl", "idf");
    if (!send_all(fd, json_line(hello))) {
        close(fd);
        return;
    }

    std::string buf;
    int64_t deadline = radio::Radio::now_ms() + 5000;
    cJSON *reply = nullptr;
    while (!reply && radio::Radio::now_ms() < deadline) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval w = {0, 200000};
        if (select(fd + 1, &rs, nullptr, nullptr, &w) <= 0) continue;
        char tmp[512];
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        size_t nl = buf.find('\n');
        if (nl != std::string::npos) {
            reply = cJSON_Parse(buf.substr(0, nl).c_str());
            buf.erase(0, nl + 1);
        }
    }
    const char *t = reply ? str_field(reply, "t") : nullptr;
    if (!t || strcmp(t, "hello") != 0) {
        ESP_LOGW(TAG, "app unit at %s refused the connection (key?)", host.c_str());
        cJSON_Delete(reply);
        close(fd);
        return;
    }
    const char *serial = str_field(reply, "serial");
    const char *mac = str_field(reply, "mac");
    peer_serial_ = serial ? serial : "?";
    std::string mac_bytes;
    if (mac) proto::unhex(mac, mac_bytes);
    cJSON_Delete(reply);
    r.set_ignore(mac_bytes, node_serial(peer_serial_));
    sent_.clear();
    connected_ = true;
    ESP_LOGI(TAG, "connected to app unit %s at %s", peer_serial_.c_str(), host.c_str());

    // Session loop.
    int64_t rx_at = radio::Radio::now_ms();
    int64_t ping_at = rx_at, scan_at = 0;
    std::vector<std::string> out;
    while (true) {
        int64_t now = radio::Radio::now_ms();
        if (now - rx_at > kDeadMs) {
            ESP_LOGW(TAG, "app unit timed out");
            break;
        }
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(fd, &rs);
        struct timeval w = {0, 100000};
        int sel = select(fd + 1, &rs, nullptr, nullptr, &w);
        if (sel > 0) {
            char tmp[1024];
            int n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;          // closed
            rx_at = radio::Radio::now_ms();
            buf.append(tmp, n);
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                cJSON *msg = cJSON_Parse(buf.substr(0, nl).c_str());
                buf.erase(0, nl + 1);
                if (msg) {
                    handle(msg, out);
                    cJSON_Delete(msg);
                }
            }
            if (buf.size() > 16384) buf.clear();   // runaway garbage
        }
        now = radio::Radio::now_ms();
        if (now - scan_at >= kScanBatchMs) {
            scan_at = now;
            r.prune(now);
            std::string m = scan_message(now);
            if (!m.empty()) out.push_back(m);
        }
        if (now - ping_at >= kPingMs) {
            ping_at = now;
            cJSON *p = cJSON_CreateObject();
            cJSON_AddStringToObject(p, "t", "ping");
            cJSON_AddNumberToObject(p, "ts", static_cast<double>(now));
            out.push_back(json_line(p));
        }
        if (!out.empty()) {
            std::string all;
            for (auto &l : out) all += l;
            out.clear();
            if (!send_all(fd, all)) break;
        }
    }
    close(fd);
}

void Client::handle(cJSON *msg, std::vector<std::string> &out)
{
    const char *t = str_field(msg, "t");
    if (!t) return;
    radio::Radio &r = radio::Radio::get();
    if (strcmp(t, "ping") == 0) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "t", "pong");
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(msg, "ts");
        cJSON_AddNumberToObject(p, "ts", cJSON_IsNumber(ts) ? ts->valuedouble : 0);
        out.push_back(json_line(p));
    } else if (strcmp(t, "pong") == 0) {
        cJSON *ts = cJSON_GetObjectItemCaseSensitive(msg, "ts");
        if (cJSON_IsNumber(ts))
            rtt_ms_ = static_cast<int>(radio::Radio::now_ms() - static_cast<int64_t>(ts->valuedouble));
    } else if (strcmp(t, "scan_active") == 0) {
        r.set_scan_active(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "on")));
    } else if (strcmp(t, "standby") == 0) {
        r.set_standby(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(msg, "on")));
    } else if (strcmp(t, "open") == 0 || strcmp(t, "close") == 0 || strcmp(t, "write") == 0) {
        if (!warned_links_) {
            warned_links_ = true;
            ESP_LOGW(TAG, "app unit asked for links ('%s'); links arrive in milestone 3", t);
        }
    }
}

// Scan-table entries updated since the last batch, in split.py's row format.
std::string Client::scan_message(int64_t now)
{
    radio::Radio &r = radio::Radio::get();
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "t", "scan");
    int rows = 0;
    struct TableRef {
        const char *name;
        radio::Table *table;
    } tables[] = {{"d", &r.direct}, {"r", &r.repeated}, {"n", &r.nodes}, {"m", &r.meaters}};
    r.lock();
    for (auto &tr : tables) {
        cJSON *arr = nullptr;
        for (auto &kv : *tr.table) {
            const std::string &key = kv.first;
            const radio::Sighting &s = kv.second;
            std::string sent_key = std::string(tr.name) + key;
            auto it = sent_.find(sent_key);
            if (it != sent_.end() && it->second == s.ms) continue;
            sent_[sent_key] = s.ms;
            cJSON *row = cJSON_CreateArray();
            double age = static_cast<double>(now - s.ms);
            cJSON_AddItemToArray(row, cJSON_CreateString(proto::hex(key).c_str()));
            if (tr.name[0] == 'm') {
                cJSON_AddItemToArray(row, cJSON_CreateString(s.name.c_str()));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(age));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(s.addr_type));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(s.rssi));
            } else {
                cJSON_AddItemToArray(row, cJSON_CreateString(b64(s.body).c_str()));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(age));
                if (tr.name[0] == 'r') {
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(s.rssi));
                } else {
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(s.addr_type));
                    cJSON_AddItemToArray(row, cJSON_CreateString(proto::hex(s.addr).c_str()));
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(s.rssi));
                }
            }
            if (!arr) {
                arr = cJSON_CreateArray();
                cJSON_AddItemToObject(msg, tr.name, arr);
            }
            cJSON_AddItemToArray(arr, row);
            rows++;
        }
    }
    // Forget "sent" marks for entries that were pruned.
    for (auto it = sent_.begin(); it != sent_.end();) {
        radio::Table *t = it->first[0] == 'd' ? &r.direct : it->first[0] == 'r' ? &r.repeated
                        : it->first[0] == 'n' ? &r.nodes : &r.meaters;
        if (t->find(it->first.substr(1)) == t->end())
            it = sent_.erase(it);
        else
            ++it;
    }
    r.unlock();
    if (!rows) {
        cJSON_Delete(msg);
        return std::string();
    }
    return json_line(msg);
}

}  // namespace split
