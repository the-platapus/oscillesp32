#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/adc.h"
#include "freertos/FreeRTOS.h"
typedef enum { I2S_NUM_0 = 0 } i2s_port_t;
typedef int i2s_mode_t;
enum { I2S_MODE_MASTER = 1, I2S_MODE_RX = 4, I2S_MODE_ADC_BUILT_IN = 32 };
enum { I2S_BITS_PER_SAMPLE_16BIT = 16, I2S_CHANNEL_FMT_ONLY_LEFT = 4, I2S_COMM_FORMAT_STAND_MSB = 2, ESP_INTR_FLAG_LEVEL1 = 2 };
typedef struct { i2s_mode_t mode; uint32_t sample_rate; int bits_per_sample, channel_format, communication_format, intr_alloc_flags, dma_buf_count, dma_buf_len; bool use_apll; } i2s_config_t;
static inline esp_err_t i2s_driver_install(i2s_port_t, const i2s_config_t*, int, void*) { return ESP_OK; }
static inline esp_err_t i2s_driver_uninstall(i2s_port_t) { return ESP_OK; }
static inline esp_err_t i2s_set_adc_mode(adc_unit_t, adc1_channel_t) { return ESP_OK; }
static inline esp_err_t i2s_adc_enable(i2s_port_t) { return ESP_OK; }
static inline esp_err_t i2s_adc_disable(i2s_port_t) { return ESP_OK; }
static inline esp_err_t i2s_read(i2s_port_t, void*, size_t, size_t* got, TickType_t) { *got = 0; return ESP_ERR_TIMEOUT; }
