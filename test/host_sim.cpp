// Host-side simulation of the acquisition engine.
// Feeds synthetic ADC streams through the real acquisition/measure/command
// code (with IDF stubs) and checks trigger, counter, roll, single, autoset.
//
//   ./test/run_host_sim.sh

#include <functional>
#include <random>
#include <vector>
#include <string>
#include "../src/acquisition.h"
#include "../src/measure.h"
#include "../src/settings.h"
#include "../src/commands.h"
#include "../src/web.h"
#include "../src/siggen.h"

int64_t g_simNowUs = 0;
void acqTestInit(float hwRate);
void acqTestFeed(const uint16_t* buf, int n, int64_t nowUs);

// ---- stubs for modules not under test ----
void webBegin() {}
void webRequestReconfig() {}
const char* webIp() { return "sim"; }
const char* webSsid() { return "sim"; }
bool webIsAp() { return true; }
int webClientCount() { return 0; }
void webPublish(const Processed&) {}
void siggenApply() {}
float siggenSquareActualHz() { return 0; }
float siggenSineActualHz() { return 0; }
float siggenOverrideSquare(float, float) { return 0; }
void siggenReleaseSquare() {}
void siggenHoldSquare(int) {}
void siggenRestoreDac() {}

// ---- simulation ----
static const double FS = 1e6;
static uint64_t sIdx = 0;
static std::mt19937 rng(1234);
static std::normal_distribution<double> noise(0.0, 2.0);   // ~2 LSB rms, like a quiet ESP32 ADC

static bool sDupSim = false;   // emulate chips that deliver every conversion twice
static double sGlitchRate = 0; // share of conversions hit by a SAR bit-decision error
static std::uniform_real_distribution<double> uni(0.0, 1.0);

// What the real ADC does now and then: one conversion decides a bit wrong.
// Just above 0x600 it lands on 0x5FF; elsewhere it drops by tens of codes.
// A third of them hit the next conversion too, like the bursts on hardware.
static uint16_t glitch(uint16_t c) {
  static bool burst = false;
  if (sGlitchRate <= 0) return c;
  if (!burst && uni(rng) >= sGlitchRate) return c;
  burst = !burst && uni(rng) < 0.33;
  if (c >= 0x600 && c < 0x640) return 0x5FF;
  return c > 60 ? c - 60 : 0;
}
static int fails = 0, checks = 0;
#define CHECK(cond, ...) do { checks++; if (!(cond)) { fails++; printf("  FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint16_t mvToCode(double mv) {
  double c = (mv - 75.0) / 0.77 + noise(rng);
  if (c < 0) c = 0;
  if (c > 4095) c = 4095;
  return (uint16_t)lround(c);
}

using Sig = std::function<double(double)>;   // pin millivolts at time t (s)

// Runs `sec` seconds of signal; calls onFrame for every newly published frame.
static void run(const Sig& sig, double sec, const std::function<void(const Frame&)>& onFrame = nullptr,
                bool pollCommands = false) {
  static Frame f;
  static uint32_t lastSeq = 0;
  uint16_t buf[1024];
  long chunks = (long)(sec * FS / 1024);
  for (long c = 0; c < chunks; c++) {
    if (sDupSim) {
      // one conversion per pair of DMA words, at half the word rate
      for (int i = 0; i < 1024; i += 2) buf[i] = buf[i + 1] = glitch(mvToCode(sig((sIdx + i) / FS)));
    } else {
      for (int i = 0; i < 1024; i++) buf[i] = glitch(mvToCode(sig((sIdx + i) / FS)));
      // the I2S DMA delivers each pair of 16-bit samples swapped
      for (int i = 0; i < 1024; i += 2) { uint16_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
    }
    sIdx += 1024;
    g_simNowUs = (int64_t)(sIdx / FS * 1e6);
    acqTestFeed(buf, 1024, g_simNowUs);
    if (acqGetFrame(f, lastSeq)) {
      lastSeq = f.seq;
      if (onFrame) onFrame(f);
    }
    if (pollCommands) commandsPoll();
  }
}

static Sig square(double hz, double lo, double hi, double duty = 0.5) {
  return [=](double t) { double ph = fmod(t * hz, 1.0); return ph < duty ? hi : lo; };
}
static Sig sine(double hz, double mid, double amp) {
  return [=](double t) { return mid + amp * sin(2 * M_PI * hz * t); };
}

// value of the processed trace at time `tSec` relative to the trigger
static float valueAt(const Processed& p, float tSec) {
  float k = tSec / p.dt - p.t0;
  int a = (int)floorf(k);
  if (a < 0 || a + 1 >= p.n) return NAN;
  float fr = k - a;
  return p.v[a] * (1 - fr) + p.v[a + 1] * fr;
}

static void cmd(const char* c) { runCommand(c); }

static Processed proc;

int main() {
  settingsInit();
  acqTestInit(FS);

  // ---------------------------------------------------------------- 1
  printf("[1] 1 kHz square 0.1..3.1 V, 1 ms/div, trigger 1.5 V rising\n");
  {
    int frames = 0, trig = 0;
    float worst = 0;
    run(square(1000, 100, 3100), 1.2, [&](const Frame& f) {
      frames++;
      if (f.flags & FRAME_TRIGGERED) trig++;
      processFrame(f, acqGetCounter(), proc);
      // edge must sit at t=0: below level just before, above just after
      float before = valueAt(proc, -20e-6f), after = valueAt(proc, 20e-6f);
      if (!(before < 1.5f && after > 1.5f)) worst = 1;
    });
    CounterResult c = acqGetCounter();
    printf("    frames %d (triggered %d), counter %.4f Hz duty %.2f%%  Vpp %.3f Vrms %.3f\n",
           frames, trig, c.freq, c.duty * 100, proc.m.vpp, proc.m.vrms);
    CHECK(frames > 20, "too few frames: %d", frames);
    CHECK(trig == frames, "untriggered frames: %d/%d", frames - trig, frames);
    CHECK(worst == 0, "edge not at trigger point");
    CHECK(fabsf(c.freq - 1000) < 0.1f, "freq %.4f", c.freq);
    CHECK(fabsf(c.duty - 0.5f) < 0.002f, "duty %.4f", c.duty);
    CHECK(fabsf(proc.m.vpp - 3.0f) < 0.05f, "vpp %.3f", proc.m.vpp);
    CHECK(fabsf(proc.m.vrms - sqrtf(0.5f * (0.1f * 0.1f + 3.1f * 3.1f))) < 0.05f, "vrms %.3f", proc.m.vrms);
  }

  // ---------------------------------------------------------------- 2
  printf("[2] 50 Hz sine 1.6 +- 1.4 V, 5 ms/div: trigger jitter\n");
  {
    cmd("tdiv 5ms");
    cmd("tlevel 1.6");
    std::vector<float> jit;
    run(sine(50, 1600, 1400), 1.5, [&](const Frame& f) {
      if (!(f.flags & FRAME_TRIGGERED)) return;
      processFrame(f, acqGetCounter(), proc);
      jit.push_back(valueAt(proc, 0) - 1.6f);
    });
    float worst = 0;
    for (float j : jit) worst = fmaxf(worst, fabsf(j));
    CounterResult c = acqGetCounter();
    printf("    %zu triggered frames, worst level error at t=0: %.1f mV, counter %.4f Hz, Vpp %.3f\n",
           jit.size(), worst * 1000, c.freq, proc.m.vpp);
    CHECK(jit.size() >= 10, "frames %zu", jit.size());
    CHECK(worst < 0.02f, "trigger point off by %.1f mV", worst * 1000);
    CHECK(fabsf(c.freq - 50) < 0.01f, "freq %.4f", c.freq);
    CHECK(fabsf(proc.m.vpp - 2.8f) < 0.06f, "vpp %.3f", proc.m.vpp);
    float dc = pinMvToVolts(c.meanMv);
    printf("    DC mean (whole periods) %.4f V\n", dc);
    CHECK(fabsf(dc - 1.6f) < 0.005f, "dc mean %.4f", dc);
  }

  // ---------------------------------------------------------------- 3
  printf("[3] 123.4 kHz sine, 5 us/div (50 points/screen)\n");
  {
    cmd("tdiv 5us");
    int frames = 0, trig = 0;
    float worst = 0;
    run(sine(123400, 1600, 1200), 0.6, [&](const Frame& f) {
      frames++;
      if (!(f.flags & FRAME_TRIGGERED)) return;
      trig++;
      processFrame(f, acqGetCounter(), proc);
      worst = fmaxf(worst, fabsf(valueAt(proc, 0) - 1.6f));
    });
    CounterResult c = acqGetCounter();
    printf("    frames %d (triggered %d), worst level error %.0f mV, counter %.2f Hz\n", frames, trig,
           worst * 1000, c.freq);
    CHECK(trig > 10, "triggered %d", trig);
    CHECK(worst < 0.15f, "trigger point off by %.0f mV (only 8 samples/period)", worst * 1000);
    CHECK(fabsf(c.freq - 123400) < 5, "freq %.2f", c.freq);
  }

  // ---------------------------------------------------------------- 4
  printf("[4] 10 kHz PWM 25%%, counter duty\n");
  {
    cmd("tdiv 50us");
    run(square(10000, 100, 3100, 0.25), 0.8);
    CounterResult c = acqGetCounter();
    printf("    counter %.3f Hz duty %.2f%%\n", c.freq, c.duty * 100);
    CHECK(fabsf(c.freq - 10000) < 1, "freq %.3f", c.freq);
    CHECK(fabsf(c.duty - 0.25f) < 0.003f, "duty %.4f", c.duty);
  }

  // ---------------------------------------------------------------- 5
  printf("[5] roll mode at 200 ms/div\n");
  {
    cmd("tdiv 200ms");
    int frames = 0, roll = 0;
    uint16_t lastValid = 0;
    run(sine(2, 1600, 1000), 0.6, [&](const Frame& f) {
      frames++;
      if (f.flags & FRAME_ROLL) roll++;
      lastValid = f.valid;
    });
    printf("    frames %d (roll %d), last valid points %u (expect ~300)\n", frames, roll, lastValid);
    CHECK(roll == frames && frames > 10, "roll frames %d/%d", roll, frames);
    CHECK(lastValid > 250 && lastValid < 320, "valid %u", lastValid);
  }

  // ---------------------------------------------------------------- 6
  printf("[6] DC 1.0 V, level 1.5 V: AUTO free-runs, NORMAL waits\n");
  {
    cmd("tdiv 1ms");
    cmd("tlevel 1.5");
    int frames = 0, trig = 0;
    run([](double) { return 1000.0; }, 1.0, [&](const Frame& f) { frames++; if (f.flags & FRAME_TRIGGERED) trig++; });
    printf("    AUTO: %d frames, %d triggered\n", frames, trig);
    CHECK(frames >= 5 && trig == 0, "auto frames %d trig %d", frames, trig);
    cmd("tmode normal");
    frames = 0;
    run([](double) { return 1000.0; }, 0.5, [&](const Frame&) { frames++; });
    printf("    NORMAL: %d frames\n", frames);
    CHECK(frames == 0, "normal produced %d frames", frames);
    cmd("tmode auto");
  }

  // ---------------------------------------------------------------- 7
  printf("[7] single shot\n");
  {
    cmd("single");
    int frames = 0;
    run(square(500, 100, 3100), 0.5, [&](const Frame& f) { frames++; (void)f; });
    printf("    frames %d, running %d, state %s\n", frames, (int)gRunning, scopeStateText());
    CHECK(frames == 1, "single produced %d frames", frames);
    CHECK(!gRunning, "still running");
    cmd("tmode auto");
  }

  // ---------------------------------------------------------------- 8
  printf("[8] autoset on a 2 kHz square 0.2..2.2 V starting from 1 s/div\n");
  {
    cmd("tdiv 1s");
    cmd("vdiv 20V");
    cmd("run");
    cmd("autoset");
    run(square(2000, 200, 2200), 1.2, nullptr, true);
    printf("    -> tdiv %s vdiv %s vpos %.3f tlevel %.3f\n", TDIV_LABEL[S.tdiv], VDIV_LABEL[S.vdiv], S.vpos, S.trigLevel);
    CHECK(S.tdiv == 5, "tdiv %s (want 200us)", TDIV_LABEL[S.tdiv]);
    CHECK(S.vdiv == 5, "vdiv %s (want 0.5V)", VDIV_LABEL[S.vdiv]);
    CHECK(fabsf(S.vpos + 1.2f) < 0.05f, "vpos %.3f", S.vpos);
    CHECK(fabsf(S.trigLevel - 1.2f) < 0.05f, "tlevel %.3f", S.trigLevel);
    int trig = 0, frames = 0;
    run(square(2000, 200, 2200), 0.3, [&](const Frame& f) { frames++; if (f.flags & FRAME_TRIGGERED) trig++; });
    CHECK(frames > 5 && trig == frames, "after autoset %d/%d triggered", trig, frames);
  }

  // ---------------------------------------------------------------- 9
  printf("[9] AC coupling: 1 kHz sine riding on 2 V\n");
  {
    cmd("coupling ac");
    cmd("tlevel 0");
    cmd("tdiv 500us");
    run(sine(1000, 2000, 300), 0.8);   // let the gate mean settle
    int trig = 0, frames = 0;
    run(sine(1000, 2000, 300), 0.3, [&](const Frame& f) {
      frames++;
      if (f.flags & FRAME_TRIGGERED) trig++;
      processFrame(f, acqGetCounter(), proc);
    });
    printf("    displayed mean %.4f V, Vpp %.3f, triggered %d/%d\n", proc.m.vavg, proc.m.vpp, trig, frames);
    CHECK(fabsf(proc.m.vavg) < 0.02f, "AC mean %.4f", proc.m.vavg);
    CHECK(trig == frames && frames > 5, "AC trigger %d/%d", trig, frames);
    cmd("coupling dc");
  }

  // ---------------------------------------------------------------- 10
  printf("[10] FFT of a 10 kHz sine, 100 us/div\n");
  {
    cmd("tdiv 100us");
    cmd("tlevel 1.6");
    run(sine(10000, 1600, 1400), 0.3, [&](const Frame& f) { processFrame(f, acqGetCounter(), proc); });
    static float db[512];
    int bins = fftDb(proc.v + (proc.n - proc.valid), proc.valid, db);
    int pk = 1;
    for (int i = 2; i < bins; i++) if (db[i] > db[pk]) pk = i;
    float pf = pk / (2.0f * bins * proc.dt);
    printf("    %d bins, peak %.0f Hz at %.2f dBV (expect ~10 kHz, ~-0.1 dBV)\n", bins, pf, db[pk]);
    CHECK(fabsf(pf - 10000) < 2000, "peak %.0f", pf);
    CHECK(fabsf(db[pk] - 20 * log10f(1.4f / sqrtf(2))) < 2.0f, "level %.2f", db[pk]);
  }

  // ---------------------------------------------------------------- 12
  printf("[12] slow signal after a fast one: 2 Hz sine, counter must still lock\n");
  {
    cmd("tdiv 100ms");
    run(sine(2, 1600, 1000), 6.0);
    CounterResult c = acqGetCounter();
    printf("    counter %.4f Hz, gate %.2f s\n", c.freq, c.gateSec);
    CHECK(fabsf(c.freq - 2.0f) < 0.01f, "freq %.4f", c.freq);
  }

  // ---------------------------------------------------------------- 13
  printf("[13] duplicated pairs (1 M words/s carrying 500 kS/s), 1 kHz square at 1 ms/div\n");
  {
    sDupSim = true;
    S.dupPairs = 1;
    cmd("tdiv 1ms");
    cmd("tlevel 1.5");
    int frames = 0, trig = 0;
    bool edgeOk = true;
    float dt = 0;
    run(square(1000, 100, 3100), 1.2, [&](const Frame& f) {
      frames++;
      if (f.flags & FRAME_TRIGGERED) trig++;
      dt = f.dt;
      processFrame(f, acqGetCounter(), proc);
      if (!(valueAt(proc, -20e-6f) < 1.5f && valueAt(proc, 20e-6f) > 1.5f)) edgeOk = false;
    });
    CounterResult c = acqGetCounter();
    printf("    rate %.0f S/s, dt %.2f us, frames %d (triggered %d), counter %.4f Hz duty %.2f%%\n",
           acqSampleRate(), dt * 1e6, frames, trig, c.freq, c.duty * 100);
    CHECK(fabsf(acqSampleRate() - 500000) < 1, "rate %.0f", acqSampleRate());
    CHECK(fabsf(dt - 10e-6f) < 1e-9f, "dt %.3g", dt);
    CHECK(frames > 20 && trig == frames, "triggered %d/%d", trig, frames);
    CHECK(edgeOk, "edge not at trigger point");
    CHECK(fabsf(c.freq - 1000) < 0.1f, "freq %.4f", c.freq);
    CHECK(fabsf(c.duty - 0.5f) < 0.003f, "duty %.4f", c.duty);
    sDupSim = false;
    S.dupPairs = 0;
    settingsChanged();
  }

  // ---------------------------------------------------------------- 14
  printf("[14] front-end B: 105 Hz sine, 0.29 Vpp at the pin, 2%% of conversions a bit off\n");
  {
    // as seen on hardware: 22k/22k/100k front-end, laptop audio out, 2.9 Vpp
    cmd("tdiv 5ms");
    cmd("cal 1500 10");
    cmd("tlevel 0");
    sGlitchRate = 0.02;
    cmd("deglitch off");
    run(sine(105, 1500, 144), 3.0);   // the counter re-learns its thresholds within a few gates
    CounterResult raw = acqGetCounter();
    cmd("deglitch on");
    run(sine(105, 1500, 144), 2.0);
    float worst = 0;
    run(sine(105, 1500, 144), 1.0, [&](const Frame& f) {
      processFrame(f, acqGetCounter(), proc);
      // the trace must stay on the sine: no spikes past its peaks
      worst = fmaxf(worst, fmaxf(proc.m.vmax - 1.44f, -1.44f - proc.m.vmin));
    });
    CounterResult c = acqGetCounter();
    printf("    counter without filter %.2f Hz, with %.4f Hz; peaks beyond the sine %.0f mV, gate min/max %.2f/%.2f V\n",
           raw.freq, c.freq, worst * 1000, pinMvToVolts(c.minMv), pinMvToVolts(c.maxMv));
    CHECK(fabsf(c.freq - 105) < 0.05f, "freq %.4f", c.freq);
    CHECK(worst < 0.15f, "trace peaks %.0f mV beyond the sine", worst * 1000);
    sGlitchRate = 0;
    cmd("fe direct");
  }

  // ---------------------------------------------------------------- 15
  printf("[15] counter range with the deglitch filter: 200 kHz square\n");
  {
    cmd("tdiv 5us");
    cmd("tlevel 1.6");
    for (double hz : {200000.0, 250000.0, 300000.0}) {
      cmd("deglitch on");
      run(square(hz, 100, 3100), 3.0);
      float on = acqGetCounter().freq;
      cmd("deglitch off");
      run(square(hz, 100, 3100), 3.0);
      float off = acqGetCounter().freq;
      printf("    %.0f kHz: filter on %.2f Hz, off %.2f Hz\n", hz / 1000, on, off);
      if (hz == 200000.0) CHECK(fabsf(on - hz) < 10, "200 kHz with filter: %.2f", on);
    }
    cmd("deglitch on");
  }

  // ---------------------------------------------------------------- 11
  printf("[11] SI formatting\n");
  {
    char b[32];
    struct { float v; const char* u; int d; const char* want; } cases[] = {
      {1000, "Hz", 3, "1.00kHz"}, {999.96f, "Hz", 3, "1.00kHz"}, {0.0123f, "V", 3, "12.3mV"},
      {0, "V", 3, "0.00V"}, {-1.5f, "V", 3, "-1.50V"}, {123400, "Hz", 4, "123.4kHz"}, {5e-6f, "s", 3, "5.00us"}};
    for (auto& c : cases) {
      fmtSI(b, c.v, c.u, c.d);
      CHECK(strcmp(b, c.want) == 0, "fmtSI(%g) = '%s', want '%s'", c.v, b, c.want);
    }
  }

  printf("\n%d checks, %d failed\n", checks, fails);
  return fails ? 1 : 0;
}
