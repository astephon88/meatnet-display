// Renders the round screen's pages on the host (core/screen_draw, the code
// the firmware runs) for a sample status, as PPM images:
//
//     ./build/tools/screen_preview [out_dir]      (default docs/preview)
//
// Same pages as tools/preview.py renders for the MicroPython screen.
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "cJSON.h"
#include "readings.h"
#include "screen_draw.h"

namespace sd = screen_draw;

static const char *kStatus = R"({
  "probes": [
    {"serial": "1000ABCD", "id": 1, "color": 0, "mode": 0, "core_c": 57.3, "surface_c": 71.0,
     "ambient_c": 121.5, "instant_c": null, "temps": [57.3, 57.3, 57.3, 57.3, 57.3, 57.3, 57.3, 57.3],
     "low_battery": false, "src": "link", "hops": 0, "rssi": -61, "age_s": 1, "alarming": false,
     "prediction": {"state": 3, "type": 1, "seconds": 4380, "setpoint_c": 63.0}},
    {"serial": "1000BEEF", "id": 2, "color": 1, "mode": 0, "core_c": 72.0, "surface_c": 80.0,
     "ambient_c": 150.0, "instant_c": null, "temps": [72, 72, 72, 72, 72, 72, 72, 72],
     "low_battery": true, "src": "advert", "hops": 0, "rssi": -78, "age_s": 2, "alarming": true,
     "prediction": null}],
  "devices": [{"serial": "GAUGE00001", "ptype": 3, "name": "Gauge", "temp_c": 135.0, "rssi": -70, "age_s": 3}],
  "links": [{"kind": "probe", "serial": "1000ABCD", "state": "ready"}],
  "inbound": 1})";

static std::vector<uint8_t> load(const std::string &path)
{
    std::ifstream f(path, std::ios::binary);
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(f), {});
}

static void write_ppm(const std::vector<sd::Prim> &prims, const std::string &path)
{
    std::vector<uint16_t> buf(sd::W * sd::H);
    sd::raster(prims, 0, sd::H, buf.data());
    FILE *f = fopen(path.c_str(), "wb");
    fprintf(f, "P6\n%d %d\n255\n", sd::W, sd::H);
    for (int y = 0; y < sd::H; y++) {
        for (int x = 0; x < sd::W; x++) {
            uint16_t v = buf[y * sd::W + x];
            v = static_cast<uint16_t>(v << 8 | v >> 8);         // panel byte order
            int dx = x - 120, dy = y - 120;
            bool in = dx * dx + dy * dy <= 120 * 120;
            uint8_t px[3] = {static_cast<uint8_t>(in ? (v >> 11) << 3 : 40),
                             static_cast<uint8_t>(in ? ((v >> 5) & 0x3F) << 2 : 40),
                             static_cast<uint8_t>(in ? (v & 0x1F) << 3 : 40)};
            fwrite(px, 1, 3, f);
        }
    }
    fclose(f);
    printf("%s\n", path.c_str());
}

static void render(const sd::Fonts &fonts, const core::Settings &cfg, const cJSON *status, const std::string &wifi,
                   const std::string &out, int n)
{
    std::string ip = wifi == "ap" ? "192.168.4.1" : "192.168.1.57";
    auto pages = readings::pages(cfg, readings::build_items(cfg, status, wifi, ip, "MeatNet-B2C4"));
    for (int i = 0; i < n && i < static_cast<int>(pages.size()); i++) {
        sd::View v;
        v.layout = cfg.layout;
        v.unit = cfg.unit;
        v.page = i;
        v.total = static_cast<int>(pages.size());
        v.items = pages[i];
        write_ppm(sd::draw(fonts, v), out + "/" + cfg.layout + "_" + wifi + "_" + std::to_string(i) + ".ppm");
    }
}

int main(int argc, char **argv)
{
    std::string root = MEATNET_ROOT, out = argc > 1 ? argv[1] : root + "/docs/preview";
    auto big = load(root + "/fonts/big.bin"), mid = load(root + "/fonts/mid.bin"),
         med = load(root + "/fonts/med.bin"), small = load(root + "/fonts/small.bin");
    sd::Font fb(big.data()), fm(mid.data()), fd(med.data()), fs(small.data());
    sd::Fonts fonts{&fb, &fm, &fd, &fs};
    cJSON *status = cJSON_Parse(kStatus);
    core::Settings cfg;
    cfg.unit = "f";
    cfg.readings = {{"probe:1000ABCD", "core", ""}, {"probe:1000ABCD", "eta", ""},
                    {"probe:1000BEEF", "core", ""}, {"node:GAUGE00001", "temp", ""}};
    render(fonts, cfg, status, "sta", out, 5);
    cfg.layout = "quad";
    render(fonts, cfg, status, "sta", out, 1);
    core::Settings ap;
    render(fonts, ap, status, "ap", out, 1);
    cJSON_Delete(status);
}
