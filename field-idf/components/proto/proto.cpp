#include "proto.h"

#include <cstring>
#include <strings.h>

namespace proto {

std::string hex(const uint8_t *p, size_t n)
{
    static const char *d = "0123456789abcdef";
    std::string s;
    s.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        s += d[p[i] >> 4];
        s += d[p[i] & 15];
    }
    return s;
}

std::string hex(const std::string &bytes)
{
    return hex(reinterpret_cast<const uint8_t *>(bytes.data()), bytes.size());
}

static int nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool unhex(const std::string &text, std::string &out)
{
    if (text.size() % 2) return false;
    out.clear();
    for (size_t i = 0; i < text.size(); i += 2) {
        int hi = nibble(text[i]), lo = nibble(text[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi << 4 | lo);
    }
    return true;
}

bool meater_name(const uint8_t *name, size_t len)
{
    return name && len >= 6 &&
           strncasecmp(reinterpret_cast<const char *>(name), "MEATER", 6) == 0;
}

// C9E2746C-59F1-4E54-A0DD-E1E54555CF8B (MEATER+) and
// DCBB67CA-64FB-41A3-99D1-5D9FD8CF33CA, little-endian byte order.
static const uint8_t kMeaterUuids[2][16] = {
    {0x8B, 0xCF, 0x55, 0x45, 0xE5, 0xE1, 0xDD, 0xA0,
     0x54, 0x4E, 0xF1, 0x59, 0x6C, 0x74, 0xE2, 0xC9},
    {0xCA, 0x33, 0xCF, 0xD8, 0x9F, 0x5D, 0xD1, 0x99,
     0xA3, 0x41, 0xFB, 0x64, 0xCA, 0x67, 0xBB, 0xDC},
};

bool meater_service_uuid(const uint8_t uuid_le[16])
{
    for (const auto &u : kMeaterUuids)
        if (memcmp(u, uuid_le, 16) == 0) return true;
    return false;
}

bool uuid128_le(const char *text, uint8_t out[16])
{
    std::string hexs;
    for (const char *p = text; *p; p++)
        if (*p != '-') hexs += *p;
    std::string raw;
    if (hexs.size() != 32 || !unhex(hexs, raw)) return false;
    for (int i = 0; i < 16; i++) out[i] = static_cast<uint8_t>(raw[15 - i]);
    return true;
}

std::string addr_display(const uint8_t val_le[6])
{
    std::string s(6, '\0');
    for (int i = 0; i < 6; i++) s[i] = static_cast<char>(val_le[5 - i]);
    return s;
}

void addr_to_le(const std::string &display6, uint8_t val_le[6])
{
    for (int i = 0; i < 6; i++)
        val_le[i] = i < (int)display6.size() ? static_cast<uint8_t>(display6[5 - i]) : 0;
}

}  // namespace proto
