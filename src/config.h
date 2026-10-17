#pragma once
// Hardware configuration. Change pins here if your wiring differs.

#include "platform.h"
#include "driver/adc.h"

// ---- Analog input (must be an ADC1 pin: WiFi blocks ADC2) ----
// GPIO34 = ADC1_CHANNEL_6. Input-only pin, no internal pull-ups.
#define PIN_PROBE          34
#define PROBE_ADC_CHANNEL  ADC1_CHANNEL_6

// ---- SSD1306 OLED (I2C) ----
// Tried first; if nothing answers, the driver scans common SDA/SCL pairs.
#define PIN_OLED_SDA       21
#define PIN_OLED_SCL       22
#define OLED_I2C_HZ        400000    // try 800000 for faster refresh with short wires

// ---- Buttons (active low) ----
// BOOT is on every NodeMCU/DevKit board. The other three are optional:
// wire a push button from the pin to GND. Unconnected pins idle high.
#define PIN_BTN_BOOT       0
#define PIN_BTN_SEL        27
#define PIN_BTN_UP         14
#define PIN_BTN_DOWN       13

// ---- Signal generator outputs ----
#define PIN_GEN_SQUARE     26   // LEDC PWM: square/pulse wave, 0-3.3 V
#define PIN_GEN_SINE       25   // DAC1 cosine generator, ~0.1-3.1 V

// ---- WiFi defaults (AP mode when no network is configured) ----
#define AP_SSID            "ESP32-Scope"
#define AP_PASS            "scope1234"   // >= 8 chars for WPA2
#define MDNS_NAME          "scope"       // http://scope.local in STA mode

// ---- Acquisition ----
#define REC_POINTS         1000       // points per screen (10 divisions)
#define RING_POINTS        2048       // must be a power of two >= REC_POINTS
#define DMA_BUF_LEN        1024       // samples per DMA buffer (max 1024)
#define DMA_BUF_COUNT      8
