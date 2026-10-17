#include "measure.h"
#include "settings.h"
#include <math.h>
#include <stdio.h>

void processFrame(const Frame& f, const CounterResult& c, Processed& p) {
  p.seq = f.seq;
  p.n = f.n;
  p.valid = f.valid;
  p.dt = f.dt;
  p.t0 = f.t0;
  p.flags = f.flags;
  p.tdiv = f.tdiv;

  const uint16_t off = f.n - f.valid;
  double sum = 0, sum2 = 0;
  for (uint16_t k = off; k < f.n; k++) {
    float v = ptToVolts(f.pts[k]);
    p.v[k] = v;
    sum += v;
  }
  for (uint16_t k = 0; k < off; k++) p.v[k] = 0;
  const float mean = f.valid ? sum / f.valid : 0;
  p.acOffset = 0;
  if (S.coupling == COUPLE_AC) {
    // use the long counter gate mean when available: steadier than the window mean
    p.acOffset = c.gateSec > 0 ? pinMvToVolts(c.meanMv) : mean;
    for (uint16_t k = off; k < f.n; k++) p.v[k] -= p.acOffset;
  }

  Measurements& m = p.m;
  m.vmax = -1e9f;
  m.vmin = 1e9f;
  sum = 0;
  for (uint16_t k = off; k < f.n; k++) {
    float v = p.v[k];
    if (v > m.vmax) m.vmax = v;
    if (v < m.vmin) m.vmin = v;
    sum += v;
    sum2 += (double)v * v;
  }
  if (!f.valid) m.vmax = m.vmin = 0;
  m.vpp = m.vmax - m.vmin;
  m.vavg = f.valid ? sum / f.valid : 0;
  m.vrms = f.valid ? sqrt(sum2 / f.valid) : 0;
  float acsq = m.vrms * m.vrms - m.vavg * m.vavg;
  m.vac = acsq > 0 ? sqrtf(acsq) : 0;

  m.freq = c.freq;
  m.duty = c.duty;
  m.dcMean = pinMvToVolts(c.meanMv);
  m.dcMin = pinMvToVolts(c.minMv);
  m.dcMax = pinMvToVolts(c.maxMv);
}

// ------------------------------------------------------------- FFT

static float fre[1024], fim[1024];

int fftDb(const float* v, int n, float* outDb) {
  int N = 1;
  while (N * 2 <= n && N < 1024) N *= 2;
  if (N < 16) return 0;
  const float* src = v + (n - N);

  float mean = 0;
  for (int i = 0; i < N; i++) mean += src[i];
  mean /= N;
  for (int i = 0; i < N; i++) {
    float w = 0.5f - 0.5f * cosf(2.0f * (float)M_PI * i / (N - 1));
    fre[i] = (src[i] - mean) * w;   // drop DC so it doesn't swamp bin 0
    fim[i] = 0;
  }
  // bit reversal
  for (int i = 1, j = 0; i < N; i++) {
    int bit = N >> 1;
    for (; j & bit; bit >>= 1) j ^= bit;
    j ^= bit;
    if (i < j) { float t = fre[i]; fre[i] = fre[j]; fre[j] = t; }
  }
  // radix-2 butterflies
  for (int len = 2; len <= N; len <<= 1) {
    float ang = -2.0f * (float)M_PI / len;
    float wr = cosf(ang), wi = sinf(ang);
    for (int i = 0; i < N; i += len) {
      float cr = 1, ci = 0;
      for (int k = 0; k < len / 2; k++) {
        int a = i + k, b = a + len / 2;
        float xr = fre[b] * cr - fim[b] * ci;
        float xi = fre[b] * ci + fim[b] * cr;
        fre[b] = fre[a] - xr; fim[b] = fim[a] - xi;
        fre[a] += xr;         fim[a] += xi;
        float ncr = cr * wr - ci * wi;
        ci = cr * wi + ci * wr;
        cr = ncr;
      }
    }
  }
  // magnitude -> RMS volts of a sine in that bin (Hann coherent gain 0.5)
  const float scale = 4.0f / N / 1.41421356f;
  for (int i = 0; i < N / 2; i++) {
    float mag = sqrtf(fre[i] * fre[i] + fim[i] * fim[i]) * scale;
    outDb[i] = 20.0f * log10f(mag + 1e-7f);
  }
  return N / 2;
}

// ------------------------------------------------------------- formatting

char* fmtSI(char* buf, float value, const char* unit, int digits) {
  if (isnan(value) || isinf(value)) { sprintf(buf, "---%s", unit); return buf; }
  static const char* pfx[] = {"n", "u", "m", "", "k", "M"};
  float a = fabsf(value);
  int e = 3;
  if (a > 0) {
    while (a < 1.0f && e > 0) { a *= 1000; e--; }
    while (a >= 1000.0f && e < 5) { a /= 1000; e++; }
  }
  int intDigits = a >= 100 ? 3 : a >= 10 ? 2 : 1;
  int dec = digits - intDigits;
  if (dec < 0) dec = 0;
  // rounding can push e.g. 999.96 to 1000.0; renormalise
  float r = roundf(a * powf(10, dec)) / powf(10, dec);
  if (r >= 1000.0f && e < 5) { a = r / 1000; e++; dec = digits - 1; }
  sprintf(buf, "%s%.*f%s%s", value < 0 ? "-" : "", dec, a, pfx[e], unit);
  return buf;
}
