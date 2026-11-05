#include "Network.h"
#include "Cellular.h"
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <WiFi.h>
#include <esp_mac.h>
#include <esp_sntp.h>
#include <map>
#include <vector>
#include "../App.h"
#include "../config/Config.h"
#include "../util/Log.h"
#include "WifiPick.h"

namespace Net {

static DNSServer dns;
static bool apOn = false, dnsOn = false, setup_ = false, mdnsOn = false, ntpOn = false;
static uint32_t staLostSince = 0, lastStaTry = 0;
static std::string apName;
static volatile bool scanning = false;    // a page asked for a scan and has not read it yet
static volatile bool scanWanted = false;  // asked, not started yet: loop() starts it
static uint32_t scanStart = 0;
static bool scanHeldReconnect = false;    // auto-reconnect is off for the length of a scan
static std::string testSsid;
static uint32_t testStart = 0;
static std::string testPass;  // credentials of the last test (a repeated test keeps the link)

// Several saved networks: scan, join the best one, and come back to a better one when it shows up.
static const uint32_t STA_RETRY_MS = 20000;     // a join that has not worked by then has failed
static const uint32_t BETTER_SCAN_MS = 300000;  // on a lower-priority network: look for a better one
static const uint32_t BAD_NET_MS = 300000;      // a network that failed is left aside this long
static const uint32_t SCAN_TIMEOUT_MS = 15000;
static const int BETTER_MIN_RSSI = -80;
static bool pickScan = false;                   // a scan of ours is running
static uint32_t pickStart = 0, lastBetter = 0, blindTurn = 0;
static std::string trying;                      // network being joined
static std::map<std::string, uint32_t> badNets; // ssid -> when it failed

static void ntpCb(struct timeval*) { gApp.onNtpSynced(); }

// Saved networks by priority, the default one first.
static std::vector<WifiNet> savedNets() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  std::vector<WifiNet> v;
  if (!gConfig.wifiSsid.empty()) v.push_back({gConfig.wifiSsid, gConfig.wifiPass});
  v.insert(v.end(), gConfig.wifiAlt.begin(), gConfig.wifiAlt.end());
  return v;
}

// What the last scan (n results) saw of the saved networks.
static std::vector<WifiCandidate> candidates(const std::vector<WifiNet>& nets, int n, uint32_t now) {
  std::vector<WifiCandidate> c(nets.size());
  for (size_t i = 0; i < nets.size(); i++) {
    for (int k = 0; k < n; k++)
      if (WiFi.SSID(k) == nets[i].ssid.c_str() && (!c[i].rssi || WiFi.RSSI(k) > c[i].rssi)) c[i].rssi = WiFi.RSSI(k);
    auto bad = badNets.find(nets[i].ssid);
    c[i].failed = bad != badNets.end() && now - bad->second < BAD_NET_MS;
  }
  return c;
}

static void joinNet(const WifiNet& n) {
  LOGI("Wi-Fi: joining \"%s\"", n.ssid.c_str());
  trying = n.ssid;
  WiFi.disconnect();
  WiFi.begin(n.ssid.c_str(), n.pass.c_str());
}

static void startPickScan(uint32_t now) {
  pickStart = now;
  pickScan = WiFi.scanNetworks(true, false, false, 300) == WIFI_SCAN_RUNNING;
}

// Result of a scan of ours: true once it is over, n = results (negative: failed).
static bool pickScanDone(uint32_t now, int& n) {
  n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING && now - pickStart < SCAN_TIMEOUT_MS) return false;
  pickScan = false;
  return true;
}

// Connected, with several saved networks: go back to a better one when it is in sight.
static void lookForBetter(uint32_t now) {
  if (pickScan) {
    int n;
    if (!pickScanDone(now, n) || n < 0) return;
    auto nets = savedNets();
    size_t cur = 0;
    while (cur < nets.size() && WiFi.SSID() != nets[cur].ssid.c_str()) cur++;
    int b = betterWifi(candidates(nets, n, now), cur, BETTER_MIN_RSSI);
    WiFi.scanDelete();
    if (b >= 0) joinNet(nets[b]);
    return;
  }
  if (now - lastBetter < BETTER_SCAN_MS) return;
  lastBetter = now;
  std::string def;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    def = gConfig.wifiSsid;
  }
  if (WiFi.SSID() != def.c_str()) startPickScan(now);
}

// Not connected, with several saved networks.
static void lookForNetwork(uint32_t now) {
  if (pickScan) {
    int n;
    if (!pickScanDone(now, n)) return;
    auto nets = savedNets();
    int i = pickWifi(candidates(nets, n > 0 ? n : 0, now), blindTurn++);
    if (n >= 0) WiFi.scanDelete();
    if (i >= 0) joinNet(nets[i]);
    return;
  }
  // Someone on the rescue access point: each scan disturbs it, so look less often.
  if (now - lastStaTry < (apOn && WiFi.softAPgetStationNum() ? 3 * STA_RETRY_MS : STA_RETRY_MS)) return;
  lastStaTry = now;
  if (!trying.empty()) {
    LOGW("Wi-Fi: \"%s\" not joined", trying.c_str());
    badNets[trying] = now;
    trying.clear();
  }
  WiFi.setAutoReconnect(false);  // it would keep retrying the same network, and block the scan
  WiFi.disconnect();
  startPickScan(now);
}

std::string apSsid() {
  if (apName.empty()) {
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);  // WiFi.macAddress() gives zeros until the Wi-Fi has started
    char b[32];
    snprintf(b, sizeof(b), "MeshCore-Hub-%02X%02X", mac[4], mac[5]);
    apName = b;
  }
  return apName;
}

static void startAp() {
  if (apOn) return;
  std::string pass;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    pass = gConfig.apPass;
  }
  WiFi.mode(WiFi.getMode() == WIFI_STA ? WIFI_AP_STA : (WiFi.getMode() == WIFI_AP_STA ? WIFI_AP_STA : WIFI_AP));
  WiFi.softAPConfig(IPAddress(NET_AP_IP), IPAddress(NET_AP_IP), IPAddress(255, 255, 255, 0));
  WiFi.softAP(apSsid().c_str(), pass.size() >= 8 ? pass.c_str() : nullptr);
  dns.setErrorReplyCode(DNSReplyCode::NoError);
  dnsOn = dns.start(53, "*", WiFi.softAPIP());
  apOn = true;
  LOGI("access point %s started (%s)", apSsid().c_str(), pass.size() >= 8 ? "password protected" : "open");
  Serial.printf("\n  >> Connect to Wi-Fi \"%s\" then open " NET_AP_URL "\n\n", apSsid().c_str());
}

static void stopAp() {
  if (!apOn) return;
  dns.stop();
  dnsOn = false;
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  apOn = false;
  LOGI("access point stopped");
}

static void startMdns() {
  if (mdnsOn) return;
  std::string host;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    host = gConfig.hostname;
  }
  if (MDNS.begin(host.c_str())) {
    MDNS.addService("http", "tcp", 80);
    mdnsOn = true;
    LOGI("mDNS: http://%s.local", host.c_str());
  }
}

static void startNtp() {
  if (ntpOn) return;
  std::string tz;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    tz = gConfig.tz;
  }
  sntp_set_time_sync_notification_cb(ntpCb);
  sntp_set_sync_interval(6UL * 3600UL * 1000UL);  // every 6 h
  configTzTime(tz.c_str(), "pool.ntp.org", "time.google.com", "time.cloudflare.com");
  ntpOn = true;
}

static void onEvent(WiFiEvent_t ev, WiFiEventInfo_t) {
  if (ev == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
    LOGI("Wi-Fi connected: %s (%s)", WiFi.SSID().c_str(), WiFi.localIP().toString().c_str());
    staLostSince = 0;
  } else if (ev == ARDUINO_EVENT_WIFI_STA_DISCONNECTED) {
    if (!staLostSince) staLostSince = millis();
  }
}

void begin(bool setupMode) {
  setup_ = setupMode;
  std::string ssid, pass, host, tz;
  bool apOnly, severalNets;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    ssid = gConfig.wifiSsid;
    pass = gConfig.wifiPass;
    severalNets = !gConfig.wifiAlt.empty();
    apOnly = gConfig.apOnly;
    host = gConfig.hostname;
    tz = gConfig.tz;
  }
  setenv("TZ", tz.c_str(), 1);
  tzset();
  WiFi.persistent(false);
  WiFi.onEvent(onEvent);
  WiFi.setHostname(host.c_str());
  WiFi.setSleep(true);  // modem sleep is required for Wi-Fi + BLE coexistence
  if (setupMode || apOnly || ssid.empty()) {
    WiFi.mode(ssid.empty() || apOnly ? WIFI_AP : WIFI_AP_STA);
    startAp();
    if (!ssid.empty() && !apOnly) WiFi.begin(ssid.c_str(), pass.c_str());
  } else {
    WiFi.mode(WIFI_STA);
    if (severalNets) {
      WiFi.setAutoReconnect(false);
      trying = ssid;  // not there: noted as failed by the first lookForNetwork()
    }
    WiFi.begin(ssid.c_str(), pass.c_str());
    staLostSince = millis();
    lastStaTry = millis();
  }
  startMdns();
}

// A join in progress makes the radio refuse the scan, and the list comes back empty just when the
// Hub is lost and someone is looking for a network: drop the join, the usual retries resume after.
static void runScan(uint32_t now) {
  scanStart = now;
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);  // scanning needs the station interface
  bool testing = !testSsid.empty() && now - testStart < 60000UL;
  if (WiFi.status() != WL_CONNECTED && !testing) {
    scanHeldReconnect = WiFi.getAutoReconnect();
    WiFi.setAutoReconnect(false);
    WiFi.disconnect();
    trying.clear();  // left on purpose: it did not fail
    lastStaTry = now;
  }
  WiFi.scanNetworks(true, false, false, 300);
  scanWanted = false;
}

void loop() {
  if (dnsOn) dns.processNextRequest();
  uint32_t now = millis();
  if (scanWanted) runScan(now);
  if (scanHeldReconnect && WiFi.scanComplete() != WIFI_SCAN_RUNNING) {
    scanHeldReconnect = false;
    WiFi.setAutoReconnect(true);
  }
  if (scanning && !scanWanted && now - scanStart > SCAN_TIMEOUT_MS) scanning = false;  // the page left
  bool apOnly, noSsid, severalNets;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);  // called every few ms: no string copies here
    apOnly = gConfig.apOnly;
    noSsid = gConfig.wifiSsid.empty();
    severalNets = !gConfig.wifiAlt.empty();
  }
  bool wizardBusy = setup_ || scanning || !(testSsid.empty() || now - testStart > 60000UL);
  if (Cellular::online()) startNtp();  // 4G only: the clock is needed for the TLS certificates
  if (WiFi.status() == WL_CONNECTED) {
    startNtp();
    // normal mode: the fallback AP goes away once the station is back for a while
    if (!setup_ && !apOnly && apOn && staLostSince == 0 && WiFi.softAPgetStationNum() == 0) stopAp();
    if (!trying.empty()) {
      badNets.erase(trying);
      trying.clear();
    }
    if (severalNets && !wizardBusy) lookForBetter(now);
    return;
  }
  if (apOnly || noSsid) return;
  // station lost: retry, and open a rescue access point after 3 minutes
  if (staLostSince && now - staLostSince > 180000UL && !apOn) {
    LOGW("Wi-Fi unreachable for 3 min: opening rescue access point");
    startAp();
  }
  if (severalNets && !setup_) {
    if (!wizardBusy) lookForNetwork(now);
    return;
  }
  // A wizard test pauses the retries until the page reads its result; a page cut off by the test
  // itself never comes back, so the pause ends after a minute.
  if (now - lastStaTry > 30000UL && (testSsid.empty() || now - testStart > 60000UL)) {
    lastStaTry = now;
    std::string ssid, pass;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      ssid = gConfig.wifiSsid;
      pass = gConfig.wifiPass;
    }
    WiFi.disconnect();
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
}

bool apActive() { return apOn; }
bool staConnected() { return WiFi.status() == WL_CONNECTED; }
bool online() { return staConnected() || Cellular::online(); }

bool isCaptiveHost(const String& host) {
  if (!apOn || host.isEmpty()) return false;
  if (host == WiFi.softAPIP().toString() || host == WiFi.localIP().toString()) return false;
  std::string h;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    h = gConfig.hostname;
  }
  if (host.equalsIgnoreCase((h + ".local").c_str()) || host.equalsIgnoreCase(h.c_str())) return false;
  return true;
}

void startScan() {
  if (scanning) return;
  scanning = true;
  scanWanted = true;
}

bool scanJson(JsonArray out) {
  if (scanWanted) return false;  // what is in memory is the previous scan
  int n = WiFi.scanComplete();
  if (n == WIFI_SCAN_RUNNING) return false;
  scanning = false;
  if (n < 0) return true;
  for (int i = 0; i < n; i++) {
    String s = WiFi.SSID(i);
    if (s.isEmpty()) continue;
    bool dup = false;
    for (JsonObject o : out)
      if (s == o["ssid"].as<const char*>()) dup = true;
    if (dup) continue;
    JsonObject o = out.add<JsonObject>();
    o["ssid"] = s;
    o["rssi"] = WiFi.RSSI(i);
    o["secure"] = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    o["ch"] = WiFi.channel(i);
  }
  return true;
}

void testConnect(const std::string& ssid, const std::string& pass) {
  testSsid = ssid;
  testStart = millis();
  std::string saved;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    saved = gConfig.wifiSsid == ssid ? gConfig.wifiPass : "";
  }
  // Already on this network with these credentials: joining again would cut the very
  // connection the wizard may be using (page opened on the home network address).
  if (WiFi.status() == WL_CONNECTED && ssid == WiFi.SSID().c_str() && (pass == testPass || pass == saved)) {
    LOGI("Wi-Fi \"%s\" already connected", ssid.c_str());
    return;
  }
  testPass = pass;
  if (WiFi.getMode() == WIFI_AP) WiFi.mode(WIFI_AP_STA);
  WiFi.disconnect();
  WiFi.begin(ssid.c_str(), pass.c_str());
  LOGI("testing Wi-Fi \"%s\"", ssid.c_str());
}

void statusJson(JsonObject o) {
  o["ap"] = apOn;
  o["apSsid"] = apSsid();
  o["apIp"] = WiFi.softAPIP().toString();
  o["connected"] = WiFi.status() == WL_CONNECTED;
  o["ssid"] = WiFi.SSID();
  o["ip"] = WiFi.localIP().toString();
  o["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  if (!testSsid.empty()) {
    o["testing"] = testSsid;
    wl_status_t st = WiFi.status();
    const char* r = st == WL_CONNECTED ? "ok" : st == WL_NO_SSID_AVAIL ? "no_ssid" : st == WL_CONNECT_FAILED ? "bad_password"
                  : millis() - testStart > 20000 ? "timeout" : "pending";
    o["test"] = r;
    if (strcmp(r, "pending") != 0) testSsid.clear();
  }
}

}  // namespace Net
