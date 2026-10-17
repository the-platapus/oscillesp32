#pragma once
// Host stub: the last full-frame write is kept in g_oledFrame for inspection.
#include <stdint.h>
#include <string.h>
#include <stddef.h>
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
extern uint8_t g_oledFrame[1024];
typedef int i2c_port_t;
#define I2C_NUM_0 0
#define I2C_MODE_MASTER 1
#define I2C_MASTER_WRITE 0
typedef struct { int mode, sda_io_num, scl_io_num, sda_pullup_en, scl_pullup_en; struct { uint32_t clk_speed; } master; } i2c_config_t;
typedef void* i2c_cmd_handle_t;
static inline esp_err_t i2c_driver_delete(i2c_port_t) { return ESP_OK; }
static inline esp_err_t i2c_param_config(i2c_port_t, const i2c_config_t*) { return ESP_OK; }
static inline esp_err_t i2c_driver_install(i2c_port_t, int, int, int, int) { return ESP_OK; }
static inline i2c_cmd_handle_t i2c_cmd_link_create() { return (void*)1; }
static inline void i2c_cmd_link_delete(i2c_cmd_handle_t) {}
static inline esp_err_t i2c_master_start(i2c_cmd_handle_t) { return ESP_OK; }
static inline esp_err_t i2c_master_stop(i2c_cmd_handle_t) { return ESP_OK; }
static inline esp_err_t i2c_master_write_byte(i2c_cmd_handle_t, uint8_t, bool) { return ESP_OK; }
static inline esp_err_t i2c_master_write(i2c_cmd_handle_t, const uint8_t* d, size_t n, bool) { if (n == 1024) memcpy(g_oledFrame, d, 1024); return ESP_OK; }
static inline esp_err_t i2c_master_cmd_begin(i2c_port_t, i2c_cmd_handle_t, TickType_t) { return ESP_OK; }
static inline esp_err_t i2c_master_write_to_device(i2c_port_t, uint8_t, const uint8_t*, size_t, TickType_t) { return ESP_OK; }
