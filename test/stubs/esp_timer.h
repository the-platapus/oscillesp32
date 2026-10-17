#pragma once
#include "sim_clock.h"
static inline int64_t esp_timer_get_time() { return g_simNowUs; }
