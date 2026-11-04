// Wi-Fi: setup access point + captive DNS, station mode, fallback AP, mDNS, NTP.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <string>

namespace Net {
// Address of the Hub on its own access point (setup wizard, fallback when the Wi-Fi is lost)
#define NET_AP_IP 192, 168, 7, 1
#define NET_AP_URL "http://192.168.7.1"
void begin(bool setupMode);
void loop();                 // call often (DNS)
bool apActive();
bool staConnected();
bool online();               // Internet through the Wi-Fi or the 4G modem
std::string apSsid();
// Wi-Fi scan for the wizard (async)
void startScan();
bool scanJson(JsonArray out);   // false while scanning
// Try credentials while keeping the AP up (wizard step). Status via statusJson.
void testConnect(const std::string& ssid, const std::string& pass);
void statusJson(JsonObject o);
bool isCaptiveHost(const String& host);  // host is not ours -> captive redirect
}  // namespace Net
