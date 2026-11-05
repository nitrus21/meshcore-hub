#include "App.h"
#include <WiFi.h>
#include <esp_sntp.h>
#include <sys/time.h>
#include <algorithm>
#include "config/Config.h"
#include "setup/Cellular.h"
#include "store/FileContactStore.h"
#include "telegram/TelegramBridge.h"
#include "telegram/TgStrings.h"
#include "transport/BleTransport.h"
#include "transport/TcpTransport.h"
#include "util/Log.h"

App gApp;

static BleTransport gBle;
static TcpTransport gTcp;
static bool gBleInit = false;
static FileContactStore gContactStore;

using namespace mc;

static bool ensureBle() {
  if (!gBleInit) gBleInit = gBle.begin("MeshCore-Hub");
  return gBleInit;
}

// ", PSRAM n kB free" on boards that have some (where the large buffers should go)
static std::string psramNote() {
  if (!psramFound()) return "";
  char b[40];
  snprintf(b, sizeof(b), ", PSRAM %u kB free", (unsigned)(ESP.getFreePsram() / 1024));
  return b;
}

static uint8_t kindForType(uint8_t t) {
  switch (t) {
    case ADV_REPEATER: return 2;
    case ADV_ROOM: return 3;
    case ADV_SENSOR: return 4;
    default: return 0;
  }
}

// ---------------------------------------------------------------- setup

void App::begin() {
  if (gContactStore.begin()) mesh_.setContactStore(&gContactStore);  // RAM fallback if LittleFS fails
  mesh_.setListener(this);
  mesh_.setAppName("MeshCoreHub");
  applyLinkConfig();
}

void App::applyLinkConfig() {
  std::string link, addr, host;
  uint8_t type;
  uint32_t pin;
  uint16_t port;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    link = gConfig.link;
    addr = gConfig.bleAddr;
    type = gConfig.bleAddrType;
    pin = gConfig.blePin;
    host = gConfig.tcpHost;
    port = gConfig.tcpPort;
  }
  if (link == "ble" && !addr.empty() && ensureBle()) {
    gBle.configure(addr, type, pin);
    mesh_.setTransport(&gBle);
  } else if (link == "tcp" && !host.empty()) {
    gTcp.configure(host, port);
    mesh_.setTransport(&gTcp);
  } else {
    mesh_.setTransport(nullptr);
  }
  LOGI("module link: %s", link.empty() ? "none" : link.c_str());
}

void App::requestBleScan() { scanRequested_ = true; }

void App::bleForget(const std::string& addr, uint8_t type) {
  if (!ensureBle()) return;
  if (addr.empty()) gBle.forgetAll();
  else gBle.forget(addr, type);
}

void App::meshLoop() {
  if (scanRequested_) {
    scanRequested_ = false;
    scanning_ = true;
    ensureBle();
    JsonDocument ev;
    ev["t"] = "ble_scan";
    ev["state"] = "scanning";
    emit(ev);
    // A configured BLE link must not hold the radio (and NimBLE drops adverts from a connected peer).
    bool paused = mesh_.transport() == &gBle && mesh_.state() != LinkState::Released && mesh_.state() != LinkState::Disabled;
    if (paused) mesh_.release(0);
    std::vector<BleTransport::Found> found;
    if (ensureBle()) found = gBle.scan(10000);
    if (paused) mesh_.reconnect();
    JsonDocument d;
    JsonArray a = d.to<JsonArray>();
    size_t nMesh = 0;
    for (auto& f : found) {
      JsonObject o = a.add<JsonObject>();
      o["addr"] = f.addr;
      o["type"] = f.addrType;
      o["name"] = f.name;
      o["rssi"] = f.rssi;
      o["mc"] = f.meshcore;
      if (f.meshcore) nMesh++;
    }
    scanHeard = gBleInit ? gBle.lastHeard() : -1;
    scanJson.clear();
    serializeJson(d, scanJson);
    lastScanMs = millis();
    scanning_ = false;
    JsonDocument done;
    done["t"] = "ble_scan";
    done["state"] = "done";
    done["devices"] = d.as<JsonArray>();
    done["heard"] = scanHeard;
    emit(done);
    LOGI("BLE scan: %u MeshCore module(s)", (unsigned)nMesh);
  }
  mesh_.loop();
  runPendingForgets();
  runPendingReplies();
  housekeeping();
}

void App::housekeeping() {
  uint32_t now = millis();
  if (now - lastHousekeep_ < 1000) return;
  lastHousekeep_ = now;
  gHistory.flush();
  if (now - lastMaint_ > 600000UL || lastMaint_ == 0) {
    lastMaint_ = now;
    gHistory.maintenance();
  }
  // Telegram: report a module that stays down (avoid noise on short reconnects)
  static uint32_t downSince = 0;
  static bool downNotified = false;
  if (mesh_.ready()) {
    if (downNotified) {
      int lg = tgs::langIndex(gConfig.lang.c_str());
      char b[160];
      snprintf(b, sizeof(b), tgs::T[tgs::MODULE_UP][lg], mesh_.selfInfo().name.c_str());
      gTelegram.notify(b);
    }
    downSince = 0;
    downNotified = false;
  } else if (mesh_.transport() && mesh_.state() != LinkState::Released) {
    if (!downSince) downSince = now;
    if (!downNotified && now - downSince > 120000UL && wasReady_) {
      downNotified = true;
      gTelegram.notify(tgs::T[tgs::MODULE_DOWN][tgs::langIndex(gConfig.lang.c_str())]);
    }
  }
  // BLE recovery: a stack that keeps failing to connect for 30 min is re-initialised
  static uint32_t bleFailSince = 0;
  if (mesh_.transport() == &gBle && !mesh_.ready() && mesh_.state() != LinkState::Released &&
      mesh_.lastError() == LinkError::ConnectFailed) {
    if (!bleFailSince) bleFailSince = now;
    if (now - bleFailSince > 1800000UL) {
      bleFailSince = now;
      gBle.resetStack();
    }
  } else {
    bleFailSince = 0;
  }
  static uint32_t lastHeapLog = 0;
  if (now - lastHeapLog > 120000UL) {
    lastHeapLog = now;
    LOGI("heap %u kB free, largest block %u kB, min %u kB%s", (unsigned)(ESP.getFreeHeap() / 1024),
         (unsigned)(ESP.getMaxAllocHeap() / 1024), (unsigned)(ESP.getMinFreeHeap() / 1024), psramNote().c_str());
  }
  // Periodic status for the web app (heap, Wi-Fi, ...)
  static uint32_t lastStatus = 0;
  if (now - lastStatus > 15000) {
    lastStatus = now;
    JsonDocument d;
    d["t"] = "status";
    statusJson(d["status"].to<JsonObject>());
    emit(d);
  }
}

void App::emit(JsonDocument& d) {
  if (sink_) sink_(d);
}

// ---------------------------------------------------------------- time

bool App::timeValid() const { return time(nullptr) > 1700000000; }

void App::onNtpSynced() {
  LOGI("time synchronised (NTP)");
  mesh_.setTimeNow();
}

void App::onModuleTime(uint32_t t) {
  if (timeValid() || t < 1700000000) return;
  struct timeval tv = {(time_t)t, 0};
  settimeofday(&tv, nullptr);
  LOGI("clock set from the module (no NTP)");
}

// ---------------------------------------------------------------- conversations

std::string App::convForKey(const uint8_t* pub) { return "c_" + toHex(pub, PREFIX_SIZE); }
std::string App::convForChannel(uint8_t idx) { return "ch_" + std::to_string(idx); }

bool App::resolveConv(const std::string& conv, ConvRef& out) {
  out.id = conv;
  if (conv.rfind("ch_", 0) == 0) {
    ChannelInfo ch;
    if (!mesh_.findChannel((uint8_t)atoi(conv.c_str() + 3), ch)) return false;
    out.name = ch.name;
    out.kind = 1;
    return true;
  }
  if (conv.rfind("c_", 0) == 0 && conv.size() == 14) {
    uint8_t p[PREFIX_SIZE];
    Contact c;
    if (!fromHex(conv.substr(2), p, PREFIX_SIZE) || !mesh_.findContact(p, PREFIX_SIZE, c)) {
      ConvMeta m;  // unknown contact but known history
      if (!gHistory.meta(conv, m)) return false;
      out.name = m.name;
      out.kind = m.kind;
      return true;
    }
    out.name = c.name;
    out.kind = kindForType(c.type);
    return true;
  }
  return false;
}

static std::string lower(std::string s) {
  for (auto& c : s) c = tolower((unsigned char)c);
  return s;
}

bool App::findConvByName(const std::string& query, bool channels, ConvRef& out, bool& ambiguous) {
  ambiguous = false;
  std::string q = lower(query);
  while (!q.empty() && q.back() == ' ') q.pop_back();
  if (channels) {
    ChannelInfo ch;
    if (!mesh_.findChannelByName(q, ch)) return false;
    out = {convForChannel(ch.idx), ch.name, 1};
    return true;
  }
  Contact c;
  if (!mesh_.findContactByName(q, c)) return false;
  out = {convForKey(c.pubKey), c.name, kindForType(c.type)};
  return true;
}

bool App::splitTarget(const std::string& args, bool channels, ConvRef& out, std::string& text, bool& ambiguous,
                      const std::vector<std::string>* only) {
  // Names may contain spaces: pick the longest name that prefixes the arguments.
  ambiguous = false;
  std::string la = lower(args);
  size_t best = 0;
  auto ends = [&](size_t i) { return i == la.size() || la[i] == ' ' || la[i] == '\n'; };
  if (channels) {
    for (auto& ch : mesh_.channels()) {
      std::string n = lower(ch.name), n2 = n[0] == '#' ? n.substr(1) : "#" + n;
      for (auto& cand : {n, n2}) {
        if (cand.size() > best && la.compare(0, cand.size(), cand) == 0 && ends(cand.size())) {
          best = cand.size();
          out = {convForChannel(ch.idx), ch.name, 1};
        }
      }
    }
  } else {
    mesh_.forEachContact([&](const Contact& c) {
      std::string id = convForKey(c.pubKey), n = lower(c.name);
      if (only && std::find(only->begin(), only->end(), id) == only->end()) return;
      if (n.size() > best && la.compare(0, n.size(), n) == 0 && ends(n.size())) {
        best = n.size();
        out = {id, c.name, kindForType(c.type)};
      }
    });
  }
  if (!best) {  // fall back to a unique partial match on the first word
    if (only) return false;  // that match looks at every contact
    size_t sp = args.find_first_of(" \n");
    if (!findConvByName(args.substr(0, sp), channels, out, ambiguous)) return false;
    text = sp == std::string::npos ? "" : args.substr(sp + 1);
    return true;
  }
  text = args.size() > best ? args.substr(best + 1) : "";
  return true;
}

std::vector<ConvRef> App::recentConversations(size_t n, bool includeChannels) {
  JsonDocument d;
  JsonArray a = d.to<JsonArray>();
  gHistory.listConversations(a);
  std::vector<std::pair<uint32_t, ConvRef>> v;
  for (JsonObject o : a) {
    ConvRef r{o["id"].as<const char*>(), o["name"] | "", (uint8_t)(o["kind"] | 0)};
    if (r.kind == 1 && !includeChannels) continue;
    if (r.kind == 2 || r.kind == 4) continue;
    v.push_back({o["lastTs"] | 0u, r});
  }
  if (includeChannels) {  // channels without history yet
    for (auto& ch : mesh_.channels()) {
      std::string id = convForChannel(ch.idx);
      bool have = std::any_of(v.begin(), v.end(), [&](auto& p) { return p.second.id == id; });
      if (!have) v.push_back({0, {id, ch.name, 1}});
    }
  } else {  // and the most recently heard contacts without history (only n of them: no full copy)
    std::vector<std::pair<uint32_t, size_t>> recent;  // (last advert, index)
    size_t idx = 0;
    mesh_.forEachContact([&](const Contact& c) {
      if (c.type == ADV_CHAT || c.type == ADV_ROOM) recent.push_back({c.lastAdvert, idx});
      idx++;
    });
    std::sort(recent.begin(), recent.end(), [](auto& a, auto& b) { return a.first > b.first; });
    for (size_t k = 0, added = 0; k < recent.size() && added < n; k++) {
      Contact c;
      if (!mesh_.contactAt(recent[k].second, c)) continue;
      std::string id = convForKey(c.pubKey);
      bool have = std::any_of(v.begin(), v.end(), [&](auto& p) { return p.second.id == id; });
      if (!have) {
        v.push_back({c.lastAdvert / 2, {id, c.name, kindForType(c.type)}});
        added++;
      }
    }
  }
  std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::vector<ConvRef> out;
  for (auto& p : v) {
    if (out.size() >= n) break;
    out.push_back(p.second);
  }
  return out;
}

size_t App::maxTextFor(const std::string& conv) {
  if (conv.rfind("ch_", 0) == 0) return maxChannelText(mesh_.selfInfo().name.size());
  return MAX_TEXT_LEN;
}

std::string App::authorName(const uint8_t* prefix, size_t n) {
  Contact c;
  if (mesh_.findContact(prefix, n, c)) return c.name;
  SelfInfo s = mesh_.selfInfo();
  if (memcmp(s.pubKey, prefix, n) == 0) return s.name;
  return toHex(prefix, n);
}

// ---------------------------------------------------------------- actions

SendResult App::sendText(const std::string& conv, const std::string& textIn, const char* via, uint8_t txtType,
                         const std::string* logText) {
  SendResult r;
  std::string text = textIn;
  while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
  if (text.empty()) {
    r.err = "empty";
    return r;
  }
  if (text.size() > maxTextFor(conv)) {
    r.err = "too_long";
    return r;
  }
  ConvRef ref;
  if (!resolveConv(conv, ref)) {
    r.err = "unknown_conv";
    return r;
  }
  r.target = ref.name;
  MsgRecord m;
  m.out = true;
  m.author = mesh_.selfInfo().name;
  m.text = logText ? *logText : text;
  m.txtType = txtType;
  m.status = (uint8_t)MsgStatus::Sending;
  m.via = via ? via : "";
  ConvMeta hint;
  hint.name = ref.name;
  hint.kind = ref.kind;
  uint32_t id = gHistory.append(conv, hint, m);
  if (ref.kind == 1) {
    mesh_.sendChannel(id, (uint8_t)atoi(conv.c_str() + 3), text);
  } else {
    uint8_t p[PREFIX_SIZE];
    fromHex(conv.substr(2), p, PREFIX_SIZE);
    mesh_.sendDirect(id, p, text, txtType);
  }
  JsonDocument ev;
  ev["t"] = "msg";
  ev["conv"] = conv;
  ev["name"] = ref.name;
  ev["kind"] = ref.kind;
  History::toJson(m, ev["m"].to<JsonObject>(), false);
  emit(ev);
  if (txtType == TXT_PLAIN && m.via != "telegram" && m.via != "route") gTelegram.relayOwn(conv, hint, m);
  r.ok = true;
  r.id = id;
  return r;
}

SendResult App::resend(const std::string& conv, uint32_t id) {
  SendResult r;
  MsgRecord m;
  ConvRef ref;
  if (!resolveConv(conv, ref) || !gHistory.find(conv, id, m) || !m.out) {
    r.err = "not_found";
    return r;
  }
  if (m.via == "route") {  // the history only has a placeholder, not the text
    r.err = "private_msg";
    return r;
  }
  if (ref.kind == 1) {
    mesh_.sendChannel(id, (uint8_t)atoi(conv.c_str() + 3), m.text);
  } else {
    uint8_t p[PREFIX_SIZE];
    fromHex(conv.substr(2), p, PREFIX_SIZE);
    mesh_.sendDirect(id, p, m.text, m.txtType);
  }
  gHistory.updateStatus(conv, id, (uint8_t)MsgStatus::Sending, 0);
  JsonDocument ev;
  ev["t"] = "st";
  ev["conv"] = conv;
  ev["id"] = id;
  ev["k"] = (int)MsgStatus::Sending;
  emit(ev);
  r.ok = true;
  r.id = id;
  r.target = ref.name;
  return r;
}

bool App::loginRoom(const std::string& conv, const std::string& password, bool remember, uint32_t rid) {
  uint8_t p[PREFIX_SIZE];
  Contact c;
  if (conv.size() != 14 || !fromHex(conv.substr(2), p, PREFIX_SIZE) || !mesh_.findContact(p, PREFIX_SIZE, c)) return false;
  if (remember) RoomPasswords::set(conv.substr(2), password);
  if (rid) loginRids_[rid] = conv;
  mesh_.login(c.pubKey, password, rid);
  return true;
}

void App::autoLoginRooms() {
  for (auto& k : RoomPasswords::keys()) {
    uint8_t p[PREFIX_SIZE];
    Contact c;
    if (!fromHex(k, p, PREFIX_SIZE) || !mesh_.findContact(p, PREFIX_SIZE, c)) continue;
    std::string pw = RoomPasswords::get(k);
    if (pw.empty()) continue;
    LOGI("auto login: %s", c.name);
    mesh_.login(c.pubKey, pw);
  }
}

void App::advert(bool flood) { mesh_.command(enc::selfAdvert(flood)); }

// ---------------------------------------------------------------- channels

int App::setChannel(int idx, std::string name, const uint8_t* secret, uint32_t rid, std::string& err) {
  // Without the module the channel list is empty and every slot, public one included, would look free.
  if (!mesh_.ready()) {
    err = "not_ready";
    return -1;
  }
  while (!name.empty() && name.back() == ' ') name.pop_back();
  while (!name.empty() && name.front() == ' ') name.erase(0, 1);
  uint8_t key[16];
  if (!secret) {
    if (!name.empty() && name[0] != '#') name = "#" + name;
    if (name.size() > 1) hashtagSecret(name, key);
  } else {
    memcpy(key, secret, 16);
  }
  if (name.empty() || name == "#" || name.size() > 31) {
    err = "bad_name";
    return -1;
  }
  int maxCh = mesh_.deviceInfo().maxChannels;
  if (!maxCh) maxCh = 8;
  if (idx >= maxCh) {
    err = "bad_slot";
    return -1;
  }
  if (idx < 0) {
    // A slot whose read failed is missing from channels() but may hold a channel: re-read first.
    if (!mesh_.channelsComplete()) {
      mesh_.refreshChannels();
      err = "channels_loading";
      return -1;
    }
    auto chs = mesh_.channels();
    std::string lname = name[0] == '#' ? name.substr(1) : name;
    for (auto& c : lname) c = tolower((unsigned char)c);
    auto same = [&](const std::string& n, const uint8_t* k) {
      std::string l = !n.empty() && n[0] == '#' ? n.substr(1) : n;
      for (auto& c : l) c = tolower((unsigned char)c);
      return l == lname || memcmp(k, key, 16) == 0;
    };
    std::lock_guard<std::mutex> l(chMtx_);
    for (auto it = slotReserved_.begin(); it != slotReserved_.end();)
      it = millis() - it->second.at > 30000 ? slotReserved_.erase(it) : std::next(it);
    bool dup = false;
    for (auto& c : chs) dup |= same(c.name, c.secret);
    for (auto& r : slotReserved_) dup |= same(r.second.name, r.second.key);  // added, not re-read yet
    if (dup) {
      err = "channel_exists";
      return -1;
    }
    // Slot 0 is the public channel: never handed out as free.
    for (int i = 1; i < maxCh && idx < 0; i++) {
      bool used = slotReserved_.count(i) > 0;
      for (auto& c : chs) used |= c.idx == i;
      if (!used) idx = i;
    }
    if (idx < 0) {
      err = "no_free_slot";
      return -1;
    }
    Reserved& r = slotReserved_[idx];
    r.at = millis();
    r.name = name;
    memcpy(r.key, key, 16);
  }
  mesh_.command(enc::setChannel((uint8_t)idx, name, key), rid);
  mesh_.refreshChannels();
  return idx;
}

bool App::deleteChannel(uint8_t idx, uint32_t rid, std::string& err) {
  ChannelInfo ch;
  if (!mesh_.ready()) err = "not_ready";
  else if (idx == 0) err = "public_channel";
  else if (!mesh_.findChannel(idx, ch)) err = "not_found";
  if (!err.empty()) return false;
  if (!rid) rid = newRid();
  {
    std::lock_guard<std::mutex> l(chMtx_);
    channelDeletes_[rid] = convForChannel(idx);
  }
  uint8_t zero[16] = {0};
  mesh_.command(enc::setChannel(idx, "", zero), rid);
  mesh_.refreshChannels();
  return true;
}

void App::queueReply(const std::string& conv, const std::string& text) {
  std::lock_guard<std::mutex> l(chMtx_);
  if (pendingReplies_.size() < 20) pendingReplies_.push_back({conv, text});
}

void App::runPendingReplies() {
  std::vector<std::pair<std::string, std::string>> v;
  {
    std::lock_guard<std::mutex> l(chMtx_);
    v.swap(pendingReplies_);
  }
  for (auto& r : v) sendText(r.first, utf8Truncate(r.second, MAX_TEXT_LEN), "telegram");
}

void App::runPendingForgets() {
  std::vector<std::string> v;
  {
    std::lock_guard<std::mutex> l(chMtx_);
    v.swap(pendingForgets_);
  }
  for (auto& c : v) forgetConversation(c);
}

void App::forgetConversation(const std::string& conv) {
  LOGI("conversation %s removed", conv.c_str());
  gHistory.removeConversation(conv);
  gTelegram.forgetConversation(conv);
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    auto& v = gConfig.tgMutedConvs;
    auto it = std::find(v.begin(), v.end(), conv);
    if (it != v.end()) {
      v.erase(it);
      gConfig.save();
    }
  }
  JsonDocument d;
  d["t"] = "conv_removed";
  d["conv"] = conv;
  emit(d);
}

// ---------------------------------------------------------------- JSON views

void App::contactJson(const Contact& c, JsonObject o) {
  o["key"] = toHex(c.pubKey, PUB_KEY_SIZE);
  o["conv"] = convForKey(c.pubKey);
  o["name"] = c.name;
  o["type"] = c.type;
  o["flags"] = c.flags;
  o["hops"] = pathHops(c.outPathLen);
  o["hashSize"] = pathHashSize(c.outPathLen);
  size_t pl = pathByteLen(c.outPathLen);
  o["path"] = toHex(c.outPath, pl < CONTACT_PATH_CACHE ? pl : CONTACT_PATH_CACHE);
  if (!c.pathCached()) o["pathPartial"] = true;
  o["lastAdvert"] = c.lastAdvert;
  if (c.lat || c.lon) {
    o["lat"] = c.lat / 1e6;
    o["lon"] = c.lon / 1e6;
  }
  o["lastmod"] = c.lastmod;
}

void App::contactsJson(JsonArray a) {
  mesh_.forEachContact([&](const Contact& c) { contactJson(c, a.add<JsonObject>()); });
}

void App::channelsJson(JsonArray a) {
  for (auto& ch : mesh_.channels()) {
    JsonObject o = a.add<JsonObject>();
    o["idx"] = ch.idx;
    o["conv"] = convForChannel(ch.idx);
    o["name"] = ch.name;
    o["secret"] = toHex(ch.secret, 16);
  }
  int maxCh = mesh_.deviceInfo().maxChannels;
  (void)maxCh;
}

void App::selfJson(JsonObject o) {
  SelfInfo s = mesh_.selfInfo();
  DeviceInfo d = mesh_.deviceInfo();
  o["name"] = s.name;
  o["key"] = toHex(s.pubKey, PUB_KEY_SIZE);
  o["lat"] = s.lat / 1e6;
  o["lon"] = s.lon / 1e6;
  o["txPower"] = s.txPower;
  o["maxTxPower"] = s.maxTxPower;
  o["freq"] = s.freq / 1000.0;
  o["bw"] = s.bw / 1000.0;
  o["sf"] = s.sf;
  o["cr"] = s.cr;
  o["multiAcks"] = s.multiAcks;
  o["advertLocPolicy"] = s.advertLocPolicy;
  o["telemetryModes"] = s.telemetryModes;
  o["manualAdd"] = s.manualAddContacts;
  o["fwVer"] = d.fwVer;
  o["fwVersion"] = d.fwVersion;
  o["buildDate"] = d.buildDate;
  o["manufacturer"] = d.manufacturer;
  o["maxContacts"] = d.maxContacts;
  o["maxChannels"] = d.maxChannels;
  o["blePin"] = d.blePin;
  o["repeat"] = d.repeat;
  o["pathHashMode"] = d.pathHashMode;
}

static int battPct(uint16_t mv) {
  if (!mv) return -1;
  int p = (int)((mv - 3300) * 100 / (4150 - 3300));
  return p < 0 ? 0 : p > 100 ? 100 : p;
}

void App::statusJson(JsonObject o) {
  o["version"] = HUB_VERSION;
  o["uptime"] = millis() / 1000;
  o["heap"] = ESP.getFreeHeap();
  o["minHeap"] = ESP.getMinFreeHeap();
  o["time"] = (uint32_t)time(nullptr);
  o["timeValid"] = timeValid();
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    o["setupDone"] = gConfig.setupDone;
    o["hostname"] = gConfig.hostname;
    o["lang"] = gConfig.lang;
    o["theme"] = gConfig.theme;
    o["accent"] = gConfig.accent;
    o["hasPassword"] = gConfig.hasPassword();
  }
  JsonObject w = o["wifi"].to<JsonObject>();
  w["mode"] = WiFi.getMode();
  w["connected"] = WiFi.status() == WL_CONNECTED;
  w["ssid"] = WiFi.SSID();
  w["rssi"] = WiFi.status() == WL_CONNECTED ? WiFi.RSSI() : 0;
  w["ip"] = WiFi.localIP().toString();
  w["apIp"] = WiFi.softAPIP().toString();
  w["apSsid"] = WiFi.softAPSSID();

  JsonObject lk = o["link"].to<JsonObject>();
  Transport* t = mesh_.transport();
  lk["type"] = t ? t->name() : "";
  lk["state"] = linkStateName(mesh_.state());
  lk["phase"] = linkPhaseName(mesh_.phase());
  lk["error"] = linkErrorName(mesh_.lastError());
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    lk["name"] = gConfig.link == "tcp" ? gConfig.tcpHost : gConfig.bleName;
    lk["addr"] = gConfig.link == "tcp" ? gConfig.tcpHost + ":" + std::to_string(gConfig.tcpPort) : gConfig.bleAddr;
  }
  if (t == &gBle && mesh_.ready()) {
    lk["rssi"] = gBle.rssi();
    lk["mtu"] = gBle.mtu();
  }
  if (mesh_.state() == LinkState::Released) {
    uint32_t until = mesh_.releasedUntilMs();
    lk["releasedFor"] = (int32_t)(until - millis()) > 0 ? (until - millis()) / 1000 : 0;
  }
  lk["lastRx"] = mesh_.lastRxMs() ? (millis() - mesh_.lastRxMs()) / 1000 : -1;
  o["lastSync"] = mesh_.lastSyncUnix();

  if (mesh_.ready() || !mesh_.selfInfo().name.empty()) selfJson(o["node"].to<JsonObject>());
  BattStorage b = mesh_.battery();
  JsonObject bt = o["battery"].to<JsonObject>();
  bt["mv"] = b.mv;
  bt["pct"] = battPct(b.mv);
  bt["usedKb"] = b.usedKb;
  bt["totalKb"] = b.totalKb;
  gTelegram.statusJson(o["telegram"].to<JsonObject>());
  Cellular::statusJson(o["cell"].to<JsonObject>());
  o["unread"] = gHistory.totalUnread();
  size_t used, total;
  gHistory.fsInfo(used, total);
  o["fsUsed"] = used;
  o["fsTotal"] = total;
}

// ---------------------------------------------------------------- Telegram summaries

static const char* typeIcon(uint8_t t) {
  switch (t) {
    case ADV_REPEATER: return "\xF0\x9F\x93\xA1";
    case ADV_ROOM: return "\xF0\x9F\x8F\xA0";
    case ADV_SENSOR: return "\xF0\x9F\x8C\xA1";
    default: return "\xF0\x9F\x91\xA4";
  }
}

static std::string ago(uint32_t ts) {
  uint32_t now = time(nullptr);
  if (!ts || now < ts || now < 1700000000) return "";
  uint32_t d = now - ts;
  char b[16];
  if (d < 3600) snprintf(b, sizeof(b), "%um", (unsigned)(d / 60));
  else if (d < 86400) snprintf(b, sizeof(b), "%uh", (unsigned)(d / 3600));
  else snprintf(b, sizeof(b), "%ud", (unsigned)(d / 86400));
  return b;
}

static std::string htmlEsc(const std::string& s) {
  std::string o;
  for (char c : s) {
    if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else if (c == '&') o += "&amp;";
    else o += c;
  }
  return o;
}

std::string App::contactsText(int lang) {
  std::vector<std::pair<uint32_t, size_t>> order;  // (last advert, index): 8 bytes per contact
  size_t idx = 0;
  mesh_.forEachContact([&](const Contact& c) { order.push_back({c.lastAdvert, idx++}); });
  if (order.empty()) return tgs::T[tgs::NO_CONTACTS][lang];
  std::sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
  std::string s = std::string(tgs::T[tgs::CONTACTS_TITLE][lang]) + "\n";
  for (size_t k = 0; k < order.size(); k++) {
    if (k >= 40) {
      s += "… (" + std::to_string(order.size()) + ")";
      break;
    }
    Contact c;
    if (!mesh_.contactAt(order[k].second, c)) continue;
    std::string a = ago(c.lastAdvert);
    s += std::string(typeIcon(c.type)) + " " + htmlEsc(c.name) + (a.empty() ? "" : " <i>(" + a + ")</i>") + "\n";
  }
  return s;
}

std::string App::channelsText(int lang) {
  auto chs = mesh_.channels();
  if (chs.empty()) return tgs::T[tgs::NO_CHANNELS][lang];
  std::string s = std::string(tgs::T[tgs::CHANNELS_TITLE][lang]) + "\n";
  for (auto& c : chs) s += "\xF0\x9F\x93\xA2 " + std::to_string(c.idx) + " · " + htmlEsc(c.name) + "\n";
  return s;
}

std::string App::statusText(int lang) {
  char b[512];
  BattStorage bt = mesh_.battery();
  uint32_t up = millis() / 1000;
  std::string node = mesh_.ready() ? htmlEsc(mesh_.selfInfo().name) + " ✅" : std::string(linkStateName(mesh_.state())) + " ❌";
  snprintf(b, sizeof(b),
           "%s v%s\n%s : %s\n%s : %.2f V (%d%%)\n%s : %s (%d dBm)\n%s : %ud %02uh%02u\n%s : %u\nRAM : %u kB",
           tgs::T[tgs::STATUS_TITLE][lang], HUB_VERSION, tgs::T[tgs::MODULE][lang], node.c_str(),
           tgs::T[tgs::BATTERY][lang], bt.mv / 1000.0, battPct(bt.mv), tgs::T[tgs::WIFI][lang], WiFi.SSID().c_str(),
           (int)WiFi.RSSI(), tgs::T[tgs::UPTIME][lang], (unsigned)(up / 86400), (unsigned)(up / 3600 % 24),
           (unsigned)(up / 60 % 60), tgs::T[tgs::UNREAD][lang], (unsigned)gHistory.totalUnread(),
           (unsigned)(ESP.getFreeHeap() / 1024));
  return b;
}

// ---------------------------------------------------------------- MeshListener

void App::onLinkState(LinkState s, LinkPhase p, LinkError e) {
  JsonDocument d;
  d["t"] = "link";
  d["state"] = linkStateName(s);
  d["phase"] = linkPhaseName(p);
  d["error"] = linkErrorName(e);
  emit(d);
  if (s == LinkState::Ready && !wasReady_) {
    wasReady_ = true;
    LOGI("heap after module sync: %u kB free, largest block %u kB, %u contacts%s", (unsigned)(ESP.getFreeHeap() / 1024),
         (unsigned)(ESP.getMaxAllocHeap() / 1024), (unsigned)mesh_.contactCount(), psramNote().c_str());
    autoLoginRooms();
  }
  if (s != LinkState::Ready) {
    if (wasReady_) LOGW("module link: %s", linkStateName(s));
    wasReady_ = false;
  }
}

void App::onDeviceInfo(const DeviceInfo& d) {
  LOGI("module: %s %s (protocol v%u)", d.manufacturer.c_str(), d.fwVersion.c_str(), (unsigned)d.fwVer);
}

void App::onSelfInfo(const SelfInfo&) {
  JsonDocument d;
  d["t"] = "self";
  selfJson(d["node"].to<JsonObject>());
  emit(d);
}

void App::onContactsLoaded() {
  mesh_.forEachContact([](const Contact& c) { gHistory.setName(convForKey(c.pubKey), c.name, kindForType(c.type)); });
  JsonDocument d;
  d["t"] = "contacts";
  emit(d);
}

void App::onContact(const Contact& c, bool isNew) {
  gHistory.setName(convForKey(c.pubKey), c.name, kindForType(c.type));
  JsonDocument d;
  d["t"] = "contact";
  d["isNew"] = isNew;
  contactJson(c, d["c"].to<JsonObject>());
  emit(d);
  if (isNew) LOGI("new contact: %s", c.name);
}

void App::onContactRemoved(const uint8_t* pub) {
  JsonDocument d;
  d["t"] = "contact_removed";
  d["key"] = toHex(pub, PUB_KEY_SIZE);
  emit(d);
}

void App::onChannelsLoaded() {
  for (auto& ch : mesh_.channels()) {
    std::string conv = convForChannel(ch.idx);
    ConvMeta m;
    // Another channel now uses this slot (deleted and re-added, here or from another app):
    // its messages and Telegram topic belong to the old channel.
    if (gHistory.meta(conv, m) && !m.name.empty() && m.name != conv && m.name != ch.name) {
      std::lock_guard<std::mutex> l(chMtx_);
      pendingForgets_.push_back(conv);
    } else {
      gHistory.setName(conv, ch.name, 1);
    }
  }
  {
    std::lock_guard<std::mutex> l(chMtx_);  // re-read: the reservations are now real channels
    for (auto& ch : mesh_.channels()) slotReserved_.erase(ch.idx);
  }
  gTelegram.channelsChanged();
  JsonDocument d;
  d["t"] = "channels";
  emit(d);
}

void App::onMessage(const RecvMsg& rm) {
  std::string conv;
  ConvMeta meta;
  MsgRecord m;
  uint32_t now = time(nullptr);
  m.ts = now > 1700000000 ? now : rm.senderTs;
  m.senderTs = rm.senderTs;
  if (rm.hasSnr) m.snr = rm.snr;
  m.hops = pathHops(rm.pathLen);
  m.txtType = rm.txtType;
  if (rm.isChannel) {
    conv = convForChannel(rm.channelIdx);
    ChannelInfo ch;
    meta.name = mesh_.findChannel(rm.channelIdx, ch) ? ch.name : conv;
    meta.kind = 1;
    m.author = rm.channelSender();
    m.text = rm.channelBody();
  } else {
    conv = convForKey(rm.prefix);
    Contact c;
    if (mesh_.findContact(rm.prefix, PREFIX_SIZE, c)) {
      meta.name = c.name;
      meta.kind = kindForType(c.type);
    } else {
      meta.name = toHex(rm.prefix, PREFIX_SIZE);
    }
    m.text = rm.text;
    if (rm.hasAuthor) {
      m.authorKey = toHex(rm.author, 4);
      m.author = authorName(rm.author, 4);
      SelfInfo s = mesh_.selfInfo();
      if (memcmp(s.pubKey, rm.author, 4) == 0) {  // our own room post, replayed by the server
        m.out = true;
        m.status = (uint8_t)MsgStatus::Delivered;
      }
    }
  }
  // "@name text" from an approved contact: for one chat of the directory, not the main chat
  bool routed = false;
  if (!rm.isChannel && meta.kind == 0 && rm.txtType == TXT_PLAIN && !rm.hasAuthor) {
    std::string logText;
    if (gTelegram.routeIncoming(conv, meta.name, m.text, logText)) {
      m.text = logText;
      routed = true;
    }
  }
  gHistory.append(conv, meta, m);
  ConvMeta full;
  gHistory.meta(conv, full);
  JsonDocument d;
  d["t"] = "msg";
  d["conv"] = conv;
  d["name"] = meta.name;
  d["kind"] = meta.kind;
  d["muted"] = full.muted;
  History::toJson(m, d["m"].to<JsonObject>(), false);
  emit(d);
  if (!m.out && !routed) gTelegram.relayIncoming(conv, full, m);
}

void App::onMsgStatus(uint32_t id, MsgStatus st, uint32_t rtt, uint8_t attempt) {
  std::string conv = gHistory.convOf(id);
  if (conv.empty()) return;
  gHistory.updateStatus(conv, id, (uint8_t)st, rtt);
  JsonDocument d;
  d["t"] = "st";
  d["conv"] = conv;
  d["id"] = id;
  d["k"] = (int)st;
  if (rtt) d["r"] = rtt;
  d["a"] = attempt;
  emit(d);
  ConvRef ref;
  resolveConv(conv, ref);
  gTelegram.onOutStatus(id, (uint8_t)st, conv, ref.name);
}

void App::onBattery(const BattStorage& b) {
  JsonDocument d;
  d["t"] = "battery";
  d["mv"] = b.mv;
  d["pct"] = battPct(b.mv);
  d["usedKb"] = b.usedKb;
  d["totalKb"] = b.totalKb;
  emit(d);
}

void App::onLogin(const LoginResult& r) {
  std::string conv = convForKey(r.prefix);
  ConvRef ref;
  resolveConv(conv, ref);
  if (!r.ok) {
    RoomPasswords::remove(conv.substr(2));  // don't retry a wrong password at every boot
    LOGW("login failed: %s", ref.name.c_str());
  } else {
    LOGI("logged into %s%s", ref.name.c_str(), r.permissions & 1 ? " (admin)" : "");
  }
  JsonDocument d;
  d["t"] = "login";
  d["conv"] = conv;
  d["ok"] = r.ok;
  d["admin"] = (r.permissions & 1) != 0;
  d["perms"] = r.permissions;
  d["acl"] = r.acl;
  emit(d);
  gTelegram.loginResult(conv, r.ok, ref.name);
}

void App::onRepeaterStatus(const RepeaterStatus& s) {
  JsonDocument d;
  d["t"] = "rstatus";
  d["conv"] = convForKey(s.prefix);
  JsonObject o = d["s"].to<JsonObject>();
  o["battMv"] = s.battMv;
  o["txQueue"] = s.txQueue;
  o["noiseFloor"] = s.noiseFloor;
  o["lastRssi"] = s.lastRssi;
  o["lastSnr"] = s.lastSnr;
  o["nRecv"] = s.nRecv;
  o["nSent"] = s.nSent;
  o["airtime"] = s.airtime;
  o["rxAirtime"] = s.rxAirtime;
  o["uptime"] = s.uptime;
  o["sentFlood"] = s.sentFlood;
  o["sentDirect"] = s.sentDirect;
  o["recvFlood"] = s.recvFlood;
  o["recvDirect"] = s.recvDirect;
  o["fullEvents"] = s.fullEvents;
  o["directDups"] = s.directDups;
  o["floodDups"] = s.floodDups;
  if (s.hasRecvErrors) o["recvErrors"] = s.recvErrors;
  emit(d);
}

void App::onTelemetry(const TelemetryResp& t) {
  JsonDocument d;
  d["t"] = "telemetry";
  d["conv"] = convForKey(t.prefix);
  d["lpp"] = toHex(t.lpp.data(), t.lpp.size());
  emit(d);
}

void App::onTrace(const TraceData& t) {
  JsonDocument d;
  d["t"] = "trace";
  d["tag"] = t.tag;
  d["flags"] = t.flags;
  d["hashes"] = toHex(t.hashes.data(), t.hashes.size());
  JsonArray a = d["snrs"].to<JsonArray>();
  for (float s : t.snrs) a.add(s);
  emit(d);
}

void App::onPathDiscovery(const PathDiscovery& p) {
  JsonDocument d;
  d["t"] = "pathdisc";
  d["conv"] = convForKey(p.prefix);
  d["outLen"] = p.outPathLen;
  d["out"] = toHex(p.outPath.data(), p.outPath.size());
  d["inLen"] = p.inPathLen;
  d["in"] = toHex(p.inPath.data(), p.inPath.size());
  emit(d);
}

void App::onStats(const Stats&) {}

void App::onContactsFull() {
  LOGW("module contact table is full");
  JsonDocument d;
  d["t"] = "contacts_full";
  emit(d);
}

void App::onResult(uint32_t rid, bool ok, uint8_t err, const uint8_t* data, size_t len) {
  JsonDocument d;
  d["t"] = "result";
  d["rid"] = rid;
  d["ok"] = ok;
  if (!ok) d["err"] = err == 0xFF ? "timeout" : errorName(err);
  if (data && len) d["data"] = toHex(data, len);
  Stats st;
  if (ok && data && dec::stats(data, len, st)) {
    JsonObject o = d["stats"].to<JsonObject>();
    o["type"] = st.type;
    if (st.type == STATS_CORE) {
      o["battMv"] = st.battMv; o["uptime"] = st.uptime; o["errFlags"] = st.errFlags; o["queueLen"] = st.queueLen;
    } else if (st.type == STATS_RADIO) {
      o["noiseFloor"] = st.noiseFloor; o["lastRssi"] = st.lastRssi; o["lastSnr"] = st.lastSnr;
      o["txAir"] = st.txAir; o["rxAir"] = st.rxAir;
    } else {
      o["recv"] = st.recv; o["sent"] = st.sent; o["sentFlood"] = st.sentFlood; o["sentDirect"] = st.sentDirect;
      o["recvFlood"] = st.recvFlood; o["recvDirect"] = st.recvDirect; o["recvErrors"] = st.recvErrors;
    }
  }
  auto it = loginRids_.find(rid);
  if (it != loginRids_.end()) {
    d["conv"] = it->second;
    loginRids_.erase(it);
  }
  {
    // Runs under the MeshClient lock: the history / NVS / Telegram cleanup is done by meshLoop.
    std::lock_guard<std::mutex> l(chMtx_);
    auto dc = channelDeletes_.find(rid);
    if (dc != channelDeletes_.end()) {
      if (ok) pendingForgets_.push_back(dc->second);
      channelDeletes_.erase(dc);
    }
  }
  gTelegram.commandResult(rid, ok, ok ? "" : d["err"].as<std::string>());
  emit(d);
}

void App::onLog(const char* msg) { LOGI("[mesh] %s", msg); }
