#pragma once
#include "platform.h"

// Buttons:
//   BOOT  click = next view (scope/meter/FFT/info), double-click = run/stop,
//         hold = autoset
//   SEL   click = next parameter, hold = previous parameter   (optional)
//   UP/DOWN  change the selected parameter, hold to repeat       (optional)
void uiBegin();
void uiPoll();
int  uiSelected();   // currently selected Param, or -1 when no SEL button is in use
