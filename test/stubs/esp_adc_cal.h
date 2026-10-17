#pragma once
#include <stdint.h>
#include "driver/adc.h"
typedef enum { ESP_ADC_CAL_VAL_EFUSE_VREF, ESP_ADC_CAL_VAL_EFUSE_TP, ESP_ADC_CAL_VAL_DEFAULT_VREF } esp_adc_cal_value_t;
typedef struct { int dummy; } esp_adc_cal_characteristics_t;
static inline esp_adc_cal_value_t esp_adc_cal_characterize(adc_unit_t, adc_atten_t, adc_bits_width_t, uint32_t, esp_adc_cal_characteristics_t*) { return ESP_ADC_CAL_VAL_EFUSE_VREF; }
// rough ESP32 11/12 dB transfer: ~75 mV at code 0, ~3228 mV at 4095
static inline uint32_t esp_adc_cal_raw_to_voltage(uint32_t raw, const esp_adc_cal_characteristics_t*) { return 75 + (raw * 770) / 1000; }
