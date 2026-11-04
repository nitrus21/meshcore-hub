#include "Cellular.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_idf_version.h>
#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include "../config/Config.h"
#include "../util/Log.h"

#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0) && defined(CONFIG_LWIP_PPP_SUPPORT)
#include <PPP.h>
#if ARDUINO_HAS_ESP_MODEM
#define HUB_CELLULAR 1
#include <driver/gpio.h>
#include <driver/uart.h>
#include <esp_event.h>
#include <esp_netif_ppp.h>
#endif
#endif

namespace Cellular {
namespace {

// Settings copied under gConfigMtx; the modem task works on its own copy.
struct Cfg {
  bool enabled = false, always = false;
  std::string apn, pin, model;
  int8_t tx = -1, rx = -1, pwr = -1;
  bool operator==(const Cfg& o) const {
    return enabled == o.enabled && always == o.always && apn == o.apn && pin == o.pin && model == o.model &&
           tx == o.tx && rx == o.rx && pwr == o.pwr;
  }
};

Cfg readCfg() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  Cfg c;
  c.enabled = gConfig.cellEnabled;
  c.always = gConfig.cellAlways;
  c.apn = gConfig.cellApn;
  c.pin = gConfig.cellSimPin;
  c.model = gConfig.cellModel;
  c.tx = gConfig.cellTx;
  c.rx = gConfig.cellRx;
  c.pwr = gConfig.cellPwr;
  return c;
}

// What the web app and the screen show; written by the modem task only. Nobody else touches the
// PPP object: it is torn down by the modem task at any time.
std::mutex stMtx;
std::string state = "off";       // off | waiting_wifi | starting | searching | connecting | online | error
std::string op, err, ip;
int rssi = 99;                    // 0..31 (99: unknown), from AT+CSQ
std::atomic<bool> up{false};
std::atomic<bool> restart{false};
std::atomic<bool> taskStarted{false};

void setState(const char* s, const std::string& e = "") {
  bool changed;
  {
    std::lock_guard<std::mutex> l(stMtx);
    changed = state != s || err != e;
    state = s;
    err = e;
  }
  // Logged outside stMtx: the log goes to the WebSocket, whose lock is held while a new client
  // gets the status (which takes stMtx): logging under stMtx could deadlock both tasks.
  if (changed) LOGI("4G: %s%s%s", s, e.empty() ? "" : " - ", e.c_str());
}

#ifdef HUB_CELLULAR
const uint32_t WIFI_GRACE_MS = 60000;      // backup mode: Wi-Fi down this long before using 4G
const uint32_t WIFI_BACK_MS = 300000;      // ...and back this long before the modem is stopped
const uint32_t ATTACH_TIMEOUT_MS = 180000; // registration on the network
const uint32_t CONNECT_TIMEOUT_MS = 60000; // PPP up after switching to data mode
const uint32_t CHECK_MS = 30000;           // CMUX: AT check of the link while online

bool started = false;
std::atomic<bool> linkErr{false};  // PPP reported an error (lost, peer dead...) since the link went up
bool pppEvents = false;
uint32_t lastCheck = 0;
uint8_t checkFails = 0;
// The SIM allows 3 wrong PINs, then wants the PUK. "SIM PIN" still asked after a start with a PIN set
// may mean a wrong PIN (or a start that failed before sending it): at most 2 such starts per PIN
// value, counted in NVS (gConfig.cellPinFails) so that restarts don't reset the count.
uint8_t pinFails() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return gConfig.cellPinFails;
}
void setPinFails(uint8_t n) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  if (gConfig.cellPinFails == n) return;
  gConfig.cellPinFails = n;
  gConfig.save();
}

// lwIP keeps one DNS list for the whole device, which the PPP link and the Wi-Fi DHCP client
// overwrite in turn: the box's servers are kept while the Wi-Fi works, the operator's once it is gone.
IPAddress lanDns[2], cellDns[2];
bool cellDnsSet = false;  // the operator's servers are the ones in use

void setDns(NetworkInterface& nif, const IPAddress* d) {
  for (uint8_t i = 0; i < 2; i++)
    if ((uint32_t)d[i] != 0) nif.dnsIP(i, d[i]);
}

void syncDns(bool wifi) {
  if (!up) {
    cellDnsSet = false;
    if (wifi) for (uint8_t i = 0; i < 2; i++) lanDns[i] = WiFi.STA.dnsIP(i);
    return;
  }
  if (!wifi && !cellDnsSet) {
    setDns(PPP, cellDns);
    cellDnsSet = true;
  } else if (wifi && cellDnsSet) {
    // Its DHCP rewrites them when the Wi-Fi comes back, but not with a static address.
    if (WiFi.STA.dnsIP(0) == cellDns[0]) setDns(WiFi.STA, lanDns);
    cellDnsSet = false;
  } else if (wifi) {
    for (uint8_t i = 0; i < 2; i++) lanDns[i] = WiFi.STA.dnsIP(i);  // follows DHCP renewals
  }
}

void onPppStatus(void*, esp_event_base_t, int32_t id, void*) {
  if ((id > NETIF_PPP_ERRORNONE && id < NETIF_PP_PHASE_OFFSET) || id == NETIF_PPP_CONNECT_FAILED) linkErr = true;
}

bool pinsOk(const Cfg& c) {
  return GPIO_IS_VALID_OUTPUT_GPIO(c.tx) && GPIO_IS_VALID_GPIO(c.rx) && c.tx != c.rx &&
         (c.pwr < 0 || (GPIO_IS_VALID_OUTPUT_GPIO(c.pwr) && c.pwr != c.tx && c.pwr != c.rx));
}

// Settings changed while an attempt is under way.
bool stale(const Cfg& c) { return restart || !(readCfg() == c); }

void stopModem() {
  if (!started) return;
  up = false;
  {
    std::lock_guard<std::mutex> l(stMtx);
    ip.clear();
  }
  // esp_modem's teardown does not leave CMUX / data mode: the modem would then ignore the plain
  // AT commands of the next start.
  PPP.mode(ESP_MODEM_MODE_COMMAND);
  PPP.end();
  started = false;
}

// PPP.begin() fails the same way for a mute modem and for a SIM PIN problem: ask the modem on
// the bare UART (PPP.begin() released it). "bad_pin": PIN missing or refused; "sim": SIM blocked
// (PUK), missing or faulty; "": no answer, or nothing wrong with the SIM.
std::string probeSim(const Cfg& c) {
  const uart_port_t u = UART_NUM_1;
  if (uart_driver_install(u, 256, 0, 0, nullptr, 0) != ESP_OK) return "";
  uart_config_t uc = {};
  uc.baud_rate = 115200;
  uc.data_bits = UART_DATA_8_BITS;
  uc.parity = UART_PARITY_DISABLE;
  uc.stop_bits = UART_STOP_BITS_1;
  uc.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
  uc.source_clk = UART_SCLK_DEFAULT;
  auto at = [u](const char* cmd) {
    uart_flush_input(u);
    uart_write_bytes(u, cmd, strlen(cmd));
    std::string r;
    char b[64];
    for (uint32_t t0 = millis(); millis() - t0 < 1500;) {
      int n = uart_read_bytes(u, b, sizeof(b), pdMS_TO_TICKS(100));
      if (n > 0) r.append(b, n);
      if (r.find("OK\r") != std::string::npos || r.find("ERROR") != std::string::npos) break;
    }
    std::transform(r.begin(), r.end(), r.begin(), [](char ch) { return (char)tolower((unsigned char)ch); });
    return r;
  };
  std::string res;
  if (uart_param_config(u, &uc) == ESP_OK &&
      uart_set_pin(u, c.tx, c.rx, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE) == ESP_OK &&
      at("AT\r").find("ok") != std::string::npos) {
    std::string r = at("AT+CPIN?\r");
    size_t e = r.find("error:");
    int cme = e == std::string::npos ? -1 : atoi(r.c_str() + e + 6);  // +CME ERROR: 10 / 13
    if (r.find("sim pin") != std::string::npos) res = "bad_pin";
    else if (r.find("puk") != std::string::npos || r.find("not inserted") != std::string::npos ||
             r.find("failure") != std::string::npos || cme == 10 || cme == 13)
      res = "sim";
  }
  uart_driver_delete(u);
  return res;
}

// Brings the modem up; returns when online or after a failure (state tells which). false: a
// settings problem, retrying would not help (and for the PIN, would end up locking the SIM).
bool startModem(const Cfg& c) {
  setState("starting");
  if (!pinsOk(c)) {
    setState("error", "pins");
    return false;
  }
  // Always set both: an empty APN gives the operator's default (PPP.begin() refuses a missing
  // one), and a value cleared in the settings must not survive in the PPP object.
  PPP.setApn(c.apn.c_str());
  if (!PPP.setPin(c.pin.empty() ? nullptr : c.pin.c_str())) {
    setState("error", "bad_pin");
    return false;
  }
  // PWRKEY of SIMCom modules: a low pulse of about 1 s switches the modem on
  if (c.pwr >= 0) PPP.setResetPin(c.pwr, true, 1000, 3000);
  if (!PPP.setPins(c.tx, c.rx)) {
    PPP.end();
    setState("error", "pins");
    return false;
  }
  if (!c.pin.empty() && pinFails() >= 2) {  // never risk the SIM's last try
    setState("error", "bad_pin");
    return false;
  }
  ppp_modem_model_t model = c.model == "sim7600" ? PPP_MODEM_SIM7600 : PPP_MODEM_GENERIC;
  bool begun = PPP.begin(model, 1, 115200);  // cleans up after itself on failure
  if (!begun && c.pwr >= 0) {
    // The Hub restarted while the modem was in data/CMUX mode: a PWRKEY pulse does not reset it.
    // Without the reset pin, begin() first tries to bring a modem in data mode back to commands.
    PPP.setResetPin(-1);
    PPP.setPins(c.tx, c.rx);
    begun = PPP.begin(model, 1, 115200);
  }
  if (!begun) {
    std::string why = probeSim(c);
    // A PIN-protected SIM also reads "SIM PIN" when begin() failed before sending the PIN (modem
    // still booting): allow one more attempt, never more (each wrong PIN costs one of 3 tries).
    if (why == "bad_pin" && !c.pin.empty()) {
      uint8_t n = pinFails() + 1;
      setPinFails(n);
      if (n < 2) why = "";  // maybe not sent at all (modem still booting): one more start
    }
    setState("error", why.empty() ? "no_modem" : why);
    return why.empty();
  }
  started = true;
  if (!pppEvents) pppEvents = esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, onPppStatus, nullptr) == ESP_OK;
  setState("searching");
  uint32_t t0 = millis();
  while (!PPP.attached()) {
    if (stale(c)) return true;
    if (millis() - t0 > ATTACH_TIMEOUT_MS) {
      setState("error", "no_network");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
  {
    std::string o = PPP.operatorName().c_str();
    int r = PPP.RSSI();
    std::lock_guard<std::mutex> l(stMtx);
    op = o;
    rssi = r >= 0 && r <= 31 ? r : 99;  // -1: AT+CSQ failed
  }
  setState("connecting");
  linkErr = false;
  setPinFails(0);  // the SIM is registered: the PIN, if any, was accepted
  // The Wi-Fi may have come up during the (long) start: note its DNS before PPP overwrites them.
  if (WiFi.status() == WL_CONNECTED)
    for (uint8_t i = 0; i < 2; i++)
      if (WiFi.STA.dnsIP(i) != IPAddress()) lanDns[i] = WiFi.STA.dnsIP(i);
  // CMUX keeps the AT channel next to the data one on the SIM7600; the A7670 family is known
  // to be unreliable with CMUX, plain data mode is used there.
  if (!PPP.mode(model == PPP_MODEM_SIM7600 ? ESP_MODEM_MODE_CMUX : ESP_MODEM_MODE_DATA)) {
    setState("error", "no_data");
    return true;
  }
  t0 = millis();
  while (!PPP.connected()) {
    if (stale(c)) return true;
    if (millis() - t0 > CONNECT_TIMEOUT_MS || linkErr) {
      setState("error", "no_data");
      return true;
    }
    vTaskDelay(pdMS_TO_TICKS(500));
  }
  // The PPP link has just written the operator's DNS servers over the box's.
  bool wifi = WiFi.status() == WL_CONNECTED;
  for (uint8_t i = 0; i < 2; i++) cellDns[i] = PPP.dnsIP(i);
  if (wifi) setDns(WiFi.STA, lanDns);
  cellDnsSet = !wifi;
  lastCheck = millis();
  checkFails = 0;
  {
    std::lock_guard<std::mutex> l(stMtx);
    ip = PPP.localIP().toString().c_str();
  }
  up = true;
  setState("online");
  return true;
}

// Without LCP echo lwIP never notices a mute peer (modem restarted, session dropped by the
// operator). In CMUX the AT channel is still there: check the registration now and then.
bool linkAlive(const Cfg& c, uint32_t now) {
  if (linkErr || !PPP.connected()) return false;
  if (c.model != "sim7600" || now - lastCheck < CHECK_MS) return true;
  lastCheck = now;
  if (!PPP.attached()) return ++checkFails < 3;
  checkFails = 0;
  int r = PPP.RSSI();
  if (r >= 0 && r <= 31) {
    std::lock_guard<std::mutex> l(stMtx);
    rssi = r;
  }
  return true;
}

void task(void*) {
  uint32_t wifiDownSince = 0, wifiUpSince = 0, retryAt = 0, backoff = 30000;
  bool hold = false;  // settings problem (pins, PIN, SIM): no new attempt before they change
  Cfg cur;
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(1000));
    Cfg c = readCfg();
    if (restart || !(c == cur)) {
      restart = false;
      stopModem();
      cur = c;
      retryAt = 0;
      backoff = 30000;
      hold = false;
    }
    uint32_t now = millis();
    bool wifi = WiFi.status() == WL_CONNECTED;
    if (wifi) {
      wifiDownSince = 0;
      if (!wifiUpSince) wifiUpSince = now ? now : 1;
    } else {
      wifiUpSince = 0;
      if (!wifiDownSince) wifiDownSince = now ? now : 1;
    }
    bool want = cur.enabled && (cur.always || (wifiDownSince && now - wifiDownSince > WIFI_GRACE_MS) ||
                                (started && !(wifiUpSince && now - wifiUpSince > WIFI_BACK_MS)));
    if (!cur.enabled) {
      setState("off");
      continue;
    }
    syncDns(wifi);
    if (hold) continue;  // the error stays on display
    if (!want) {
      if (started) {
        LOGI("4G: Wi-Fi is back, modem stopped");
        stopModem();
      }
      setState("waiting_wifi");
      continue;
    }
    if (started && up) {
      if (linkAlive(cur, now)) continue;
      LOGW("4G: link lost, restarting the modem");
      stopModem();
      retryAt = now + 5000;
    }
    if (retryAt && (int32_t)(now - retryAt) < 0) continue;
    hold = !startModem(cur);
    if (!up) {
      stopModem();
      retryAt = millis() + backoff;  // no SIM, no coverage...: don't hammer the modem
      backoff = backoff < 600000 ? backoff * 2 : 600000;
    } else {
      backoff = 30000;
    }
  }
}
#endif

// The task (6 kB of internal RAM) only exists once the modem has been enabled.
void startTask() {
#ifdef HUB_CELLULAR
  if (!readCfg().enabled || taskStarted.exchange(true)) return;
  if (xTaskCreatePinnedToCore(task, "cell", 6144, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE) != pdPASS) {
    LOGW("4G: cannot start the modem task");
    taskStarted = false;
  }
#endif
}

}  // namespace

void begin() { startTask(); }

void reconfigure() {
  restart = true;
  startTask();
}

bool online() { return up; }

bool supported() {
#ifdef HUB_CELLULAR
  return true;
#else
  return false;
#endif
}

void statusJson(JsonObject o) {
  o["supported"] = supported();
  o["online"] = (bool)up;
  std::lock_guard<std::mutex> l(stMtx);
  o["state"] = supported() ? state : std::string("unsupported");
  if (!err.empty()) o["error"] = err;
  if (!op.empty()) o["operator"] = op;
  if (rssi != 99) o["rssi"] = rssi;
  if (up && !ip.empty()) o["ip"] = ip;
}

}  // namespace Cellular
