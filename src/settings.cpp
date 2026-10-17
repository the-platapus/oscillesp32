#include "settings.h"
#include "nvs_flash.h"
#include "nvs.h"

const float TDIV_TABLE[] = {
  5e-6f, 10e-6f, 20e-6f, 50e-6f, 100e-6f, 200e-6f, 500e-6f,
  1e-3f, 2e-3f, 5e-3f, 10e-3f, 20e-3f, 50e-3f, 100e-3f, 200e-3f, 500e-3f,
  1.0f, 2.0f, 5.0f };
const char* const TDIV_LABEL[] = {
  "5us", "10us", "20us", "50us", "100us", "200us", "500us",
  "1ms", "2ms", "5ms", "10ms", "20ms", "50ms", "100ms", "200ms", "500ms",
  "1s", "2s", "5s" };
const int TDIV_COUNT = sizeof(TDIV_TABLE) / sizeof(TDIV_TABLE[0]);

const float VDIV_TABLE[] = {
  0.01f, 0.02f, 0.05f, 0.1f, 0.2f, 0.5f, 1.0f, 2.0f, 5.0f, 10.0f, 20.0f };
const char* const VDIV_LABEL[] = {
  "10mV", "20mV", "50mV", "0.1V", "0.2V", "0.5V", "1V", "2V", "5V", "10V", "20V" };
const int VDIV_COUNT = sizeof(VDIV_TABLE) / sizeof(VDIV_TABLE[0]);

Settings S;
volatile uint32_t gSettingsVersion = 1;
volatile bool gRunning = true;
SemaphoreHandle_t gCmdMutex = nullptr;

static nvs_handle_t sNvs = 0;
static uint32_t saveDueMs = 0;
static const uint32_t SETTINGS_MAGIC = 0x53435035;  // bump when Settings layout changes

void applyFrontEndPreset(uint8_t fe) {
  S.frontEnd = fe;
  switch (fe) {
    case FE_DIRECT: S.calZero = 0.0f;    S.calGain = 1.0f;  break;
    case FE_BIAS:   S.calZero = 1500.0f; S.calGain = 11.0f; break;   // 100k in, 20k/20k bias: Vpin = 1.5 V + Vin/11
    default: break;  // custom: keep whatever calibration is there
  }
}

void settingsDefaults() {
  memset(&S, 0, sizeof(S));
  S.tdiv = 7;            // 1 ms/div
  S.hpos = 0.5f;
  S.vdiv = 5;            // 0.5 V/div
  S.vpos = -1.5f;        // put 0-3 V signals on screen in direct mode
  S.coupling = COUPLE_DC;
  S.trigMode = TRIG_AUTO;
  S.trigEdge = EDGE_RISE;
  S.trigLevel = 1.5f;
  S.view = VIEW_SCOPE;
  applyFrontEndPreset(FE_DIRECT);
  S.swapPairs = 1;
  S.invert = 0;
  S.rateCorr = 1.0f;
  // conservative until `selftest` finds the fastest setting that reads correctly
  S.hwRate = 125000;
  S.sarClkDiv = 0;
  S.sarCycles = 0;
  S.dupPairs = 0;
  S.genSquareOn = 1;     // 1 kHz test square on GPIO26 out of the box
  S.genSquareHz = 1000.0f;
  S.genSquareDuty = 50.0f;
  S.genSineOn = 0;
  S.genSineHz = 1000.0f;
  S.wifiMode = 1;
}

void settingsInit() {
  gCmdMutex = xSemaphoreCreateRecursiveMutex();
  settingsDefaults();
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    nvs_flash_erase();
    nvs_flash_init();
  }
  if (nvs_open("scope", NVS_READWRITE, &sNvs) == ESP_OK) {
    uint32_t magic = 0;
    size_t len = sizeof(Settings);
    Settings tmp;
    if (nvs_get_u32(sNvs, "magic", &magic) == ESP_OK && magic == SETTINGS_MAGIC &&
        nvs_get_blob(sNvs, "s", &tmp, &len) == ESP_OK && len == sizeof(Settings)) {
      S = tmp;
    }
  }
  // sanity clamps in case of corrupted storage
  if (S.tdiv >= TDIV_COUNT) S.tdiv = 7;
  if (S.vdiv >= VDIV_COUNT) S.vdiv = 5;
  if (!(S.hpos >= 0.05f && S.hpos <= 0.95f)) S.hpos = 0.5f;
  if (!(S.calGain > 0.001f && S.calGain < 10000.0f)) applyFrontEndPreset(FE_DIRECT);
  if (!(S.rateCorr > 0.9f && S.rateCorr < 1.1f)) S.rateCorr = 1.0f;
  if (S.view >= VIEW_COUNT) S.view = VIEW_SCOPE;
  if (S.hwRate < 20000 || S.hwRate > 2000000) S.hwRate = 125000;
  if (S.sarCycles > 7) S.sarCycles = 0;
  S.staSsid[32] = 0;
  S.staPass[64] = 0;
}

void settingsChanged() {
  gSettingsVersion++;
  saveDueMs = millis() + 2000;   // debounce flash writes
  if (saveDueMs == 0) saveDueMs = 1;
}

void settingsSaveNow() {
  if (sNvs) {
    nvs_set_blob(sNvs, "s", &S, sizeof(Settings));
    nvs_set_u32(sNvs, "magic", SETTINGS_MAGIC);
    nvs_commit(sNvs);
  }
  saveDueMs = 0;
}

void settingsSaveIfDue() {
  if (saveDueMs && (int32_t)(millis() - saveDueMs) >= 0) {
    xSemaphoreTakeRecursive(gCmdMutex, portMAX_DELAY);
    settingsSaveNow();
    xSemaphoreGiveRecursive(gCmdMutex);
  }
}
