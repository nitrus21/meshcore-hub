// MeshCore Hub - ESP32 firmware entry point.
//
// Tasks:
//   loopTask (Arduino)  Wi-Fi / DNS / web deferred work / LED / button / ArduinoOTA
//   mesh                BLE or TCP link to the Companion radio, history, events
//   tg_poll, tg_send    Telegram bridge (see telegram/TelegramBridge.cpp)
//   async_tcp           web server and WebSocket (library task)
#include <Arduino.h>
#include <ArduinoOTA.h>
#include <LittleFS.h>
#include <esp_idf_version.h>
#include <esp_system.h>
#include <esp_task_wdt.h>
#include "App.h"
#include "config/Config.h"
#include "display/Display.h"
#include "setup/Cellular.h"
#include "setup/Network.h"
#include "store/History.h"
#include "telegram/TelegramBridge.h"
#include "util/Board.h"
#include "util/Log.h"
#include "web/Web.h"

static const uint32_t WDT_SECONDS = 60;  // BLE pairing can legitimately block ~30 s
static bool setupMode = false;
static bool otaStarted = false;
static std::string otaMd5Started;  // hash ArduinoOTA was given
// Arduino 2 keeps the first password hash for good (setPasswordHash is ignored once begun, even
// after end()); Arduino 3 takes a new one after end(), so ArduinoOTA simply restarts with it.
#if ESP_ARDUINO_VERSION_MAJOR >= 3
static const bool OTA_HASH_STICKY = false;
#else
static const bool OTA_HASH_STICKY = true;
#endif
// For the settings API: ArduinoOTA turned back on with a hash it cannot take any more needs a restart.
bool arduinoOtaNeedsRestart(const std::string& md5) {
  return OTA_HASH_STICKY && !otaMd5Started.empty() && md5 != otaMd5Started;
}

static void meshTask(void*) {
  esp_task_wdt_add(nullptr);
  for (;;) {
    esp_task_wdt_reset();
    gApp.meshLoop();
    vTaskDelay(pdMS_TO_TICKS(5));
  }
}

static void startArduinoOta() {
  std::string host, md5;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (!gConfig.arduinoOta || gConfig.otaMd5.empty()) return;
    host = gConfig.hostname;
    md5 = gConfig.otaMd5;
  }
  // Arduino 2: after a password change it stays off until the next restart rather than accept the old one.
  if (arduinoOtaNeedsRestart(md5)) return;
  otaMd5Started = md5;
  ArduinoOTA.setHostname(host.c_str());
  ArduinoOTA.setPasswordHash(md5.c_str());  // same password as the web app
  ArduinoOTA.setMdnsEnabled(false);         // mDNS is already running
  ArduinoOTA.onStart([] { LOGI("ArduinoOTA: update started"); });
  ArduinoOTA.begin();
  otaStarted = true;
  LOGI("ArduinoOTA enabled");
}

void setup() {
  Serial.begin(115200);
  delay(50);
  Log::begin();
  Board::begin();
  Board::setLed(Board::Led::Searching);

  if (!LittleFS.begin(true, "/littlefs", 10, "littlefs")) {
    Serial.println("LittleFS mount failed");
  }
  gConfig.load();
  setupMode = !gConfig.setupDone;
  static const char* const RESET_REASONS[] = {"unknown", "power-on", "external pin", "software restart", "crash (panic)",
                                               "interrupt watchdog", "task watchdog", "other watchdog", "deep sleep",
                                               "brownout (power supply)", "SDIO", "USB", "JTAG", "eFuse", "power glitch",
                                               "CPU lock-up"};
  esp_reset_reason_t rr = esp_reset_reason();
  LOGI("MeshCore Hub %s on %s, %u kB free%s", HUB_VERSION, ESP.getChipModel(), (unsigned)(ESP.getFreeHeap() / 1024),
       setupMode ? " - SETUP MODE" : "");
  if (psramFound()) LOGI("PSRAM: %u kB free", (unsigned)(ESP.getFreePsram() / 1024));
  LOGI("last restart: %s",(unsigned)rr < sizeof(RESET_REASONS) / sizeof(RESET_REASONS[0]) ? RESET_REASONS[rr] : "?");

  gHistory.begin();
  Net::begin(setupMode);
  Cellular::begin();
  gApp.setEventSink([](JsonDocument& d) {
    Web::broadcast(d);
    Display::onEvent(d);
  });
  Log::setHook([](const char* level, const char* line) {
    JsonDocument d;
    d["t"] = "log";
    d["l"] = level;
    d["line"] = line;
    Web::broadcast(d);
  });
  Web::begin();
  gApp.begin();
  gTelegram.begin();
  Display::begin();

  Board::onLongPress([] {
    LOGW("BOOT held 5 s: back to the setup wizard");
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    gConfig.setupDone = false;
    gConfig.save();
    delay(300);
    ESP.restart();
  });

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  // Arduino 3 / IDF 5 already runs the task watchdog: change its timeout (or start it if not)
  const esp_task_wdt_config_t wdt = {WDT_SECONDS * 1000, 0, true};  // timeout, idle cores not watched, panic
  if (esp_task_wdt_reconfigure(&wdt) != ESP_OK) esp_task_wdt_init(&wdt);
#else
  esp_task_wdt_init(WDT_SECONDS, true);
#endif
  esp_task_wdt_add(nullptr);  // loopTask
  xTaskCreatePinnedToCore(meshTask, "mesh", 8192, nullptr, 2, nullptr, ARDUINO_RUNNING_CORE);
}

void loop() {
  esp_task_wdt_reset();
  Net::loop();
  Web::loop();
  Board::setLed(setupMode ? Board::Led::Setup
                          : gApp.mesh().ready() ? Board::Led::Connected : Board::Led::Searching);
  Board::loop();
  if (!otaStarted && Net::staConnected()) startArduinoOta();
  static uint32_t lastOtaCheck = 0;
  if (otaStarted && millis() - lastOtaCheck > 1000) {  // turned off or password changed in the web app
    lastOtaCheck = millis();
    bool keep;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      keep = gConfig.arduinoOta && gConfig.otaMd5 == otaMd5Started;
    }
    if (!keep) {
      ArduinoOTA.end();
      otaStarted = false;
      LOGI("ArduinoOTA stopped");
    }
  }
  if (otaStarted) ArduinoOTA.handle();

  static uint32_t lastHeap = 0;
  if (millis() - lastHeap > 600000UL) {  // memory report every 10 min
    lastHeap = millis();
    LOGI("heap: %u kB free, min %u kB, largest block %u kB", (unsigned)(ESP.getFreeHeap() / 1024),
         (unsigned)(ESP.getMinFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
  }
  delay(2);
}
