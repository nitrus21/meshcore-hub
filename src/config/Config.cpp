#include "Config.h"
#include "../setup/WifiPick.h"
#include <Preferences.h>
#include <driver/gpio.h>
#include <algorithm>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/aes.h>
#include <mbedtls/md5.h>
#include "../core/Route.h"
#include "../protocol/Frames.h"

HubConfig gConfig;
std::recursive_mutex gConfigMtx;

static const char* NS = "hub";
static const char* NS_SEC = "hubsec";

static std::string getStr(Preferences& p, const char* k, const std::string& def) {
  if (!p.isKey(k)) return def;
  return std::string(p.getString(k, def.c_str()).c_str());
}

// 4G modem pin: -1 (not used) or a GPIO of this chip, able to drive an output for TX / PWRKEY.
static bool cellPinOk(long v, bool output) {
  if (v == -1) return true;
  if (v < 0 || v >= 64 || !(output ? GPIO_IS_VALID_OUTPUT_GPIO(v) : GPIO_IS_VALID_GPIO(v))) return false;
  if (v == 0) return false;  // BOOT strapping pin
#if CONFIG_IDF_TARGET_ESP32S3
  if (v >= 26 && v <= 37) return false;  // SPI flash and octal PSRAM: driving them crashes the chip
  if (v == 19 || v == 20) return false;  // native USB (console, flashing)
#endif
#ifdef HUB_DISPLAY_TDISPLAY_S3
  // T-Display-S3: LCD bus (5-9, 38-48 minus the free 43/44), LCD power 15, KEY button 14
  if ((v >= 5 && v <= 9) || v == 14 || v == 15 || (v >= 38 && v <= 48 && v != 43 && v != 44)) return false;
#endif
  return true;
}

// SIM PIN: none, or 4 to 8 digits.
static bool simPinOk(const std::string& pin) {
  if (pin.empty()) return true;
  if (pin.size() < 4 || pin.size() > 8) return false;
  for (char c : pin)
    if (c < '0' || c > '9') return false;
  return true;
}

static std::string join(const std::vector<std::string>& v) {
  std::string s;
  for (auto& x : v) { if (!s.empty()) s += ','; s += x; }
  return s;
}

static std::vector<std::string> split(const std::string& s) {
  std::vector<std::string> out;
  size_t a = 0;
  while (a < s.size()) {
    size_t b = s.find(',', a);
    if (b == std::string::npos) b = s.size();
    if (b > a) out.push_back(s.substr(a, b - a));
    a = b + 1;
  }
  return out;
}

void HubConfig::load() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  Preferences p;
  p.begin(NS, true);
  setupDone = p.getBool("setup", false);
  lang = getStr(p, "lang", lang);
  theme = getStr(p, "theme", theme);
  accent = getStr(p, "accent", accent);
  tz = getStr(p, "tz", tz);
  wifiSsid = getStr(p, "ssid", "");
  wifiPass = getStr(p, "wpass", "");
  wifiAlt.clear();
  {
    JsonDocument alt;
    if (!deserializeJson(alt, getStr(p, "wifialt", "[]")))
      for (JsonArrayConst e : alt.as<JsonArrayConst>()) wifiAlt.push_back({e[0] | "", e[1] | ""});
  }
  apOnly = p.getBool("aponly", false);
  hostname = getStr(p, "host", hostname);
  apPass = getStr(p, "appass", "");
  cellEnabled = p.getBool("cellon", false);
  cellAlways = p.getBool("cellalw", false);
  cellApn = getStr(p, "cellapn", "");
  cellSimPin = getStr(p, "cellpin", "");
  cellPinFails = p.getUChar("cellpinf", 0);
  cellModel = getStr(p, "cellmod", cellModel);
  cellTx = p.getChar("celltx", cellTx);
  cellRx = p.getChar("cellrx", cellRx);
  cellPwr = p.getChar("cellpwr", cellPwr);
  // saved by a build for another board (the default used to be 43/44 everywhere)
  if (!cellPinOk(cellTx, true)) cellTx = -1;
  if (!cellPinOk(cellRx, false)) cellRx = -1;
  if (!cellPinOk(cellPwr, true)) cellPwr = -1;
  link = getStr(p, "link", "");
  bleAddr = getStr(p, "bleaddr", "");
  bleName = getStr(p, "blename", "");
  bleAddrType = p.getUChar("bletype", 0);
  blePin = p.getUInt("blepin", 123456);
  tcpHost = getStr(p, "tcphost", "");
  tcpPort = p.getUShort("tcpport", 5000);
  tgEnabled = p.getBool("tgon", false);
  tgToken = getStr(p, "tgtoken", "");
  tgChats.clear();
  for (auto& s : split(getStr(p, "tgchats", ""))) tgChats.push_back(strtoll(s.c_str(), nullptr, 10));
  tgMode = getStr(p, "tgmode", tgMode);
  tgRadioInfo = p.getBool("tgradio", true);
  tgRelayDm = p.getBool("tgdm", true);
  tgRelayChannels = p.getBool("tgch", true);
  tgRelayRooms = p.getBool("tgroom", true);
  tgRelayPublic = p.getBool("tgpublic", true);
  tgSkipOwn = p.getBool("tgskip", true);
  tgInsecure = p.getBool("tginsec", false);
  tgMutedConvs = split(getStr(p, "tgmuted", ""));
  tgRouteOn = p.getBool("tgrton", false);
  tgRouteKeep = p.getBool("tgrtkeep", false);
  tgRoutes.clear();
  {
    JsonDocument r;
    if (!deserializeJson(r, getStr(p, "tgroutes", "[]")))
      for (JsonArrayConst e : r.as<JsonArrayConst>()) tgRoutes.push_back({e[0] | "", e[1].as<int64_t>()});
  }
  tgRouteFrom = split(getStr(p, "tgrtfrom", ""));
  adminHash = getStr(p, "admhash", "");
  adminSalt = getStr(p, "admsalt", "");
  otaMd5 = getStr(p, "otamd5", "");
  historyDays = p.getUShort("histdays", 0);
  historyMaxPct = p.getUChar("histpct", 85);
  arduinoOta = p.getBool("aota", false);
  p.end();
}

void HubConfig::save() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  Preferences p;
  p.begin(NS, false);
  p.putBool("setup", setupDone);
  p.putString("lang", lang.c_str());
  p.putString("theme", theme.c_str());
  p.putString("accent", accent.c_str());
  p.putString("tz", tz.c_str());
  p.putString("ssid", wifiSsid.c_str());
  p.putString("wpass", wifiPass.c_str());
  {
    JsonDocument alt;
    JsonArray a = alt.to<JsonArray>();
    for (auto& n : wifiAlt) {
      JsonArray e = a.add<JsonArray>();
      e.add(n.ssid);
      e.add(n.pass);
    }
    std::string js;
    serializeJson(alt, js);
    p.putString("wifialt", js.c_str());
  }
  p.putBool("aponly", apOnly);
  p.putString("host", hostname.c_str());
  p.putString("appass", apPass.c_str());
  p.putBool("cellon", cellEnabled);
  p.putBool("cellalw", cellAlways);
  p.putString("cellapn", cellApn.c_str());
  p.putString("cellpin", cellSimPin.c_str());
  p.putUChar("cellpinf", cellPinFails);
  p.putString("cellmod", cellModel.c_str());
  p.putChar("celltx", cellTx);
  p.putChar("cellrx", cellRx);
  p.putChar("cellpwr", cellPwr);
  p.putString("link", link.c_str());
  p.putString("bleaddr", bleAddr.c_str());
  p.putString("blename", bleName.c_str());
  p.putUChar("bletype", bleAddrType);
  p.putUInt("blepin", blePin);
  p.putString("tcphost", tcpHost.c_str());
  p.putUShort("tcpport", tcpPort);
  p.putBool("tgon", tgEnabled);
  p.putString("tgtoken", tgToken.c_str());
  std::vector<std::string> chats;
  for (auto c : tgChats) chats.push_back(std::to_string(c));
  p.putString("tgchats", join(chats).c_str());
  p.putString("tgmode", tgMode.c_str());
  p.putBool("tgradio", tgRadioInfo);
  p.putBool("tgdm", tgRelayDm);
  p.putBool("tgch", tgRelayChannels);
  p.putBool("tgroom", tgRelayRooms);
  p.putBool("tgpublic", tgRelayPublic);
  p.putBool("tgskip", tgSkipOwn);
  p.putBool("tginsec", tgInsecure);
  p.putString("tgmuted", join(tgMutedConvs).c_str());
  p.putBool("tgrton", tgRouteOn);
  p.putBool("tgrtkeep", tgRouteKeep);
  {
    JsonDocument r;
    JsonArray a = r.to<JsonArray>();
    for (auto& e : tgRoutes) {
      JsonArray x = a.add<JsonArray>();
      x.add(e.name);
      x.add(e.chat);
    }
    std::string js;
    serializeJson(r, js);
    p.putString("tgroutes", js.c_str());
  }
  p.putString("tgrtfrom", join(tgRouteFrom).c_str());
  p.putString("admhash", adminHash.c_str());
  p.putString("admsalt", adminSalt.c_str());
  p.putString("otamd5", otaMd5.c_str());
  p.putUShort("histdays", historyDays);
  p.putUChar("histpct", historyMaxPct);
  p.putBool("aota", arduinoOta);
  p.end();
}

static std::string hashPw(const std::string& salt, const std::string& pw) {
  std::string in = salt + pw;
  uint8_t h[32];
  mc::sha256((const uint8_t*)in.data(), in.size(), h);
  return mc::toHex(h, 32);
}

bool HubConfig::checkPassword(const std::string& pw) const {
  if (adminHash.empty()) return true;
  std::string h = hashPw(adminSalt, pw);
  // constant-time compare
  uint8_t diff = h.size() != adminHash.size();
  for (size_t i = 0; i < h.size() && i < adminHash.size(); i++) diff |= h[i] ^ adminHash[i];
  return diff == 0;
}

void HubConfig::setPassword(const std::string& pw) {
  uint8_t salt[8];
  esp_fill_random(salt, sizeof(salt));
  adminSalt = mc::toHex(salt, sizeof(salt));
  adminHash = hashPw(adminSalt, pw);
  uint8_t md5[16];
  mbedtls_md5((const uint8_t*)pw.data(), pw.size(), md5);
  otaMd5 = mc::toHex(md5, 16);
}

bool HubConfig::tgMuted(const std::string& conv) const {
  for (auto& c : tgMutedConvs) if (c == conv) return true;
  return false;
}

void HubConfig::toJson(JsonObject o, bool secrets) const {
  o["setupDone"] = setupDone;
  o["lang"] = lang;
  o["theme"] = theme;
  o["accent"] = accent;
  o["tz"] = tz;
  o["wifiSsid"] = wifiSsid;
  o["wifiPass"] = secrets ? wifiPass : std::string(wifiPass.empty() ? "" : "********");
  JsonArray alt = o["wifiAlt"].to<JsonArray>();
  for (auto& n : wifiAlt) {
    JsonObject e = alt.add<JsonObject>();
    e["ssid"] = n.ssid;
    e["pass"] = secrets ? n.pass : std::string(n.pass.empty() ? "" : "********");
  }
  o["apOnly"] = apOnly;
  o["hostname"] = hostname;
  o["apPass"] = secrets ? apPass : std::string(apPass.empty() ? "" : "********");
  o["cellEnabled"] = cellEnabled;
  o["cellAlways"] = cellAlways;
  o["cellApn"] = cellApn;
  o["cellSimPin"] = secrets ? cellSimPin : std::string(cellSimPin.empty() ? "" : "********");
  o["cellModel"] = cellModel;
  o["cellTx"] = cellTx;
  o["cellRx"] = cellRx;
  o["cellPwr"] = cellPwr;
  o["link"] = link;
  o["bleAddr"] = bleAddr;
  o["bleName"] = bleName;
  o["bleAddrType"] = bleAddrType;
  o["blePin"] = secrets ? blePin : 0;
  o["tcpHost"] = tcpHost;
  o["tcpPort"] = tcpPort;
  o["tgEnabled"] = tgEnabled;
  if (secrets) o["tgToken"] = tgToken;
  else o["tgToken"] = tgToken.empty() ? "" : tgToken.substr(0, tgToken.find(':')) + ":********";
  JsonArray chats = o["tgChats"].to<JsonArray>();
  for (auto c : tgChats) chats.add(c);
  o["tgMode"] = tgMode;
  o["tgRadioInfo"] = tgRadioInfo;
  o["tgRelayDm"] = tgRelayDm;
  o["tgRelayChannels"] = tgRelayChannels;
  o["tgRelayRooms"] = tgRelayRooms;
  o["tgRelayPublic"] = tgRelayPublic;
  o["tgSkipOwn"] = tgSkipOwn;
  o["tgInsecure"] = tgInsecure;
  JsonArray muted = o["tgMutedConvs"].to<JsonArray>();
  for (auto& c : tgMutedConvs) muted.add(c);
  o["tgRouteOn"] = tgRouteOn;
  o["tgRouteKeep"] = tgRouteKeep;
  JsonArray routes = o["tgRoutes"].to<JsonArray>();
  for (auto& e : tgRoutes) {
    JsonObject x = routes.add<JsonObject>();
    x["name"] = e.name;
    x["chat"] = e.chat;
  }
  JsonArray rfrom = o["tgRouteFrom"].to<JsonArray>();
  for (auto& c : tgRouteFrom) rfrom.add(c);
  o["historyDays"] = historyDays;
  o["historyMaxPct"] = historyMaxPct;
  o["arduinoOta"] = arduinoOta;
  o["hasPassword"] = hasPassword();
  if (secrets) {
    o["adminHash"] = adminHash;
    o["adminSalt"] = adminSalt;
    o["otaMd5"] = otaMd5;
  }
}

template <typename T>
static bool take(JsonObjectConst o, const char* k, T& dst) {
  if (!o[k].is<T>()) return false;
  T v = o[k].as<T>();
  if (v == dst) return false;
  dst = v;
  return true;
}

static bool takeStr(JsonObjectConst o, const char* k, std::string& dst, bool isSecret = false) {
  if (!o[k].is<const char*>()) return false;
  std::string v = o[k].as<const char*>();
  if (isSecret && v.find("********") != std::string::npos) return false;  // masked value sent back
  if (v == dst) return false;
  dst = v;
  return true;
}

bool normalizeHostname(std::string& h) {
  std::string v = h;
  while (!v.empty() && isspace((unsigned char)v.back())) v.pop_back();
  size_t b = 0;
  while (b < v.size() && isspace((unsigned char)v[b])) b++;
  v = v.substr(b);
  for (auto& c : v) c = (char)tolower((unsigned char)c);
  if (v.size() > 6 && v.compare(v.size() - 6, 6, ".local") == 0) v.resize(v.size() - 6);
  if (v.empty() || v.size() > 63 || v.front() == '-' || v.back() == '-') return false;
  for (char c : v)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-')) return false;
  h = v;
  return true;
}

// Number in [lo, hi], rounded and clamped (an out-of-range value would not fit `is<T>()`).
template <typename T>
static bool takeClamped(JsonObjectConst o, const char* k, T& dst, long lo, long hi) {
  if (!o[k].is<double>()) return false;
  double d = o[k].as<double>();
  long v = d < lo ? lo : d > hi ? hi : lround(d);
  if ((T)v == dst) return false;
  dst = (T)v;
  return true;
}

// A pin that does not exist on this chip is refused (kept as it was).
// A pin that does not exist is refused (the previous value stays), never clamped to another one.
static bool takeCellPin(JsonObjectConst o, const char* k, int8_t& dst, bool output) {
  if (!o[k].is<double>()) return false;
  double d = o[k].as<double>();
  if (d != (long)d || d < -1 || d > 63) return false;
  int8_t v = (int8_t)d;
  if (v == dst || !cellPinOk(v, output)) return false;
  dst = v;
  return true;
}

bool HubConfig::fromJson(JsonObjectConst o) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  bool ch = false;
  ch |= take(o, "setupDone", setupDone);
  ch |= takeStr(o, "lang", lang);
  ch |= takeStr(o, "theme", theme);
  ch |= takeStr(o, "accent", accent);
  ch |= takeStr(o, "tz", tz);
  std::vector<WifiNet> savedNets = wifiAlt;
  savedNets.insert(savedNets.begin(), WifiNet{wifiSsid, wifiPass});
  ch |= takeStr(o, "wifiSsid", wifiSsid);
  if (o["wifiPass"].is<const char*>()) {
    std::string pass = Net::wifiPassFor(savedNets, wifiSsid, o["wifiPass"].as<const char*>());
    if (pass.size() <= 63 && pass != wifiPass) {
      wifiPass = pass;
      ch = true;
    }
  }
  if (o["wifiAlt"].is<JsonArrayConst>()) {
    std::vector<WifiNet> v;
    for (JsonObjectConst e : o["wifiAlt"].as<JsonArrayConst>()) {
      WifiNet n{e["ssid"] | "", e["pass"] | ""};
      bool dup = n.ssid == wifiSsid;
      for (auto& x : v) dup |= x.ssid == n.ssid;
      if (n.ssid.empty() || n.ssid.size() > 32 || n.pass.size() > 63 || dup || v.size() >= 3) continue;
      n.pass = Net::wifiPassFor(savedNets, n.ssid, n.pass);
      v.push_back(n);
    }
    if (!(v == wifiAlt)) {
      wifiAlt = v;
      ch = true;
    }
  }
  ch |= take(o, "apOnly", apOnly);
  if (o["hostname"].is<const char*>()) {
    std::string hn = o["hostname"].as<const char*>();
    if (normalizeHostname(hn) && hn != hostname) { hostname = hn; ch = true; }  // invalid: kept as it was
  }
  ch |= takeStr(o, "apPass", apPass, true);
  ch |= take(o, "cellEnabled", cellEnabled);
  ch |= take(o, "cellAlways", cellAlways);
  ch |= takeStr(o, "cellApn", cellApn);
  {
    std::string pin = cellSimPin;
    if (takeStr(o, "cellSimPin", pin, true) && simPinOk(pin)) {
      cellSimPin = pin;
      cellPinFails = 0;  // a new PIN value gets its own (limited) tries
      ch = true;
    }
  }
  if (o["cellModel"].is<const char*>()) {
    std::string m = o["cellModel"].as<const char*>();
    if ((m == "a7670" || m == "sim7600" || m == "generic") && m != cellModel) { cellModel = m; ch = true; }
  }
  ch |= takeCellPin(o, "cellTx", cellTx, true);
  ch |= takeCellPin(o, "cellRx", cellRx, false);
  ch |= takeCellPin(o, "cellPwr", cellPwr, true);
  ch |= takeStr(o, "link", link);
  ch |= takeStr(o, "bleAddr", bleAddr);
  ch |= takeStr(o, "bleName", bleName);
  ch |= take(o, "bleAddrType", bleAddrType);
  if (o["blePin"].is<uint32_t>() && o["blePin"].as<uint32_t>() != 0) ch |= take(o, "blePin", blePin);
  ch |= takeStr(o, "tcpHost", tcpHost);
  ch |= take(o, "tcpPort", tcpPort);
  ch |= take(o, "tgEnabled", tgEnabled);
  ch |= takeStr(o, "tgToken", tgToken, true);
  if (o["tgChats"].is<JsonArrayConst>()) {
    std::vector<int64_t> v;
    for (JsonVariantConst x : o["tgChats"].as<JsonArrayConst>()) v.push_back(x.as<int64_t>());
    if (v != tgChats) { tgChats = v; ch = true; }
  }
  ch |= takeStr(o, "tgMode", tgMode);
  ch |= take(o, "tgRadioInfo", tgRadioInfo);
  ch |= take(o, "tgRelayDm", tgRelayDm);
  ch |= take(o, "tgRelayChannels", tgRelayChannels);
  ch |= take(o, "tgRelayRooms", tgRelayRooms);
  ch |= take(o, "tgRelayPublic", tgRelayPublic);
  ch |= take(o, "tgSkipOwn", tgSkipOwn);
  ch |= take(o, "tgInsecure", tgInsecure);
  if (o["tgMutedConvs"].is<JsonArrayConst>()) {
    std::vector<std::string> v;
    for (JsonVariantConst x : o["tgMutedConvs"].as<JsonArrayConst>()) v.push_back(x.as<const char*>());
    if (v != tgMutedConvs) { tgMutedConvs = v; ch = true; }
  }
  ch |= take(o, "tgRouteOn", tgRouteOn);
  ch |= take(o, "tgRouteKeep", tgRouteKeep);
  if (o["tgRoutes"].is<JsonArrayConst>()) {
    std::vector<TgRoute> v;
    for (JsonObjectConst e : o["tgRoutes"].as<JsonArrayConst>()) {
      TgRoute r{route::fold(e["name"] | ""), e["chat"].as<int64_t>()};
      bool dup = false;
      for (auto& x : v) dup |= x.name == r.name;
      if (route::validName(r.name) && r.chat && !dup && v.size() < route::MAX_ENTRIES) v.push_back(r);
    }
    if (!(v == tgRoutes)) { tgRoutes = v; ch = true; }
  }
  if (o["tgRouteFrom"].is<JsonArrayConst>()) {
    std::vector<std::string> v;
    for (JsonVariantConst x : o["tgRouteFrom"].as<JsonArrayConst>()) {
      std::string c = x | "";
      if (c.rfind("c_", 0) == 0 && c.size() <= 80 && c.find(',') == std::string::npos &&
          std::find(v.begin(), v.end(), c) == v.end() && v.size() < route::MAX_SENDERS) v.push_back(c);
    }
    if (v != tgRouteFrom) { tgRouteFrom = v; ch = true; }
  }
  ch |= takeClamped(o, "historyDays", historyDays, 0, 65535);
  ch |= takeClamped(o, "historyMaxPct", historyMaxPct, 30, 95);
  ch |= take(o, "arduinoOta", arduinoOta);
  // restore from a backup made with secrets
  ch |= takeStr(o, "adminHash", adminHash);
  ch |= takeStr(o, "adminSalt", adminSalt);
  ch |= takeStr(o, "otaMd5", otaMd5);
  if (historyMaxPct < 30) historyMaxPct = 30;
  if (historyMaxPct > 95) historyMaxPct = 95;
  if (hostname.empty()) hostname = "meshcore-hub";
  return ch;
}

// ---------------------------------------------------------------- room passwords

namespace RoomPasswords {

static void deviceKey(uint8_t key[16]) {
  uint8_t mac[6];
  esp_efuse_mac_get_default(mac);
  uint8_t buf[6 + 12];
  memcpy(buf, mac, 6);
  memcpy(buf + 6, "meshcore-hub", 12);
  uint8_t h[32];
  mc::sha256(buf, sizeof(buf), h);
  memcpy(key, h, 16);
}

// AES-128-CTR; output = nonce(8) || ciphertext, hex encoded
static std::string encrypt(const std::string& plain) {
  uint8_t key[16], nonce[16] = {0}, stream[16];
  deviceKey(key);
  esp_fill_random(nonce, 8);
  std::string out = mc::toHex(nonce, 8);
  std::vector<uint8_t> c(plain.size());
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  size_t off = 0;
  mbedtls_aes_crypt_ctr(&ctx, plain.size(), &off, nonce, stream, (const uint8_t*)plain.data(), c.data());
  mbedtls_aes_free(&ctx);
  return out + mc::toHex(c.data(), c.size());
}

static std::string decrypt(const std::string& hex) {
  mc::Bytes b = mc::fromHexVar(hex);
  if (b.size() < 8) return "";
  uint8_t key[16], nonce[16] = {0}, stream[16];
  deviceKey(key);
  memcpy(nonce, b.data(), 8);
  std::string out(b.size() - 8, '\0');
  mbedtls_aes_context ctx;
  mbedtls_aes_init(&ctx);
  mbedtls_aes_setkey_enc(&ctx, key, 128);
  size_t off = 0;
  mbedtls_aes_crypt_ctr(&ctx, out.size(), &off, nonce, stream, b.data() + 8, (uint8_t*)&out[0]);
  mbedtls_aes_free(&ctx);
  return out;
}

static JsonDocument loadAll() {
  JsonDocument d;
  Preferences p;
  p.begin(NS_SEC, true);
  String s = p.getString("rooms", "{}");
  p.end();
  if (deserializeJson(d, s) || !d.is<JsonObject>()) d.to<JsonObject>();
  return d;
}

static void saveAll(JsonDocument& d) {
  String s;
  serializeJson(d, s);
  Preferences p;
  p.begin(NS_SEC, false);
  p.putString("rooms", s);
  p.end();
}

void set(const std::string& k, const std::string& pw) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  JsonDocument d = loadAll();
  d[k] = encrypt(pw);
  saveAll(d);
}

std::string get(const std::string& k) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  JsonDocument d = loadAll();
  if (!d[k].is<const char*>()) return "";
  return decrypt(d[k].as<const char*>());
}

void remove(const std::string& k) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  JsonDocument d = loadAll();
  d.remove(k);
  saveAll(d);
}

std::vector<std::string> keys() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  JsonDocument d = loadAll();
  std::vector<std::string> out;
  for (JsonPair kv : d.as<JsonObject>()) out.push_back(kv.key().c_str());
  return out;
}

void clear() {
  Preferences p;
  p.begin(NS_SEC, false);
  p.clear();
  p.end();
}

}  // namespace RoomPasswords

void factoryResetConfig() {
  Preferences p;
  p.begin(NS, false);
  p.clear();
  p.end();
  RoomPasswords::clear();
  p.begin("hubsess", false);
  p.clear();
  p.end();
}
