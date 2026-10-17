#pragma once
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
typedef uint32_t nvs_handle_t;
enum { NVS_READWRITE = 1 };
static inline esp_err_t nvs_open(const char*, int, nvs_handle_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_get_u32(nvs_handle_t, const char*, uint32_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_get_blob(nvs_handle_t, const char*, void*, size_t*) { return ESP_FAIL; }
static inline esp_err_t nvs_set_blob(nvs_handle_t, const char*, const void*, size_t) { return ESP_OK; }
static inline esp_err_t nvs_set_u32(nvs_handle_t, const char*, uint32_t) { return ESP_OK; }
static inline esp_err_t nvs_commit(nvs_handle_t) { return ESP_OK; }
