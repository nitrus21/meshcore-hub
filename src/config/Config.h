// Hub settings, persisted in NVS (Preferences). Everything is editable from the web app.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <mutex>
#include <string>
#include <vector>

// 4G modem UART pins of the board, from its build flags (T-Display-S3: 43/44)
#ifndef CELL_DEFAULT_TX
#define CELL_DEFAULT_TX -1
#endif
#ifndef CELL_DEFAULT_RX
#define CELL_DEFAULT_RX -1
#endif

struct WifiNet {
  std::string ssid, pass;
  bool operator==(const WifiNet& o) const { return ssid == o.ssid && pass == o.pass; }
};

// Directory of the private routing: "@name" over the mesh -> one Telegram chat (person or group)
struct TgRoute {
  std::string name;
  int64_t chat = 0;
  bool operator==(const TgRoute& o) const { return name == o.name && chat == o.chat; }
};

struct HubConfig {
  bool setupDone = false;
  std::string lang = "fr";
  std::string theme = "auto";        // auto | light | dark
  std::string accent = "#2f6fed";
  std::string tz = "CET-1CEST,M3.5.0,M10.5.0/3";  // POSIX TZ (Europe/Paris)

  // Wi-Fi
  std::string wifiSsid, wifiPass;    // the default network
  std::vector<WifiNet> wifiAlt;      // others, by priority: used while the default one is out of reach
  bool apOnly = false;               // stay in access-point mode (no Internet, no Telegram)
  std::string hostname = "meshcore-hub";
  std::string apPass;                // optional password of the setup access point

  // 4G modem on a UART (PPP, Arduino-ESP32 3.x): Internet when the Wi-Fi is down, or always
  bool cellEnabled = false;
  bool cellAlways = false;           // false: only while the Wi-Fi is unreachable
  std::string cellApn;               // empty: the operator's default
  std::string cellSimPin;            // SIM PIN code, if the SIM has one
  uint8_t cellPinFails = 0;          // "SIM PIN" still asked after sending cellSimPin (kept across restarts)
  std::string cellModel = "a7670";   // a7670 | sim7600 | generic
  int8_t cellTx = CELL_DEFAULT_TX;   // ESP32 TX -> modem RXD (-1: not set)
  int8_t cellRx = CELL_DEFAULT_RX;   // ESP32 RX <- modem TXD
  int8_t cellPwr = -1;               // modem PWRKEY / reset (-1: none, the modem starts by itself)

  // Module link
  std::string link;                  // "ble" | "tcp" | "" (none)
  std::string bleAddr, bleName;
  uint8_t bleAddrType = 0;
  uint32_t blePin = 123456;
  std::string tcpHost;
  uint16_t tcpPort = 5000;

  // Telegram
  bool tgEnabled = false;
  std::string tgToken;
  std::vector<int64_t> tgChats;      // whitelist; the first one receives the relayed messages
  std::string tgMode = "single";     // single | topics
  bool tgRadioInfo = true;           // add "SNR 7.5 · 2 hops" line
  bool tgRelayDm = true, tgRelayChannels = true, tgRelayRooms = true;
  bool tgRelayPublic = true;         // the public channel (slot 0), often too noisy to follow
  bool tgSkipOwn = true;             // don't relay messages sent from the web app
  bool tgInsecure = false;           // TLS without certificate check (fallback)
  std::vector<std::string> tgMutedConvs;
  bool tgRouteOn = false;            // "@name text" from an approved contact goes to tgRoutes
  bool tgRouteKeep = false;          // keep the text of routed messages in the history
  std::vector<TgRoute> tgRoutes;
  std::vector<std::string> tgRouteFrom;  // conversations ("c_...") allowed to use the directory

  // Security
  std::string adminHash, adminSalt;  // SHA-256(salt + password), hex
  std::string otaMd5;                // MD5(password) for ArduinoOTA (its own protocol)

  // History
  uint16_t historyDays = 0;          // 0 = keep until space runs out
  uint8_t historyMaxPct = 85;        // rotate when LittleFS is fuller than this

  bool arduinoOta = false;

  bool hasPassword() const { return !adminHash.empty(); }
  bool checkPassword(const std::string& pw) const;
  void setPassword(const std::string& pw);
  bool tgMuted(const std::string& conv) const;

  void load();
  void save();
  // secrets=false hides passwords and tokens (export / API).
  void toJson(JsonObject o, bool secrets) const;
  // Applies the known keys present in `o`; returns true if something changed.
  bool fromJson(JsonObjectConst o);
};

// Lowercases, strips a trailing ".local", then checks a DNS label (a-z 0-9 '-', 1..63 chars,
// no leading/trailing '-'). Returns false (h untouched) if invalid.
bool normalizeHostname(std::string& h);

extern HubConfig gConfig;
extern std::recursive_mutex gConfigMtx;

// Room / repeater passwords, encrypted in NVS with a per-device key.
namespace RoomPasswords {
void set(const std::string& prefixHex, const std::string& password);
std::string get(const std::string& prefixHex);
void remove(const std::string& prefixHex);
std::vector<std::string> keys();
void clear();
}  // namespace RoomPasswords

void factoryResetConfig();
