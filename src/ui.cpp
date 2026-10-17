#include "ui.h"
#include "config.h"
#include "settings.h"
#include "commands.h"
#include "display.h"
#include "driver/gpio.h"

struct Button {
  uint8_t pin;
  bool down = false;        // debounced state
  bool raw = false;
  uint32_t changedAt = 0;   // raw edge time for debounce
  uint32_t pressedAt = 0;
  uint32_t lastRepeat = 0;
  bool longFired = false;
  explicit Button(uint8_t p) : pin(p) {}
};

enum Event { EV_NONE, EV_CLICK, EV_LONG, EV_REPEAT };

static Button bBoot(PIN_BTN_BOOT), bSel(PIN_BTN_SEL), bUp(PIN_BTN_UP), bDown(PIN_BTN_DOWN);
static int sSelected = -1;
static uint32_t sBootClickAt = 0;
static bool sBootPendingClick = false;

static const uint32_t DEBOUNCE_MS = 25, LONG_MS = 650, REPEAT_DELAY_MS = 400, REPEAT_MS = 110,
                      DOUBLE_MS = 300;

static Event poll(Button& b, bool repeat) {
  uint32_t now = millis();
  bool r = gpio_get_level((gpio_num_t)b.pin) == 0;
  if (r != b.raw) { b.raw = r; b.changedAt = now; }
  if (now - b.changedAt < DEBOUNCE_MS || r == b.down) {
    if (b.down) {
      uint32_t held = now - b.pressedAt;
      if (repeat && held > REPEAT_DELAY_MS && now - b.lastRepeat > REPEAT_MS) {
        b.lastRepeat = now;
        b.longFired = true;
        return EV_REPEAT;
      }
      if (!repeat && !b.longFired && held > LONG_MS) { b.longFired = true; return EV_LONG; }
    }
    return EV_NONE;
  }
  b.down = r;
  if (r) { b.pressedAt = now; b.lastRepeat = now; b.longFired = false; return repeat ? EV_REPEAT : EV_NONE; }
  return (!b.longFired && !repeat) ? EV_CLICK : EV_NONE;
}

int uiSelected() { return sSelected; }

static void toastParam(int p) { displayToast(paramName(p), paramValue(p)); }

static void adjust(int dir) {
  if (sSelected < 0) sSelected = P_TDIV;
  char cmd[40];
  const float vdiv = VDIV_TABLE[S.vdiv];
  switch (sSelected) {
    case P_TDIV:   runCommand(dir > 0 ? "tdiv +" : "tdiv -"); break;
    case P_VDIV:   runCommand(dir > 0 ? "vdiv +" : "vdiv -"); break;
    case P_TLEVEL: snprintf(cmd, sizeof(cmd), "tlevel %.5f", S.trigLevel + dir * 0.2f * vdiv); runCommand(cmd); break;
    case P_TMODE: {
      static const char* m[] = {"tmode auto", "tmode normal", "tmode single"};
      runCommand(m[(S.trigMode + (dir > 0 ? 1 : 2)) % 3]);
      break;
    }
    case P_TEDGE:  runCommand("tedge toggle"); break;
    case P_VPOS:   snprintf(cmd, sizeof(cmd), "vpos %.5f", S.vpos + dir * 0.5f * vdiv); runCommand(cmd); break;
    case P_HPOS:   snprintf(cmd, sizeof(cmd), "hpos %.3f", S.hpos + dir * 0.05f); runCommand(cmd); break;
    case P_COUPLING: runCommand("coupling toggle"); break;
    case P_VIEW: {
      char c[16];
      static const char* v[] = {"scope", "meter", "fft", "info"};
      snprintf(c, sizeof(c), "view %s", v[(S.view + (dir > 0 ? 1 : VIEW_COUNT - 1)) % VIEW_COUNT]);
      runCommand(c);
      break;
    }
  }
  toastParam(sSelected);
}

void uiBegin() {
  gpio_config_t io = {};
  io.mode = GPIO_MODE_INPUT;
  io.pull_up_en = GPIO_PULLUP_ENABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  io.pin_bit_mask = (1ULL << PIN_BTN_BOOT) | (1ULL << PIN_BTN_SEL) | (1ULL << PIN_BTN_UP) | (1ULL << PIN_BTN_DOWN);
  gpio_config(&io);
}

void uiPoll() {
  uint32_t now = millis();

  Event e = poll(bBoot, false);
  if (e == EV_LONG) {
    runCommand("autoset");
    displayToast("AUTOSET", "measuring...");
  } else if (e == EV_CLICK) {
    if (sBootPendingClick && now - sBootClickAt < DOUBLE_MS) {
      sBootPendingClick = false;
      runCommand("runstop");
      displayToast("RUN/STOP", gRunning ? "RUN" : "STOP");
    } else {
      sBootPendingClick = true;
      sBootClickAt = now;
    }
  }
  if (sBootPendingClick && now - sBootClickAt >= DOUBLE_MS) {
    sBootPendingClick = false;
    runCommand("view next");
    toastParam(P_VIEW);
  }

  e = poll(bSel, false);
  if (e == EV_CLICK) {
    sSelected = (sSelected + 1) % P_COUNT;
    toastParam(sSelected);
  } else if (e == EV_LONG) {
    sSelected = (sSelected + P_COUNT - 1) % P_COUNT;
    if (sSelected < 0) sSelected = P_COUNT - 1;
    toastParam(sSelected);
  }

  if (poll(bUp, true) == EV_REPEAT) adjust(+1);
  if (poll(bDown, true) == EV_REPEAT) adjust(-1);
}
