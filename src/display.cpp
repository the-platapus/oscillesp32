// SSD1306 128x64 renderer.
//
// Scope layout:
//   y 0..7    status: time/div, volts/div, trigger, state
//   y 8..55   waveform, 10 x 8 divisions of 12 x 6 px, x 4..123
//   y 56..63  measurements
// Left margin carries the ground marker, right margin the trigger level.

#include "display.h"
#include "settings.h"
#include "commands.h"
#include "config.h"
#include "web.h"
#include "ssd1306.h"

static Oled oled;
static char sToastTitle[24], sToastValue[24];
static uint32_t sToastUntil = 0;
static float sFftDb[512];

static const int GX = 4, GY = 8, GW = 120, GH = 48;   // graticule

const char* paramName(int p) {
  static const char* names[] = {"TIME/DIV", "VOLTS/DIV", "TRIG LEVEL", "TRIG MODE",
                                "TRIG EDGE", "V POSITION", "TRIG POS", "COUPLING", "VIEW"};
  return p >= 0 && p < P_COUNT ? names[p] : "";
}

const char* paramValue(int p) {
  static char b[24];
  static const char* views[] = {"SCOPE", "METER", "FFT", "INFO"};
  switch (p) {
    case P_TDIV:     return TDIV_LABEL[S.tdiv];
    case P_VDIV:     return VDIV_LABEL[S.vdiv];
    case P_TLEVEL:   return fmtSI(b, S.trigLevel, "V", 3);
    case P_TMODE:    return S.trigMode == TRIG_AUTO ? "AUTO" : S.trigMode == TRIG_NORMAL ? "NORMAL" : "SINGLE";
    case P_TEDGE:    return S.trigEdge == EDGE_RISE ? "RISING" : "FALLING";
    case P_VPOS:     return fmtSI(b, S.vpos, "V", 3);
    case P_HPOS:     snprintf(b, sizeof(b), "%d%%", (int)lroundf(S.hpos * 100)); return b;
    case P_COUPLING: return S.coupling == COUPLE_DC ? "DC" : "AC";
    case P_VIEW:     return views[S.view % VIEW_COUNT];
  }
  return "";
}

bool displayBegin() {
  bool ok = oled.begin(PIN_OLED_SDA, PIN_OLED_SCL, OLED_I2C_HZ);
  if (ok) printf("SSD1306 at 0x%02X on SDA=%d SCL=%d\n", oled.addr, oled.sda, oled.scl);
  return ok;
}

void displayMessage(const char* l1, const char* l2, const char* l3) {
  if (!oled.ok) return;
  oled.clear();
  oled.text(0, 0, l1);
  if (l2) oled.text(0, 16, l2);
  if (l3) oled.text(0, 32, l3);
  oled.display();
}

void displayToast(const char* title, const char* value) {
  strlcpy(sToastTitle, title, sizeof(sToastTitle));
  strlcpy(sToastValue, value, sizeof(sToastValue));
  sToastUntil = millis() + 1500;
}

// ------------------------------------------------------------- helpers

static void text(int x, int y, const char* s, bool inverse = false) {
  if (inverse) oled.textInv(x, y, s);
  else oled.text(x, y, s);
}

// compact 4-char volts for the status bar
static void fmtV4(char* b, float v) {
  float a = fabsf(v);
  if (a < 9.995f) sprintf(b, v < 0 ? "%.1f" : "%.2f", v);
  else if (a < 99.95f) sprintf(b, v < 0 ? "%.0f" : "%.1f", v);
  else sprintf(b, "%.0f", v);
}

static int voltY(float v) {
  float div = (v + S.vpos) / VDIV_TABLE[S.vdiv];
  int y = (int)lroundf(GY + (4.0f - div) * (GH / 8.0f));
  return clampv(y, GY, GY + GH - 1);
}

static void graticule() {
  for (int i = 0; i <= 10; i++)
    for (int j = 0; j <= 8; j++)
      oled.px(GX + i * 12 - (i == 10), GY + j * 6 - (j == 8));
  // centre axes, dotted
  for (int x = GX; x < GX + GW; x += 3) oled.px(x, GY + GH / 2);
  for (int y = GY; y < GY + GH; y += 3) oled.px(GX + GW / 2, y);
}

static void statusBar(int sel) {
  char b[12];
  static const char* vshort[] = {"10mV", "20mV", "50mV", ".1V", ".2V", ".5V", "1V", "2V", "5V", "10V", "20V"};
  text(0, 0, TDIV_LABEL[S.tdiv], sel == P_TDIV);
  text(33, 0, vshort[S.vdiv], sel == P_VDIV);
  char m[2] = {"ANS"[S.trigMode % 3], 0};
  text(60, 0, m, sel == P_TMODE);
  char e[2] = {(char)(S.trigEdge == EDGE_RISE ? 0x18 : 0x19), 0};
  text(66, 0, e, sel == P_TEDGE);
  fmtV4(b, S.trigLevel);
  text(73, 0, b, sel == P_TLEVEL);
  text(104, 0, scopeStateText());
}

static void markers(const Processed& p) {
  // ground (or AC mean) marker, left margin
  int gy = voltY(0);
  oled.line(0, gy - 2, 2, gy);
  oled.line(0, gy + 2, 2, gy);
  oled.px(0, gy);
  // trigger level, right margin
  int ty = voltY(S.trigLevel);
  oled.line(127, ty - 2, 125, ty);
  oled.line(127, ty + 2, 125, ty);
  oled.px(127, ty);
  // trigger position, top edge
  if (!(p.flags & FRAME_ROLL)) {
    int tx = GX + (int)lroundf(S.hpos * GW);
    oled.hline(tx - 2, tx + 2, GY);
    oled.hline(tx - 1, tx + 1, GY + 1);
  }
}

static void waveform(const Processed& p) {
  if (!p.n) return;
  const float n = p.n;
  const bool roll = p.flags & FRAME_ROLL;
  const float base = roll ? 0 : S.hpos * n + p.t0;
  int px = -1, py = 0;
  for (int k = p.n - p.valid; k < p.n; k++) {
    int x = GX + (int)((base + k) / n * GW);
    if (x < GX || x >= GX + GW) continue;
    int y = voltY(p.v[k]);
    if (px >= 0) oled.line(px, py, x, y);
    else oled.px(x, y);
    px = x;
    py = y;
  }
}

static void measRow(const Processed& p) {
  char b[40], b2[24];
  const Measurements& m = p.m;
  if (m.freq > 0) {
    text(0, 56, fmtSI(b, m.freq, "Hz", 4));
    fmtSI(b2, m.vpp, "V", 3);
    strcat(b2, "pp");
    text(58, 56, b2);
    snprintf(b, sizeof(b), "%d%%", (int)lroundf(m.duty * 100));
    text(128 - oled.textWidth(b), 56, b);
  } else {
    fmtSI(b2, m.vavg, "V", 3);
    snprintf(b, sizeof(b), "avg %s", b2);
    text(0, 56, b);
    fmtSI(b2, m.vpp, "V", 3);
    strcat(b2, "pp");
    text(128 - oled.textWidth(b2), 56, b2);
  }
}

static void toast() {
  if (!sToastUntil || (int32_t)(millis() - sToastUntil) > 0) return;
  oled.fill(10, 18, 108, 28, false);
  oled.rect(10, 18, 108, 28);
  text(64 - oled.textWidth(sToastTitle) / 2, 22, sToastTitle);
  text(64 - oled.textWidth(sToastValue) / 2, 34, sToastValue);
}

// ------------------------------------------------------------- views

static void viewScope(const Processed& p, bool have, int sel) {
  statusBar(sel);
  graticule();
  if (have) {
    waveform(p);
    markers(p);
    measRow(p);
  } else {
    text(19, 28, "waiting for ADC");
  }
}

static void viewMeter(const Processed& p) {
  const Measurements& m = p.m;
  char b[32], v[24];
  text(0, 0, S.coupling == COUPLE_AC ? "METER  AC" : "METER  DC");
  text(104, 0, scopeStateText());
  if (m.freq > 0) oled.text(0, 10, fmtSI(b, m.freq, "Hz", 5), true, 2);
  else oled.text(0, 10, fmtSI(b, m.dcMean, "V", 4), true, 2);

  snprintf(b, sizeof(b), "Vpp %s", fmtSI(v, m.vpp, "V", 4));   text(0, 28, b);
  if (m.freq > 0) { snprintf(b, sizeof(b), "Duty %.1f%%", m.duty * 100); text(66, 28, b); }
  snprintf(b, sizeof(b), "Vrm %s", fmtSI(v, m.vrms, "V", 4));   text(0, 37, b);
  snprintf(b, sizeof(b), "AC %s", fmtSI(v, m.vac, "V", 3));     text(66, 37, b);
  snprintf(b, sizeof(b), "Max %s", fmtSI(v, m.vmax, "V", 4));   text(0, 46, b);
  snprintf(b, sizeof(b), "Mn %s", fmtSI(v, m.vmin, "V", 3));    text(66, 46, b);
  snprintf(b, sizeof(b), "DC  %s", fmtSI(v, m.dcMean, "V", 4)); text(0, 55, b);
  if (m.freq > 0) { snprintf(b, sizeof(b), "T %s", fmtSI(v, 1.0f / m.freq, "s", 3)); text(66, 55, b); }
}

static void viewFft(const Processed& p, bool have) {
  char b[32], v[24];
  if (!have || p.valid < 32) { text(0, 0, "FFT: no data"); return; }
  int bins = fftDb(p.v + (p.n - p.valid), p.valid, sFftDb);
  if (!bins) return;
  float span = 0.5f / p.dt;
  snprintf(b, sizeof(b), "FFT 0-%s", fmtSI(v, span, "Hz", 3));
  text(0, 0, b);
  text(104, 0, scopeStateText());

  // +20 dBV at top, -80 dBV at bottom; dotted line every 20 dB
  const float top = 20, range = 100;
  for (int db = 0; db <= 100; db += 20) {
    int y = GY + (int)(db / range * (GH - 1));
    for (int x = GX; x < GX + GW; x += 4) oled.px(x, y);
  }
  int peak = 1;
  for (int i = 2; i < bins; i++) if (sFftDb[i] > sFftDb[peak]) peak = i;
  for (int x = 0; x < GW; x++) {
    int a = x * bins / GW, e = (x + 1) * bins / GW;
    if (e <= a) e = a + 1;
    float mx = -200;
    for (int i = a; i < e && i < bins; i++) mx = fmaxf(mx, sFftDb[i]);
    int y = GY + (int)((top - mx) / range * (GH - 1));
    if (y < GY) y = GY;
    if (y > GY + GH - 1) continue;
    oled.vline(GX + x, y, GY + GH - 1);
  }
  float pf = peak / (2.0f * bins * p.dt);
  snprintf(b, sizeof(b), "pk %s", fmtSI(v, pf, "Hz", 4));
  text(0, 56, b);
  snprintf(b, sizeof(b), "%.0fdBV", sFftDb[peak]);
  text(128 - oled.textWidth(b), 56, b);
}

static void viewInfo() {
  char b[48], v[24];
  text(0, 0, "INFO  fw 1.0");
  if (S.wifiMode == 0) text(0, 9, "WiFi off");
  else {
    snprintf(b, sizeof(b), "%s %s", webIsAp() ? "AP" : "NET", webSsid());
    text(0, 9, b);
  }
  snprintf(b, sizeof(b), "IP %s", webIp());
  text(0, 18, b);
  if (webIsAp()) { snprintf(b, sizeof(b), "pw %s", AP_PASS); text(0, 27, b); }
  snprintf(b, sizeof(b), "ADC %s", fmtSI(v, acqSampleRate(), "S/s", 4));
  text(0, 36, b);
  static const char* fe[] = {"direct", "bias", "custom"};
  snprintf(b, sizeof(b), "FE %s g%.2f", fe[S.frontEnd % 3], S.calGain);
  text(0, 45, b);
  snprintf(b, sizeof(b), "web:%d swap%d inv%d", webClientCount(), S.swapPairs, S.invert);
  text(0, 54, b);
}

void displayRender(const Processed& p, bool have, int sel) {
  if (!oled.ok) return;
  oled.clear();
  switch (S.view) {
    case VIEW_METER: viewMeter(p); break;
    case VIEW_FFT:   viewFft(p, have); break;
    case VIEW_INFO:  viewInfo(); break;
    default:         viewScope(p, have, sel); break;
  }
  toast();
  oled.display();
}
