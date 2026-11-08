#include "captive.h"

#include <atomic>
#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"

namespace captive {

static const char *TAG = "captive";
static std::atomic<bool> g_run{false};
static std::atomic<bool> g_running{false};
static uint32_t g_ip;

// Builds the reply to one query in place; returns its length, or 0 to drop.
// Answers A/IN questions with g_ip and everything else with no answers.
static int answer(uint8_t *buf, int len, int cap)
{
    if (len < 12 || (buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) return 0;   // one question
    int p = 12;
    while (p < len && buf[p]) {
        if (buf[p] & 0xC0) return 0;            // no compression in questions
        p += buf[p] + 1;
    }
    if (p + 5 > len) return 0;
    uint16_t qtype = buf[p + 1] << 8 | buf[p + 2];
    uint16_t qclass = buf[p + 3] << 8 | buf[p + 4];
    int end = p + 5;                            // just past the question
    bool a = qtype == 1 && (qclass & 0x7FFF) == 1;
    buf[2] = 0x84 | (buf[2] & 0x01);            // response, authoritative, keep RD
    buf[3] = 0x00;                              // no recursion, NOERROR
    buf[6] = 0;
    buf[7] = a ? 1 : 0;                         // answers
    memset(buf + 8, 0, 4);                      // no authority/additional records
    if (!a) return end;
    if (end + 16 > cap) return 0;
    const uint8_t rr[12] = {0xC0, 0x0C,        // name: the question's
                            0, 1, 0, 1,        // type A, class IN
                            0, 0, 0, 60,       // TTL 60 s
                            0, 4};
    memcpy(buf + end, rr, sizeof(rr));
    memcpy(buf + end + 12, &g_ip, 4);
    return end + 16;
}

static void task(void *)
{
    int fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = g_ip;                // the setup network only
    timeval tv = {1, 0};                        // wake up to notice stop()
    if (fd < 0 || bind(fd, (sockaddr *)&addr, sizeof(addr)) != 0
        || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv)) != 0) {
        ESP_LOGE(TAG, "DNS socket failed");
        if (fd >= 0) close(fd);
        g_running = false;
        vTaskDelete(nullptr);
        return;
    }
    ESP_LOGI(TAG, "DNS answering every name with the setup address");
    uint8_t buf[512];
    while (g_run) {
        sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(fd, buf, sizeof(buf), 0, (sockaddr *)&from, &flen);
        if (n <= 0) continue;
        int out = answer(buf, n, sizeof(buf));
        if (out > 0) sendto(fd, buf, out, 0, (sockaddr *)&from, flen);
    }
    close(fd);
    g_running = false;
    vTaskDelete(nullptr);
}

void start(uint32_t ip)
{
    if (g_running) return;
    g_ip = ip;
    g_run = true;
    g_running = true;
    xTaskCreate(task, "captive_dns", 3072, nullptr, 4, nullptr);
}

void stop() { g_run = false; }   // the task closes its socket within a second

}  // namespace captive
