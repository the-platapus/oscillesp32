#include "commands.h"
#include "settings.h"
#include "acquisition.h"
#include "measure.h"
#include "siggen.h"
#include "web.h"
#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include <stdlib.h>
#include <algorithm>
#include "esp_system.h"

#define FW_VERSION "1.0"

static const char* HELP =
  "run | stop | single | autoset\n"
  "tdiv <idx|+|-|1ms>      vdiv <idx|+|-|0.5V>\n"
  "vpos <V>  hpos <0.05..0.95>  coupling dc|ac|toggle\n"
  "tmode auto|normal|single  tedge rise|fall|toggle  tlevel <V>\n"
  "view scope|meter|fft|info|next\n"
  "fe direct|bias|custom   (front-end preset)\n"
  "calzero                 (probe shorted to GND)\n"
  "calgain <V>             (probe on a known voltage, e.g. 5.00)\n"
  "cal <zero_mV> <gain>    (manual)\n"
  "swap 0|1  invert 0|1  ratecorr <factor>  deglitch on|off\n"
  "adccfg <rate> [sar_div] [cycles] [dup]   adcdiag [raw]   jumpercheck\n"
  "gen sq <Hz> [duty%] | gen sq off | gen sine <Hz> | gen sine off\n"
  "wifi ap | wifi off | wifi join \"ssid\" \"password\"\n"
  "selftest [3V3 volts]    (jumper GPIO26 -> GPIO34, or probe on GPIO26)\n"
  "status | save | factory | help";

// ------------------------------------------------------------- helpers

struct Lock {
  Lock()  { xSemaphoreTakeRecursive(gCmdMutex, portMAX_DELAY); }
  ~Lock() { xSemaphoreGiveRecursive(gCmdMutex); }
};

static std::string lower(std::string s) {
  for (auto& c : s) c = (char)tolower((unsigned char)c);
  return s;
}

static std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
static std::string fmt(const char* f, ...) {
  char b[200];
  va_list ap;
  va_start(ap, f);
  vsnprintf(b, sizeof(b), f, ap);
  va_end(ap);
  return b;
}

// splits on spaces; "double quoted" tokens may contain spaces and \" escapes
static int tokenize(const std::string& line, std::string* out, int maxTok) {
  int n = 0;
  size_t i = 0, len = line.size();
  while (i < len && n < maxTok) {
    while (i < len && isspace((unsigned char)line[i])) i++;
    if (i >= len) break;
    std::string t;
    if (line[i] == '"') {
      i++;
      while (i < len && line[i] != '"') {
        if (line[i] == '\\' && i + 1 < len) i++;
        t += line[i++];
      }
      i++;
    } else {
      while (i < len && !isspace((unsigned char)line[i])) t += line[i++];
    }
    out[n++] = t;
  }
  return n;
}

static bool parseFloat(const std::string& s, float& v) {
  if (s.empty()) return false;
  char* end;
  v = strtof(s.c_str(), &end);
  return end != s.c_str() && isfinite(v);
}

static int stepIndex(const std::string& arg, int cur, int count, const char* const* labels) {
  if (arg == "+") return cur + 1 < count ? cur + 1 : cur;
  if (arg == "-") return cur > 0 ? cur - 1 : cur;
  for (int i = 0; i < count; i++) if (strcasecmp(arg.c_str(), labels[i]) == 0) return i;
  if (!arg.empty() && isdigit((unsigned char)arg[0])) {
    int v = atoi(arg.c_str());
    if (v >= 0 && v < count) return v;
  }
  return -1;
}

static std::string jsonEscape(const char* s) {
  std::string o;
  for (; *s; s++) {
    if (*s == '"' || *s == '\\') { o += '\\'; o += *s; }
    else if ((uint8_t)*s < 0x20) o += ' ';
    else o += *s;
  }
  return o;
}

// waits until the always-on counter has finished `gates` fresh gates
static bool waitGates(int gates, uint32_t timeoutMs = 4000) {
  uint32_t start = acqGetCounter().seq;
  uint32_t t0 = millis();
  while (acqGetCounter().seq < start + gates) {
    if (millis() - t0 > timeoutMs) return false;
    delay(10);
  }
  return true;
}

const char* scopeStateText() {
  if (!gRunning) return "STOP";
  switch (acqState()) {
    case ACQ_ROLL: return "ROLL";
    case ACQ_STOPPED: return "STOP";
    default: break;
  }
  if (acqLastTriggered()) return "TRIG";
  return S.trigMode == TRIG_AUTO ? "AUTO" : "WAIT";
}

// ------------------------------------------------------------- autoset

static volatile bool sAutosetActive = false;
static uint32_t sAutosetSeq = 0, sAutosetT0 = 0;

bool autosetBusy() { return sAutosetActive; }

static void autosetApply() {
  CounterResult c = acqGetCounter();
  Lock lk;
  float vmin = pinMvToVolts(c.minMv), vmax = pinMvToVolts(c.maxMv);
  float vpp = vmax - vmin;
  bool ac = S.coupling == COUPLE_AC;
  float center = ac ? 0 : 0.5f * (vmax + vmin);

  if (c.freq > 0) {
    // signal fills ~6 of 8 divisions, centred
    int vi = VDIV_COUNT - 1;
    for (int i = 0; i < VDIV_COUNT; i++) if (VDIV_TABLE[i] * 6.0f >= vpp) { vi = i; break; }
    S.vdiv = vi;
    S.vpos = -center;
    // ~2.5 periods across the screen
    float want = 0.25f / c.freq;
    int ti = TDIV_COUNT - 1;
    for (int i = 0; i < TDIV_COUNT; i++) if (TDIV_TABLE[i] >= want) { ti = i; break; }
    S.tdiv = ti;
    S.trigLevel = ac ? 0 : center;
  } else {
    // DC or no signal: keep ground at mid-screen and show the level
    float peak = fmaxf(fabsf(vmax), fabsf(vmin));
    int vi = VDIV_COUNT - 1;
    for (int i = 0; i < VDIV_COUNT; i++) if (VDIV_TABLE[i] * 3.5f >= peak) { vi = i; break; }
    S.vdiv = vi;
    S.vpos = 0;
    S.tdiv = 7;   // 1 ms
    S.trigLevel = center;
  }
  S.trigMode = TRIG_AUTO;
  S.trigEdge = EDGE_RISE;
  S.hpos = 0.5f;
  if (S.view != VIEW_METER) S.view = VIEW_SCOPE;
  gRunning = true;
  settingsChanged();
}

void commandsPoll() {
  if (!sAutosetActive) return;
  // two fresh gates: the first may still use thresholds from the old signal
  if (acqGetCounter().seq >= sAutosetSeq + 2) {
    autosetApply();
    sAutosetActive = false;
  } else if (millis() - sAutosetT0 > 5000) {
    sAutosetActive = false;
  }
}

// ------------------------------------------------------------- selftest

// keeps a bipolar front-end's trace on screen: 0 V mid-screen
static void bipolarView() { S.vpos = 0; S.trigLevel = 0; S.vdiv = 8; }

// Front-end calibration through the probe: GPIO26 held low puts 0 V on the
// probe, held high puts the 3V3 rail there (front-end B draws ~15 uA, which
// moves a GPIO output by well under a millivolt). Two points: zero and gain.
static bool frontEndCal(std::string& r, float vdd, bool vddGiven) {
  float mv[2];
  for (int lvl = 0; lvl < 2; lvl++) {
    siggenHoldSquare(lvl);
    // the first fresh gate may straddle the switch
    if (!waitGates(2)) { r += "FAIL: counter timed out\n"; return false; }
    mv[lvl] = acqGetCounter().meanMv;
  }
  r += fmt("front-end: probe at 0 V -> %.1f mV at GPIO34, at %.2f V%s -> %.1f mV\n",
           mv[0], vdd, vddGiven ? "" : " (3V3 rail, assumed)", mv[1]);
  const float adcLo = codeToMv(0) + 20, adcHi = codeToMv(4095) - 20;
  if (mv[0] < adcLo || mv[1] > adcHi) {
    r += fmt("FAIL: GPIO34 reached the end of the ADC range (%.0f..%.0f mV).\n", adcLo, adcHi);
    return false;
  }
  float dv = (mv[1] - mv[0]) * 0.001f;
  if (dv < 0.02f) { r += "FAIL: GPIO34 barely rises with GPIO26.\n"; return false; }
  Lock lk;
  if (S.frontEnd == FE_DIRECT) bipolarView();
  S.calZero = mv[0];
  S.calGain = vdd / dv;
  S.frontEnd = FE_CUSTOM;
  settingsChanged();
  r += fmt("calibrated: zero %.1f mV, gain %.3f V/V, probe range about %+.1f..%+.1f V\n",
           S.calZero, S.calGain, pinMvToVolts(codeToMv(0)), pinMvToVolts(codeToMv(4095)));
  if (!vddGiven) r += "  (for an exact gain, measure the 3V3 pin and run `selftest <volts>`)\n";
  return true;
}

static std::string selftest(float vdd, bool vddGiven) {
  std::string r = "SELFTEST (GPIO26 wired to GPIO34, straight or through the probe front-end)\n";

  // 1. find the fastest ADC rate/timing that reads DC levels correctly
  r += "[1/3] tuning ADC timing (takes ~10 s)...\n";
  siggenReleaseSquare();
  delay(20);
  static char report[2000];
  TuneResult tuned = acqTune(report, sizeof(report), 30000);
  r += report;
  if (tuned == TUNE_FAIL) {
    siggenOverrideSquare(0, 0);
    return r + "Tuning failed; ADC settings unchanged.";
  }
  if (tuned == TUNE_OK) {
    {
      Lock lk;
      settingsChanged();
    }
    r += fmt("ADC now %.0f S/s\n", acqSampleRate());
  } else {
    // too small a swing to judge ADC timing by, but just what the front-end
    // calibration needs
    r += fmt("ADC timing is only tuned over a plain jumper; keeping the current %.0f S/s.\n",
             acqSampleRate());
    if (!frontEndCal(r, vdd, vddGiven)) {
      siggenOverrideSquare(0, 0);
      return r + "Front-end calibration failed; calibration unchanged.";
    }
  }

  // 2. sample order and polarity, from a 10 kHz 25% square
  r += "[2/3] sample order and polarity\n";
  const uint32_t N = 8192;
  float sqHz = siggenOverrideSquare(10000, 25);
  delay(50);
  uint16_t* raw = (uint16_t*)malloc(N * sizeof(uint16_t));
  if (!raw) { siggenOverrideSquare(0, 0); return r + "out of memory"; }
  uint32_t got = acqCaptureRaw(raw, N, 3000);
  if (got < N) {
    free(raw);
    siggenOverrideSquare(0, 0);
    return r + "FAIL: ADC stream stalled";
  }
  for (uint32_t i = 0; i < N; i++) raw[i] &= 0x0FFF;
  int mn = 4095, mx = 0;
  for (uint32_t i = 0; i < N; i++) { mn = std::min(mn, (int)raw[i]); mx = std::max(mx, (int)raw[i]); }
  r += fmt("raw span: %d..%d\n", mn, mx);

  // wrong pair order turns every other edge into a spike, so the ordering
  // with less total variation is the right one
  int swap = -1;
  if (acqHalfRate()) {
    r += "pair order: n/a (each conversion arrives twice; one of each pair is used)\n";
  } else {
    uint64_t tvA = 0, tvB = 0;
    for (uint32_t i = 0; i + 1 < N; i++) {
      tvA += abs((int)raw[i + 1] - (int)raw[i]);
      tvB += abs((int)raw[(i + 1) ^ 1] - (int)raw[i ^ 1]);
    }
    swap = tvB * 100 < tvA * 85 ? 1 : tvA * 100 < tvB * 85 ? 0 : -1;
    r += fmt("pair order: TV as-is %llu, swapped %llu -> %s\n",
             (unsigned long long)tvA, (unsigned long long)tvB,
             swap < 0 ? "inconclusive (kept)" : swap ? "SWAP" : "as-is");
  }

  // a 25% duty square must read as mostly low
  int mid = (mn + mx) / 2, high = 0;
  for (uint32_t i = 0; i < N; i++) if (raw[i] > mid) high++;
  float duty = (float)high / N;
  int inv = duty < 0.4f ? 0 : duty > 0.6f ? 1 : -1;
  r += fmt("raw duty %.1f%% (expect 25%%) -> %s\n", duty * 100,
           inv < 0 ? "inconclusive (kept)" : inv ? "INVERTED" : "normal");
  free(raw);
  {
    Lock lk;
    if (swap >= 0) S.swapPairs = swap;
    if (inv >= 0) S.invert = inv;
    settingsChanged();
  }

  // 3. timebase: the LEDC square comes from the crystal, so the counter's
  // reading of it gives the true ADC rate. Median of 5 gates: now and then a
  // gate gains or loses an edge (400 ppm at 10 kHz), more so behind a front-end.
  r += "[3/3] timebase\n";
  float gates[5];
  int ng = 0;
  CounterResult c = {};
  if (waitGates(2)) {   // the first fresh gate may straddle the switch to 10 kHz
    for (; ng < 5 && waitGates(1); ng++) {
      c = acqGetCounter();
      gates[ng] = c.freq;
    }
  }
  if (ng == 5) {
    std::sort(gates, gates + 5);
    const float f = gates[2];
    r += fmt("counter: %.3f Hz, median of %.3f..%.3f (generator %.3f Hz), duty %.2f%%\n",
             f, gates[0], gates[4], sqHz, c.duty * 100);
    if (f > 0 && sqHz > 0 && fabsf(f / sqHz - 1) < 0.03f) {
      Lock lk;
      S.rateCorr = S.rateCorr * sqHz / f;
      settingsChanged();
      r += fmt("rate correction %.6f -> ADC %.1f S/s\n", S.rateCorr, acqSampleRate());
    } else {
      r += "rate correction skipped (reading out of range)\n";
    }
  } else {
    r += "counter timed out\n";
  }
  siggenOverrideSquare(0, 0);
  r += "PASS. Settings saved.";
  Lock lk;
  settingsSaveNow();
  return r;
}

// Raw look at what the DMA delivers right now: which ADC channel the words
// come from, and the spread of the 12-bit codes. `all` appends every word.
static std::string adcdiag(bool all) {
  const uint32_t N = 4096;
  uint16_t* raw = (uint16_t*)malloc(N * sizeof(uint16_t));
  if (!raw) return "out of memory";
  uint32_t got = acqCaptureRaw(raw, N, 3000);
  std::string r = fmt("ADC %lu S/s requested, %.0f S/s real (expected %.0f), SAR div %d cycles %d,\n"
                      "dup %d swap %d invert %d, sampling task CPU load %.0f%%\n",
                      (unsigned long)S.hwRate, acqMeasuredRate(), acqSampleRate(), S.sarClkDiv, S.sarCycles,
                      acqHalfRate() ? 1 : 0, S.swapPairs, S.invert, acqCpuLoad() * 100);
  if (got < N) { free(raw); return r + "stream stalled"; }
  uint32_t chan[16] = {0}, bins[8] = {0};
  double sum = 0, sum2 = 0;
  int mn = 4095, mx = 0;
  for (uint32_t i = 0; i < N; i++) {
    int c = raw[i] & 0x0FFF;
    chan[raw[i] >> 12]++;
    bins[c >> 9]++;
    sum += c;
    sum2 += (double)c * c;
    mn = std::min(mn, c);
    mx = std::max(mx, c);
  }
  double mean = sum / N, var = sum2 / N - mean * mean;
  r += "channel nibble:";
  for (int i = 0; i < 16; i++) if (chan[i]) r += fmt(" ch%d=%lu", i, (unsigned long)chan[i]);
  r += fmt(" (expect ch%d)\n", (int)PROBE_ADC_CHANNEL);
  r += fmt("codes: min %d max %d mean %.1f sd %.1f\nhistogram (512-code bins):", mn, mx, mean, var > 0 ? sqrt(var) : 0);
  for (int i = 0; i < 8; i++) r += fmt(" %lu", (unsigned long)bins[i]);
  r += all ? "\nwords:" : "\nfirst words:";
  for (uint32_t i = 0; i < (all ? N : 16); i++) r += fmt("%s%04x", i % 16 ? " " : "\n", raw[i]);
  free(raw);
  return r;
}

// ------------------------------------------------------------- status

std::string statusJson() {
  CounterResult c = acqGetCounter();
  char buf[1100];
  snprintf(buf, sizeof(buf),
    "{\"type\":\"status\",\"ver\":\"" FW_VERSION "\",\"run\":%d,\"state\":\"%s\",\"autoset\":%d,"
    "\"tdiv\":%d,\"vdiv\":%d,\"vpos\":%.4f,\"hpos\":%.3f,\"cpl\":%d,"
    "\"tmode\":%d,\"tedge\":%d,\"tlevel\":%.4f,\"view\":%d,"
    "\"fe\":%d,\"calZero\":%.2f,\"calGain\":%.5f,\"swap\":%d,\"inv\":%d,\"rateCorr\":%.6f,"
    "\"hwRate\":%lu,\"sarDiv\":%d,\"sarCyc\":%d,\"dup\":%d,\"dgl\":%d,\"load\":%.2f,"
    "\"fs\":%.1f,\"fsMeas\":%.1f,\"fsReq\":%.0f,\"fps\":%.1f,\"calSrc\":\"%s\","
    "\"freq\":%.4f,\"duty\":%.4f,\"dcMean\":%.4f,\"dcMin\":%.4f,\"dcMax\":%.4f,"
    "\"gsq\":%d,\"gsqHz\":%.3f,\"gsqDuty\":%.2f,\"gsqAct\":%.3f,"
    "\"gsin\":%d,\"gsinHz\":%.1f,\"gsinAct\":%.1f,"
    "\"wifi\":%d,\"ssid\":\"%s\",\"ip\":\"%s\",\"heap\":%u}",
    gRunning ? 1 : 0, scopeStateText(), sAutosetActive ? 1 : 0,
    S.tdiv, S.vdiv, S.vpos, S.hpos, S.coupling,
    S.trigMode, S.trigEdge, S.trigLevel, S.view,
    S.frontEnd, S.calZero, S.calGain, S.swapPairs, S.invert, S.rateCorr,
    (unsigned long)S.hwRate, S.sarClkDiv, S.sarCycles, acqHalfRate() ? 1 : 0, S.deglitchOff ? 0 : 1, acqCpuLoad(),
    acqSampleRate(), acqMeasuredRate(), acqRequestedRate(), acqFrameRate(), acqCalSource(),
    c.freq, c.duty, pinMvToVolts(c.meanMv), pinMvToVolts(c.minMv), pinMvToVolts(c.maxMv),
    S.genSquareOn, S.genSquareHz, S.genSquareDuty, siggenSquareActualHz(),
    S.genSineOn, S.genSineHz, siggenSineActualHz(),
    S.wifiMode, jsonEscape(webSsid()).c_str(), webIp(),
    (unsigned)esp_get_free_heap_size());
  return buf;
}

// ------------------------------------------------------------- dispatcher

std::string runCommand(const std::string& lineIn) {
  std::string tok[7];
  int n = tokenize(lineIn, tok, 7);
  if (n == 0) return "";
  const std::string cmd = lower(tok[0]);
  const std::string& a = tok[1];
  const std::string al = lower(a);
  float f;

  // --- commands that wait: no lock held while waiting ---
  if (cmd == "selftest") {
    if (a.empty()) return selftest(3.3f, false);
    if (!parseFloat(a, f) || f < 2.8f || f > 3.6f) return "err: selftest [3V3 pin volts, 2.8..3.6]";
    return selftest(f, true);
  }
  if (cmd == "adcdiag") return adcdiag(al == "raw");
  if (cmd == "jumpercheck") {
    char out[600];
    siggenReleaseSquare();
    acqTune(out, sizeof(out), 5000, true);
    siggenOverrideSquare(0, 0);
    return out;
  }
  if (cmd == "calzero") {
    if (!waitGates(2)) return "err: counter timeout";
    CounterResult c = acqGetCounter();
    Lock lk;
    S.calZero = c.meanMv;
    if (S.frontEnd == FE_DIRECT) S.frontEnd = FE_CUSTOM;
    settingsChanged();
    return fmt("ok zero = %.1f mV at pin", S.calZero);
  }
  if (cmd == "calgain") {
    if (!parseFloat(a, f) || fabsf(f) < 0.05f) return "err: calgain <known volts>, e.g. calgain 5.0";
    if (!waitGates(2)) return "err: counter timeout";
    CounterResult c = acqGetCounter();
    float dv = (c.meanMv - S.calZero) * 0.001f;
    if (fabsf(dv) < 0.02f) return "err: input barely differs from zero; check wiring";
    float g = f / dv;
    if (g <= 0) return "err: polarity mismatch (negative gain); is calzero done?";
    Lock lk;
    S.calGain = g;
    S.frontEnd = FE_CUSTOM;
    settingsChanged();
    return fmt("ok gain = %.5f V/V", g);
  }

  Lock lk;
  bool changed = true;
  std::string reply = "ok";

  if (cmd == "help" || cmd == "?") { changed = false; reply = HELP; }
  else if (cmd == "status") { changed = false; reply = statusJson(); }
  else if (cmd == "run") {
    gRunning = true;
    if (S.trigMode == TRIG_SINGLE) acqRearm();
    changed = false;
  }
  else if (cmd == "stop") { gRunning = false; changed = false; }
  else if (cmd == "runstop") { gRunning = !gRunning; if (gRunning) acqRearm(); changed = false; }
  else if (cmd == "single") {
    S.trigMode = TRIG_SINGLE;
    gRunning = true;
    acqRearm();
  }
  else if (cmd == "autoset" || cmd == "auto") {
    sAutosetSeq = acqGetCounter().seq;
    sAutosetT0 = millis();
    sAutosetActive = true;
    changed = false;
    reply = "ok autoset running";
  }
  else if (cmd == "tdiv") {
    int i = stepIndex(al, S.tdiv, TDIV_COUNT, TDIV_LABEL);
    if (i < 0) return fmt("err: tdiv <0..%d|+|-|label>", TDIV_COUNT - 1);
    S.tdiv = i;
    reply = std::string("ok tdiv ") + TDIV_LABEL[i];
  }
  else if (cmd == "vdiv") {
    int i = stepIndex(al, S.vdiv, VDIV_COUNT, VDIV_LABEL);
    if (i < 0) return fmt("err: vdiv <0..%d|+|-|label>", VDIV_COUNT - 1);
    S.vdiv = i;
    reply = std::string("ok vdiv ") + VDIV_LABEL[i];
  }
  else if (cmd == "vpos") {
    if (!parseFloat(a, f)) return "err: vpos <volts>";
    S.vpos = clampv(f, -1000.0f, 1000.0f);
  }
  else if (cmd == "hpos") {
    if (!parseFloat(a, f)) return "err: hpos <0.05..0.95>";
    S.hpos = clampv(f, 0.05f, 0.95f);
  }
  else if (cmd == "coupling") {
    if (al == "dc") S.coupling = COUPLE_DC;
    else if (al == "ac") S.coupling = COUPLE_AC;
    else if (al == "toggle") S.coupling ^= 1;
    else return "err: coupling dc|ac|toggle";
  }
  else if (cmd == "tmode") {
    if (al == "auto") S.trigMode = TRIG_AUTO;
    else if (al == "normal") S.trigMode = TRIG_NORMAL;
    else if (al == "single") { S.trigMode = TRIG_SINGLE; acqRearm(); }
    else if (al == "next") S.trigMode = (S.trigMode + 1) % 3;
    else return "err: tmode auto|normal|single";
    if (S.trigMode != TRIG_SINGLE) gRunning = true;
  }
  else if (cmd == "tedge") {
    if (al == "rise") S.trigEdge = EDGE_RISE;
    else if (al == "fall") S.trigEdge = EDGE_FALL;
    else if (al == "toggle") S.trigEdge ^= 1;
    else return "err: tedge rise|fall|toggle";
  }
  else if (cmd == "tlevel") {
    if (!parseFloat(a, f)) return "err: tlevel <volts>";
    S.trigLevel = clampv(f, -1000.0f, 1000.0f);
  }
  else if (cmd == "view") {
    if (al == "scope") S.view = VIEW_SCOPE;
    else if (al == "meter") S.view = VIEW_METER;
    else if (al == "fft") S.view = VIEW_FFT;
    else if (al == "info") S.view = VIEW_INFO;
    else if (al == "next") S.view = (S.view + 1) % VIEW_COUNT;
    else return "err: view scope|meter|fft|info|next";
  }
  else if (cmd == "fe") {
    if (al == "direct") applyFrontEndPreset(FE_DIRECT);
    else if (al == "bias") applyFrontEndPreset(FE_BIAS);
    else if (al == "custom") applyFrontEndPreset(FE_CUSTOM);
    else return "err: fe direct|bias|custom";
    // keep the trace on screen after a range change
    if (S.frontEnd == FE_BIAS) bipolarView();
    if (S.frontEnd == FE_DIRECT) { S.vpos = -1.5f; S.trigLevel = 1.5f; S.vdiv = 5; }
  }
  else if (cmd == "cal") {
    float g;
    if (!parseFloat(a, f) || !parseFloat(tok[2], g) || g <= 0) return "err: cal <zero_mV> <gain>";
    S.calZero = f;
    S.calGain = g;
    S.frontEnd = FE_CUSTOM;
  }
  else if (cmd == "deglitch") {
    if (al == "on") S.deglitchOff = 0;
    else if (al == "off") S.deglitchOff = 1;
    else return "err: deglitch on|off";
  }
  else if (cmd == "swap") S.swapPairs = atoi(a.c_str()) ? 1 : 0;
  else if (cmd == "invert") S.invert = atoi(a.c_str()) ? 1 : 0;
  else if (cmd == "adccfg") {
    // adccfg <rate> [sar_div] [sample_cycles] [dup 0|1]   (0 = ESP-IDF default)
    int rate = atoi(a.c_str());
    if (rate < 20000 || rate > 2000000) return "err: adccfg <20000..2000000 S/s> [sar_div 0|2..255] [cycles 0..7]";
    int dv = tok[2].empty() ? S.sarClkDiv : atoi(tok[2].c_str());
    int cy = tok[3].empty() ? S.sarCycles : atoi(tok[3].c_str());
    if (dv == 1 || dv > 255 || cy < 0 || cy > 7) return "err: sar_div 0 or 2..255, cycles 0..7";
    S.hwRate = rate;
    S.sarClkDiv = dv;
    S.sarCycles = cy;
    if (!tok[4].empty()) S.dupPairs = atoi(tok[4].c_str()) ? 1 : 0;
    acqReinit();
    reply = fmt("ok ADC restarting at %d S/s, SAR div %d, cycles %d", rate, dv, cy);
  }
  else if (cmd == "ratecorr") {
    if (!parseFloat(a, f) || f < 0.9f || f > 1.1f) return "err: ratecorr <0.9..1.1>";
    S.rateCorr = f;
  }
  else if (cmd == "gen") {
    const std::string b = lower(tok[2]);
    if (al == "off") { S.genSquareOn = 0; S.genSineOn = 0; }
    else if (al == "sq" || al == "square") {
      if (b == "off") S.genSquareOn = 0;
      else if (b == "on") S.genSquareOn = 1;
      else {
        if (!parseFloat(b, f) || f < 1 || f > 10e6f) return "err: gen sq <1..10000000 Hz> [duty%]";
        S.genSquareHz = f;
        float dty;
        if (parseFloat(tok[3], dty)) S.genSquareDuty = clampv(dty, 0.0f, 100.0f);
        S.genSquareOn = 1;
      }
    } else if (al == "sine" || al == "sin") {
      if (b == "off") S.genSineOn = 0;
      else if (b == "on") S.genSineOn = 1;
      else {
        if (!parseFloat(b, f) || f < 130 || f > 55000) return "err: gen sine <130..55000 Hz>";
        S.genSineHz = f;
        S.genSineOn = 1;
      }
    } else return "err: gen sq <Hz> [duty] | gen sine <Hz> | gen off";
    siggenApply();
    reply = fmt("ok square %s %.3f Hz, sine %s %.0f Hz",
                S.genSquareOn ? "on" : "off", siggenSquareActualHz(),
                S.genSineOn ? "on" : "off", siggenSineActualHz());
  }
  else if (cmd == "wifi") {
    if (al == "off") S.wifiMode = 0;
    else if (al == "ap") S.wifiMode = 1;
    else if (al == "join") {
      if (tok[2].empty()) return "err: wifi join \"ssid\" \"password\"";
      strlcpy(S.staSsid, tok[2].c_str(), sizeof(S.staSsid));
      strlcpy(S.staPass, tok[3].c_str(), sizeof(S.staPass));
      S.wifiMode = 2;
    } else return "err: wifi ap|off|join \"ssid\" \"password\"";
    settingsSaveNow();
    webRequestReconfig();
    reply = "ok wifi reconfiguring";
  }
  else if (cmd == "save") { settingsSaveNow(); changed = false; }
  else if (cmd == "factory") {
    settingsDefaults();
    settingsSaveNow();
    siggenApply();
    webRequestReconfig();
  }
  else return "err: unknown command '" + cmd + "' (try help)";

  if (changed) settingsChanged();
  return reply;
}
