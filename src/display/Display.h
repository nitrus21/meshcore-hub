// Status screen for boards with a display (LilyGO T-Display-S3, built with -DHUB_DISPLAY_TDISPLAY_S3).
//   pages: home (module / Wi-Fi / Telegram / contacts), last messages, web app QR code
//   setup mode: Wi-Fi QR code and address of the wizard
//   KEY button (DISPLAY_BUTTON_PIN): next page; the backlight dims after a while and wakes up on
//   a button press or a new message.
// Without a display every function is a no-op.
#pragma once
#include <ArduinoJson.h>

namespace Display {
void begin();                    // starts the display task
void onEvent(JsonDocument& d);    // App events (new messages wake the screen); never blocks
}  // namespace Display
