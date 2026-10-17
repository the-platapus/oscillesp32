#pragma once
// Small helpers so the rest of the code reads like Arduino code.
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static inline uint32_t millis() { return (uint32_t)(esp_timer_get_time() / 1000ULL); }
static inline void delay(uint32_t ms) { vTaskDelay(ms >= portTICK_PERIOD_MS ? pdMS_TO_TICKS(ms) : 1); }

template <typename T> static inline T clampv(T v, T lo, T hi) { return v < lo ? lo : v > hi ? hi : v; }
