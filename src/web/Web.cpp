#include "Web.h"
#include <AsyncJson.h>
#include <ESPAsyncWebServer.h>
#include <LittleFS.h>
#include <Preferences.h>
#include <Update.h>
#include <esp_random.h>
#include <sys/time.h>
#include <memory>
#include "../App.h"
#include "../config/Config.h"
#include "../setup/Cellular.h"
#include "../setup/Network.h"
#include "../telegram/TelegramBridge.h"
#include "../util/Log.h"

using namespace mc;
bool arduinoOtaNeedsRestart(const std::string& md5);  // main.cpp

namespace Web {

static AsyncWebServer server(80);
static AsyncWebSocket ws("/ws");
static uint32_t rebootAt = 0;
static volatile uint32_t lastRequest = 0;

uint32_t lastRequestMs() { return lastRequest; }
static bool restorePending = false;
static File restoreFile;          // /restore.tmp while a backup is being uploaded
static bool restoreBad = false;   // the upload could not be stored completely

// ---------------------------------------------------------------- sessions

static const size_t MAX_SESSIONS = 6;
static std::vector<std::string> sessions;
static std::mutex sessMtx;

static void loadSessions() {
  Preferences p;
  p.begin("hubsess", true);
  String s = p.getString("t", "");
  p.end();
  sessions.clear();
  int a = 0;
  while (a < (int)s.length()) {
    int b = s.indexOf(',', a);
    if (b < 0) b = s.length();
    if (b - a == 32) sessions.push_back(s.substring(a, b).c_str());
    a = b + 1;
  }
}

static void saveSessions() {
  std::string s;
  for (auto& t : sessions) { if (!s.empty()) s += ','; s += t; }
  Preferences p;
  p.begin("hubsess", false);
  p.putString("t", s.c_str());
  p.end();
}

static std::string newSession() {
  uint8_t r[16];
  esp_fill_random(r, sizeof(r));
  std::string t = toHex(r, sizeof(r));
  std::lock_guard<std::mutex> l(sessMtx);
  sessions.push_back(t);
  while (sessions.size() > MAX_SESSIONS) sessions.erase(sessions.begin());
  saveSessions();
  return t;
}

static std::string cookieToken(AsyncWebServerRequest* r) {
  if (!r->hasHeader("Cookie")) return "";
  String c = r->header("Cookie");
  int i = c.indexOf("mch=");
  if (i < 0) return "";
  String t = c.substring(i + 4, i + 4 + 32);
  return t.c_str();
}

static bool authed(AsyncWebServerRequest* r) {
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (!gConfig.setupDone || !gConfig.hasPassword()) return true;  // first-time wizard
  }
  std::string t = cookieToken(r);
  if (t.size() != 32) return false;
  std::lock_guard<std::mutex> l(sessMtx);
  for (auto& s : sessions) if (s == t) return true;
  return false;
}

// ---------------------------------------------------------------- helpers

static void sendJson(AsyncWebServerRequest* r, JsonDocument& d, int code = 200) {
  AsyncResponseStream* res = r->beginResponseStream("application/json");
  res->setCode(code);
  res->addHeader("Cache-Control", "no-store");
  serializeJson(d, *res);
  r->send(res);
}

static void sendOk(AsyncWebServerRequest* r, uint32_t rid = 0) {
  JsonDocument d;
  d["ok"] = true;
  if (rid) d["rid"] = rid;
  sendJson(r, d);
}

static void sendErr(AsyncWebServerRequest* r, const char* err, int code = 400) {
  JsonDocument d;
  d["ok"] = false;
  d["err"] = err;
  sendJson(r, d, code);
}

static bool guard(AsyncWebServerRequest* r) {
  if (authed(r)) return true;
  sendErr(r, "auth", 401);
  return false;
}

void broadcast(JsonDocument& d) {
  if (!ws.count()) return;
  String s;
  serializeJson(d, s);
  ws.textAll(s);
}

// Contact from "key" (64 hex) or "conv" (c_ + 12 hex prefix)
static bool contactFrom(JsonVariantConst j, Contact& c) {
  std::string key = j["key"] | "";
  std::string conv = j["conv"] | "";
  uint8_t buf[PUB_KEY_SIZE];
  if (key.size() == 64 && fromHex(key, buf, 32)) return gApp.mesh().findContact(buf, 32, c);
  if (conv.size() == 14 && fromHex(conv.substr(2), buf, PREFIX_SIZE)) return gApp.mesh().findContact(buf, PREFIX_SIZE, c);
  return false;
}

static void mkdirs(const std::string& path) {
  for (size_t i = 1; i < path.size(); i++) {
    if (path[i] == '/') {
      std::string d = path.substr(0, i);
      if (!LittleFS.exists(d.c_str())) LittleFS.mkdir(d.c_str());
    }
  }
}

static void rmTree(const std::string& path) {
  File dir = LittleFS.open(path.c_str());
  if (!dir) return;
  if (!dir.isDirectory()) {
    dir.close();
    LittleFS.remove(path.c_str());
    return;
  }
  std::vector<std::pair<std::string, bool>> items;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    items.push_back({path + "/" + f.name(), f.isDirectory()});
    f.close();
  }
  dir.close();
  for (auto& it : items) {
    if (it.second) rmTree(it.first);
    else LittleFS.remove(it.first.c_str());
  }
  LittleFS.rmdir(path.c_str());
}

// ---------------------------------------------------------------- /api/do

// "Change module" trial (ble_pair / tcp_set): the link that was configured before, kept until
// the new module gets ready, so that link_restore can bring it back. Guarded by gConfigMtx.
struct LinkSnapshot {
  bool valid = false;
  std::string link, bleAddr, bleName, tcpHost;
  uint8_t bleAddrType = 0;
  uint32_t blePin = 0;
  uint16_t tcpPort = 0;
};
static LinkSnapshot prevLink;
static volatile bool linkTrial = false;

static void snapshotLink() {
  if (prevLink.valid || gConfig.link.empty()) return;  // keep the original through repeated attempts
  prevLink.valid = true;
  prevLink.link = gConfig.link;
  prevLink.bleAddr = gConfig.bleAddr;
  prevLink.bleName = gConfig.bleName;
  prevLink.bleAddrType = gConfig.bleAddrType;
  prevLink.blePin = gConfig.blePin;
  prevLink.tcpHost = gConfig.tcpHost;
  prevLink.tcpPort = gConfig.tcpPort;
}

// All the 4G settings in one string, to tell whether they changed.
static std::string cellSettings() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  char b[40];
  snprintf(b, sizeof(b), "%d%d|%d|%d|%d|", gConfig.cellEnabled, gConfig.cellAlways, gConfig.cellTx, gConfig.cellRx,
           gConfig.cellPwr);
  return b + gConfig.cellApn + '|' + gConfig.cellSimPin + '|' + gConfig.cellModel;
}

// Returns an error key, or nullptr once applied.
static const char* applySettings(JsonObjectConst o, JsonDocument& res) {
  if (o["hostname"].is<const char*>()) {
    std::string hn = o["hostname"].as<const char*>();
    if (!normalizeHostname(hn)) return "bad_hostname";
  }
  std::string oldTz, oldLink, oldAddr, oldHost, oldSsid, oldPass, oldHostname, oldToken;
  bool oldApOnly, oldTg, oldIns;
  uint32_t oldPin;
  uint16_t oldPort;
  std::string oldMode;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    oldTz = gConfig.tz; oldLink = gConfig.link; oldAddr = gConfig.bleAddr; oldHost = gConfig.tcpHost;
    oldPort = gConfig.tcpPort; oldPin = gConfig.blePin; oldSsid = gConfig.wifiSsid; oldPass = gConfig.wifiPass;
    oldApOnly = gConfig.apOnly; oldHostname = gConfig.hostname; oldToken = gConfig.tgToken; oldTg = gConfig.tgEnabled;
    oldIns = gConfig.tgInsecure; oldMode = gConfig.tgMode;
  }
  if (o["password"].is<const char*>() && strlen(o["password"]) >= 4) {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    gConfig.setPassword(o["password"].as<const char*>());
  }
  std::string oldCell = cellSettings();
  gConfig.fromJson(o);
  gConfig.save();
  bool linkChanged, tgChanged, needReboot;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (gConfig.tz != oldTz) {
      setenv("TZ", gConfig.tz.c_str(), 1);
      tzset();
    }
    linkChanged = gConfig.link != oldLink || gConfig.bleAddr != oldAddr || gConfig.tcpHost != oldHost ||
                  gConfig.tcpPort != oldPort || gConfig.blePin != oldPin;
    tgChanged = gConfig.tgToken != oldToken || gConfig.tgEnabled != oldTg || gConfig.tgInsecure != oldIns ||
                gConfig.tgMode != oldMode;
    needReboot = gConfig.setupDone && (gConfig.wifiSsid != oldSsid || gConfig.wifiPass != oldPass ||
                                       gConfig.apOnly != oldApOnly || gConfig.hostname != oldHostname ||
                                       (gConfig.arduinoOta && arduinoOtaNeedsRestart(gConfig.otaMd5)));  // see main.cpp
  }
  // Outside gConfigMtx: the mesh task takes its own lock, then gConfigMtx (lock order mesh -> config).
  if (linkChanged) gApp.applyLinkConfig();
  if (tgChanged) gTelegram.reconfigure();
  if (cellSettings() != oldCell) Cellular::reconfigure();
  res["reboot"] = needReboot;
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  gConfig.toJson(res["settings"].to<JsonObject>(), false);  // what was really kept (clamped, normalized)
  return nullptr;
}

static void doOp(AsyncWebServerRequest* r, JsonVariant& body) {
  if (!guard(r)) return;
  JsonObjectConst j = body.as<JsonObjectConst>();
  std::string op = j["op"] | "";
  MeshClient& mesh = gApp.mesh();
  std::string conv = j["conv"] | "";
  Contact c;

  if (op == "send" || op == "cli") {
    SendResult s = gApp.sendText(conv, j["text"] | "", "web", op == "cli" ? TXT_CLI_DATA : TXT_PLAIN);
    if (!s.ok) return sendErr(r, s.err.c_str());
    JsonDocument d;
    d["ok"] = true;
    d["id"] = s.id;
    return sendJson(r, d);
  }
  if (op == "resend") {
    SendResult s = gApp.resend(conv, j["id"] | 0u);
    return s.ok ? sendOk(r) : sendErr(r, s.err.c_str());
  }
  if (op == "read") {
    gHistory.markRead(conv);
    return sendOk(r);
  }
  if (op == "flags") {
    gHistory.setFlags(conv, j["pinned"].is<bool>() ? (int)j["pinned"].as<bool>() : -1,
                      j["muted"].is<bool>() ? (int)j["muted"].as<bool>() : -1,
                      j["archived"].is<bool>() ? (int)j["archived"].as<bool>() : -1);
    return sendOk(r);
  }
  if (op == "delete_conv") {
    gHistory.removeConversation(conv);
    return sendOk(r);
  }

  uint32_t rid = gApp.newRid();
  // ---- contacts
  if (op == "contact_remove" || op == "contact_reset_path" || op == "contact_share" || op == "contact_fav") {
    if (!contactFrom(j, c)) return sendErr(r, "unknown_contact");
    if (op == "contact_remove") mesh.command(enc::removeContact(c.pubKey), rid);
    else if (op == "contact_reset_path") mesh.command(enc::resetPath(c.pubKey), rid);
    else if (op == "contact_share") mesh.command(enc::shareContact(c.pubKey), rid);
    else mesh.setFavorite(c.pubKey, j["fav"] | false, rid);  // refreshes the list itself
    if (op == "contact_remove" || op == "contact_reset_path") mesh.refreshContacts();
    return sendOk(r, rid);
  }
  if (op == "contact_export") {
    bool self = !(j["key"].is<const char*>() || j["conv"].is<const char*>());
    if (!self && !contactFrom(j, c)) return sendErr(r, "unknown_contact");
    mesh.command(enc::exportContact(self ? nullptr : c.pubKey), rid);
    return sendOk(r, rid);
  }
  if (op == "contact_import") {
    std::string hex = j["hex"] | "";
    if (hex.rfind("meshcore://", 0) == 0) hex = hex.substr(11);
    Bytes pkt = fromHexVar(hex);
    if (pkt.size() < 32 + 64) return sendErr(r, "bad_contact");
    mesh.command(enc::importContact(pkt), rid);
    mesh.refreshContacts();
    return sendOk(r, rid);
  }
  if (op == "contact_add") {  // from a meshcore://contact/add?name=..&public_key=..&type=.. link
    Contact n;
    if (!fromHex(j["key"] | "", n.pubKey, 32)) return sendErr(r, "bad_contact");
    strncpy(n.name, j["name"] | "?", 31);
    n.type = j["type"] | 1;
    n.outPathLen = OUT_PATH_UNKNOWN;
    n.lastmod = time(nullptr);
    mesh.command(enc::addUpdateContact(n), rid);
    mesh.refreshContacts();
    return sendOk(r, rid);
  }
  // ---- channels
  if (op == "channel_set") {
    int idx = j["idx"].is<int>() ? j["idx"].as<int>() : -1;
    bool hashtag = j["hashtag"] | false;
    uint8_t secret[16];
    if (!hashtag && !fromHex(j["secret"] | "", secret, 16)) return sendErr(r, "bad_secret");
    std::string err;
    if (gApp.setChannel(idx, j["name"] | "", hashtag ? nullptr : secret, rid, err) < 0) return sendErr(r, err.c_str());
    return sendOk(r, rid);
  }
  if (op == "channel_delete") {
    std::string err;
    if (!gApp.deleteChannel((uint8_t)(j["idx"] | 0), rid, err)) return sendErr(r, err.c_str());
    return sendOk(r, rid);
  }
  // ---- rooms / repeaters
  if (op == "login") {
    if (!gApp.loginRoom(conv, j["password"] | "", j["remember"] | true, rid)) return sendErr(r, "unknown_contact");
    return sendOk(r, rid);
  }
  if (op == "logout" || op == "status_req" || op == "telemetry_req" || op == "path_disc" || op == "trace") {
    if (!contactFrom(j, c)) return sendErr(r, "unknown_contact");
    if (op == "logout") {
      RoomPasswords::remove(conv.substr(2));
      mesh.command(enc::logout(c.pubKey), rid);
    } else if (op == "status_req") {
      mesh.request(enc::statusReq(c.pubKey), rid);
    } else if (op == "telemetry_req") {
      mesh.request(enc::telemetryReq(c.pubKey), rid);
    } else if (op == "path_disc") {
      mesh.request(enc::pathDiscoveryReq(c.pubKey), rid);
    } else {
      // round trip: out path, the target, then the out path reversed
      if (c.outPathLen == OUT_PATH_UNKNOWN) return sendErr(r, "no_path");
      if (!c.pathCached()) return sendErr(r, "path_too_long");
      int hs = pathHashSize(c.outPathLen), hops = pathHops(c.outPathLen);
      if (hs == 3) return sendErr(r, "unsupported");
      Bytes path(c.outPath, c.outPath + hops * hs);
      path.insert(path.end(), c.pubKey, c.pubKey + hs);
      for (int h = hops - 1; h >= 0; h--) path.insert(path.end(), c.outPath + h * hs, c.outPath + (h + 1) * hs);
      uint32_t tag = esp_random();
      mesh.command(enc::tracePath(tag, 0, hs == 1 ? 0 : 1, path), rid);
      JsonDocument d;
      d["ok"] = true;
      d["rid"] = rid;
      d["tag"] = tag;
      return sendJson(r, d);
    }
    return sendOk(r, rid);
  }
  if (op == "forget_password") {
    RoomPasswords::remove(conv.substr(2));
    return sendOk(r);
  }
  // ---- node
  if (op == "advert") { mesh.command(enc::selfAdvert(j["flood"] | true), rid); return sendOk(r, rid); }
  if (op == "stats") { mesh.command(enc::getStats(j["type"] | 0), rid); return sendOk(r, rid); }
  if (op == "node_name") {
    std::string n = j["name"] | "";
    if (n.empty()) return sendErr(r, "bad_name");
    mesh.command(enc::setAdvertName(n), rid);
    mesh.refreshSelf();
    return sendOk(r, rid);
  }
  if (op == "node_pos") {
    mesh.command(enc::setAdvertLatLon((int32_t)lround((j["lat"] | 0.0) * 1e6), (int32_t)lround((j["lon"] | 0.0) * 1e6)), rid);
    mesh.refreshSelf();
    return sendOk(r, rid);
  }
  if (op == "node_radio") {
    uint32_t f = (uint32_t)lround((j["freq"] | 0.0) * 1000.0);
    uint32_t bw = (uint32_t)lround((j["bw"] | 0.0) * 1000.0);
    mesh.command(enc::setRadioParams(f, bw, j["sf"] | 0, j["cr"] | 0, j["repeat"] | false), rid);
    if (j["pathHashSize"].is<int>()) mesh.command(enc::setPathHashMode((uint8_t)(j["pathHashSize"].as<int>() - 1)));
    mesh.refreshSelf();
    return sendOk(r, rid);
  }
  if (op == "node_tx") { mesh.command(enc::setTxPower((int8_t)(j["dbm"] | 20)), rid); mesh.refreshSelf(); return sendOk(r, rid); }
  if (op == "node_pin") {
    uint32_t pin = j["pin"] | 0u;
    mesh.command(enc::setDevicePin(pin), rid);
    if (pin) {  // keep our own pairing PIN in sync
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      gConfig.blePin = pin;
      gConfig.save();
    }
    mesh.refreshSelf();
    return sendOk(r, rid);
  }
  if (op == "node_other") {
    mesh.command(enc::setOtherParams(j["manualAdd"] | 0, j["telemetryModes"] | 0, j["advertLocPolicy"] | 0, j["multiAcks"] | 0), rid);
    mesh.refreshSelf();
    return sendOk(r, rid);
  }
  if (op == "autoadd_get") { mesh.command(enc::getAutoAddConfig(), rid); return sendOk(r, rid); }
  if (op == "autoadd_set") { mesh.command(enc::setAutoAddConfig(j["cfg"] | 0, j["maxHops"] | 0), rid); return sendOk(r, rid); }
  if (op == "node_reboot") { mesh.command(enc::reboot(), rid); return sendOk(r, rid); }
  if (op == "refresh") { mesh.refreshContacts(); mesh.refreshChannels(); mesh.refreshSelf(); mesh.syncMessages(); return sendOk(r); }
  // ---- link
  if (op == "module_release") { mesh.release(j["minutes"] | 0u); return sendOk(r); }
  if (op == "module_reconnect") { mesh.reconnect(); return sendOk(r); }
  if (op == "ble_scan") { gApp.requestBleScan(); return sendOk(r); }
  if (op == "ble_pair") {
    if (!(j["addr"] | "")[0]) return sendErr(r, "bad_addr");  // an empty address would forget every bond
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      snapshotLink();
      gConfig.link = "ble";
      gConfig.bleAddr = j["addr"] | "";
      gConfig.bleAddrType = j["type"] | 0;
      gConfig.bleName = j["name"] | "";
      uint32_t pin = j["pin"] | 0u;
      if (pin) gConfig.blePin = pin;
      gConfig.save();
    }
    gApp.bleForget(j["addr"] | "", j["type"] | 0);  // fresh pairing with the PIN given now
    gApp.applyLinkConfig();
    linkTrial = true;  // after applyLinkConfig: the old link may still read as ready before it
    mesh.reconnect();
    return sendOk(r);
  }
  if (op == "ble_forget") {
    std::string addr;
    uint8_t type;
    bool wasBle;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      wasBle = gConfig.link == "ble";
      addr = gConfig.bleAddr;
      type = gConfig.bleAddrType;
      if (wasBle) {
        gConfig.bleAddr.clear();
        gConfig.bleName.clear();
      } else {
        gConfig.tcpHost.clear();
      }
      gConfig.link = "";
      gConfig.save();
      linkTrial = false;
      prevLink.valid = false;
    }
    gApp.applyLinkConfig();
    // Only the bond of this module; never forgetAll (empty address), nor start BLE for a TCP link.
    if (wasBle && !addr.empty()) gApp.bleForget(addr, type);
    return sendOk(r);
  }
  if (op == "tcp_set") {
    std::string host = j["host"] | "";
    if (host.empty()) return sendErr(r, "bad_host");
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      snapshotLink();
      gConfig.link = "tcp";
      gConfig.tcpHost = host;
      gConfig.tcpPort = j["port"] | 5000;
      gConfig.save();
    }
    gApp.applyLinkConfig();
    linkTrial = true;
    mesh.reconnect();
    return sendOk(r);
  }
  // "Change module" cancelled before the new one ever got ready: put the previous one back.
  if (op == "link_restore") {
    bool restored = false;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      if (linkTrial && prevLink.valid) {
        gConfig.link = prevLink.link;
        gConfig.bleAddr = prevLink.bleAddr;
        gConfig.bleName = prevLink.bleName;
        gConfig.bleAddrType = prevLink.bleAddrType;
        gConfig.blePin = prevLink.blePin;
        gConfig.tcpHost = prevLink.tcpHost;
        gConfig.tcpPort = prevLink.tcpPort;
        gConfig.save();
        restored = true;
      }
      linkTrial = false;
      prevLink.valid = false;
    }
    if (restored) {
      gApp.applyLinkConfig();
      mesh.reconnect();
    }
    JsonDocument d;
    d["ok"] = true;
    d["restored"] = restored;
    return sendJson(r, d);
  }
  // ---- hub
  if (op == "wifi_scan") { Net::startScan(); return sendOk(r); }
  if (op == "wifi_test") { Net::testConnect(j["ssid"] | "", j["pass"] | ""); return sendOk(r); }
  if (op == "settings") {
    JsonDocument d;
    d["ok"] = true;
    if (const char* err = applySettings(j["settings"].as<JsonObjectConst>(), d)) return sendErr(r, err);
    return sendJson(r, d);
  }
  if (op == "password") {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (!gConfig.checkPassword(j["old"] | "")) return sendErr(r, "bad_password");
    std::string pw = j["new"] | "";
    if (pw.size() < 4) return sendErr(r, "too_short");
    gConfig.setPassword(pw);
    gConfig.save();
    std::lock_guard<std::mutex> sl(sessMtx);
    std::string mine = cookieToken(r);
    sessions.clear();
    if (mine.size() == 32) sessions.push_back(mine);
    saveSessions();
    JsonDocument d;
    d["ok"] = true;
    d["reboot"] = gConfig.arduinoOta && arduinoOtaNeedsRestart(gConfig.otaMd5);  // Arduino 2 only, see main.cpp
    return sendJson(r, d);
  }
  if (op == "tg_test") {
    std::string tok = j["token"] | "";
    if ((j["useSaved"] | false) || tok.find("****") != std::string::npos) {  // masked value from /api/settings
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      tok = gConfig.tgToken;
    }
    gTelegram.testToken(tok);
    return sendOk(r);
  }
  if (op == "tg_claim") {
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      if (j["reset"] | false) {
        gConfig.tgChats.clear();
        gConfig.save();
      }
    }
    gTelegram.startClaim();
    return sendOk(r);
  }
  if (op == "time") {
    uint32_t t = j["unix"] | 0u;
    if (!gApp.timeValid() && t > 1700000000) {
      struct timeval tv = {(time_t)t, 0};
      settimeofday(&tv, nullptr);
      LOGI("clock set from the browser");
      mesh.setTimeNow();
    }
    return sendOk(r);
  }
  if (op == "history_wipe") { gHistory.wipe(); return sendOk(r); }
  if (op == "setup_finish") {
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      if (!gConfig.hasPassword()) return sendErr(r, "no_password");
      gConfig.setupDone = true;
      gConfig.save();
    }
    rebootAt = millis() + 2500;
    return sendOk(r);
  }
  if (op == "reboot") { rebootAt = millis() + 800; return sendOk(r); }
  if (op == "factory_reset") {
    factoryResetConfig();
    gApp.bleForget("", 0);
    gHistory.wipe();
    LittleFS.remove("/tg.json");
    rebootAt = millis() + 1000;
    return sendOk(r);
  }
  sendErr(r, "unknown_op");
}

// ---------------------------------------------------------------- backup (chunked)

struct BackupState {
  int phase = 0;
  bool secrets = false;
  std::vector<std::string> files;
  size_t fileIdx = 0;
  File cur;
  std::string conv, month;
  std::string pending;
  size_t pendingOff = 0;
  bool first = true;
};

static size_t backupFill(std::shared_ptr<BackupState> st, uint8_t* buf, size_t max) {
  size_t out = 0;
  while (out < max) {
    if (st->pendingOff < st->pending.size()) {
      size_t n = std::min(max - out, st->pending.size() - st->pendingOff);
      memcpy(buf + out, st->pending.data() + st->pendingOff, n);
      out += n;
      st->pendingOff += n;
      continue;
    }
    st->pending.clear();
    st->pendingOff = 0;
    if (st->phase == 0) {
      JsonDocument s;
      gConfig.toJson(s.to<JsonObject>(), st->secrets);
      std::string js;
      serializeJson(s, js);
      st->pending = "{\"meshcorehub_backup\":1,\n\"settings\":" + js + ",\n\"messages\":[\n";
      st->files = gHistory.files();
      st->phase = 1;
    } else if (st->phase == 1) {
      if (!st->cur) {
        if (st->fileIdx >= st->files.size()) {
          st->pending = "{}]}\n";
          st->phase = 2;
          continue;
        }
        const std::string& f = st->files[st->fileIdx++];  // /h/<conv>/<month>.jl
        size_t a = f.find('/', 1), b = f.rfind('/');
        st->conv = f.substr(a + 1, b - a - 1);
        st->month = f.substr(b + 1, 6);
        st->cur = LittleFS.open(f.c_str(), "r");
        continue;
      }
      if (!st->cur.available()) {
        st->cur.close();
        continue;
      }
      String line = st->cur.readStringUntil('\n');
      if (line.length() < 5) continue;
      st->pending = "{\"c\":\"" + st->conv + "\",\"m\":\"" + st->month + "\",\"r\":" + line.c_str() + "},\n";
    } else {
      break;
    }
  }
  return out;
}

// ---------------------------------------------------------------- web app pack upload

struct PackState {
  int stage = 0;  // 0 magic, 1 path, 2 size, 3 body
  std::string acc;
  std::string path;
  size_t left = 0;
  File f;
  bool error = false;
  int files = 0;
};
static PackState pack;

static void packFeed(const uint8_t* d, size_t n) {
  for (size_t i = 0; i < n && !pack.error; i++) {
    uint8_t c = d[i];
    if (pack.stage == 3) {
      size_t k = std::min(pack.left, n - i);
      pack.f.write(d + i, k);
      pack.left -= k;
      i += k - 1;
      if (!pack.left) {
        pack.f.close();
        pack.files++;
        pack.stage = 1;
      }
      continue;
    }
    if (c != '\n') {
      pack.acc += (char)c;
      if (pack.acc.size() > 200) pack.error = true;
      continue;
    }
    if (pack.stage == 0) {
      if (pack.acc != "MCHPACK1") pack.error = true;
      pack.stage = 1;
    } else if (pack.stage == 1) {
      if (pack.acc.empty() || pack.acc.find("..") != std::string::npos) pack.error = true;
      pack.path = "/www.new/" + pack.acc;
      pack.stage = 2;
    } else if (pack.stage == 2) {
      pack.left = strtoul(pack.acc.c_str(), nullptr, 10);
      mkdirs(pack.path);
      pack.f = LittleFS.open(pack.path.c_str(), "w");
      if (!pack.f) pack.error = true;
      pack.stage = pack.left ? 3 : 1;
      if (!pack.left) pack.f.close();
    }
    pack.acc.clear();
  }
}

// ---------------------------------------------------------------- restore

static void processRestore() {
  File f = LittleFS.open("/restore.tmp", "r");
  if (!f) return;
  LOGI("restoring backup...");
  int n = 0;
  while (f.available()) {
    String line = f.readStringUntil('\n');
    if (line.startsWith("\"settings\":")) {
      String js = line.substring(11);
      if (js.endsWith(",")) js.remove(js.length() - 1);
      JsonDocument d;
      if (!deserializeJson(d, js)) {
        gConfig.fromJson(d.as<JsonObjectConst>());
        gConfig.save();
      }
    } else if (line.startsWith("{\"c\":\"")) {
      if (line.endsWith(",")) line.remove(line.length() - 1);
      // {"c":"conv","m":"YYYYMM","r":{...}}
      int q = line.indexOf('"', 6);
      int m = line.indexOf("\"m\":\"", q);
      int r = line.indexOf("\"r\":", m);
      if (q < 0 || m < 0 || r < 0) continue;
      std::string conv = line.substring(6, q).c_str();
      std::string month = line.substring(m + 5, m + 11).c_str();
      std::string rec = line.substring(r + 4, line.length() - 1).c_str();
      if (gHistory.importLine(conv, month, rec)) n++;
    }
  }
  f.close();
  LittleFS.remove("/restore.tmp");
  gHistory.rebuildIndexFromFiles();
  LOGI("restore done: %d records", n);
}

// ---------------------------------------------------------------- setup

void begin() {
  loadSessions();

  // Every request: mark the app as active (Telegram then frees its TLS memory) and refuse
  // politely instead of crashing when RAM is nearly exhausted.
  server.addMiddleware([](AsyncWebServerRequest* r, ArMiddlewareNext next) {
    lastRequest = millis() ? millis() : 1;
    if (ESP.getMaxAllocHeap() < 6 * 1024) {
      r->send(503, "text/plain", "busy");
      return;
    }
    next();
  });

  ws.handleHandshake([](AsyncWebServerRequest* r) { return authed(r); });
  ws.onEvent([](AsyncWebSocket*, AsyncWebSocketClient* c, AwsEventType t, void*, uint8_t*, size_t) {
    if (t == WS_EVT_CONNECT) {
      JsonDocument d;
      d["t"] = "hello";
      gApp.statusJson(d["status"].to<JsonObject>());
      String s;
      serializeJson(d, s);
      c->text(s);
    }
  });
  server.addHandler(&ws);

  server.on("/api/status", HTTP_GET, [](AsyncWebServerRequest* r) {
    JsonDocument d;
    bool ok = authed(r);
    d["auth"] = ok;
    if (ok) {
      gApp.statusJson(d.to<JsonObject>());
      d["auth"] = true;
    } else {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      d["setupDone"] = gConfig.setupDone;
      d["lang"] = gConfig.lang;
      d["theme"] = gConfig.theme;
      d["accent"] = gConfig.accent;
      d["version"] = HUB_VERSION;
    }
    sendJson(r, d);
  });

  server.addHandler(new AsyncCallbackJsonWebHandler("/api/login", [](AsyncWebServerRequest* r, JsonVariant& j) {
    static uint32_t lastFail = 0;
    if (millis() - lastFail < 1500) return sendErr(r, "slow_down", 429);  // brute force brake
    bool ok;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      ok = gConfig.checkPassword(j["password"] | "");
    }
    if (!ok) {
      lastFail = millis();
      LOGW("web login failed from %s", r->client()->remoteIP().toString().c_str());
      return sendErr(r, "bad_password", 401);
    }
    std::string t = newSession();
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", ("mch=" + t + "; Path=/; Max-Age=2592000; HttpOnly; SameSite=Strict").c_str());
    r->send(res);
  }));

  server.on("/api/logout", HTTP_POST, [](AsyncWebServerRequest* r) {
    std::string t = cookieToken(r);
    {
      std::lock_guard<std::mutex> l(sessMtx);
      sessions.erase(std::remove(sessions.begin(), sessions.end(), t), sessions.end());
      saveSessions();
    }
    AsyncWebServerResponse* res = r->beginResponse(200, "application/json", "{\"ok\":true}");
    res->addHeader("Set-Cookie", "mch=; Path=/; Max-Age=0");
    r->send(res);
  });

  auto* doH = new AsyncCallbackJsonWebHandler("/api/do", doOp);
  doH->setMaxContentLength(8192);
  server.addHandler(doH);

  // Streamed one contact at a time: a full JSON document for 350 contacts would not fit in RAM.
  server.on("/api/contacts", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    struct St { size_t idx = 0; std::string pending; size_t off = 0; bool done = false; };
    auto st = std::make_shared<St>();
    st->pending = "[";
    AsyncWebServerResponse* res = r->beginChunkedResponse("application/json", [st](uint8_t* buf, size_t max, size_t) -> size_t {
      size_t out = 0;
      while (out < max) {
        if (st->off < st->pending.size()) {
          size_t n = std::min(max - out, st->pending.size() - st->off);
          memcpy(buf + out, st->pending.data() + st->off, n);
          out += n;
          st->off += n;
          continue;
        }
        if (st->done) break;
        st->pending.clear();
        st->off = 0;
        Contact c;
        if (!gApp.mesh().contactAt(st->idx, c)) {
          st->pending = "]";
          st->done = true;
          continue;
        }
        JsonDocument d;
        App::contactJson(c, d.to<JsonObject>());
        std::string one;
        serializeJson(d, one);  // serializeJson() clears its target: never write into pending directly
        st->pending = st->idx++ ? "," + one : one;
      }
      return out;
    });
    res->addHeader("Cache-Control", "no-store");
    r->send(res);
  });
  server.on("/api/channels", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    gApp.channelsJson(d["channels"].to<JsonArray>());
    d["max"] = gApp.mesh().deviceInfo().maxChannels;
    sendJson(r, d);
  });
  server.on("/api/conversations", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    gHistory.listConversations(d.to<JsonArray>());
    sendJson(r, d);
  });
  server.on("/api/messages", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    if (!r->hasParam("conv")) return sendErr(r, "conv");
    std::string conv = r->getParam("conv")->value().c_str();
    uint32_t before = r->hasParam("before") ? strtoul(r->getParam("before")->value().c_str(), nullptr, 10) : 0;
    int limit = r->hasParam("limit") ? r->getParam("limit")->value().toInt() : 40;
    JsonDocument d;
    gHistory.read(conv, before, limit, d["messages"].to<JsonArray>());
    d["max"] = gApp.maxTextFor(conv);
    sendJson(r, d);
  });
  server.on("/api/settings", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    gConfig.toJson(d["settings"].to<JsonObject>(), false);
    JsonArray rooms = d["savedRooms"].to<JsonArray>();
    for (auto& k : RoomPasswords::keys()) rooms.add("c_" + k);
    Net::statusJson(d["net"].to<JsonObject>());
    sendJson(r, d);
  });
  server.on("/api/node", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    gApp.selfJson(d.to<JsonObject>());
    sendJson(r, d);
  });
  server.on("/api/wifi", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    d["done"] = Net::scanJson(d["networks"].to<JsonArray>());
    Net::statusJson(d["net"].to<JsonObject>());
    sendJson(r, d);
  });
  server.on("/api/ble", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    d["scanning"] = gApp.bleScanBusy();
    d["age"] = gApp.lastScanMs ? (millis() - gApp.lastScanMs) / 1000 : -1;
    d["heard"] = gApp.scanHeard;
    JsonDocument list;
    deserializeJson(list, gApp.scanJson);
    d["devices"] = list.as<JsonArray>();
    sendJson(r, d);
  });
  server.on("/api/logs", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    JsonDocument d;
    JsonArray a = d["lines"].to<JsonArray>();
    for (auto& l : Log::recent()) a.add(l);
    sendJson(r, d);
  });
  server.on("/api/backup", HTTP_GET, [](AsyncWebServerRequest* r) {
    if (!guard(r)) return;
    auto st = std::make_shared<BackupState>();
    st->secrets = r->hasParam("secrets") && r->getParam("secrets")->value() == "1";
    AsyncWebServerResponse* res = r->beginChunkedResponse("application/json", [st](uint8_t* buf, size_t max, size_t) {
      return backupFill(st, buf, max);
    });
    res->addHeader("Content-Disposition", "attachment; filename=\"meshcore-hub-backup.json\"");
    r->send(res);
  });

  // firmware OTA (.bin)
  server.on(
      "/api/ota", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        if (!guard(r)) return;
        bool ok = !Update.hasError() && Update.isFinished();
        if (ok) rebootAt = millis() + 1500;
        JsonDocument d;
        d["ok"] = ok;
        if (!ok) d["err"] = Update.errorString();
        sendJson(r, d, ok ? 200 : 500);
      },
      [](AsyncWebServerRequest* r, String, size_t index, uint8_t* data, size_t len, bool final) {
        if (!authed(r)) return;
        if (!index) {
          LOGI("OTA: receiving firmware");
          Update.begin(UPDATE_SIZE_UNKNOWN, U_FLASH);
        }
        if (Update.isRunning() && Update.write(data, len) != len) Update.printError(Serial);
        if (final && Update.isRunning()) {
          if (Update.end(true)) LOGI("OTA: firmware written (%u bytes), rebooting", (unsigned)(index + len));
          else LOGE("OTA failed: %s", Update.errorString());
        }
      });

  // web app update (webapp.pack) without touching the history
  server.on(
      "/api/webapp", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        if (!guard(r)) return;
        if (pack.error || pack.stage != 1 || !pack.files) {
          rmTree("/www.new");
          return sendErr(r, "bad_pack", 500);
        }
        rmTree("/www");
        LittleFS.rename("/www.new", "/www");
        LOGI("web app updated (%d files)", pack.files);
        sendOk(r);
      },
      [](AsyncWebServerRequest* r, String, size_t index, uint8_t* data, size_t len, bool) {
        if (!authed(r)) return;
        if (!index) {
          rmTree("/www.new");
          pack = PackState();
        }
        packFeed(data, len);
      });

  server.on(
      "/api/restore", HTTP_POST,
      [](AsyncWebServerRequest* r) {
        if (!guard(r)) return;
        // Still open: the upload never reached its final chunk.
        bool ok = !restoreBad && !restoreFile;
        if (restoreFile) restoreFile.close();
        if (ok) {
          File t = LittleFS.open("/restore.tmp", "r");
          ok = t && t.size() > 0 && t.peek() == '{';
          if (t) t.close();
        }
        if (!ok) {
          LittleFS.remove("/restore.tmp");
          return sendErr(r, "bad_backup");
        }
        restorePending = true;
        sendOk(r);
      },
      [](AsyncWebServerRequest* r, String, size_t index, uint8_t* data, size_t len, bool final) {
        if (!authed(r)) return;
        if (!index) {
          if (restoreFile) restoreFile.close();
          restoreFile = LittleFS.open("/restore.tmp", "w");
          restoreBad = !restoreFile;
        }
        if (restoreFile && restoreFile.write(data, len) != len) restoreBad = true;
        if (final && restoreFile) restoreFile.close();
      });

  // no-cache: after a web app update the browser must not keep an old index.html
  server.serveStatic("/", LittleFS, "/www/").setDefaultFile("index.html").setCacheControl("no-cache");

  server.onNotFound([](AsyncWebServerRequest* r) {
    if (Net::isCaptiveHost(r->host())) {  // captive portal: send every foreign host to the wizard
      r->redirect(NET_AP_URL "/");
      return;
    }
    if (r->url().startsWith("/api/")) return sendErr(r, "not_found", 404);
    if (LittleFS.exists("/www/index.html.gz") || LittleFS.exists("/www/index.html")) {
      r->send(LittleFS, "/www/index.html", "text/html");  // single page app routes
    } else {
      r->send(200, "text/html",
              "<h1>MeshCore Hub</h1><p>Web app missing: run <code>pio run -t uploadfs</code> "
              "or upload <code>webapp.pack</code>.</p>");
    }
  });

  DefaultHeaders::Instance().addHeader("X-Content-Type-Options", "nosniff");
  server.begin();
  LOGI("web server started");
}

void loop() {
  static uint32_t lastClean = 0;
  if (millis() - lastClean > 1000) {
    lastClean = millis();
    ws.cleanupClients(4);
  }
  if (linkTrial && gApp.mesh().ready()) {  // the new module works: nothing to restore any more
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    linkTrial = false;
    prevLink.valid = false;
  }
  if (restorePending) {
    restorePending = false;
    processRestore();
    rebootAt = millis() + 1000;
  }
  if (rebootAt && (int32_t)(millis() - rebootAt) >= 0) {
    gHistory.flush();
    LOGI("rebooting");
    delay(200);
    ESP.restart();
  }
}

}  // namespace Web
