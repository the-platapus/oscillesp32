// ESP32 + SSD1306 oscilloscope (ESP-IDF 4.4)
//
// Core 1: acquisition task (ADC DMA, trigger, counter) + this loop (OLED, buttons, serial)
// Core 0: WiFi, HTTP/WebSocket server, browser command task

#include "config.h"
#include "settings.h"
#include "acquisition.h"
#include "measure.h"
#include "display.h"
#include "siggen.h"
#include "commands.h"
#include "web.h"
#include "ui.h"
#include "driver/uart.h"

static Frame sFrame;
static Processed sProc;
static bool sHave = false;

static void serialPoll() {
  static char line[200];
  static int len = 0;
  uint8_t c;
  while (uart_read_bytes(UART_NUM_0, &c, 1, 0) == 1) {
    if (c == '\r') continue;
    if (c == '\n') {
      line[len] = 0;
      len = 0;
      std::string r = runCommand(line);
      if (!r.empty()) printf("%s\n", r.c_str());
    } else if (len < (int)sizeof(line) - 1) {
      line[len++] = (char)c;
    }
  }
}

extern "C" void app_main(void) {
  uart_driver_install(UART_NUM_0, 512, 0, 0, nullptr, 0);
  printf("\nESP32 Scope starting\n");
  settingsInit();
  if (!displayBegin()) printf("OLED not found (scanned common SDA/SCL pairs) - continuing without it\n");
  displayMessage("ESP32 Scope", "calibrating ADC...");
  uiBegin();
  siggenApply();
  acqBegin();
  webBegin();

  // wait for the acquisition task to finish its rate calibration
  uint32_t t0 = millis();
  while (!acqGetFrame(sFrame, 0) && millis() - t0 < 3000) delay(20);
  printf("ADC: requested %.0f S/s, measured %.0f S/s, using %.1f S/s, cal: %s\n",
         acqRequestedRate(), acqMeasuredRate(), acqSampleRate(), acqCalSource());
  printf("WiFi: join '%s' (password '%s') and open http://192.168.4.1/\n", AP_SSID, AP_PASS);
  printf("Type 'help' + Enter for commands. 1 kHz test square on GPIO26.\n");

  uint32_t lastProc = 0, lastDraw = 0, lastCounterSeq = 0;
  for (;;) {
    uint32_t now = millis();
    serialPoll();
    uiPoll();
    commandsPoll();
    settingsSaveIfDue();

    if (now - lastProc >= 15) {
      if (acqGetFrame(sFrame, sProc.seq)) {
        processFrame(sFrame, acqGetCounter(), sProc);
        sHave = true;
        webPublish(sProc);
        lastProc = now;
      } else if (sHave) {
        // keep counter readouts live while stopped or waiting for a trigger
        CounterResult c = acqGetCounter();
        if (c.seq != lastCounterSeq) {
          lastCounterSeq = c.seq;
          sProc.m.freq = c.freq;
          sProc.m.duty = c.duty;
          sProc.m.dcMean = pinMvToVolts(c.meanMv);
          sProc.m.dcMin = pinMvToVolts(c.minMv);
          sProc.m.dcMax = pinMvToVolts(c.maxMv);
        }
      }
    }

    if (now - lastDraw >= 33) {
      displayRender(sProc, sHave, uiSelected());
      lastDraw = now;
    }
    delay(1);
  }
}
