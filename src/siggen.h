#pragma once
#include "platform.h"

// Square/pulse wave on PIN_GEN_SQUARE (LEDC, crystal accurate) and a
// cosine on PIN_GEN_SINE (DAC cosine generator, ~130 Hz..55 kHz, the RC
// clock behind it is only good to a few percent).
void   siggenApply();             // apply S.gen* settings
float  siggenSquareActualHz();
float  siggenSineActualHz();

// Temporarily drive the square output without touching saved settings
// (used by selftest). Pass hz <= 0 to restore the saved settings.
float  siggenOverrideSquare(float hz, float dutyPct);

// Stops the square output and frees GPIO26 (selftest drives DAC2 there).
// siggenApply()/siggenOverrideSquare() take it back.
void   siggenReleaseSquare();

// Re-enables the sine output after something switched the DACs off
// (ESP-IDF's I2S driver uninstall does).
void   siggenRestoreDac();
