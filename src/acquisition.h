#pragma once
#include "config.h"

// One screenful of samples. Point values are ADC codes * 16: decimation
// averages several conversions, which buys sub-LSB resolution on slow
// timebases.
struct Frame {
  uint32_t seq;
  uint16_t n;        // points in the record
  uint16_t valid;    // valid points; < n only while roll mode is filling
  float    dt;       // seconds between points
  float    t0;       // time of point 0 relative to the trigger, in points
  uint8_t  flags;
  uint8_t  tdiv;
  uint16_t pts[REC_POINTS];
};
enum : uint8_t { FRAME_TRIGGERED = 1, FRAME_ROLL = 2 };

// Hardware frequency counter: runs on every single ADC conversion, independent
// of timebase, with auto mid-level threshold and hysteresis.
struct CounterResult {
  uint32_t seq;
  float    freq;       // Hz, 0 when no periodic signal
  float    duty;       // 0..1
  float    minMv, maxMv, meanMv;   // pin millivolts over the gate
  float    gateSec;
};

enum AcqState : uint8_t { ACQ_ARMING, ACQ_WAIT, ACQ_POST, ACQ_STOPPED, ACQ_ROLL };

void  acqBegin();
bool  acqGetFrame(Frame& out, uint32_t lastSeq);   // false if nothing newer
CounterResult acqGetCounter();
uint8_t acqState();
bool  acqLastTriggered();
void  acqRearm();
float acqSampleRate();       // calibrated ADC rate in S/s
float acqMeasuredRate();     // long-term rate measured against the CPU timer
float acqRequestedRate();    // what the I2S peripheral was asked for
const char* acqCalSource();  // which eFuse calibration the ADC LUT uses
float acqFrameRate();        // published frames per second
float acqCpuLoad();          // 0..1 share of core 1 spent on sample processing
bool  acqHalfRate();         // using one of each pair of DMA words (saved, or detected this start)

// Captures n raw 16-bit DMA words (channel in the top 4 bits, code in the low
// 12; before swap/invert) for selftest and adcdiag.
uint32_t acqCaptureRaw(uint16_t* dst, uint32_t n, uint32_t timeoutMs);

// Restart the ADC with S.hwRate / S.sarClkDiv / S.sarCycles.
void  acqReinit();

// Finds the fastest ADC rate/timing that reads correctly (needs GPIO26
// jumpered to GPIO34; drives GPIO26 itself, so the square generator must be
// released first). Starts with a digital continuity check of the jumper;
// jumperOnly stops after it. Writes a report into out; updates S on TUNE_OK.
// TUNE_FRONTEND: GPIO26 reaches GPIO34 only through an attenuating front-end,
// too small a swing to tune with; the ADC settings are left alone.
enum TuneResult : uint8_t { TUNE_FAIL = 0, TUNE_OK = 1, TUNE_FRONTEND = 2 };
TuneResult acqTune(char* out, size_t len, uint32_t timeoutMs, bool jumperOnly = false);

// conversions
float    codeToMv(float code);          // ADC code (may be fractional) -> pin mV
float    ptToVolts(uint16_t pt);        // point -> probe volts (current calibration)
uint16_t voltsToPt(float v);            // probe volts -> point (clamped)
inline float ptToMv(uint16_t pt) { return codeToMv(pt * (1.0f / 16.0f)); }
