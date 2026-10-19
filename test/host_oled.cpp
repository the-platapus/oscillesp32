// Renders the OLED screens on the PC (via the I2C stub) for visual checks.
//   ./test/run_host_oled.sh   -> writes oled_*.pbm next to the binary

#include <functional>
#include <random>
#include "../src/acquisition.h"
#include "../src/measure.h"
#include "../src/settings.h"
#include "../src/commands.h"
#include "../src/display.h"
#include "../src/web.h"
#include "../src/siggen.h"

int64_t g_simNowUs = 0;
uint8_t g_oledFrame[1024];
void acqTestInit(float hwRate);
void acqTestFeed(const uint16_t* buf, int n, int64_t nowUs);

void webBegin() {}
void webRequestReconfig() {}
const char* webIp() { return "192.168.4.1"; }
const char* webSsid() { return "ESP32-Scope"; }
bool webIsAp() { return true; }
int webClientCount() { return 1; }
void webPublish(const Processed&) {}
void siggenApply() {}
float siggenSquareActualHz() { return 0; }
float siggenSineActualHz() { return 0; }
float siggenOverrideSquare(float, float) { return 0; }
void siggenReleaseSquare() {}
void siggenHoldSquare(int) {}
void siggenRestoreDac() {}

static uint64_t sIdx = 0;
static std::mt19937 rng(7);
static std::normal_distribution<double> noise(0.0, 2.0);
static Processed proc;
static bool have = false;

static void run(const std::function<double(double)>& sig, double sec) {
  static Frame f;
  uint16_t buf[1024];
  for (long c = 0; c < (long)(sec * 1e6 / 1024); c++) {
    for (int i = 0; i < 1024; i++) {
      double code = (sig((sIdx + i) / 1e6) - 75.0) / 0.77 + noise(rng);
      buf[i] = (uint16_t)fmin(4095, fmax(0, lround(code)));
    }
    for (int i = 0; i < 1024; i += 2) { uint16_t t = buf[i]; buf[i] = buf[i + 1]; buf[i + 1] = t; }
    sIdx += 1024;
    g_simNowUs = (int64_t)(sIdx);
    acqTestFeed(buf, 1024, g_simNowUs);
    if (acqGetFrame(f, proc.seq)) { processFrame(f, acqGetCounter(), proc); have = true; }
    commandsPoll();
  }
}

static void dump(const char* name, int sel) {
  displayRender(proc, have, sel);
  char path[256];
  snprintf(path, sizeof(path), "%s/oled_%s.pbm", getenv("OUTDIR") ? getenv("OUTDIR") : ".", name);
  FILE* f = fopen(path, "w");
  fprintf(f, "P1\n128 64\n");
  for (int y = 0; y < 64; y++) {
    for (int x = 0; x < 128; x++) fputc(g_oledFrame[x + (y >> 3) * 128] & (1 << (y & 7)) ? '1' : '0', f);
    fputc('\n', f);
  }
  fclose(f);
  printf("wrote %s\n", path);
}

int main() {
  settingsInit();
  acqTestInit(1e6);
  displayBegin();
  auto sq = [](double t) { return fmod(t * 1000, 1.0) < 0.5 ? 3100.0 : 100.0; };
  auto sine50 = [](double t) { return 1600 + 1400 * sin(2 * M_PI * 50 * t); };

  run(sq, 1.0);
  dump("scope_square", P_TDIV);

  runCommand("tdiv 5ms");
  runCommand("tlevel 1.6");
  run(sine50, 1.0);
  displayToast(paramName(P_TLEVEL), paramValue(P_TLEVEL));
  dump("scope_sine_toast", P_TLEVEL);
  // let the toast expire
  run(sine50, 1.6);

  runCommand("view meter");
  dump("meter", -1);

  runCommand("view fft");
  runCommand("tdiv 100us");
  run([](double t) { return 1600 + 1000 * sin(2 * M_PI * 10000 * t) + 300 * sin(2 * M_PI * 30000 * t); }, 0.6);
  dump("fft", -1);

  runCommand("view info");
  dump("info", -1);

  runCommand("view scope");
  runCommand("tdiv 500ms");
  run([](double t) { return 1600 + 1000 * sin(2 * M_PI * 0.7 * t); }, 2.5);
  dump("roll", -1);
  return 0;
}
