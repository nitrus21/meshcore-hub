// Web server: static app (LittleFS /www, gzip), REST API /api/*, WebSocket /ws, OTA, backup.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>

namespace Web {
void begin();
void loop();                       // deferred work (reboot, restore), client cleanup
void broadcast(JsonDocument& d);   // push an event to every open web socket
// millis() of the last HTTP request: Telegram yields its TLS session (~45 kB) while the app is used.
uint32_t lastRequestMs();
}  // namespace Web
