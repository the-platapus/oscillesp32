#pragma once
#include "platform.h"

// ---- Lookup tables ----
extern const float TDIV_TABLE[];      // seconds per division
extern const char* const TDIV_LABEL[];
extern const int TDIV_COUNT;
extern const float VDIV_TABLE[];      // volts per division
extern const char* const VDIV_LABEL[];
extern const int VDIV_COUNT;

enum TrigMode : uint8_t { TRIG_AUTO = 0, TRIG_NORMAL = 1, TRIG_SINGLE = 2 };
enum TrigEdge : uint8_t { EDGE_RISE = 0, EDGE_FALL = 1 };
enum Coupling : uint8_t { COUPLE_DC = 0, COUPLE_AC = 1 };
enum View     : uint8_t { VIEW_SCOPE = 0, VIEW_METER = 1, VIEW_FFT = 2, VIEW_INFO = 3, VIEW_COUNT = 4 };

// Analog front-end presets. calZero/calGain are what actually get used;
// a preset just loads sensible starting values for them.
enum FrontEnd : uint8_t { FE_DIRECT = 0, FE_BIAS = 1, FE_CUSTOM = 2 };

struct Settings {
  // horizontal
  uint8_t tdiv;          // index into TDIV_TABLE
  float   hpos;          // trigger position as fraction of screen, 0.05..0.95
  // vertical
  uint8_t vdiv;          // index into VDIV_TABLE
  float   vpos;          // trace offset in volts (added before drawing)
  uint8_t coupling;
  // trigger
  uint8_t trigMode;
  uint8_t trigEdge;
  float   trigLevel;     // volts at the probe (relative to mean in AC coupling)
  // display
  uint8_t view;
  // front-end calibration:  Vin = (Vpin_mV - calZero) / 1000 * calGain
  uint8_t frontEnd;
  float   calZero;       // pin millivolts that read as 0 V at the probe
  float   calGain;       // probe volts per pin volt
  // ADC data-path quirks (verified by `selftest`)
  uint8_t swapPairs;     // I2S delivers 16-bit samples in swapped pairs
  uint8_t invert;        // ADC codes inverted
  float   rateCorr;      // true sample rate = nominal * rateCorr
  // ADC hardware timing (found by `selftest`; 0 = ESP-IDF default)
  uint32_t hwRate;       // I2S/ADC conversion rate requested, S/s
  uint8_t sarClkDiv;     // SAR clock = 80 MHz / sarClkDiv
  uint8_t sarCycles;     // sample-and-hold time in SAR clocks (1..7)
  uint8_t dupPairs;      // DMA delivers every conversion twice: use one of each pair
  // signal generator
  uint8_t genSquareOn;
  float   genSquareHz;
  float   genSquareDuty; // percent
  uint8_t genSineOn;
  float   genSineHz;
  // network
  uint8_t wifiMode;      // 0 = off, 1 = access point, 2 = join network (falls back to AP)
  char    staSsid[33];
  char    staPass[65];
};

extern Settings S;
extern volatile uint32_t gSettingsVersion;   // bumped on every change
extern volatile bool gRunning;               // run/stop
extern SemaphoreHandle_t gCmdMutex;          // serialises commands from serial/web/buttons

void settingsInit();                  // load from NVS (or defaults)
void settingsDefaults();
void settingsChanged();               // bump version + schedule save
void settingsSaveIfDue();             // call periodically
void settingsSaveNow();
void applyFrontEndPreset(uint8_t fe);

// Vin(volts) <-> pin millivolts using current calibration
inline float pinMvToVolts(float mv) { return (mv - S.calZero) * 0.001f * S.calGain; }
inline float voltsToPinMv(float v)  { return v * 1000.0f / S.calGain + S.calZero; }
