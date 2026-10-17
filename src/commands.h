#pragma once
#include "platform.h"
#include <string>

// One command language for the serial console, the web UI and the buttons.
// Returns a human-readable reply (or JSON for `status`).
std::string runCommand(const std::string& line);

std::string statusJson();
const char* scopeStateText();   // "RUN", "TRIG", "AUTO", "WAIT", "ROLL", "STOP"
void commandsPoll();            // drives async jobs (autoset); call from loop()
bool autosetBusy();
