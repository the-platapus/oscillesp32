#pragma once
#include "measure.h"

// Parameters adjustable from the buttons (highlighted on the OLED).
enum Param : uint8_t {
  P_TDIV, P_VDIV, P_TLEVEL, P_TMODE, P_TEDGE, P_VPOS, P_HPOS, P_COUPLING, P_VIEW, P_COUNT
};

bool displayBegin();
void displayMessage(const char* l1, const char* l2 = nullptr, const char* l3 = nullptr);
void displayRender(const Processed& p, bool haveFrame, int selected);
void displayToast(const char* title, const char* value);   // shown ~1.5 s
const char* paramName(int p);
const char* paramValue(int p);   // static buffer
