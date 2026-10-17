#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef enum { DAC_CHANNEL_1 = 0, DAC_CHANNEL_2 = 1 } dac_channel_t;
static inline esp_err_t dac_output_enable(dac_channel_t) { return ESP_OK; }
static inline esp_err_t dac_output_disable(dac_channel_t) { return ESP_OK; }
static inline esp_err_t dac_output_voltage(dac_channel_t, uint8_t) { return ESP_OK; }
