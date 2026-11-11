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
