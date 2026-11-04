// Optional 4G modem (SIMCom A7670 / SIM7600, or any modem speaking standard AT + PPP) on a UART:
// Internet for Telegram when the Wi-Fi is down ("backup") or all the time ("always").
// Needs Arduino-ESP32 3.x (PPP / esp_modem); with 2.x it only reports "unsupported".
#pragma once
#include <ArduinoJson.h>

namespace Cellular {
void begin();                    // starts the modem task if the modem is enabled
void reconfigure();              // cell* settings changed: restart the modem (starts the task if needed)
bool online();                   // PPP link up with an IP address
bool supported();                // built with PPP support
void statusJson(JsonObject o);   // state for the web app and the screen
}  // namespace Cellular
