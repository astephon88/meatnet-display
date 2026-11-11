#include "cJSON.h"
#include "doctest.h"
#include "unit_settings.h"

TEST_CASE("settings: merge like config.load, round trip, hidden secrets")
{
    core::Settings s;
    core::merge_settings(s, std::string(R"({"upstream_node": "G000001CA8", "max_conn": 5,
        "pins": {"bl": 2}, "linked_probes": ["10018F16", 7], "unit": 3, "nope": 1,
        "meater_probes": [{"addr": "AA:BB:CC:DD:EE:FF", "id": 3}], "split_key": "k"})"));
    CHECK(s.upstream_node == std::vector<std::string>{"G000001CA8"});
    CHECK(s.max_conn == 5);
    CHECK(s.pins.bl == 2);
    CHECK(s.pins.sck == 10);                    // other pins keep their defaults
    CHECK(s.linked_probes == std::vector<std::string>{"10018F16"});
    CHECK(s.unit == "c");                       // wrong type: ignored
    REQUIRE(s.meater_probes.size() == 1);
    CHECK(s.meater_probes[0].id == 3);
    CHECK(s.meater_probes[0].color == 0);

    core::merge_settings(s, std::string(R"({"upstream_node": ""})"));
    CHECK(s.upstream_node.empty());

    cJSON *full = core::settings_to_json(s, true);
    core::Settings t;
    core::merge_settings(t, full);
    cJSON_Delete(full);
    CHECK(t.max_conn == 5);
    CHECK(t.pins.bl == 2);
    CHECK(t.split_key == "k");
    CHECK(t.meater_probes.size() == 1);

    cJSON *pub = core::settings_to_json(s, false);
    CHECK(cJSON_GetObjectItem(pub, "split_key") == nullptr);
    CHECK(cJSON_IsTrue(cJSON_GetObjectItem(pub, "split_key_set")));
    CHECK(cJSON_IsFalse(cJSON_GetObjectItem(pub, "wifi_password_set")));
    cJSON_Delete(pub);
}

static bool apply(core::Settings &s, const char *json, std::string &err, bool &reboot)
{
    cJSON *o = cJSON_Parse(json);
    bool ok = core::apply_config(s, o, err, reboot);
    cJSON_Delete(o);
    return ok;
}

TEST_CASE("settings: apply_config validates, normalises and flags reboots")
{
    core::Settings s;
    s.split_key = "secret";
    std::string err;
    bool reboot = true;
    REQUIRE(apply(s, R"({"unit": "f", "linked_probes": ["10018f16"], "upstream_node": "G000001CA8",
                         "nope": 3})", err, reboot));
    CHECK_FALSE(reboot);
    CHECK(s.unit == "f");
    CHECK(s.linked_probes == std::vector<std::string>{"10018F16"});
    CHECK(s.upstream_node == std::vector<std::string>{"G000001CA8"});

    REQUIRE(apply(s, R"({"max_conn": 5, "split_key": "", "pins": {"bl": 2}})", err, reboot));
    CHECK(reboot);
    CHECK(s.max_conn == 5);
    CHECK(s.split_key == "secret");             // blank keeps the saved key
    CHECK(s.pins.bl == 2);

    // One bad key: nothing changes.
    CHECK_FALSE(apply(s, R"({"unit": "c", "brightness": 101})", err, reboot));
    CHECK(err.find("brightness") == 0);
    CHECK(s.unit == "f");
    CHECK_FALSE(apply(s, R"({"wifi_password": "short"})", err, reboot));
    CHECK_FALSE(apply(s, R"({"linked_probes": ["XYZ"]})", err, reboot));
    CHECK_FALSE(apply(s, R"({"role": "both"})", err, reboot));
    CHECK_FALSE(apply(s, R"({"meater_probes": [{"addr": "AA:BB", "id": 3}]})", err, reboot));

    REQUIRE(apply(s, R"({"wifi_password": "", "meater_probes": [{"addr": "aa:bb:cc:dd:ee:ff", "id": 3}]})",
                  err, reboot));
    CHECK(s.meater_probes[0].addr == "AA:BB:CC:DD:EE:FF");
    CHECK(s.meater_probes[0].id == 3);
}
