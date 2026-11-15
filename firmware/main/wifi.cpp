// WiFi: joins the saved network; with none set (or none reachable) opens the
// setup network, whose captive portal leads to the web page.
#include "wifi.h"

#include <atomic>
#include <cstring>

#include "esp_coexist.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "mdns.h"

#include "captive.h"
#include "settings.h"

namespace wifi {

static const char *TAG = "net";

static constexpr int64_t kSetupApAfterMs = 30000;   // no IP this long: open the setup network
// While the setup network is up, WiFi is retried this rarely: every attempt
// scans all channels, and the setup network (same radio) can't be joined
// meanwhile. No retries at all while a phone is on the setup network.
static constexpr int64_t kSetupRetryMs = 60000;

static portMUX_TYPE g_net_mux = portMUX_INITIALIZER_UNLOCKED;
static char g_ip[16] = "";
static char g_setup_ap[33] = "";       // setup network's SSID while it is up
static esp_timer_handle_t g_ap_timer;
static esp_timer_handle_t g_retry_timer;
static std::atomic<int> g_ap_clients{0};
static esp_netif_ip_info_t g_ap_ip;    // the setup network's address (192.168.4.1)
static std::string g_serial, g_hostname;

static void set_net(char *dst, size_t n, const char *value)
{
    portENTER_CRITICAL(&g_net_mux);
    strlcpy(dst, value, n);
    portEXIT_CRITICAL(&g_net_mux);
}

static std::string get_net(const char *src)
{
    portENTER_CRITICAL(&g_net_mux);
    std::string v(src);
    portEXIT_CRITICAL(&g_net_mux);
    return v;
}

// Setup network "MeatNet-<serial>" next to the station, so the web page can
// be reached at 192.168.4.1 to enter (or fix) the WiFi settings. Open unless
// a setup password is saved; its DNS sends phones to the page (captive
// portal).
static void setup_ap_start(void * = nullptr)
{
    if (!get_net(g_setup_ap).empty()) return;
    std::string ssid = "MeatNet-" + g_serial;
    std::string pw = settings::get().ap_password;
    wifi_config_t ap;
    memset(&ap, 0, sizeof(ap));
    strlcpy((char *)ap.ap.ssid, ssid.c_str(), sizeof(ap.ap.ssid));
    ap.ap.ssid_len = ssid.size();
    strlcpy((char *)ap.ap.password, pw.c_str(), sizeof(ap.ap.password));
    ap.ap.authmode = pw.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    ap.ap.channel = 1;
    // The station stays on (idle without a network) so the page can scan.
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    set_net(g_setup_ap, sizeof(g_setup_ap), ssid.c_str());
    captive::start(g_ap_ip.ip.addr);
    esp_wifi_disconnect();          // stop the current attempt; retried slowly from now on
    ESP_LOGW(TAG, "setup network '%s' up (%s): settings at http://" IPSTR "/", ssid.c_str(),
             pw.empty() ? "open" : "WPA2", IP2STR(&g_ap_ip.ip));
}

static void setup_ap_stop()
{
    if (get_net(g_setup_ap).empty()) return;
    esp_timer_stop(g_retry_timer);
    captive::stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    set_net(g_setup_ap, sizeof(g_setup_ap), "");
    ESP_LOGI(TAG, "setup network closed");
}

static void sta_retry(void *)
{
    if (g_ap_clients > 0)
        esp_timer_start_once(g_retry_timer, kSetupRetryMs * 1000);   // a phone is setting us up
    else
        esp_wifi_connect();
}

static void mdns_start();

static void wifi_event(void *, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        if (!settings::get().wifi_ssid.empty()) esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        set_net(g_ip, sizeof(g_ip), "");
        if (get_net(g_setup_ap).empty()) {
            ESP_LOGW(TAG, "WiFi disconnected, retrying");
            esp_wifi_connect();
        } else if (!esp_timer_is_active(g_retry_timer)) {
            ESP_LOGW(TAG, "WiFi not reachable; retrying in %llds (setup network up)",
                     (long long)(kSetupRetryMs / 1000));
            esp_timer_start_once(g_retry_timer, kSetupRetryMs * 1000);
        }
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STACONNECTED) {
        g_ap_clients++;
        ESP_LOGI(TAG, "a device joined the setup network (%d on it)", g_ap_clients.load());
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (g_ap_clients > 0) g_ap_clients--;
        ESP_LOGI(TAG, "a device left the setup network (%d on it)", g_ap_clients.load());
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        auto *ev = static_cast<ip_event_got_ip_t *>(data);
        char ip[16];
        snprintf(ip, sizeof(ip), IPSTR, IP2STR(&ev->ip_info.ip));
        set_net(g_ip, sizeof(g_ip), ip);
        ESP_LOGI(TAG, "WiFi up, IP %s", ip);
        mdns_start();
        esp_timer_stop(g_ap_timer);
        setup_ap_stop();
    }
}

// <hostname>.local, and the web page as an _http._tcp service.
static void mdns_start()
{
    static bool started = false;
    if (started) return;
    started = true;
    if (mdns_init() != ESP_OK) return;
    mdns_hostname_set(g_hostname.c_str());
    mdns_instance_name_set(("MeatNet " + g_serial).c_str());
    mdns_service_add(nullptr, "_http", "_tcp", 80, nullptr, 0);
    ESP_LOGI(TAG, "mDNS: %s.local", g_hostname.c_str());
}

void start(const std::string &serial, const core::Settings &s)
{
    g_serial = serial;
    g_hostname = s.hostname;
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, g_hostname.c_str());     // DHCP; mDNS once WiFi is up
    esp_netif_t *ap = esp_netif_create_default_wifi_ap();
    // The setup network's DHCP hands out the unit itself as DNS server, and
    // the page as captive-portal address (RFC 8910) for phones that ask.
    static char portal_uri[32];
    esp_netif_get_ip_info(ap, &g_ap_ip);
    snprintf(portal_uri, sizeof(portal_uri), "http://" IPSTR "/", IP2STR(&g_ap_ip.ip));
    esp_netif_dns_info_t dns = {};
    dns.ip.type = ESP_IPADDR_TYPE_V4;
    dns.ip.u_addr.ip4 = g_ap_ip.ip;
    uint8_t offer_dns = 1;
    esp_netif_dhcps_stop(ap);
    esp_netif_set_dns_info(ap, ESP_NETIF_DNS_MAIN, &dns);
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER, &offer_dns,
                           sizeof(offer_dns));
    esp_netif_dhcps_option(ap, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, portal_uri,
                           strlen(portal_uri));
    esp_netif_dhcps_start(ap);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event, nullptr));
    esp_timer_create_args_t ta = {};
    ta.callback = setup_ap_start;
    ta.name = "setup_ap";
    ESP_ERROR_CHECK(esp_timer_create(&ta, &g_ap_timer));
    ta.callback = sta_retry;
    ta.name = "sta_retry";
    ESP_ERROR_CHECK(esp_timer_create(&ta, &g_retry_timer));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));

    if (s.wifi_ssid.empty()) {
        ESP_LOGW(TAG, "no WiFi network set");
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        ESP_ERROR_CHECK(esp_wifi_start());
        setup_ap_start();
    } else {
        wifi_config_t wc;
        memset(&wc, 0, sizeof(wc));
        strlcpy((char *)wc.sta.ssid, s.wifi_ssid.c_str(), sizeof(wc.sta.ssid));
        strlcpy((char *)wc.sta.password, s.wifi_password.c_str(), sizeof(wc.sta.password));
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
        ESP_ERROR_CHECK(esp_wifi_start());
        esp_timer_start_once(g_ap_timer, kSetupApAfterMs * 1000);
        ESP_LOGI(TAG, "joining WiFi '%s'", s.wifi_ssid.c_str());
    }
    // Who gets the shared radio when WiFi and Bluetooth both want it. The
    // API is deprecated in this IDF and may be a no-op on some chips: log
    // the result instead of assuming it helps.
    esp_coex_prefer_t pref = s.coex == "wifi" ? ESP_COEX_PREFER_WIFI
                           : s.coex == "balance" ? ESP_COEX_PREFER_BALANCE : ESP_COEX_PREFER_BT;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
    esp_err_t cx = esp_coex_preference_set(pref);
#pragma GCC diagnostic pop
    ESP_LOGI(TAG, "coexistence preference %s: %s", s.coex.c_str(), esp_err_to_name(cx));
}

std::string ip() { return get_net(g_ip); }

std::string setup_ssid() { return get_net(g_setup_ap); }

uint32_t setup_addr() { return g_ap_ip.ip.addr; }

std::string setup_ip()
{
    char text[16];
    snprintf(text, sizeof(text), IPSTR, IP2STR(&g_ap_ip.ip));
    return text;
}

}  // namespace wifi
