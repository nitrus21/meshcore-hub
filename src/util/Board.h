// Status LED and BOOT button.
//   LED: slow blink = looking for the module, solid = connected, fast blink = setup mode
//   BOOT held 5 s = back to the setup wizard (settings and history are kept)
#pragma once
#include <Arduino.h>
#include <functional>

namespace Board {
enum class Led : uint8_t { Off, Searching, Connected, Setup };
void begin();
void loop();
void setLed(Led m);
void onLongPress(std::function<void()> cb);
}  // namespace Board
