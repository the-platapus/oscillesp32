// ADC sampling engine.
//
// The ESP32's I2S0 peripheral can drive the SAR ADC directly and DMA the
// results ("built-in ADC mode"). We run it continuously (up to ~1 MS/s, the
// rate `selftest` finds this chip reads correctly at) and do everything in
// software on the stream:
//   - frequency counter on every conversion
//   - boxcar decimation down to the rate the timebase needs
//   - edge trigger with hysteresis, pre-trigger ring buffer, sub-sample
//     trigger interpolation (keeps fast timebases from jittering)
//   - roll mode for slow timebases

#include "acquisition.h"
#include "settings.h"
#include <stdlib.h>
#include <stdarg.h>
#include "driver/i2s.h"
#include "driver/adc.h"
#include "driver/dac.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_adc_cal.h"
#include "hal/adc_ll.h"
#include "esp_log.h"
#include "siggen.h"

static const char* TAG = "acq";

#define RING_MASK (RING_POINTS - 1)
static_assert((RING_POINTS & RING_MASK) == 0, "RING_POINTS must be a power of two");
static_assert(RING_POINTS >= REC_POINTS, "ring too small");

static const i2s_port_t I2S_PORT = I2S_NUM_0;
static const size_t READ_BYTES = DMA_BUF_LEN * sizeof(uint16_t);

// ---- calibration ----
static uint16_t sLut[4097];                 // code -> pin mV (extra entry for interpolation)
static const char* sCalSource = "default";

// ---- rates ----
static float sRequestedRate = 125000;
static float sHwRateNominal = 125000;
static volatile float sMeasuredRate = 0;
static volatile float sFrameRate = 0;
static volatile float sCpuLoad = 0;          // share of core 1 spent processing samples
static volatile bool sForceHalf = false;     // this session's stream repeats words: use one of each pair

static inline bool halfRate() { return S.dupPairs || sForceHalf; }

// ---- shared outputs ----
static Frame sPub;
static SemaphoreHandle_t sFrameMutex;
static portMUX_TYPE sMux = portMUX_INITIALIZER_UNLOCKED;
static CounterResult sCounterOut;
static volatile uint8_t sState = ACQ_ARMING;
static volatile bool sLastTriggered = false;
static volatile bool sRearmReq = false;
static volatile bool sReinitReq = false;     // restart I2S with S.hwRate / S.sarClkDiv / S.sarCycles

// ---- tuner (selftest) ----
static volatile bool sTuneReq = false, sTuneDone = false, sTuneOk = false;
static volatile bool sTuneJumperOnly = false;
static char* volatile sTuneOut = nullptr;
static volatile size_t sTuneOutLen = 0;

// ---- raw capture (selftest) ----
static uint16_t* volatile sRawDst = nullptr;
static volatile uint32_t sRawWant = 0;
static volatile uint32_t sRawGot = 0;

// ============================================================ conversions

static void buildLut() {
  esp_adc_cal_characteristics_t ch;
  esp_adc_cal_value_t src = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_12,
                                                     ADC_WIDTH_BIT_12, 1100, &ch);
  sCalSource = src == ESP_ADC_CAL_VAL_EFUSE_TP   ? "eFuse two-point"
             : src == ESP_ADC_CAL_VAL_EFUSE_VREF ? "eFuse Vref"
                                                 : "default Vref";
  for (int i = 0; i < 4096; i++) sLut[i] = (uint16_t)esp_adc_cal_raw_to_voltage(i, &ch);
  sLut[4096] = sLut[4095];
}

float codeToMv(float code) {
  if (code <= 0) return sLut[0];
  if (code >= 4095) return sLut[4095];
  int i = (int)code;
  float f = code - i;
  return sLut[i] + (sLut[i + 1] - sLut[i]) * f;
}

float ptToVolts(uint16_t pt) { return pinMvToVolts(ptToMv(pt)); }

uint16_t voltsToPt(float v) {
  float mv = voltsToPinMv(v);
  if (mv <= sLut[0]) return 0;
  if (mv >= sLut[4095]) return 4095 * 16;
  // largest code with lut <= mv (lut is monotonic non-decreasing)
  int lo = 0, hi = 4095;
  while (hi - lo > 1) {
    int mid = (lo + hi) >> 1;
    if (sLut[mid] <= mv) lo = mid; else hi = mid;
  }
  float span = (float)sLut[lo + 1] - sLut[lo];
  float f = span > 0 ? (mv - sLut[lo]) / span : 0;
  if (f > 1) f = 1;
  return (uint16_t)(lo * 16 + f * 16 + 0.5f);
}

// ============================================================ I2S

struct HwCfg {
  uint32_t rate;
  uint8_t div;   // SAR clock = 80 MHz / div; 0 = ESP-IDF default (16, i.e. 5 MHz)
  uint8_t cyc;   // sample-and-hold SAR clocks; 0 = ESP-IDF default (2)
};

static HwCfg savedCfg() { return {S.hwRate, S.sarClkDiv, S.sarCycles}; }

static bool sI2sRunning = false;

static void i2sStop();

static bool i2sStart(const HwCfg& h) {
  i2s_config_t c;
  memset(&c, 0, sizeof(c));
  c.mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_ADC_BUILT_IN);
  c.sample_rate = h.rate;
  c.bits_per_sample = I2S_BITS_PER_SAMPLE_16BIT;
  c.channel_format = I2S_CHANNEL_FMT_ONLY_LEFT;
  c.communication_format = I2S_COMM_FORMAT_STAND_MSB;
  c.intr_alloc_flags = ESP_INTR_FLAG_LEVEL1;
  c.dma_buf_count = DMA_BUF_COUNT;
  c.dma_buf_len = DMA_BUF_LEN;
  c.use_apll = false;
  if (i2s_driver_install(I2S_PORT, &c, 0, nullptr) != ESP_OK) return false;
  sI2sRunning = true;
  if (i2s_set_adc_mode(ADC_UNIT_1, PROBE_ADC_CHANNEL) != ESP_OK) { i2sStop(); return false; }
  adc1_config_channel_atten(PROBE_ADC_CHANNEL, ADC_ATTEN_DB_12);
  if (i2s_adc_enable(I2S_PORT) != ESP_OK) { i2sStop(); return false; }
  // i2s_adc_enable() just reset the SAR controller to ESP-IDF's defaults
  // (80 MHz / 16 SAR clock, 2 sample cycles). Override if selftest picked
  // something quieter.
  if (h.div) {
    adc_ll_digi_set_clk_div(h.div);
    adc_ll_digi_set_fsm_time(8, h.div, 100);   // start wait = one SAR clock, as ESP-IDF does
  }
  if (h.cyc) adc_ll_set_sample_cycle(h.cyc);
  sRequestedRate = h.rate;
  return true;
}

static void i2sStop() {
  if (!sI2sRunning) return;
  sI2sRunning = false;
  i2s_adc_disable(I2S_PORT);
  // ESP-IDF 4.4's uninstall also switches off both DAC outputs
  // (i2s_set_dac_mode(DISABLE)), which would kill the sine generator
  i2s_driver_uninstall(I2S_PORT);
  siggenRestoreDac();
}

// Drops everything queued plus the buffer in flight, so the next read is fresh.
static void flushDma(uint16_t* buf) {
  size_t got;
  while (i2s_read(I2S_PORT, buf, READ_BYTES, &got, 0) == ESP_OK && got == READ_BYTES) {}
  i2s_read(I2S_PORT, buf, READ_BYTES, &got, pdMS_TO_TICKS(500));
}

// Measures the real conversion rate by timing DMA buffer completions.
static float measureRate(uint16_t* buf, uint32_t ms) {
  size_t got;
  flushDma(buf);
  int64_t t0 = esp_timer_get_time(), t1 = t0;
  uint64_t samples = 0;
  while (t1 - t0 < (int64_t)ms * 1000) {
    if (i2s_read(I2S_PORT, buf, READ_BYTES, &got, pdMS_TO_TICKS(500)) != ESP_OK) break;
    samples += got / 2;
    t1 = esp_timer_get_time();
  }
  return t1 > t0 ? samples * 1e6f / (float)(t1 - t0) : 0;
}

struct LevelStats { float mean, sd, chanOk, dup; };

// Reads 4 fresh DMA buffers: mean and noise of the codes, how many words carry
// the right channel, and how often neighbouring words are identical.
static bool captureStats(uint16_t* buf, LevelStats& r) {
  flushDma(buf);
  double sum = 0, sum2 = 0;
  uint32_t n = 0, good = 0, eqA = 0, eqB = 0, pairs = 0;
  size_t got;
  for (int b = 0; b < 4; b++) {
    if (i2s_read(I2S_PORT, buf, READ_BYTES, &got, pdMS_TO_TICKS(500)) != ESP_OK || !got) return false;
    const size_t cnt = got / 2;
    for (size_t i = 0; i < cnt; i++) {
      uint16_t w = buf[i], c = w & 0x0FFF;
      if ((w >> 12) == PROBE_ADC_CHANNEL) good++;
      sum += c;
      sum2 += (double)c * c;
      n++;
    }
    // identical neighbours: with ~10 codes of noise, independent conversions
    // match only a few % of the time; repeated ones always do
    for (size_t i = 0; i + 2 < cnt; i += 2) {
      eqA += (buf[i] & 0x0FFF) == (buf[i + 1] & 0x0FFF);
      eqB += (buf[i + 1] & 0x0FFF) == (buf[i + 2] & 0x0FFF);
      pairs++;
    }
  }
  r.mean = sum / n;
  double var = sum2 / n - (sum / n) * (sum / n);
  r.sd = var > 0 ? sqrt(var) : 0;
  r.chanOk = (float)good / n;
  r.dup = pairs ? (float)(eqA > eqB ? eqA : eqB) / pairs : 0;
  return true;
}

// Chance that two independent samples with Gaussian noise sd land on the same
// integer code: ~1/(2*sqrt(pi)*sd). Far above that, words are repeats.
static float chanceEqual(float sd) { return 1.0f / (3.545f * fmaxf(sd, 1.0f)); }

static bool measureLevel(uint16_t* buf, uint8_t dacLevel, LevelStats& r) {
  dac_output_voltage(DAC_CHANNEL_2, dacLevel);
  return captureStats(buf, r);
}

// Starts the hardware and works out the true conversion rate. The SAR
// controller pauses for 2 conversions every 256 (its measurement limit), so
// the ADC delivers 256/258 of the requested rate; other silicon/driver
// combinations have been reported at 2x or 1/2. Snap to a known ratio when the
// measurement agrees, otherwise trust the measurement.
static void startHw(HwCfg h, uint16_t* buf) {
  if (!i2sStart(h)) {
    ESP_LOGE(TAG, "ADC start failed at %lu S/s, falling back to 125 kS/s defaults", (unsigned long)h.rate);
    h = {125000, 0, 0};
    S.dupPairs = 0;
    i2sStart(h);
  }
  float m = measureRate(buf, 300);
  const float req = h.rate;
  const float ratios[] = {256.0f / 258.0f, 1.0f, 2.0f * 256.0f / 258.0f, 0.5f * 256.0f / 258.0f};
  sHwRateNominal = m > 0 ? m : req;
  for (float r : ratios) if (fabsf(m / (req * r) - 1.0f) < 0.003f) sHwRateNominal = req * r;
  sMeasuredRate = m;
  ESP_LOGI(TAG, "ADC: requested %.0f S/s, SAR div %d cycles %d, measured %.0f S/s, using %.1f",
           req, h.div, h.cyc, m, sHwRateNominal);

  // Whether this setting delivers fresh or doubled conversions can change from
  // one start to the next. One of each pair is evenly spaced either way, so if
  // full rate was saved but this start repeats words, drop to half rate. (A
  // dead-flat input, e.g. a grounded probe at boot, also looks repeated; that
  // only costs half the rate until the next restart.)
  sForceHalf = false;
  LevelStats st;
  if (!S.dupPairs && captureStats(buf, st) && st.dup - chanceEqual(st.sd) >= 0.08f) {
    sForceHalf = true;
    ESP_LOGW(TAG, "ADC repeats %.0f%% of samples this start: using one of each pair (%.0f S/s)",
             st.dup * 100, sHwRateNominal / 2);
  }
}

// ---- tuner -----------------------------------------------------------------
// Needs GPIO26 jumpered to GPIO34. GPIO26 is also DAC channel 2, so we can put
// known DC levels on the input. A slow conversion rate with ESP-IDF's default
// timing is the reference; every faster setting must read the same levels.

static void appendf(char* out, size_t len, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void appendf(char* out, size_t len, const char* fmt, ...) {
  size_t used = strlen(out);
  if (used + 1 >= len) return;
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(out + used, len - used, fmt, ap);
  va_end(ap);
}

// i2sStart + (re)enable the DAC test signal on GPIO26: every i2sStop()
// switches it off (see i2sStop).
static bool tuneStart(const HwCfg& h) {
  if (!i2sStart(h)) return false;
  dac_output_enable(DAC_CHANNEL_2);
  rtc_gpio_pullup_dis((gpio_num_t)PIN_GEN_SQUARE);    // a pull-up would skew the DAC levels
  rtc_gpio_pulldown_dis((gpio_num_t)PIN_GEN_SQUARE);
  return true;
}

// Digital continuity test, independent of the ADC: drive GPIO26 as a logic
// output and read GPIO34 as a logic input. Must run with the I2S/ADC stopped;
// i2sStart() hands GPIO34 back to the ADC.
static bool jumperCheck(char* out, size_t len) {
  gpio_reset_pin((gpio_num_t)PIN_GEN_SQUARE);   // also takes it out of DAC/RTC mode
  gpio_set_direction((gpio_num_t)PIN_GEN_SQUARE, GPIO_MODE_OUTPUT);
  gpio_config_t in = {};
  in.pin_bit_mask = 1ULL << PIN_PROBE;
  in.mode = GPIO_MODE_INPUT;                    // GPIO34 has no pull resistors
  gpio_config(&in);
  const int tries = 10;
  int match = 0, highs = 0;
  for (int i = 0; i < tries; i++) {
    int lvl = i & 1;
    gpio_set_level((gpio_num_t)PIN_GEN_SQUARE, lvl);
    vTaskDelay(pdMS_TO_TICKS(3));
    int r = gpio_get_level((gpio_num_t)PIN_PROBE);
    if (r == lvl) match++;
    highs += r;
  }
  gpio_set_level((gpio_num_t)PIN_GEN_SQUARE, 0);
  gpio_reset_pin((gpio_num_t)PIN_GEN_SQUARE);
  bool ok = match == tries;
  appendf(out, len, "jumper check: GPIO34 followed GPIO26 %d/%d times -> %s\n", match, tries,
          ok ? "connected" : "NOT CONNECTED");
  if (!ok)
    appendf(out, len, "  GPIO34 read %s. The wire is not joining GPIO26 and GPIO34: check the\n"
                      "  pin labels (34 and 26, not 35/36/39 or 25), breadboard rows and contact.\n",
            highs == 0 ? "always low" : highs == tries ? "always high" : "random levels");
  return ok;
}

static bool runTune(uint16_t* buf, char* out, size_t len) {
  static const uint8_t LEVELS[3] = {40, 128, 215};          // ~0.5, 1.65, 2.8 V
  // Some chips deliver each conversion twice, so 2 MS/s requested can mean
  // 1 MS/s of real samples. What counts is the unique rate.
  static const uint32_t RATES[] = {2000000, 1000000, 500000, 250000, 125000};
  static const uint8_t PROFILES[][2] = {{0, 0}, {2, 2}, {2, 6}, {4, 2}, {4, 6}, {8, 2}, {8, 6}};
  out[0] = 0;

  i2sStop();
  bool connected = jumperCheck(out, len);
  if (!connected || sTuneJumperOnly) return connected;

  // reference
  LevelStats ref[3];
  tuneStart({40000, 0, 0});
  bool refOk = true;
  for (int l = 0; l < 3; l++) refOk &= measureLevel(buf, LEVELS[l], ref[l]);
  i2sStop();
  appendf(out, len, "reference @40kS/s: %.0f %.0f %.0f (codes for DAC 40/128/215)\n",
          ref[0].mean, ref[1].mean, ref[2].mean);
  if (!refOk || ref[2].mean - ref[0].mean < 1500 || ref[1].mean <= ref[0].mean || ref[2].mean <= ref[1].mean) {
    appendf(out, len, "FAIL: reference readings don't follow the DAC levels on GPIO26.\n"
                      "Check the GPIO26->GPIO34 jumper. If it is fine, run `adcdiag` and share the output.\n");
    dac_output_disable(DAC_CHANNEL_2);
    return false;
  }

  HwCfg best = {0, 0, 0};
  bool bestDup = false;
  uint32_t bestReal = 0;
  float bestScore = 1e9f;
  appendf(out, len, "request  div cyc  err  noise  dup   real    result\n");
  for (uint32_t rate : RATES) {
    for (auto& p : PROFILES) {
      HwCfg h = {rate, p[0], p[1]};
      if (!tuneStart(h)) {
        appendf(out, len, "%-8lu %3d %3d   -      -     -   start failed\n", (unsigned long)rate, h.div, h.cyc);
        continue;
      }
      float err = 0, noise = 0, chan = 1, dup = 1;
      bool ok = true;
      for (int l = 0; l < 3 && ok; l++) {
        LevelStats st;
        ok = measureLevel(buf, LEVELS[l], st);
        err = fmaxf(err, fabsf(st.mean - ref[l].mean));
        noise = fmaxf(noise, st.sd);
        chan = fminf(chan, st.chanOk);
        dup = fminf(dup, st.dup);
      }
      i2sStop();
      // Well above chance, words repeat because the ADC hasn't finished a new
      // conversion. Exactly doubled is fine (use one of each pair, half rate);
      // partly repeated means irregular sample timing, which we reject.
      float chance = chanceEqual(noise);
      bool doubled = dup > 0.9f;
      bool unique = dup - chance < 0.08f;
      uint32_t real = doubled ? rate / 2 : unique ? rate : 0;
      bool accurate = ok && err <= 30 && noise <= 30 && chan > 0.99f;
      bool good = accurate && real > 0;
      appendf(out, len, "%-8lu %3d %3d %5.0f %5.1f %3.0f%%  %4luk   %s\n", (unsigned long)rate, h.div, h.cyc,
              err, noise, dup * 100, (unsigned long)(real / 1000),
              !ok ? "no data" : chan <= 0.99f ? "wrong channel" : !accurate ? "bad" : real == 0 ? "irregular" : "ok");
      // highest real sample rate wins, then accuracy + noise
      if (good && (real > bestReal || (real == bestReal && err + noise < bestScore))) {
        best = h;
        bestReal = real;
        bestScore = err + noise;
        bestDup = doubled;
      }
    }
    if (best.rate) break;   // fastest rate that works; don't bother with slower ones
  }
  dac_output_disable(DAC_CHANNEL_2);

  if (!best.rate) {
    appendf(out, len, "FAIL: no setting matched the reference.\n");
    // was the test signal still there? re-measure the reference
    LevelStats again[3];
    tuneStart({40000, 0, 0});
    bool againOk = true;
    for (int l = 0; l < 3; l++) againOk &= measureLevel(buf, LEVELS[l], again[l]);
    i2sStop();
    dac_output_disable(DAC_CHANNEL_2);
    appendf(out, len, "reference again: %.0f %.0f %.0f%s\n", again[0].mean, again[1].mean, again[2].mean,
            againOk && fabsf(again[1].mean - ref[1].mean) < 100 ? "" : "  (test signal lost during tuning)");
    return false;
  }
  S.hwRate = best.rate;
  S.sarClkDiv = best.div;
  S.sarCycles = best.cyc;
  S.dupPairs = bestDup;
  if (bestDup)
    appendf(out, len, "every conversion arrives twice: using one of each pair (%lu S/s real)\n",
            (unsigned long)bestReal);
  appendf(out, len, "chosen: %lu S/s, SAR div %d, cycles %d\n", (unsigned long)best.rate, best.div, best.cyc);
  return true;
}

// ============================================================ engine state

struct Derived {
  uint32_t ver = 0;
  uint32_t D = 1;          // decimation factor
  float    invD16 = 16;    // 16 / D
  uint16_t N = REC_POINTS;
  uint16_t pre = REC_POINTS / 2, post = REC_POINTS / 2;
  bool     roll = false;
  uint8_t  mode = TRIG_AUTO, edge = EDGE_RISE;
  uint8_t  tdiv = 0;
  uint16_t lvl = 0, lvlLo = 0, lvlHi = 0;
  uint32_t autoTimeoutUs = 100000;
  float    dt = 1e-6f;
};

static Derived d;
static uint16_t ring[RING_POINTS];
static uint32_t wr = 0;            // total points written
static uint32_t armStart = 0;
static uint32_t trigPos = 0;
static float    trigFrac = 0;
static bool     trigReal = false;
static bool     armed = false;
static uint16_t prevPt = 0;
static uint32_t rollCount = 0;
static int64_t  waitStartUs = 0, lastPublishUs = 0;
static uint32_t pubCount = 0;
static int64_t  pubWindowStart = 0;

// decimator
static uint32_t acc = 0, dcnt = 0;

// counter (gate-relative sample index)
static uint32_t cN = 0, cRises = 0, cHighCount = 0, cHighAtFirst = 0, cHighAtLast = 0;
static float    cFirstT = 0, cLastT = 0;
static bool     cHi = false;
static int      cThHi = 2100, cThLo = 2000;
static uint16_t cPrev = 0, cMin = 4095, cMax = 0;
static uint64_t cSum = 0;
static uint64_t cSumAtFirst = 0, cSumAtLast = 0;   // running sums at the first/last rising edge
static uint32_t cIdxFirst = 0, cIdxLast = 0;
static float    cMeanMv = 0;       // last gate mean, for AC-coupled trigger

static void computeTriggerLevels() {
  float lvlV = S.trigLevel;
  if (S.coupling == COUPLE_AC) lvlV += pinMvToVolts(cMeanMv);
  float hystV = 0.2f * VDIV_TABLE[S.vdiv];
  int lvl = voltsToPt(lvlV);
  int lo = voltsToPt(lvlV - hystV), hi = voltsToPt(lvlV + hystV);
  int minH = (d.D >= 8 ? 3 : 8) * 16;   // ADC noise floor, more averaging = less noise
  if (lvl - lo < minH) lo = lvl - minH;
  if (hi - lvl < minH) hi = lvl + minH;
  d.lvl = lvl;
  d.lvlLo = lo < 0 ? 0 : lo;
  d.lvlHi = hi > 65535 ? 65535 : hi;
}

static void resetCapture() {
  acc = dcnt = 0;
  armStart = wr;
  rollCount = 0;
  armed = false;
  sState = d.roll ? ACQ_ROLL : ACQ_ARMING;
  waitStartUs = esp_timer_get_time();
}

static void recompute() {
  Derived old = d;
  d.ver = gSettingsVersion;
  float window = 10.0f * TDIV_TABLE[S.tdiv];
  float total = window * acqSampleRate();
  if (total >= REC_POINTS) {
    d.N = REC_POINTS;
    d.D = (uint32_t)(total / REC_POINTS + 0.5f);
  } else {
    d.N = (uint16_t)(total + 0.5f);
    d.D = 1;
  }
  if (d.N < 4) d.N = 4;
  d.invD16 = 16.0f / d.D;
  d.tdiv = S.tdiv;
  d.mode = S.trigMode;
  d.edge = S.trigEdge;
  d.roll = TDIV_TABLE[S.tdiv] >= 0.1f && S.trigMode == TRIG_AUTO;
  d.pre = (uint16_t)(S.hpos * d.N + 0.5f);
  if (d.pre < 1) d.pre = 1;
  if (d.pre > d.N - 1) d.pre = d.N - 1;
  d.post = d.N - d.pre;
  d.dt = d.D / acqSampleRate();
  float to = 2.0f * window;
  d.autoTimeoutUs = (uint32_t)((to < 0.1f ? 0.1f : to) * 1e6f);
  computeTriggerLevels();
  if (old.D != d.D || old.N != d.N || old.pre != d.pre || old.roll != d.roll ||
      old.mode != d.mode || old.edge != d.edge) {
    resetCapture();
  }
}

// ============================================================ publishing

static void publish(uint32_t start, uint16_t valid, float t0, uint8_t flags) {
  xSemaphoreTake(sFrameMutex, portMAX_DELAY);
  sPub.seq++;
  sPub.n = d.N;
  sPub.valid = valid;
  sPub.dt = d.dt;
  sPub.t0 = t0;
  sPub.flags = flags;
  sPub.tdiv = d.tdiv;
  uint16_t off = d.N - valid;   // roll mode fills from the right
  for (uint16_t k = 0; k < off; k++) sPub.pts[k] = 0;
  for (uint16_t k = 0; k < valid; k++) sPub.pts[off + k] = ring[(start + k) & RING_MASK];
  xSemaphoreGive(sFrameMutex);
  lastPublishUs = esp_timer_get_time();
  pubCount++;
}

static void finishTriggered() {
  uint32_t start = trigPos - d.pre;
  // crossing happened at (trigPos - 1 + frac); express point 0 relative to it
  float t0 = trigReal ? (float)start - ((float)trigPos - 1.0f + trigFrac) : -(float)d.pre;
  sLastTriggered = trigReal;
  // Fast signals can trigger 100k times a second; nobody can look at more
  // than ~200 frames/s, and copying them all would eat the CPU.
  const int64_t now = esp_timer_get_time();
  if (d.mode == TRIG_SINGLE || now - lastPublishUs >= 5000)
    publish(start, d.N, t0, trigReal ? FRAME_TRIGGERED : 0);
  if (d.mode == TRIG_SINGLE) {
    gRunning = false;
    sState = ACQ_STOPPED;
  } else {
    sState = ACQ_WAIT;
    armed = false;
    waitStartUs = now;
  }
}

static inline void pushPoint(uint16_t pt) {
  ring[wr & RING_MASK] = pt;
  wr++;
  switch (sState) {
    case ACQ_ARMING:
      if (wr - armStart >= d.pre) { sState = ACQ_WAIT; armed = false; }
      break;
    case ACQ_WAIT: {
      bool hit = false;
      if (d.edge == EDGE_RISE) {
        if (pt < d.lvlLo) armed = true;
        else if (armed && pt >= d.lvl && prevPt < d.lvl) hit = true;
      } else {
        if (pt > d.lvlHi) armed = true;
        else if (armed && pt <= d.lvl && prevPt > d.lvl) hit = true;
      }
      if (hit) {
        trigPos = wr - 1;
        int den = (int)pt - (int)prevPt;
        trigFrac = den != 0 ? ((float)d.lvl - prevPt) / den : 0;
        if (trigFrac < 0) trigFrac = 0;
        if (trigFrac > 1) trigFrac = 1;
        trigReal = true;
        sState = ACQ_POST;
      }
      break;
    }
    case ACQ_POST:
      if (wr - trigPos >= d.post) finishTriggered();
      break;
    case ACQ_ROLL:
      if (rollCount < d.N) rollCount++;
      break;
    default:
      break;
  }
  prevPt = pt;
}

static void finishGate() {
  float fs = acqSampleRate();
  CounterResult r;
  r.gateSec = cN / fs;
  r.minMv = codeToMv(cMin);
  r.maxMv = codeToMv(cMax);
  // mean over whole periods when there are any: a partial period biases it
  if (cRises >= 2 && cIdxLast > cIdxFirst)
    r.meanMv = codeToMv((float)((double)(cSumAtLast - cSumAtFirst) / (cIdxLast - cIdxFirst)));
  else
    r.meanMv = codeToMv((float)cSum / (cN ? cN : 1));
  int span = (int)cMax - (int)cMin;
  if (cRises >= 2 && span >= 24 && cLastT > cFirstT) {
    float periods = cLastT - cFirstT;
    r.freq = (cRises - 1) * fs / periods;
    r.duty = (cHighAtLast - cHighAtFirst) / periods;
  } else {
    r.freq = 0;
    r.duty = cHi ? 1.0f : 0.0f;
  }
  // thresholds for the next gate: signal midpoint, hysteresis = 1/8 of swing
  int mid = ((int)cMin + (int)cMax) / 2;
  int h = span / 16;
  if (h < 4) h = 4;
  cThHi = mid + h;
  cThLo = mid - h;
  cMeanMv = r.meanMv;

  portENTER_CRITICAL(&sMux);
  r.seq = sCounterOut.seq + 1;
  sCounterOut = r;
  portEXIT_CRITICAL(&sMux);

  cN = cRises = cHighCount = cHighAtFirst = cHighAtLast = 0;
  cFirstT = cLastT = 0;
  cMin = 4095; cMax = 0; cSum = 0;
}

// ============================================================ task

static int64_t rateT0 = 0;
static uint64_t rateSamples = 0;
static int64_t busyUs = 0;

// Everything that happens to one DMA buffer of n raw samples.
static void processChunk(const uint16_t* buf, int n, int64_t now) {
  if (d.ver != gSettingsVersion) recompute();
  if (sRearmReq) { sRearmReq = false; resetCapture(); }
  if (!gRunning) sState = ACQ_STOPPED;
  else if (sState == ACQ_STOPPED) resetCapture();

  // raw capture for selftest/adcdiag: 16-bit words as delivered (channel
  // nibble included), no swap/invert
  if (sRawDst && sRawGot < sRawWant) {
    uint32_t take = sRawWant - sRawGot;
    if (take > (uint32_t)n) take = n;
    memcpy(sRawDst + sRawGot, buf, take * sizeof(uint16_t));
    sRawGot += take;
  }

  // duplicated pairs: take one word of each pair; otherwise undo the pair swap
  const int step = halfRate() ? 2 : 1;
  const int sw = (S.swapPairs && step == 1) ? 1 : 0;
  const uint16_t inv = S.invert ? 0x0FFF : 0;
  const bool capturing = sState != ACQ_STOPPED;
  const uint32_t D = d.D;
  const float invD16 = d.invD16;
  uint32_t chunkSum = 0;

  for (int i = 0; i < n; i += step) {
    const uint16_t code = (buf[i ^ sw] & 0x0FFF) ^ inv;

    // --- frequency counter ---
    if (!cHi) {
      if (code > cThHi) {
        cHi = true;
        int den = (int)code - (int)cPrev;
        float f = den > 0 ? (float)(cThHi - (int)cPrev) / den : 1.0f;
        if (f < 0) f = 0;
        float t = (float)cN - 1.0f + f;
        const uint64_t sumNow = cSum + chunkSum;
        if (cRises == 0) { cFirstT = t; cHighAtFirst = cHighCount; cSumAtFirst = sumNow; cIdxFirst = cN; }
        cLastT = t;
        cHighAtLast = cHighCount;
        cSumAtLast = sumNow;
        cIdxLast = cN;
        cRises++;
      }
    } else if (code < cThLo) {
      cHi = false;
    }
    if (cHi) cHighCount++;
    if (code < cMin) cMin = code;
    if (code > cMax) cMax = code;
    chunkSum += code;
    cPrev = code;
    cN++;

    // --- decimation + trigger ---
    if (capturing) {
      acc += code;
      if (++dcnt >= D) {
        uint16_t pt = D == 1 ? (uint16_t)(acc << 4) : (uint16_t)(acc * invD16 + 0.5f);
        acc = 0;
        dcnt = 0;
        pushPoint(pt);
      }
    }
  }
  cSum += chunkSum;

  // counter gate: >= 0.25 s with a few periods, or 2 s max. A gate whose
  // signal sits entirely above, below or inside the hysteresis band is using
  // thresholds from an old signal; end it at 0.5 s so they re-centre quickly.
  const float fs = acqSampleRate();
  const bool stale = cRises == 0 &&
      (cMin > cThHi || cMax < cThLo || (cMin > cThLo && cMax < cThHi));
  if ((cN >= 0.25f * fs && cRises >= 3) || (cN >= 0.5f * fs && stale) || cN >= 2.0f * fs) {
    finishGate();
    if (S.coupling == COUPLE_AC) computeTriggerLevels();
  }

  // auto trigger: force a frame if nothing triggered in time
  if (sState == ACQ_WAIT && d.mode == TRIG_AUTO && now - waitStartUs > d.autoTimeoutUs) {
    trigPos = wr - 1;
    trigReal = false;
    sState = ACQ_POST;
  }
  if (sState == ACQ_ROLL && rollCount > 0 && now - lastPublishUs > 33000) {
    sLastTriggered = false;
    publish(wr - rollCount, rollCount, -S.hpos * d.N, FRAME_ROLL);
  }

  // long-term rate measurement and frame-rate statistics
  rateSamples += n;   // DMA words; acqMeasuredRate() converts to unique samples
  if (now - rateT0 > 2000000) {
    sMeasuredRate = rateSamples * 1e6f / (float)(now - rateT0);
    sCpuLoad = (float)busyUs / (float)(now - rateT0);
    busyUs = 0;
    rateT0 = now;
    rateSamples = 0;
    sFrameRate = pubCount * 1e6f / (float)(now - pubWindowStart);
    pubCount = 0;
    pubWindowStart = now;
  }
}

static void acqTask(void*) {
  uint16_t* buf = (uint16_t*)malloc(READ_BYTES);
  startHw(savedCfg(), buf);
  recompute();
  resetCapture();
  rateT0 = pubWindowStart = esp_timer_get_time();

  for (;;) {
    if (sTuneReq || sReinitReq) {
      if (sTuneReq) sTuneOk = runTune(buf, sTuneOut, sTuneOutLen);   // stops the driver itself
      else i2sStop();
      startHw(savedCfg(), buf);
      if (sTuneReq) sTuneDone = true;
      sTuneReq = sReinitReq = false;
      d.ver = 0;   // recompute decimation for the new rate
      rateT0 = pubWindowStart = esp_timer_get_time();
      rateSamples = 0;
      continue;
    }
    size_t got = 0;
    i2s_read(I2S_PORT, buf, READ_BYTES, &got, portMAX_DELAY);
    const int n = (int)(got / 2) & ~1;
    if (n <= 0) continue;
    const int64_t t0 = esp_timer_get_time();
    processChunk(buf, n, t0);
    busyUs += esp_timer_get_time() - t0;
  }
}

#ifdef HOST_TEST
// Lets test/host_sim.cpp drive the engine with synthetic ADC data.
void acqTestInit(float hwRate) {
  buildLut();
  sFrameMutex = xSemaphoreCreateMutex();
  sHwRateNominal = hwRate;
  recompute();
  resetCapture();
}
void acqTestFeed(const uint16_t* buf, int n, int64_t nowUs) { processChunk(buf, n, nowUs); }
#endif

// ============================================================ API

void acqBegin() {
  buildLut();
  sFrameMutex = xSemaphoreCreateMutex();
  memset(&sPub, 0, sizeof(sPub));
  memset(&sCounterOut, 0, sizeof(sCounterOut));
  // core 1, above loop(): WiFi lives on core 0
  xTaskCreatePinnedToCore(acqTask, "acq", 6144, nullptr, configMAX_PRIORITIES - 2, nullptr, 1);
}

bool acqGetFrame(Frame& out, uint32_t lastSeq) {
  xSemaphoreTake(sFrameMutex, portMAX_DELAY);
  bool fresh = sPub.seq != lastSeq && sPub.n > 0;
  if (fresh) memcpy(&out, &sPub, sizeof(Frame));
  xSemaphoreGive(sFrameMutex);
  return fresh;
}

CounterResult acqGetCounter() {
  portENTER_CRITICAL(&sMux);
  CounterResult r = sCounterOut;
  portEXIT_CRITICAL(&sMux);
  return r;
}

uint8_t acqState() { return sState; }
bool acqLastTriggered() { return sLastTriggered; }
void acqRearm() { sRearmReq = true; }
// unique conversions per second (half the DMA word rate when pairs are duplicated)
float acqSampleRate() { return sHwRateNominal * S.rateCorr / (halfRate() ? 2 : 1); }
float acqMeasuredRate() { return sMeasuredRate / (halfRate() ? 2 : 1); }
bool  acqHalfRate() { return halfRate(); }
float acqRequestedRate() { return sRequestedRate; }
const char* acqCalSource() { return sCalSource; }
float acqFrameRate() { return sFrameRate; }
float acqCpuLoad() { return sCpuLoad; }

void acqReinit() { sReinitReq = true; }

bool acqTune(char* out, size_t len, uint32_t timeoutMs, bool jumperOnly) {
  out[0] = 0;
  sTuneJumperOnly = jumperOnly;
  sTuneOut = out;
  sTuneOutLen = len;
  sTuneDone = false;
  sTuneReq = true;
  uint32_t t0 = millis();
  while (!sTuneDone && millis() - t0 < timeoutMs) delay(20);
  return sTuneDone && sTuneOk;
}

uint32_t acqCaptureRaw(uint16_t* dst, uint32_t n, uint32_t timeoutMs) {
  sRawGot = 0;
  sRawWant = n;
  sRawDst = dst;
  uint32_t t0 = millis();
  while (sRawGot < n && millis() - t0 < timeoutMs) delay(2);
  sRawDst = nullptr;
  delay(5);   // let the acquisition task leave its copy loop
  return sRawGot;
}
