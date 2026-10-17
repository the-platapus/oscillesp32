#pragma once
#include "esp_err.h"
typedef enum { ADC1_CHANNEL_6 = 6 } adc1_channel_t;
typedef enum { ADC_UNIT_1 = 1 } adc_unit_t;
typedef enum { ADC_ATTEN_DB_12 = 3 } adc_atten_t;
typedef enum { ADC_WIDTH_BIT_12 = 3 } adc_bits_width_t;
static inline esp_err_t adc1_config_channel_atten(adc1_channel_t, adc_atten_t) { return ESP_OK; }
