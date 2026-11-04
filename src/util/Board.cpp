#include "Board.h"

namespace Board {

static Led mode = Led::Off;
static std::function<void()> longPress;
static uint32_t pressedAt = 0;
static bool fired = false;
static bool ledState = false;
static uint32_t lastToggle = 0;

static void ledWrite(bool on) {
#ifdef STATUS_LED_PIN
#if defined(STATUS_LED_RGB) && STATUS_LED_RGB
  neopixelWrite(STATUS_LED_PIN, 0, on ? 6 : 0, on ? 24 : 0);  // dim blue
#else
  digitalWrite(STATUS_LED_PIN, on ? HIGH : LOW);
#endif
#else
  (void)on;
#endif
}

void begin() {
#ifdef STATUS_LED_PIN
#if !(defined(STATUS_LED_RGB) && STATUS_LED_RGB)
  pinMode(STATUS_LED_PIN, OUTPUT);
#endif
#endif
#ifdef BOOT_BUTTON_PIN
  pinMode(BOOT_BUTTON_PIN, INPUT_PULLUP);
#endif
  ledWrite(false);
}

void setLed(Led m) { mode = m; }
void onLongPress(std::function<void()> cb) { longPress = cb; }

void loop() {
  uint32_t now = millis();
  uint32_t period = mode == Led::Setup ? 120 : mode == Led::Searching ? 900 : 0;
  if (mode == Led::Connected || mode == Led::Off) {
    bool want = mode == Led::Connected;
    if (ledState != want) ledWrite(ledState = want);
  } else if (now - lastToggle >= period) {
    lastToggle = now;
    ledWrite(ledState = !ledState);
  }
#ifdef BOOT_BUTTON_PIN
  bool down = digitalRead(BOOT_BUTTON_PIN) == LOW;
  if (down) {
    if (!pressedAt) pressedAt = now ? now : 1;
    if (!fired && now - pressedAt > 5000) {
      fired = true;
      if (longPress) longPress();
    }
  } else {
    pressedAt = 0;
    fired = false;
  }
#endif
}

}  // namespace Board
