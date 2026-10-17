#pragma once
#include "measure.h"

void        webBegin();             // WiFi + HTTP/WebSocket server (core 0)
void        webRequestReconfig();   // re-apply S.wifiMode / credentials
const char* webIp();                // "192.168.4.1", "off", ...
const char* webSsid();
bool        webIsAp();
int         webClientCount();

// Latest processed frame for the browser (copied, never blocks the caller).
void        webPublish(const Processed& p);
