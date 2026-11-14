// MeatNet unit on a Linux host (BlueZ): the app, field or standalone role.
//
//     meatnet app        [--config host/app.json]   [--web-port 8080] [--adapter hci0]
//     meatnet field      [--config host/field.json] [--web-port 8081] [--peer HOST] [--key KEY | --key-file FILE]
//     meatnet standalone [--config host/standalone.json] [--web-port 8080]
//
// The Node (app and standalone roles) and all D-Bus work run on the GLib main
// loop; the split link and the web server have threads of their own.
#include <gio/gio.h>
#include <glib-unix.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <atomic>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>

#include "cJSON.h"
#include "central.h"
#include "http.h"
#include "linktable.h"
#include "log_ring.h"
#include "node.h"
#include "peripheral.h"
#include "split.h"
#include "status.h"
#include "host_transport.h"
#include "unit_settings.h"
#include "util.h"

extern const char index_html[];
extern const size_t index_html_len;

using namespace linux_unit;

namespace {

core::LogRing g_logring(200, 300);

void log(const std::string &msg)
{
    char ts[16];
    time_t t = time(nullptr);
    strftime(ts, sizeof(ts), "%H:%M:%S ", localtime(&t));
    printf("%s%s\n", ts, msg.c_str());
    fflush(stdout);
    g_logring.push(msg);
}

std::string read_file(const std::string &path)
{
    std::ifstream f(path);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool write_file(const std::string &path, const std::string &text)
{
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp);
        if (!(f << text)) return false;
    }
    return rename(tmp.c_str(), path.c_str()) == 0;
}

std::string json_text(cJSON *o)
{
    char *t = cJSON_PrintUnformatted(o);
    std::string s = t ? t : "{}";
    cJSON_free(t);
    cJSON_Delete(o);
    return s;
}

// The settings file (host/app.json format: config.json's keys), loaded at
// start and rewritten on every change from the web page.
class SettingsFile {
public:
    explicit SettingsFile(std::string path) : path_(std::move(path)) { core::merge_settings(s_, read_file(path_)); }
    core::Settings get()
    {
        std::lock_guard<std::mutex> g(mu_);
        return s_;
    }
    // Command-line overrides: in effect, but not written to the file.
    void override(const std::string &role, const std::optional<std::string> &peer,
                  const std::optional<std::string> &key)
    {
        std::lock_guard<std::mutex> g(mu_);
        role_ = role;
        peer_ = peer;
        key_ = key;
        apply_overrides(s_);
    }
    bool update(const cJSON *req, std::string &err, bool &reboot)
    {
        std::lock_guard<std::mutex> g(mu_);
        core::Settings saved;
        core::merge_settings(saved, read_file(path_));
        if (!core::apply_config(saved, req, err, reboot)) return false;
        if (!write_file(path_, json_text(core::settings_to_json(saved, true)))) {
            err = "could not write " + path_;
            return false;
        }
        s_ = saved;
        apply_overrides(s_);
        return true;
    }

private:
    void apply_overrides(core::Settings &s)
    {
        s.role = role_;
        if (peer_) s.split_peer = *peer_;
        if (key_) s.split_key = *key_;
    }
    std::string path_, role_;
    std::optional<std::string> peer_, key_;
    std::mutex mu_;
    core::Settings s_;
};

std::string machine_id_serial(char role)
{
    std::string id = read_file("/etc/machine-id");
    std::string hex;
    for (char c : id)
        if (isxdigit(static_cast<unsigned char>(c))) hex += static_cast<char>(toupper(c));
    if (hex.size() < 12) hex = "000000000001";
    return (std::string("H") + role + hex).substr(0, 10);
}

std::string local_ip()
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(1);
    inet_pton(AF_INET, "10.255.255.255", &a.sin_addr);       // no packet is sent for a UDP connect
    std::string ip = "127.0.0.1";
    if (connect(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0) {
        sockaddr_in me = {};
        socklen_t len = sizeof(me);
        char buf[INET_ADDRSTRLEN];
        if (getsockname(s, reinterpret_cast<sockaddr *>(&me), &len) == 0 &&
            inet_ntop(AF_INET, &me.sin_addr, buf, sizeof(buf)))
            ip = buf;
    }
    close(s);
    return ip;
}

struct Options {
    std::string role, config, adapter = "hci0";
    int web_port = 0;
    std::optional<std::string> peer, key;
};

bool parse(int argc, char **argv, Options &o)
{
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&](std::string &out) {
            if (i + 1 >= argc) return false;
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--config" && next(v)) o.config = v;
        else if (a == "--web-port" && next(v)) o.web_port = atoi(v.c_str());
        else if (a == "--adapter" && next(v)) o.adapter = v;
        else if (a == "--peer" && next(v)) o.peer = v;
        else if (a == "--key" && next(v)) o.key = v;
        else if (a == "--key-file" && next(v)) {
            // Keeps the key off the command line (e.g. a mounted Secret).
            std::string k = read_file(v);
            while (!k.empty() && (k.back() == '\n' || k.back() == '\r')) k.pop_back();
            o.key = k;
        }
        else if (a[0] != '-' && o.role.empty()) o.role = a;
        else return false;
    }
    if (o.role != "app" && o.role != "field" && o.role != "standalone") return false;
    if (o.config.empty()) o.config = "host/" + o.role + ".json";
    if (!o.web_port) o.web_port = o.role == "field" ? 8081 : 8080;
    return true;
}

// What the web server shows, published by the loop thread.
std::mutex g_snap_mu;
std::string g_snap = "{}";
std::atomic<bool> g_cfg_changed{false};
std::string g_exe;                      // our binary (re-exec'd by "Reboot"); keeps the process name
char **g_argv;

}  // namespace

int main(int argc, char **argv)
{
    Options opt;
    if (!parse(argc, argv, opt)) {
        fprintf(stderr, "usage: %s app|field|standalone [--config FILE] [--web-port N] [--adapter hciN] "
                        "[--peer HOST] [--key KEY | --key-file FILE]\n", argv[0]);
        return 2;
    }
    g_argv = argv;
    char exe[4096];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    g_exe = n > 0 ? std::string(exe, static_cast<size_t>(n)) : std::string(argv[0]);
    protocol::set_random_source([] { return static_cast<uint32_t>(g_random_int()); });
    SettingsFile settings(opt.config);
    settings.override(opt.role, opt.peer, opt.key);
    core::Settings cfg = settings.get();
    std::string serial = cfg.serial.empty() ? machine_id_serial(static_cast<char>(toupper(opt.role[0])))
                                            : cfg.serial.substr(0, 10);

    GError *err = nullptr;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, nullptr, &err);
    if (!bus) {
        log(std::string("[host] no system bus: ") + err->message);
        return 1;
    }
    GMainLoop *loop = g_main_loop_new(nullptr, FALSE);
    std::string adapter = "/org/bluez/" + opt.adapter;
    int64_t started = core::ticks_ms();
    std::string ip = local_ip();

    std::unique_ptr<Peripheral> periph;
    std::unique_ptr<Central> central;
    std::unique_ptr<LinuxTransport> local;
    std::unique_ptr<split::RemoteRadio> remote;
    std::unique_ptr<split::SplitServer> server;
    std::unique_ptr<split::FieldRadio> fradio;
    std::unique_ptr<split::SplitClient> client;
    std::unique_ptr<core::Node> node;
    core::LinkTable links;
    std::string links_path = opt.config.substr(0, opt.config.rfind('.')) + "-links.json";
    links.load(read_file(links_path));
    links.saver = [links_path](const std::string &json) { write_file(links_path, json); };
    std::vector<std::thread> threads;

    if (opt.role != "app") {
        central = std::make_unique<Central>(bus, g_main_context_default(), adapter, log);
        central->start();
    }
    if (opt.role != "field") {
        periph = std::make_unique<Peripheral>(bus, adapter, serial, cfg.dis_model, cfg.dis_firmware, log);
        periph->start();
        local = std::make_unique<LinuxTransport>(*periph, central.get());
        local->max_conn = cfg.max_conn;
        core::Transport *t = local.get();
        if (opt.role == "app") {
            remote = std::make_unique<split::RemoteRadio>(local.get(), log);
            t = remote.get();
            split::ServerConfig sc;
            sc.key = cfg.split_key;
            sc.serial = serial;
            sc.mac = periph->mac;
            sc.port = static_cast<uint16_t>(cfg.split_port);
            server = std::make_unique<split::SplitServer>(*remote, sc, log);
            threads.emplace_back([&server] { server->run(); });
        }
        node = std::make_unique<core::Node>(*t, cfg, serial, log, &links);
    } else {
        fradio = std::make_unique<split::FieldRadio>(*central, log);
        split::ClientConfig cc;
        cc.peer = cfg.split_peer;
        cc.key = cfg.split_key;
        cc.port = static_cast<uint16_t>(cfg.split_port);
        cc.serial = serial;
        cc.max_conn = cfg.max_conn;
        cc.mac = central->mac;
        cc.impl = "linux";
        client = std::make_unique<split::SplitClient>(*fradio, cc, log);
        threads.emplace_back([&client] { client->run(); });
    }

    // The loop thread: the Node at 10 Hz, housekeeping and the snapshot at 1 Hz.
    struct Tick {
        int n = 0;
        std::function<void()> fn;
    } tick;
    tick.fn = [&] {
        bool second = tick.n++ % 10 == 0;
        if (node) {
            if (g_cfg_changed.exchange(false)) {
                core::Settings fresh = settings.get();
                if (fresh.dis_firmware != cfg.dis_firmware) node->t.set_firmware(fresh.dis_firmware);
                cfg = fresh;
            }
            node->service();
            periph->service();
            if (second) node->tick();
        }
        if (!second) return;
        cJSON *o;
        if (node) {
            o = core::node_status(*node, core::ticks_ms());
        } else {
            auto t = central->tables();
            o = core::field_status(t.direct, t.nodes, t.meaters, central->link_summaries(), cfg.max_conn,
                                   core::ticks_ms());
        }
        cJSON *sp = cJSON_AddObjectToObject(o, "split");
        if (remote) {
            cJSON_AddBoolToObject(sp, "connected", remote->connected);
            if (remote->connected) {
                cJSON_AddStringToObject(sp, "peer", remote->peer_serial.c_str());
                cJSON_AddStringToObject(sp, "ip", remote->peer_ip.c_str());
                cJSON_AddNumberToObject(sp, "rtt_ms", remote->rtt_ms);
                cJSON_AddNumberToObject(sp, "max_conn", remote->max_conn);
            }
        } else if (client) {
            cJSON_AddBoolToObject(sp, "connected", client->connected());
            if (client->connected()) {
                cJSON_AddStringToObject(sp, "peer", client->peer().c_str());
                cJSON_AddStringToObject(sp, "ip", client->peer_ip().c_str());
                cJSON_AddNumberToObject(sp, "rtt_ms", client->rtt_ms());
            }
        } else {
            cJSON_DeleteItemFromObject(o, "split");
            cJSON_AddNullToObject(o, "split");
        }
        std::lock_guard<std::mutex> g(g_snap_mu);
        g_snap = json_text(o);
    };
    g_timeout_add(100, [](gpointer p) -> gboolean { static_cast<Tick *>(p)->fn(); return G_SOURCE_CONTINUE; }, &tick);
    g_unix_signal_add(SIGINT, [](gpointer l) -> gboolean { g_main_loop_quit(static_cast<GMainLoop *>(l)); return G_SOURCE_REMOVE; }, loop);
    g_unix_signal_add(SIGTERM, [](gpointer l) -> gboolean { g_main_loop_quit(static_cast<GMainLoop *>(l)); return G_SOURCE_REMOVE; }, loop);

    // bluetoothd forgets our GATT server, adverts and discovery when it
    // restarts: start over once it is back.
    static bool bluez_lost = false;
    g_bus_watch_name_on_connection(
        bus, gdbus::BLUEZ, G_BUS_NAME_WATCHER_FLAGS_NONE,
        [](GDBusConnection *, const gchar *, const gchar *, gpointer) {
            if (!bluez_lost) return;
            log("[host] bluetoothd is back: restarting");
            execv(g_exe.c_str(), g_argv);
        },
        [](GDBusConnection *, const gchar *, gpointer) {
            if (bluez_lost) return;
            bluez_lost = true;
            log("[host] bluetoothd went away; waiting for it");
        },
        nullptr, nullptr);

    HttpServer web(opt.web_port, [&](const Request &r) -> Response {
        Response res;
        auto result = [](bool ok, const std::string &error, const char *flag = nullptr) {
            cJSON *o = cJSON_CreateObject();
            cJSON_AddBoolToObject(o, "ok", ok);
            if (!ok) cJSON_AddStringToObject(o, "error", error.c_str());
            if (flag) cJSON_AddBoolToObject(o, flag, true);
            return json_text(o);
        };
        if (r.path == "/") {
            res.type = "text/html";
            res.body.assign(index_html, index_html_len);
        } else if (r.path == "/api/state") {
            std::string snap;
            {
                std::lock_guard<std::mutex> g(g_snap_mu);
                snap = g_snap;
            }
            cJSON *o = cJSON_Parse(snap.c_str());
            if (!o) o = cJSON_CreateObject();
            cJSON_AddStringToObject(o, "serial", serial.c_str());
            cJSON_DeleteItemFromObject(o, "role");
            cJSON_AddStringToObject(o, "role", opt.role.c_str());
            cJSON *w = cJSON_AddObjectToObject(o, "wifi");
            cJSON_AddStringToObject(w, "mode", "sta");
            cJSON_AddStringToObject(w, "ip", ip.c_str());
            cJSON_AddStringToObject(w, "ssid", "");
            cJSON_AddStringToObject(w, "ap_ssid", "");
            cJSON_AddNumberToObject(o, "heap", 0);
            cJSON_AddNumberToObject(o, "uptime", static_cast<double>((core::ticks_ms() - started) / 1000));
            cJSON_AddStringToObject(o, "display", "none (host)");
            cJSON_AddNullToObject(o, "power");
            res.body = json_text(o);
        } else if (r.path == "/api/config" && r.method == "GET") {
            res.body = json_text(core::settings_to_json(settings.get(), false));
        } else if (r.path == "/api/config" && r.method == "POST") {
            cJSON *req = cJSON_Parse(r.body.c_str());
            std::string error;
            bool reboot = false;
            bool ok = req && settings.update(req, error, reboot);
            if (!req) error = "expected a JSON object";
            cJSON_Delete(req);
            if (ok) g_cfg_changed = true;
            res.body = result(ok, error, ok && reboot ? "reboot" : nullptr);
        } else if (r.path == "/api/wifi/scan") {
            res.body = R"({"ok":true,"networks":[]})";
        } else if (r.path == "/api/wifi") {
            res.body = result(false, "this host's network is set up outside MeatNet");
        } else if (r.path == "/api/power") {
            res.body = result(false, "power management unavailable");
        } else if (r.path == "/api/log") {
            res.body = json_text(g_logring.since(static_cast<uint32_t>(strtoul(query_value(r.query, "since").c_str(), nullptr, 10))));
        } else if (r.path == "/api/reboot" && r.method == "POST") {
            res.body = result(true, "");
            // "Reboot": start this process again with the same arguments, once the reply is out.
            std::thread([] {
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                execv(g_exe.c_str(), g_argv);
            }).detach();
        } else {
            res.status = 302;
            res.type = "text/plain";
        }
        return res;
    });
    if (!web.listen()) {
        log("[host] web UI: port " + std::to_string(opt.web_port) + " is in use");
        return 1;
    }
    threads.emplace_back([&web] { web.run(); });

    log("[host] " + opt.role + " unit " + serial + " on " + opt.adapter + ", web UI http://" + ip + ":" +
        std::to_string(opt.web_port) + "/" +
        (opt.role == "app" ? ", field unit on TCP " + std::to_string(cfg.split_port) : "") +
        (opt.role == "field" ? ", app unit " + (cfg.split_peer.empty() ? std::string("by beacon") : cfg.split_peer) : ""));
    g_main_loop_run(loop);

    log("[host] stopping");
    web.stop();
    if (server) server->stop();
    if (client) client->stop();
    for (auto &t : threads) t.join();
    if (central) central->shutdown();
    periph.reset();                     // unregisters the advertisements
    return 0;
}
