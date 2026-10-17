#include "siggen.h"
#include "settings.h"
#include "config.h"
#include "driver/ledc.h"
#include "driver/dac.h"
#include "driver/gpio.h"
#include "hal/misc.h"
#include "soc/sens_struct.h"

static const ledc_mode_t MODE = LEDC_HIGH_SPEED_MODE;
static const ledc_timer_t TIMER = LEDC_TIMER_0;
static const ledc_channel_t CH = LEDC_CHANNEL_0;

static float sSquareHz = 0, sSineHz = 0;
static bool sSquareRunning = false, sSineRunning = false;

static float squareStart(float hz, float dutyPct) {
  hz = clampv(hz, 1.0f, 10e6f);
  // as many duty bits as the 80 MHz clock allows (max 16); fall back to
  // fewer bits if the divider can't reach the frequency
  int bits = (int)floorf(log2f(80e6f / hz)) - 1;
  bits = clampv(bits, 1, 16);
  for (; bits >= 1; bits--) {
    ledc_timer_config_t t = {};
    t.speed_mode = MODE;
    t.duty_resolution = (ledc_timer_bit_t)bits;
    t.timer_num = TIMER;
    t.freq_hz = (uint32_t)lroundf(hz);
    t.clk_cfg = LEDC_AUTO_CLK;
    if (ledc_timer_config(&t) == ESP_OK) break;
  }
  if (bits < 1) return 0;

  uint32_t full = 1UL << bits;
  uint32_t duty = (uint32_t)(full * clampv(dutyPct, 0.0f, 100.0f) / 100.0f + 0.5f);
  if (duty > full) duty = full;
  if (!sSquareRunning) {
    // back to a plain digital pin (selftest may have left it routed to DAC2)
    gpio_reset_pin((gpio_num_t)PIN_GEN_SQUARE);
    ledc_channel_config_t c = {};
    c.gpio_num = PIN_GEN_SQUARE;
    c.speed_mode = MODE;
    c.channel = CH;
    c.timer_sel = TIMER;
    c.duty = duty;
    c.hpoint = 0;
    ledc_channel_config(&c);
    sSquareRunning = true;
  } else {
    ledc_set_duty(MODE, CH, duty);
    ledc_update_duty(MODE, CH);
  }
  return (float)ledc_get_freq(MODE, TIMER);
}

static void squareStop() {
  if (sSquareRunning) {
    ledc_stop(MODE, CH, 0);
    gpio_reset_pin((gpio_num_t)PIN_GEN_SQUARE);
    sSquareRunning = false;
  }
  sSquareHz = 0;
}

static void sineApply() {
  if (S.genSineOn) {
    float hz = clampv(S.genSineHz, 130.0f, 55000.0f);
    dac_cw_config_t cw = {};
    cw.en_ch = DAC_CHANNEL_1;
    cw.scale = DAC_CW_SCALE_2;     // ~1.6 Vpp around mid-rail
    cw.phase = DAC_CW_PHASE_0;
    cw.freq = (uint32_t)hz;
    cw.offset = 0;
    dac_cw_generator_config(&cw);
    // ESP-IDF 4.4's dac_ll_cw_set_dc_offset() writes -128 into the DC-offset
    // register when asked for 0. That shifts the wave down by half the DAC
    // range, so the negative half-cycles clamp at 0 V and only half a sine
    // comes out. Write the real zero.
    HAL_FORCE_MODIFY_U32_REG_FIELD(SENS.sar_dac_ctrl2, dac_dc1, 0);
    dac_cw_generator_enable();
    dac_output_enable(DAC_CHANNEL_1);
    sSineRunning = true;
    // the generator steps in units of RTC8M/65536 (~130 Hz)
    const float step = 8.5e6f / 65536.0f;
    sSineHz = roundf(hz / step) * step;
  } else if (sSineRunning) {
    dac_cw_generator_disable();
    dac_output_disable(DAC_CHANNEL_1);
    sSineRunning = false;
    sSineHz = 0;
  }
}

void siggenApply() {
  if (S.genSquareOn) sSquareHz = squareStart(S.genSquareHz, S.genSquareDuty);
  else squareStop();
  sineApply();
}

float siggenOverrideSquare(float hz, float dutyPct) {
  if (hz <= 0) { siggenApply(); return sSquareHz; }
  sSquareHz = squareStart(hz, dutyPct);
  return sSquareHz;
}

void siggenReleaseSquare() { squareStop(); }

void siggenRestoreDac() {
  if (sSineRunning) sineApply();
}

float siggenSquareActualHz() { return sSquareHz; }
float siggenSineActualHz() { return sSineHz; }
