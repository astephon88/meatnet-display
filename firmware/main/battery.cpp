#include "battery.h"

#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"

static const char *TAG = "battery";

std::optional<double> battery_uv(int pin)
{
    static adc_oneshot_unit_handle_t unit;
    static adc_cali_handle_t cali;
    static adc_channel_t ch;
    static int state = 0;               // 0 = not set up yet, 1 = ready, -1 = unavailable
    if (state == 0) {
        state = -1;
        adc_unit_t id;
        if (pin < 0) return std::nullopt;
        if (adc_oneshot_io_to_channel(pin, &id, &ch) != ESP_OK || id != ADC_UNIT_1) {
            ESP_LOGW(TAG, "battery pin %d has no ADC1 channel", pin);
            return std::nullopt;
        }
        adc_oneshot_unit_init_cfg_t uc = {};
        uc.unit_id = id;
        adc_oneshot_chan_cfg_t cc = {};
        cc.atten = ADC_ATTEN_DB_12;
        cc.bitwidth = ADC_BITWIDTH_DEFAULT;
        adc_cali_curve_fitting_config_t kc = {};
        kc.unit_id = id;
        kc.chan = ch;
        kc.atten = ADC_ATTEN_DB_12;
        kc.bitwidth = ADC_BITWIDTH_DEFAULT;
        if (adc_oneshot_new_unit(&uc, &unit) != ESP_OK || adc_oneshot_config_channel(unit, ch, &cc) != ESP_OK ||
            adc_cali_create_scheme_curve_fitting(&kc, &cali) != ESP_OK) {
            ESP_LOGW(TAG, "battery ADC unavailable");
            return std::nullopt;
        }
        state = 1;
    }
    int raw, mv;
    if (state != 1 || adc_oneshot_read(unit, ch, &raw) != ESP_OK || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK)
        return std::nullopt;
    return mv * 1000.0;
}
