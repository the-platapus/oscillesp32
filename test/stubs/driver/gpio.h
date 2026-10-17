#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
static inline esp_err_t gpio_reset_pin(gpio_num_t) { return ESP_OK; }
static inline esp_err_t gpio_config(const gpio_config_t*) { return ESP_OK; }
static inline esp_err_t gpio_set_direction(gpio_num_t, gpio_mode_t) { return ESP_OK; }
static inline esp_err_t gpio_set_level(gpio_num_t, uint32_t) { return ESP_OK; }
static inline int gpio_get_level(gpio_num_t) { return 0; }
