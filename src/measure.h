#pragma once
#include "acquisition.h"

struct Measurements {
  // from the displayed record (what's on screen, after coupling)
  float vmax, vmin, vpp, vavg, vrms, vac;
  // from the always-on counter (full-rate, independent of timebase)
  float freq, duty;
  float dcMean, dcMin, dcMax;     // probe volts over the counter gate
};

struct Processed {
  uint32_t seq;
  uint16_t n, valid;
  float    dt, t0;
  uint8_t  flags, tdiv;
  float    acOffset;              // volts removed by AC coupling
  float    v[REC_POINTS];         // probe volts
  Measurements m;
};

void processFrame(const Frame& f, const CounterResult& c, Processed& p);

// Hann-windowed FFT of the last n samples (n rounded down to a power of two,
// max 1024). Writes n/2 magnitudes in dBV (RMS) into outDb and returns n/2.
int fftDb(const float* v, int n, float* outDb);

// SI formatting: fmtSI(buf, 0.00123, "V", 3) -> "1.23mV"
char* fmtSI(char* buf, float value, const char* unit, int digits = 3);
