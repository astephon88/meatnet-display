#include "split.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <thread>

#include "cJSON.h"
#include "proto.h"
#include "util.h"

namespace split {

// Keeps sockets from leaking into a re-exec'd process (the Linux unit's
// "Reboot"). lwIP has no exec, nor these flags.
static int cloexec(int fd)
{
#if defined(F_SETFD) && defined(FD_CLOEXEC)
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
#endif
    return fd;
}

using core::ticks_ms;

// -- codec ---------------------------------------------------------------------------------

static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string b64(const Bytes &data)
{
    std::string out;
    size_t i = 0;
    for (; i + 2 < data.size(); i += 3) {
        uint32_t v = uint8_t(data[i]) << 16 | uint8_t(data[i + 1]) << 8 | uint8_t(data[i + 2]);
        for (int s = 18; s >= 0; s -= 6) out += kB64[v >> s & 63];
    }
    size_t rest = data.size() - i;
    if (rest) {
        uint32_t v = uint8_t(data[i]) << 16 | (rest > 1 ? uint8_t(data[i + 1]) << 8 : 0);
        out += kB64[v >> 18 & 63];
        out += kB64[v >> 12 & 63];
        out += rest > 1 ? kB64[v >> 6 & 63] : '=';
        out += '=';
    }
    return out;
}

Bytes unb64(const std::string &text)
{
    Bytes out;
    uint32_t v = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == '\n' || c == '\r') continue;
        const char *p = strchr(kB64, c);
        if (!p || !c) return Bytes();
        v = v << 6 | uint32_t(p - kB64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += static_cast<char>(v >> bits & 0xFF);
        }
    }
    return out;
}

std::string encode(cJSON *msg)
{
    char *text = cJSON_PrintUnformatted(msg);
    std::string line = text ? std::string(text) + "\n" : std::string();
    cJSON_free(text);
    cJSON_Delete(msg);
    return line;
}

cJSON *decode(const std::string &line)
{
    cJSON *msg = cJSON_Parse(line.c_str());
    if (!cJSON_IsObject(msg) || !cJSON_IsString(cJSON_GetObjectItem(msg, "t"))) {
        cJSON_Delete(msg);
        return nullptr;
    }
    return msg;
}

const std::vector<std::string> &link_subs(const std::string &kind)
{
    static const std::map<std::string, std::vector<std::string>> subs = {
        {"probe", {"status", "uart"}}, {"node", {"uart"}}, {"meater", {"temp", "battery"}}};
    static const std::vector<std::string> none;
    auto it = subs.find(kind);
    return it != subs.end() ? it->second : none;
}

static const char *str_of(const cJSON *msg, const char *name)
{
    const cJSON *v = cJSON_GetObjectItem(msg, name);
    return cJSON_IsString(v) ? v->valuestring : nullptr;
}

static double num_of(const cJSON *msg, const char *name, double dflt)
{
    const cJSON *v = cJSON_GetObjectItem(msg, name);
    return cJSON_IsNumber(v) ? v->valuedouble : dflt;
}

static Bytes hex_of(const cJSON *msg, const char *name)
{
    const char *s = str_of(msg, name);
    Bytes out;
    if (!s || !proto::unhex(s, out)) return Bytes();
    return out;
}

static cJSON *msg_of(const char *t)
{
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "t", t);
    return m;
}

static cJSON *hello_msg(const char *role, const std::string &serial, const std::string &key,
                        int max_conn, const Bytes &mac, const std::string &impl)
{
    cJSON *m = msg_of("hello");
    cJSON_AddNumberToObject(m, "v", VERSION);
    cJSON_AddStringToObject(m, "role", role);
    cJSON_AddStringToObject(m, "serial", serial.c_str());
    cJSON_AddStringToObject(m, "key", key.c_str());
    cJSON_AddNumberToObject(m, "max_conn", max_conn);
    cJSON_AddStringToObject(m, "mac", proto::hex(mac).c_str());
    if (!impl.empty()) cJSON_AddStringToObject(m, "impl", impl.c_str());
    return m;
}

// -- sockets (BSD: lwIP on the ESP32, POSIX elsewhere) ---------------------------------------

static bool send_all(int fd, const std::string &data)
{
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    size_t off = 0;
    while (off < data.size()) {
        int n = ::send(fd, data.data() + off, data.size() - off, flags);
        if (n <= 0) return false;
        off += n;
    }
    return true;
}

// Waits up to ms for fd to be readable.
static bool readable(int fd, int ms)
{
    fd_set rs;
    FD_ZERO(&rs);
    FD_SET(fd, &rs);
    struct timeval tv = {ms / 1000, (ms % 1000) * 1000};
    return select(fd + 1, &rs, nullptr, nullptr, &tv) > 0;
}

// Complete lines from buf (the rest stays in buf).
static std::vector<std::string> take_lines(std::string &buf)
{
    std::vector<std::string> out;
    size_t nl;
    while ((nl = buf.find('\n')) != std::string::npos) {
        out.push_back(buf.substr(0, nl));
        buf.erase(0, nl + 1);
    }
    if (buf.size() > 65536) buf.clear();        // runaway garbage
    return out;
}

static void sleep_ms(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

// -- field side ------------------------------------------------------------------------------

void FieldRadio::reset()
{
    sent_.clear();
    links_.clear();
}

// Don't report the app unit's own advertisements (its Display info, the
// probes it repeats, its synthesized MEATERs) back to it.
void FieldRadio::set_ignore(const Bytes &mac, const std::string &serial)
{
    ignore_serial_ = serial.empty() ? Bytes() : protocol::encode_serial(serial);
    bool zero = std::all_of(mac.begin(), mac.end(), [](char c) { return c == 0; });
    src_.set_ignore(zero ? Bytes() : mac, ignore_serial_);
}

void FieldRadio::prune(int64_t now) { src_.prune(now, FIELD_STALE_MS); }

std::string FieldRadio::scan_message(int64_t now)
{
    FieldSource::Tables tb = src_.tables();
    struct Ref {
        char name;
        std::map<Bytes, core::Observation> *table;
    } refs[] = {{'d', &tb.direct}, {'r', &tb.repeated}, {'n', &tb.nodes}, {'m', &tb.meaters}};
    cJSON *msg = msg_of("scan");
    int rows = 0;
    for (auto &ref : refs) {
        cJSON *arr = nullptr;
        for (auto &kv : *ref.table) {
            const Bytes &k = kv.first;
            const core::Observation &o = kv.second;
            if (ref.name == 'n' && !ignore_serial_.empty() && k == ignore_serial_) continue;
            std::string sk = std::string(1, ref.name) + k;
            auto it = sent_.find(sk);
            if (it != sent_.end() && it->second == o.ticks) continue;
            sent_[sk] = o.ticks;
            cJSON *row = cJSON_CreateArray();
            double age = static_cast<double>(now - o.ticks);
            cJSON_AddItemToArray(row, cJSON_CreateString(proto::hex(k).c_str()));
            if (ref.name == 'm') {
                cJSON_AddItemToArray(row, cJSON_CreateString(o.name.c_str()));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(age));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(o.addr_type));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(o.rssi));
            } else {
                cJSON_AddItemToArray(row, cJSON_CreateString(b64(o.body).c_str()));
                cJSON_AddItemToArray(row, cJSON_CreateNumber(age));
                if (ref.name == 'r') {
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(o.rssi));
                } else {
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(o.addr_type));
                    cJSON_AddItemToArray(row, cJSON_CreateString(proto::hex(o.addr).c_str()));
                    cJSON_AddItemToArray(row, cJSON_CreateNumber(o.rssi));
                }
            }
            if (!arr) {
                arr = cJSON_CreateArray();
                cJSON_AddItemToObject(msg, std::string(1, ref.name).c_str(), arr);
            }
            cJSON_AddItemToArray(arr, row);
            rows++;
        }
    }
    // Forget "sent" marks for entries that are gone (pruned or ignored).
    for (auto it = sent_.begin(); it != sent_.end();) {
        auto *t = it->first[0] == 'd' ? &tb.direct : it->first[0] == 'r' ? &tb.repeated
                : it->first[0] == 'n' ? &tb.nodes : &tb.meaters;
        it = t->count(it->first.substr(1)) ? std::next(it) : sent_.erase(it);
    }
    if (!rows) {
        cJSON_Delete(msg);
        return std::string();
    }
    stats["scan"] += rows;
    return encode(msg);
}

std::vector<std::string> FieldRadio::link_messages()
{
    std::vector<std::string> out;
    for (auto &r : src_.collect_links()) {
        std::string sig = r.state + "|" + r.desc + "|" + std::to_string(r.mtu) + "|";
        for (auto &kv : r.info) sig += kv.first + "=" + kv.second + ";";
        if (r.has_stats) {
            sig += "|" + std::to_string(r.connects) + "|" + r.params + "|" + r.asked + "|";
            for (auto &kv : r.drops) sig += kv.first + "=" + std::to_string(kv.second) + ";";
        }
        std::string khex = proto::hex(r.key);
        if (links_[r.key] != sig) {
            links_[r.key] = sig;
            cJSON *m = msg_of("link");
            cJSON_AddStringToObject(m, "k", khex.c_str());
            cJSON_AddStringToObject(m, "kind", r.kind.c_str());
            cJSON_AddStringToObject(m, "state", r.state.c_str());
            cJSON_AddStringToObject(m, "desc", r.desc.c_str());
            cJSON_AddNumberToObject(m, "mtu", r.mtu);
            cJSON_AddNumberToObject(m, "count", r.count);
            cJSON *info = cJSON_AddObjectToObject(m, "info");
            for (auto &kv : r.info) cJSON_AddStringToObject(info, kv.first.c_str(), kv.second.c_str());
            if (r.has_stats) {
                cJSON *st = cJSON_AddObjectToObject(m, "stats");
                cJSON_AddNumberToObject(st, "connects", r.connects);
                cJSON *drops = cJSON_AddObjectToObject(st, "drops");
                for (auto &kv : r.drops) cJSON_AddNumberToObject(drops, kv.first.c_str(), kv.second);
                cJSON_AddStringToObject(st, "params", r.params.c_str());
                cJSON_AddStringToObject(st, "asked", r.asked.c_str());
            }
            out.push_back(encode(m));
        }
        for (auto &rx : r.rx) {
            cJSON *m = msg_of("rx");
            cJSON_AddStringToObject(m, "k", khex.c_str());
            cJSON_AddStringToObject(m, "sub", rx.first.c_str());
            cJSON_AddStringToObject(m, "data", b64(rx.second).c_str());
            out.push_back(encode(m));
            stats["rx"]++;
        }
    }
    for (auto &key : src_.take_removed_links()) {
        links_.erase(key);
        cJSON *m = msg_of("link");
        cJSON_AddStringToObject(m, "k", proto::hex(key).c_str());
        cJSON_AddStringToObject(m, "kind", "");
        cJSON_AddStringToObject(m, "state", "closed");
        cJSON_AddStringToObject(m, "desc", "");
        cJSON_AddNumberToObject(m, "mtu", 23);
        cJSON_AddNumberToObject(m, "count", 0);
        out.push_back(encode(m));
    }
    return out;
}

void FieldRadio::apply(const std::string &line)
{
    if (cJSON *msg = decode(line)) {
        apply(msg);
        cJSON_Delete(msg);
    }
}

void FieldRadio::apply(cJSON *msg)
{
    std::string t = str_of(msg, "t");
    stats["cmd"]++;
    if (t == "open") {
        const char *kind = str_of(msg, "kind");
        Bytes key = hex_of(msg, "k"), addr = hex_of(msg, "addr");
        if (kind && !key.empty() && addr.size() == 6)
            src_.open_link(key, kind, static_cast<int>(num_of(msg, "at", 0)), addr);
    } else if (t == "close") {
        Bytes key = hex_of(msg, "k");
        if (!key.empty()) src_.close_link(key);
    } else if (t == "write") {
        Bytes key = hex_of(msg, "k");
        const char *data = str_of(msg, "data");
        if (!key.empty() && data) src_.write_link(key, unb64(data));
    } else if (t == "scan_active") {
        src_.set_scan_active(cJSON_IsTrue(cJSON_GetObjectItem(msg, "on")));
    } else if (t == "standby") {
        src_.set_standby(cJSON_IsTrue(cJSON_GetObjectItem(msg, "on")));
    }
}

void SplitClient::logf(const char *fmt, ...)
{
    if (!log_) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(buf);
}

std::string SplitClient::peer() const
{
    std::lock_guard<std::mutex> g(mu_);
    return peer_;
}

std::string SplitClient::peer_ip() const
{
    std::lock_guard<std::mutex> g(mu_);
    return peer_ip_;
}

void SplitClient::run()
{
    int backoff_s = 2;
    while (!stop_) {
        std::string host;
        if (find_app(host)) {
            session(host);
            fr_.drop_links();       // without the app unit nobody wants the links
            if (connected_) {
                logf("[split] app unit offline");
                backoff_s = 2;
            }
            connected_ = false;
            rtt_ms_ = -1;
        }
        for (int i = 0; i < backoff_s * 10 && !stop_; i++) sleep_ms(100);
        backoff_s = std::min(backoff_s * 2, 16);
    }
}

// The configured app unit address, or the sender of the app unit's UDP
// beacon {"t":"beacon","serial":...,"port":N} within 5 s.
bool SplitClient::find_app(std::string &host)
{
    if (!cfg_.peer.empty()) {
        host = cfg_.peer;
        return true;
    }
    int fd = cloexec(socket(AF_INET, SOCK_DGRAM, 0));
    if (fd < 0) return false;
    int yes = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.port);
    a.sin_addr.s_addr = htonl(INADDR_ANY);
    bool found = false;
    if (bind(fd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) == 0) {
        int64_t until = ticks_ms() + 5000;
        while (!found && !stop_ && ticks_ms() < until) {
            if (!readable(fd, 200)) continue;
            char buf[256];
            struct sockaddr_in from = {};
            socklen_t fl = sizeof(from);
            int n = recvfrom(fd, buf, sizeof(buf) - 1, 0, reinterpret_cast<struct sockaddr *>(&from), &fl);
            if (n <= 0) continue;
            buf[n] = 0;
            cJSON *msg = decode(buf);
            if (msg && std::string(str_of(msg, "t")) == "beacon") {
                cfg_.port = static_cast<uint16_t>(num_of(msg, "port", cfg_.port));
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                host = ip;
                found = true;
            }
            cJSON_Delete(msg);
        }
    }
    close(fd);
    return found;
}

void SplitClient::session(const std::string &host)
{
    char port[8];
    snprintf(port, sizeof(port), "%u", cfg_.port);
    struct addrinfo hints = {};
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    if (getaddrinfo(host.c_str(), port, &hints, &res) != 0 || !res) {
        logf("[split] can't resolve app unit '%s'", host.c_str());
        return;
    }
    int fd = cloexec(socket(res->ai_family, res->ai_socktype, 0));
    if (fd < 0) {
        freeaddrinfo(res);
        return;
    }
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    freeaddrinfo(res);
    if (rc != 0) {
        logf("[split] connection to app unit %s:%s failed (errno %d)", host.c_str(), port, errno);
        close(fd);
        return;
    }
    if (!send_all(fd, encode(hello_msg("field", cfg_.serial, cfg_.key, cfg_.max_conn, cfg_.mac,
                                       cfg_.impl)))) {
        close(fd);
        return;
    }
    // The app unit answers with its own hello, or closes on a wrong key.
    std::string buf;
    cJSON *reply = nullptr;
    int64_t deadline = ticks_ms() + 5000;
    while (!reply && ticks_ms() < deadline && !stop_) {
        if (!readable(fd, 200)) continue;
        char tmp[512];
        int n = recv(fd, tmp, sizeof(tmp), 0);
        if (n <= 0) break;
        buf.append(tmp, n);
        for (auto &line : take_lines(buf)) {
            if (!reply) reply = decode(line);
        }
    }
    if (!reply || std::string(str_of(reply, "t")) != "hello") {
        logf("[split] app unit at %s refused the connection (key?)", host.c_str());
        cJSON_Delete(reply);
        close(fd);
        return;
    }
    const char *serial = str_of(reply, "serial");
    Bytes app_mac = hex_of(reply, "mac");
    {
        std::lock_guard<std::mutex> g(mu_);
        peer_ = serial ? serial : "?";
        peer_ip_ = host;
    }
    cJSON_Delete(reply);
    fr_.reset();
    fr_.set_ignore(app_mac, peer());
    connected_ = true;
    logf("[split] connected to app unit %s at %s", peer().c_str(), host.c_str());

    int64_t rx_at = ticks_ms(), ping_at = rx_at, scan_at = 0;
    std::vector<std::string> out;
    while (!stop_) {
        int64_t now = ticks_ms();
        if (now - rx_at > DEAD_MS) {
            logf("[split] app unit timed out");
            break;
        }
        if (readable(fd, 100)) {
            char tmp[2048];
            int n = recv(fd, tmp, sizeof(tmp), 0);
            if (n <= 0) break;      // closed
            rx_at = ticks_ms();
            buf.append(tmp, n);
            for (auto &line : take_lines(buf)) {
                cJSON *msg = decode(line);
                if (!msg) continue;
                std::string t = str_of(msg, "t");
                if (t == "ping") {
                    cJSON *p = msg_of("pong");
                    cJSON_AddNumberToObject(p, "ts", num_of(msg, "ts", 0));
                    out.push_back(encode(p));
                } else if (t == "pong") {
                    rtt_ms_ = static_cast<int>(ticks_ms() - static_cast<int64_t>(num_of(msg, "ts", 0)));
                } else {
                    fr_.apply(msg);
                }
                cJSON_Delete(msg);
            }
        }
        for (auto &m : fr_.link_messages()) out.push_back(std::move(m));
        now = ticks_ms();
        if (now - scan_at >= SCAN_BATCH_MS) {
            scan_at = now;
            fr_.prune(now);
            std::string m = fr_.scan_message(now);
            if (!m.empty()) out.push_back(m);
        }
        if (now - ping_at >= PING_MS) {
            ping_at = now;
            cJSON *p = msg_of("ping");
            cJSON_AddNumberToObject(p, "ts", static_cast<double>(now));
            out.push_back(encode(p));
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

// -- app side ---------------------------------------------------------------------------------

std::vector<Bytes> RemoteLink::take(const std::string &sub)
{
    auto it = rx_.find(sub);
    if (it == rx_.end() || it->second.empty()) return {};
    std::vector<Bytes> q = std::move(it->second);
    it->second.clear();
    return q;
}

bool RemoteLink::write(const Bytes &data)
{
    if (!ready()) return false;
    cJSON *m = msg_of("write");
    cJSON_AddStringToObject(m, "k", proto::hex(key_).c_str());
    cJSON_AddStringToObject(m, "data", b64(data).c_str());
    radio_.queue(m);
    return true;
}

RemoteRadio::RemoteRadio(core::Transport *local, LogFn log) : local_(local), log_(std::move(log))
{
    if (local_) {
        mac = local_->mac;
        inbound = local_->inbound;
    }
}

void RemoteRadio::queue(cJSON *msg)
{
    if (!connected) {
        cJSON_Delete(msg);
        return;
    }
    std::string line = encode(msg);
    std::lock_guard<std::mutex> g(mu_);
    outbox_.push_back(std::move(line));
}

std::vector<std::string> RemoteRadio::take_outbox()
{
    std::lock_guard<std::mutex> g(mu_);
    return std::move(outbox_);
}

std::shared_ptr<core::Link> RemoteRadio::open_link(const Bytes &key, const std::string &kind, int addr_type,
                                                   const Bytes &addr)
{
    auto &slot = links[key];
    if (!slot) {
        slot = std::make_shared<RemoteLink>(*this, key, kind);
        if (!connected) static_cast<RemoteLink &>(*slot).state_ = "error: field unit offline";
    }
    auto &link = static_cast<RemoteLink &>(*slot);
    link.enabled_ = true;
    cJSON *m = msg_of("open");
    cJSON_AddStringToObject(m, "k", proto::hex(key).c_str());
    cJSON_AddStringToObject(m, "kind", kind.c_str());
    cJSON_AddNumberToObject(m, "at", addr_type);
    cJSON_AddStringToObject(m, "addr", proto::hex(addr).c_str());
    link_args_[key] = encode(m);
    if (!link.opened_ && connected) {
        queue(decode(link_args_[key]));
        link.opened_ = true;
    }
    return slot;
}

void RemoteRadio::close_link(const Bytes &key)
{
    links.erase(key);
    link_args_.erase(key);
    cJSON *m = msg_of("close");
    cJSON_AddStringToObject(m, "k", proto::hex(key).c_str());
    queue(m);
}

void RemoteRadio::set_scan_active(bool on)
{
    if (on == scan_active_) return;
    scan_active_ = on;
    cJSON *m = msg_of("scan_active");
    cJSON_AddBoolToObject(m, "on", on);
    queue(m);
}

void RemoteRadio::set_standby(bool on)
{
    if (local_) local_->set_standby(on);
    if (on == standby_) return;
    standby_ = on;
    cJSON *m = msg_of("standby");
    cJSON_AddBoolToObject(m, "on", on);
    queue(m);
}

std::vector<std::pair<int, Bytes>> RemoteRadio::poll_rx()
{
    pump(ticks_ms());
    if (!local_) return {};
    auto rx = local_->poll_rx();
    inbound = local_->inbound;
    mac = local_->mac;
    return rx;
}

void RemoteRadio::send(const Bytes &frame, const std::vector<int> &conns)
{
    if (local_) local_->send(frame, conns);
}

void RemoteRadio::set_advertising(const std::vector<Advert> &adverts, bool fast)
{
    if (local_) local_->set_advertising(adverts, fast);
}

void RemoteRadio::rotate_advertisement()
{
    if (local_) local_->rotate_advertisement();
}

void RemoteRadio::set_firmware(const std::string &fw)
{
    if (local_) local_->set_firmware(fw);
}

size_t RemoteRadio::txq_size() const { return local_ ? local_->txq_size() : 0; }

uint32_t RemoteRadio::tx_dropped() const { return local_ ? local_->tx_dropped() : 0; }

void RemoteRadio::pump(int64_t now)
{
    std::vector<Event> events;
    {
        std::lock_guard<std::mutex> g(mu_);
        events.swap(inbox_);
    }
    for (auto &e : events) {
        if (e.kind == Event::CONNECT) on_connect(e.line, e.ip);
        else if (e.kind == Event::DISCONNECT) on_disconnect();
        else apply(e.line, now);
    }
}

void RemoteRadio::on_connect(const std::string &hello_line, const std::string &ip)
{
    cJSON *hello = cJSON_Parse(hello_line.c_str());
    connected = true;
    max_conn = static_cast<int>(num_of(hello, "max_conn", 4));
    const char *s = str_of(hello, "serial");
    peer_serial = s ? s : "?";
    peer_ip = ip;
    cJSON_Delete(hello);
    {
        std::lock_guard<std::mutex> g(mu_);
        outbox_.clear();
    }
    cJSON *m = msg_of("scan_active");
    cJSON_AddBoolToObject(m, "on", scan_active_);
    queue(m);
    m = msg_of("standby");
    cJSON_AddBoolToObject(m, "on", standby_);
    queue(m);
    for (auto &kv : links) {
        auto &link = static_cast<RemoteLink &>(*kv.second);
        link.opened_ = false;
        link.state_ = "idle";
        auto args = link_args_.find(kv.first);
        if (link.enabled_ && args != link_args_.end()) {
            queue(decode(args->second));
            link.opened_ = true;
        }
    }
    if (log_)
        log_("[split] field unit " + peer_serial + " connected" + (ip.empty() ? "" : " from " + ip));
}

void RemoteRadio::on_disconnect()
{
    if (connected && log_) log_("[split] field unit offline");
    connected = false;
    rtt_ms = -1;
    {
        std::lock_guard<std::mutex> g(mu_);
        outbox_.clear();
    }
    for (auto &kv : links) {
        auto &link = static_cast<RemoteLink &>(*kv.second);
        link.state_ = "error: field unit offline";
        link.opened_ = false;
    }
}

void RemoteRadio::apply(const std::string &line, int64_t now)
{
    if (cJSON *msg = decode(line)) {
        apply(msg, now);
        cJSON_Delete(msg);
    }
}

void RemoteRadio::apply(cJSON *msg, int64_t now)
{
    std::string t = str_of(msg, "t");
    if (t == "scan") {
        auto rows = [&](const char *name, auto fn) {
            const cJSON *row;
            cJSON_ArrayForEach(row, cJSON_GetObjectItem(msg, name)) {
                if (!cJSON_IsArray(row)) continue;
                const cJSON *k = cJSON_GetArrayItem(row, 0);
                Bytes key;
                if (!cJSON_IsString(k) || !proto::unhex(k->valuestring, key)) continue;
                fn(key, row);
            }
        };
        auto item_str = [](const cJSON *row, int i) {
            const cJSON *v = cJSON_GetArrayItem(row, i);
            return cJSON_IsString(v) ? std::string(v->valuestring) : std::string();
        };
        auto item_num = [](const cJSON *row, int i) {
            const cJSON *v = cJSON_GetArrayItem(row, i);
            return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
        };
        auto with_addr = [&](std::map<Bytes, core::Observation> &table, bool skip_own) {
            return [&, skip_own](const Bytes &key, const cJSON *row) {
                if (skip_own && key == own_serial) return;
                core::Observation o;
                o.body = unb64(item_str(row, 1));
                o.ticks = now - static_cast<int64_t>(item_num(row, 2));
                o.addr_type = static_cast<int>(item_num(row, 3));
                proto::unhex(item_str(row, 4), o.addr);
                o.rssi = static_cast<int>(item_num(row, 5));
                table[key] = o;
            };
        };
        rows("d", with_addr(direct, false));
        rows("n", with_addr(nodes, true));
        rows("r", [&](const Bytes &key, const cJSON *row) {
            core::Observation o;
            o.body = unb64(item_str(row, 1));
            o.ticks = now - static_cast<int64_t>(item_num(row, 2));
            o.rssi = static_cast<int>(item_num(row, 3));
            repeated[key] = o;
        });
        rows("m", [&](const Bytes &key, const cJSON *row) {
            core::Observation o;
            o.name = item_str(row, 1);
            o.ticks = now - static_cast<int64_t>(item_num(row, 2));
            o.addr_type = static_cast<int>(item_num(row, 3));
            o.rssi = static_cast<int>(item_num(row, 4));
            meaters[key] = o;
        });
    } else if (t == "link") {
        auto it = links.find(hex_of(msg, "k"));
        if (it == links.end()) return;
        auto &link = static_cast<RemoteLink &>(*it->second);
        const char *state = str_of(msg, "state");
        std::string st = state ? state : "";
        link.state_ = st == "closed" ? "idle" : st;
        const char *desc = str_of(msg, "desc");
        link.desc_ = desc ? desc : "";
        link.mtu_ = static_cast<int>(num_of(msg, "mtu", 23));
        link.count_ = static_cast<uint32_t>(num_of(msg, "count", link.count_));
        const cJSON *info = cJSON_GetObjectItem(msg, "info");
        if (cJSON_IsObject(info) && info->child) {
            link.info_.clear();
            const cJSON *e;
            cJSON_ArrayForEach(e, info) if (cJSON_IsString(e)) link.info_[e->string] = e->valuestring;
        }
        const cJSON *stats = cJSON_GetObjectItem(msg, "stats");
        if (cJSON_IsObject(stats) && stats->child) {
            char *text = cJSON_PrintUnformatted(stats);
            link.stats_ = text ? text : "";
            cJSON_free(text);
        }
        if (st == "closed") link.opened_ = false;   // reopen on the next open_link()
    } else if (t == "rx") {
        auto it = links.find(hex_of(msg, "k"));
        const char *sub = str_of(msg, "sub"), *data = str_of(msg, "data");
        if (it == links.end() || !sub || !data) return;
        auto &link = static_cast<RemoteLink &>(*it->second);
        auto &q = link.rx_[sub];
        q.push_back(unb64(data));
        link.count_++;
        if (q.size() > RX_KEEP) q.erase(q.begin());
    }
}

void RemoteRadio::net_connected(const std::string &hello_line, const std::string &ip)
{
    std::lock_guard<std::mutex> g(mu_);
    outbox_.clear();            // nothing from an earlier session
    inbox_.push_back({Event::CONNECT, hello_line, ip});
    net_connected_ = true;
}

void RemoteRadio::net_received(const std::string &line)
{
    std::lock_guard<std::mutex> g(mu_);
    inbox_.push_back({Event::MESSAGE, line, std::string()});
}

void RemoteRadio::net_disconnected()
{
    std::lock_guard<std::mutex> g(mu_);
    inbox_.push_back({Event::DISCONNECT, std::string(), std::string()});
    net_connected_ = false;
}

std::vector<std::string> RemoteRadio::net_take_outbox()
{
    std::lock_guard<std::mutex> g(mu_);
    return std::move(outbox_);
}

// -- server -------------------------------------------------------------------------------------

SplitServer::SplitServer(RemoteRadio &remote, ServerConfig cfg, LogFn log)
    : remote_(remote), cfg_(std::move(cfg)), log_(std::move(log))
{
    remote_.own_serial = protocol::encode_serial(cfg_.serial);
}

void SplitServer::logf(const char *fmt, ...)
{
    if (!log_) return;
    char buf[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_(buf);
}

void SplitServer::run()
{
    int lfd = cloexec(socket(AF_INET, SOCK_STREAM, 0));
    int yes = 1;
    setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    struct sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(cfg_.port);
    inet_pton(AF_INET, cfg_.bind.c_str(), &a.sin_addr);
    if (lfd < 0 || bind(lfd, reinterpret_cast<struct sockaddr *>(&a), sizeof(a)) != 0 || listen(lfd, 2) != 0) {
        logf("[split] can't listen on port %u (errno %d)", cfg_.port, errno);
        if (lfd >= 0) close(lfd);
        return;
    }
    int bfd = -1;
    if (cfg_.beacon) {
        bfd = cloexec(socket(AF_INET, SOCK_DGRAM, 0));
        if (bfd >= 0) setsockopt(bfd, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));
    }

    // Connections that haven't sent their hello yet.
    struct Candidate {
        int fd;
        std::string ip, buf;
        int64_t deadline;
    };
    std::vector<Candidate> candidates;
    int sfd = -1;               // the field unit's session
    std::string sbuf;
    int64_t rx_at = 0, ping_at = 0, beacon_at = 0;

    auto end_session = [&]() {
        if (sfd < 0) return;
        close(sfd);
        sfd = -1;
        sbuf.clear();
        remote_.net_disconnected();
    };

    while (!stop_) {
        fd_set rs;
        FD_ZERO(&rs);
        FD_SET(lfd, &rs);
        int maxfd = lfd;
        for (auto &c : candidates) {
            FD_SET(c.fd, &rs);
            maxfd = std::max(maxfd, c.fd);
        }
        if (sfd >= 0) {
            FD_SET(sfd, &rs);
            maxfd = std::max(maxfd, sfd);
        }
        struct timeval tv = {0, 50000};
        int sel = select(maxfd + 1, &rs, nullptr, nullptr, &tv);
        int64_t now = ticks_ms();

        if (sel > 0 && FD_ISSET(lfd, &rs)) {
            struct sockaddr_in from = {};
            socklen_t fl = sizeof(from);
            int fd = cloexec(accept(lfd, reinterpret_cast<struct sockaddr *>(&from), &fl));
            if (fd >= 0) {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                candidates.push_back({fd, ip, std::string(), now + 5000});
            }
        }
        for (size_t i = 0; i < candidates.size();) {
            Candidate &c = candidates[i];
            bool done = false, ok = false;
            std::string hello_line;
            if (sel > 0 && FD_ISSET(c.fd, &rs)) {
                char tmp[512];
                int n = recv(c.fd, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    done = true;
                } else {
                    c.buf.append(tmp, n);
                    auto lines = take_lines(c.buf);
                    if (!lines.empty()) {
                        done = true;
                        cJSON *hello = decode(lines[0]);
                        const char *role = hello ? str_of(hello, "role") : nullptr;
                        if (hello && std::string(str_of(hello, "t")) == "hello" && role &&
                            std::string(role) == "field") {
                            const char *key = str_of(hello, "key");
                            if ((key ? key : "") == cfg_.key) {
                                ok = true;
                                hello_line = lines[0];
                            } else {
                                const char *s = str_of(hello, "serial");
                                logf("[split] field unit %s rejected: wrong key", s ? s : "?");
                            }
                        }
                        cJSON_Delete(hello);
                    }
                }
            }
            if (!done && now > c.deadline) done = true;
            if (!done) {
                ++i;
                continue;
            }
            if (ok) {
                end_session();          // a newer connection replaces the old one
                if (send_all(c.fd, encode(hello_msg("app", cfg_.serial, cfg_.key, 0, cfg_.mac, "")))) {
                    sfd = c.fd;
                    sbuf = c.buf;
                    rx_at = ping_at = now;
                    remote_.net_connected(hello_line, c.ip);
                } else {
                    close(c.fd);
                }
            } else {
                close(c.fd);
            }
            candidates.erase(candidates.begin() + i);
        }

        if (sfd >= 0) {
            bool alive = true;
            if (sel > 0 && FD_ISSET(sfd, &rs)) {
                char tmp[4096];
                int n = recv(sfd, tmp, sizeof(tmp), 0);
                if (n <= 0) {
                    alive = false;
                } else {
                    rx_at = now;
                    sbuf.append(tmp, n);
                    std::string replies;
                    for (auto &line : take_lines(sbuf)) {
                        cJSON *msg = decode(line);
                        if (!msg) continue;
                        std::string t = str_of(msg, "t");
                        if (t == "ping") {
                            cJSON *p = msg_of("pong");
                            cJSON_AddNumberToObject(p, "ts", num_of(msg, "ts", 0));
                            replies += encode(p);
                        } else if (t == "pong") {
                            remote_.rtt_ms = static_cast<int>(now - static_cast<int64_t>(num_of(msg, "ts", 0)));
                        } else {
                            remote_.net_received(line);
                        }
                        cJSON_Delete(msg);
                    }
                    if (!replies.empty() && !send_all(sfd, replies)) alive = false;
                }
            }
            if (alive && now - rx_at > DEAD_MS) {
                logf("[split] field unit timed out");
                alive = false;
            }
            if (alive && now - ping_at >= PING_MS) {
                ping_at = now;
                cJSON *p = msg_of("ping");
                cJSON_AddNumberToObject(p, "ts", static_cast<double>(now));
                if (!send_all(sfd, encode(p))) alive = false;
            }
            if (alive) {
                std::string all;
                for (auto &l : remote_.net_take_outbox()) all += l;
                if (!all.empty() && !send_all(sfd, all)) alive = false;
            }
            if (!alive) end_session();
        }

        if (bfd >= 0 && sfd < 0 && now - beacon_at >= 2000) {
            beacon_at = now;
            cJSON *b = msg_of("beacon");
            cJSON_AddStringToObject(b, "serial", cfg_.serial.c_str());
            cJSON_AddNumberToObject(b, "port", cfg_.port);
            std::string line = encode(b);
            struct sockaddr_in to = {};
            to.sin_family = AF_INET;
            to.sin_port = htons(cfg_.port);
            to.sin_addr.s_addr = htonl(INADDR_BROADCAST);
            sendto(bfd, line.data(), line.size(), 0, reinterpret_cast<struct sockaddr *>(&to), sizeof(to));
        }
    }
    for (auto &c : candidates) close(c.fd);
    end_session();
    if (bfd >= 0) close(bfd);
    close(lfd);
}

}  // namespace split
