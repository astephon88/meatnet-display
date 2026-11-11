#include <cstring>

#include "doctest.h"
#include "proto.h"

TEST_CASE("hex round trip")
{
    std::string raw("\x00\x10\xAB\xff", 4), back;
    CHECK(proto::hex(raw) == "0010abff");
    CHECK(proto::unhex("0010ABff", back));
    CHECK(back == raw);
    CHECK_FALSE(proto::unhex("abc", back));
    CHECK_FALSE(proto::unhex("zz", back));
}

TEST_CASE("uuid128_le reverses the bytes")
{
    uint8_t u[16];
    REQUIRE(proto::uuid128_le("6E400003-B5A3-F393-E0A9-E50E24DCCA9E", u));
    CHECK(u[0] == 0x9E);
    CHECK(u[15] == 0x6E);
    CHECK_FALSE(proto::uuid128_le("6E400003", u));
}

TEST_CASE("MEATER detection")
{
    CHECK(proto::meater_name((const uint8_t *)"MEATER+ ", 8));
    CHECK(proto::meater_name((const uint8_t *)"meater", 6));
    CHECK_FALSE(proto::meater_name((const uint8_t *)"MEAT", 4));
    uint8_t u[16];
    REQUIRE(proto::uuid128_le("C9E2746C-59F1-4E54-A0DD-E1E54555CF8B", u));
    CHECK(proto::meater_service_uuid(u));
    u[0] ^= 1;
    CHECK_FALSE(proto::meater_service_uuid(u));
}

TEST_CASE("addresses: little-endian <-> display order")
{
    const uint8_t le[6] = {0x8E, 0x26, 0x77, 0x92, 0x05, 0xD4};
    std::string disp = proto::addr_display(le);
    CHECK(proto::hex(disp) == "d4059277268e");
    uint8_t back[6];
    proto::addr_to_le(disp, back);
    CHECK(memcmp(back, le, 6) == 0);
}
