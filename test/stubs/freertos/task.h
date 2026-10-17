#pragma once
#include "FreeRTOS.h"
typedef void (*TaskFunction_t)(void*);
typedef void* TaskHandle_t;
static inline void vTaskDelay(TickType_t) {}
static inline BaseType_t xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*, int, TaskHandle_t*, int) { return pdPASS; }
