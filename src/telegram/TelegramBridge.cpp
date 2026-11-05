#include "TelegramBridge.h"
#include <HTTPClient.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <algorithm>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <esp_system.h>
#include "../App.h"
#include "../config/Config.h"
#include "../core/Route.h"
#include "../util/Log.h"
#include "../setup/Network.h"
#include "../web/Web.h"
#include "TelegramCA.h"
#include "TgStrings.h"

TelegramBridge gTelegram;

using Lock = std::lock_guard<std::mutex>;
static const char* MAPS = "/tg.json";
static const int POLL_SECONDS = 4;
static const uint32_t WEB_PRIORITY_MS = 20000;
static const uint32_t IDLE_GAP_MS = 6000;  // TLS closed between polls when nothing happens
// mbedTLS allocates two separate ~16.7 kB record buffers (in and out) plus ~10 kB for the
// handshake. Asking for one 30 kB block instead blocked the bridge for good once the web
// server had fragmented the heap (69 kB free, largest block 22 kB).
// 16384 content + 320 overhead (IV 16, MAC 48, padding 256) + 13 header = 16717 bytes, rounded up.
// Not more: both buffers often share one ~33 kB block, which a 17 kB probe would refuse.
// (Reserving them for good at boot was tried: 33 kB less starves the web server and crashes.)
static const size_t TLS_RECORD_BUF = 16768;
static const uint32_t TLS_MIN_FREE = 50 * 1024;
static const uint32_t LOW_RAM_REBOOT_MS = 30UL * 60 * 1000;  // last resort, see pollLoop
static const size_t ROUTE_MSGS = 100;              // routed messages that can still be answered
static const size_t ROUTE_INVITES = route::MAX_ENTRIES;
static const uint32_t ROUTE_INVITE_MS = 24UL * 3600 * 1000;
static const uint32_t CLAIM_MS = 15UL * 60 * 1000;  // "link another chat" does not stay armed for ever
static const uint8_t ROUTE_PER_MINUTE = 6;         // chats of the directory cannot flood the radio

// Would a TLS session fit right now? Both record buffers are really tried, then given back.
static bool tlsFits() {
  if (ESP.getFreeHeap() < TLS_MIN_FREE) return false;
  const uint32_t caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;  // what mbedTLS allocates from
  void* a = heap_caps_malloc(TLS_RECORD_BUF, caps);
  void* b = a ? heap_caps_malloc(TLS_RECORD_BUF, caps) : nullptr;
  bool ok = a && b;
  heap_caps_free(b);
  heap_caps_free(a);
  return ok;
}
static const char* E_SENT = "\xE2\x9C\x8D";            // ✍ (allowed bot reactions are a fixed list)
static const char* E_DELIVERED = "\xF0\x9F\x91\x8C";   // 👌
static const char* E_FAILED = "\xF0\x9F\x92\x94";      // 💔

static std::string esc(const std::string& s) {
  std::string o;
  o.reserve(s.size() + 8);
  for (char c : s) {
    if (c == '&') o += "&amp;";
    else if (c == '<') o += "&lt;";
    else if (c == '>') o += "&gt;";
    else o += c;
  }
  return o;
}

static std::string trim(const std::string& s) {
  size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}

static std::string fmt(const char* f, const std::string& a) {
  // Sized to fit: a truncated HTML entity or UTF-8 sequence would make Telegram reject the message.
  int n = snprintf(nullptr, 0, f, a.c_str());
  if (n <= 0) return "";
  std::string o((size_t)n, '\0');
  snprintf(&o[0], o.size() + 1, f, a.c_str());
  return o;
}

static std::string token() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return gConfig.tgToken;
}

static bool enabled() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return gConfig.tgEnabled && !gConfig.tgToken.empty() && !gConfig.apOnly;
}

int TelegramBridge::lang() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return tgs::langIndex(gConfig.lang.c_str());
}

#define TR(id) tgs::T[tgs::id][lang()]

int64_t TelegramBridge::mainChat() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return gConfig.tgChats.empty() ? 0 : gConfig.tgChats[0];
}

bool TelegramBridge::topicsMode() {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  return gConfig.tgMode == "topics";
}

bool TelegramBridge::allowed(int64_t chat) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  for (auto c : gConfig.tgChats) if (c == chat) return true;
  return false;
}

// ---------------------------------------------------------------- lifecycle

void TelegramBridge::begin() {
  loadMaps();
#if defined(CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC) || defined(CONFIG_MBEDTLS_DEFAULT_MEM_ALLOC)
  roomy_ = psramFound();  // TLS buffers may live in PSRAM: two sessions cost no internal RAM
#endif
  xTaskCreatePinnedToCore(pollTaskFn, "telegram", 9216, this, 1, &pollTask_, ARDUINO_RUNNING_CORE);
  if (roomy_) xTaskCreatePinnedToCore(sendTaskFn, "tg_send", 9216, this, 1, &sendTask_, ARDUINO_RUNNING_CORE);
  LOGI("telegram: %s", roomy_ ? "persistent sessions (long polling, separate sender)" : "single session, closed between polls");
}

void TelegramBridge::reconfigure() {
  tokenOk_ = false;  // the send task waits for getMe with the new settings
  online_ = false;
  {
    std::lock_guard<std::mutex> l(errMtx_);
    botName_.clear();  // don't show the previous bot as connected meanwhile
    botUser_.clear();
  }
  sendGen_++;
  reconfig_ = true;  // last: also cuts short the poll task's long poll (AbortableClient)
}

void TelegramBridge::setError(const std::string& e) {
  std::lock_guard<std::mutex> l(errMtx_);
  lastError_ = e;
}

std::string TelegramBridge::error() {
  std::lock_guard<std::mutex> l(errMtx_);
  return lastError_;
}
void TelegramBridge::startClaim() {
  claimAt_ = millis();
  claiming_ = true;
}

bool TelegramBridge::claimActive() {
  if (claiming_ && millis() - claimAt_ > CLAIM_MS) claiming_ = false;
  return claiming_;
}

void TelegramBridge::testToken(const std::string& t) {
  Lock l(mtx_);
  testToken_ = t;
  test_.busy = true;
  testPending_ = true;
  if (sendTask_) xTaskNotifyGive(sendTask_);  // roomy boards: the send task runs the test
}

void TelegramBridge::statusJson(JsonObject o) {
  o["enabled"] = enabled();
  o["online"] = (bool)online_;
  {
    std::lock_guard<std::mutex> l(errMtx_);
    o["bot"] = botName_;
    o["user"] = botUser_;
    o["error"] = lastError_;
  }
  o["claiming"] = claimActive();
  o["topicsEnabled"] = (bool)topicsEnabled_;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    o["chats"] = gConfig.tgChats.size();  // 0: messages would have nowhere to go
  }
  Lock l(mtx_);
  o["queued"] = outbox_.size();
  JsonObject t = o["test"].to<JsonObject>();
  t["seq"] = test_.seq;
  t["pending"] = test_.busy;
  t["ok"] = test_.ok;
  t["name"] = test_.name;
  t["user"] = test_.user;
  t["err"] = test_.err;
}

void TelegramBridge::setupClient(WiFiClientSecure& c) {
  bool insecure;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    insecure = gConfig.tgInsecure;
  }
  if (insecure) c.setInsecure();
  else c.setCACert(TELEGRAM_ROOT_CA);
  c.setHandshakeTimeout(15);
}

// ---------------------------------------------------------------- HTTP

// HTTPClient's destructor stops the socket it was given: detach it so the TLS session we
// own survives between requests (one handshake instead of one per call).
struct KeepAliveHttp : HTTPClient {
  ~KeepAliveHttp() { _client = nullptr; }
};

int TelegramBridge::call(WiFiClientSecure& cli, const std::string& tok, const char* method, const std::string& body,
                         JsonDocument& resp, uint32_t timeoutMs, JsonDocument* filter, std::string* err) {
  auto fail = [&](const std::string& e) {
    setError(e);
    if (err) *err = e;
  };
  KeepAliveHttp http;
  http.setReuse(true);
  http.setTimeout(timeoutMs);
  http.setConnectTimeout(10000);
  String url = String("https://api.telegram.org/bot") + tok.c_str() + "/" + method;
  if (!http.begin(cli, url)) return -1;
  http.addHeader("Content-Type", "application/json");
  int code = body.empty() ? http.GET() : http.POST((uint8_t*)body.data(), body.size());
  if (code <= 0) {
    char tls[96];
    // mbedTLS reports "... Memory allocation failed" / "Allocation of memory failed"
    tlsNoRam_ = cli.lastError(tls, sizeof(tls)) < 0 && strstr(tls, "llocation");
    fail(tlsNoRam_ ? std::string("TLS: ") + tls : std::string(http.errorToString(code).c_str()));
    http.end();
    cli.stop();
    return code;
  }
  DeserializationError e;
  if (http.getSize() >= 0) {
    // Content-Length known: parse straight from the socket, never holding the body in RAM.
    e = filter ? deserializeJson(resp, http.getStream(), DeserializationOption::Filter(*filter))
               : deserializeJson(resp, http.getStream());
  } else {  // chunked: let HTTPClient decode it
    String payload = http.getString();
    e = filter ? deserializeJson(resp, payload, DeserializationOption::Filter(*filter)) : deserializeJson(resp, payload);
  }
  http.end();
  if (e) {
    fail("bad json");
    cli.stop();  // the stream position is unknown: start a clean session next time
    resp.clear();
    // A 200 means Telegram accepted the request: never send it again because of our parsing.
    return code;
  }
  if (code != 200) fail(std::string(resp["description"] | "HTTP error"));
  return code;
}

// ---------------------------------------------------------------- outbox

void TelegramBridge::enqueue(Out o) {
  if (!enabled()) return;
  if (!o.chat) o.chat = mainChat();
  if (!o.chat) return;
  std::string lostConv, lostName;
  {
    Lock l(mtx_);
    if (outbox_.size() >= 300) {
      // A routed message has no other copy: the oldest ordinary message goes first.
      LOGW("telegram outbox full, dropping oldest");
      auto it = std::find_if(outbox_.begin(), outbox_.end(), [](const Out& x) { return x.route.empty(); });
      if (it == outbox_.end()) {
        it = outbox_.begin();
        lostConv = it->route;
        lostName = it->routeName;
      }
      outbox_.erase(it);
    }
    o.seq = ++outSeq_;
    outbox_.push_back(std::move(o));
    if (sendTask_) xTaskNotifyGive(sendTask_);
  }
  if (!lostConv.empty()) gApp.queueReply(lostConv, fmt(TR(ROUTE_FAILED), lostName));
}

void TelegramBridge::reply(int64_t chat, int32_t thread, const std::string& html, int32_t replyTo, const std::string& markup) {
  Out o;
  o.chat = chat;
  o.text = html;
  o.replyTo = replyTo;
  o.markup = markup;
  if (thread) o.conv = "#thread:" + std::to_string(thread);  // reply inside the same topic
  enqueue(o);
}

void TelegramBridge::notify(const std::string& html) {
  Out o;
  o.text = html;
  enqueue(o);
}

std::string TelegramBridge::format(const std::string& conv, const ConvMeta& meta, const MsgRecord& m, bool own, bool topics) {
  bool radio;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    radio = gConfig.tgRadioInfo;
  }
  std::string s;
  std::string name = esc(meta.name.empty() ? conv : meta.name);
  if (own) {
    s = topics ? "\xE2\x9E\xA1\xEF\xB8\x8F " : std::string("\xE2\x9E\xA1\xEF\xB8\x8F <b>") + TR(YOU) + "</b> \xE2\x86\x92 " + name + " : ";
    return s + esc(m.text);
  }
  std::string author = esc(m.author);
  switch (meta.kind) {
    case 1: {  // channel
      std::string ch = meta.name.empty() || meta.name[0] == '#' ? name : "#" + name;
      if (topics) s = author.empty() ? "" : "<b>" + author + "</b> : ";
      else s = "\xF0\x9F\x93\xA2 <b>" + ch + "</b>" + (author.empty() ? "" : " \xC2\xB7 " + author) + " : ";
      break;
    }
    case 3:  // room
      if (topics) s = author.empty() ? "" : "<b>" + author + "</b> : ";
      else s = "\xF0\x9F\x8F\xA0 <b>" + name + "</b>" + (author.empty() ? "" : " \xC2\xB7 " + author) + " : ";
      break;
    case 2:
      s = topics ? "" : "\xF0\x9F\x93\xA1 <b>" + name + "</b> : ";
      break;
    case 4:
      s = topics ? "" : "\xF0\x9F\x8C\xA1 <b>" + name + "</b> : ";
      break;
    default:
      s = topics ? "" : "\xF0\x9F\x91\xA4 <b>" + name + "</b> : ";
  }
  s += esc(m.text);
  if (radio && (!isnan(m.snr) || m.hops != -2)) {
    char line[64] = "";
    std::string hops = m.hops == -1 ? std::string(TR(DIRECT))
                                    : (m.hops >= 0 ? std::to_string(m.hops) + " " + TR(HOPS) : std::string());
    if (!isnan(m.snr)) snprintf(line, sizeof(line), "SNR %.1f", m.snr);
    std::string l = line;
    if (!hops.empty()) l += (l.empty() ? "" : " \xC2\xB7 ") + hops;
    s += "\n<i>" + l + "</i>";
  }
  return s;
}

void TelegramBridge::relayIncoming(const std::string& conv, const ConvMeta& meta, const MsgRecord& m) {
  if (!enabled()) return;
  const char* skip = nullptr;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (gConfig.tgMuted(conv) || meta.muted) skip = "muted";
    else if (meta.kind == 1 && !gConfig.tgRelayChannels) skip = "channels off";
    else if (conv == App::convForChannel(0) && !gConfig.tgRelayPublic) skip = "public channel off";
    else if (meta.kind == 3 && !gConfig.tgRelayRooms) skip = "rooms off";
    else if ((meta.kind == 0 || meta.kind == 2 || meta.kind == 4) && !gConfig.tgRelayDm) skip = "direct messages off";
  }
  if (!skip && m.txtType == 1) skip = "CLI reply";  // stays in the web console
  LOGI("telegram: message in %s %s", conv.c_str(), skip ? skip : "queued");
  if (skip) return;
  Out o;
  o.conv = conv;
  o.text = format(conv, meta, m, false, topicsMode());
  if (topicsMode()) o.flat = format(conv, meta, m, false, false);  // if the chat has no topics
  enqueue(o);
}

void TelegramBridge::relayOwn(const std::string& conv, const ConvMeta& meta, const MsgRecord& m) {
  if (!enabled()) return;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    if (gConfig.tgSkipOwn || gConfig.tgMuted(conv)) return;
    if (conv == App::convForChannel(0) && !gConfig.tgRelayPublic) return;
  }
  Out o;
  o.conv = conv;
  o.text = format(conv, meta, m, true, topicsMode());
  if (topicsMode()) o.flat = format(conv, meta, m, true, false);
  enqueue(o);
}

void TelegramBridge::trackOutgoing(uint32_t id, int64_t chat, int32_t msgId, bool noResend) {
  Lock l(mtx_);
  origin_[id] = {chat, msgId, noResend};
  while (origin_.size() > 100) origin_.erase(origin_.begin());
}

void TelegramBridge::onOutStatus(uint32_t id, uint8_t status, const std::string& conv, const std::string& convName) {
  Origin o;
  {
    Lock l(mtx_);
    auto it = origin_.find(id);
    if (it == origin_.end()) return;
    o = it->second;
    if (status == (uint8_t)mc::MsgStatus::Delivered || status == (uint8_t)mc::MsgStatus::Failed) origin_.erase(it);
  }
  Out r;
  r.kind = Out::Reaction;
  r.chat = o.chat;
  r.msgId = o.msgId;
  if (status == (uint8_t)mc::MsgStatus::Sent) r.text = E_SENT;
  else if (status == (uint8_t)mc::MsgStatus::Delivered) r.text = E_DELIVERED;
  else if (status == (uint8_t)mc::MsgStatus::Failed) r.text = E_FAILED;
  else return;
  enqueue(r);
  if (status == (uint8_t)mc::MsgStatus::Failed) {
    std::string markup;
    if (!o.noResend) {
      JsonDocument kb;
      JsonObject b = kb["inline_keyboard"].to<JsonArray>().add<JsonArray>().add<JsonObject>();
      b["text"] = TR(RESEND);
      b["callback_data"] = "rs|" + conv + "|" + std::to_string(id);
      serializeJson(kb, markup);
    }
    Out m;
    m.chat = o.chat;
    m.replyTo = o.msgId;
    m.text = fmt(TR(NOT_DELIVERED), esc(convName));
    m.markup = markup;
    enqueue(m);
  }
}

void TelegramBridge::trackLogin(const std::string& conv, int64_t chat) {
  Lock l(mtx_);
  pendingLogins_[conv] = chat;
}

void TelegramBridge::loginResult(const std::string& conv, bool ok, const std::string& name) {
  int64_t chat;
  {
    Lock l(mtx_);
    auto it = pendingLogins_.find(conv);
    if (it == pendingLogins_.end()) return;
    chat = it->second;
    pendingLogins_.erase(it);
  }
  reply(chat, 0, std::string(ok ? "\xE2\x9C\x85 " : "\xE2\x9D\x8C ") + esc(name));
}

void TelegramBridge::trackCommand(uint32_t rid, int64_t chat, int32_t thread, const std::string& okHtml) {
  Lock l(mtx_);
  pendingCmds_[rid] = {chat, thread, okHtml};
}

void TelegramBridge::commandResult(uint32_t rid, bool ok, const std::string& err) {
  PendingCmd p;
  {
    Lock l(mtx_);
    auto it = pendingCmds_.find(rid);
    if (it == pendingCmds_.end()) return;
    p = it->second;
    pendingCmds_.erase(it);
  }
  reply(p.chat, p.thread, ok ? p.okHtml : fmt(TR(CH_REFUSED), esc(err)));
}

void TelegramBridge::forgetConversation(const std::string& conv) {
  Lock l(mtx_);
  topics_.erase(conv);  // a channel re-added in this slot gets a topic of its own
  for (auto it = msgConv_.begin(); it != msgConv_.end();) it = it->second == conv ? msgConv_.erase(it) : std::next(it);
  for (auto it = pendingTarget_.begin(); it != pendingTarget_.end();)
    it = it->second == conv ? pendingTarget_.erase(it) : std::next(it);
  for (auto it = routeMsgs_.begin(); it != routeMsgs_.end();) it = it->conv == conv ? routeMsgs_.erase(it) : std::next(it);
  for (auto& o : outbox_) {
    if (o.conv != conv) continue;
    if (!o.flat.empty()) o.text = o.flat;  // main chat wording, with the conversation name
    o.conv.clear();                        // no topic, no reply mapping to a slot that may be reused
    o.noThread = true;
  }
  forgetGen_++;
  mapsDirty_ = true;
  seedTopics_ = true;  // a new channel in this slot needs its own topic
}

// ---------------------------------------------------------------- private routing

bool TelegramBridge::routeIncoming(const std::string& conv, const std::string& sender, const std::string& text, std::string& logText) {
  route::Parsed p = route::parse(text);
  if (!p.routed) return false;
  bool keep, on;
  std::vector<std::string> directory, known, unknown;
  std::vector<int64_t> chats;  // of `known`
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    auto& from = gConfig.tgRouteFrom;
    if (std::find(from.begin(), from.end(), conv) == from.end()) return false;
    on = gConfig.tgRouteOn;
    keep = gConfig.tgRouteKeep;
    for (auto& r : gConfig.tgRoutes) directory.push_back(r.name);
    for (auto& name : p.names) {
      auto it = std::find_if(gConfig.tgRoutes.begin(), gConfig.tgRoutes.end(), [&](const TgRoute& r) { return r.name == name; });
      if (it == gConfig.tgRoutes.end()) unknown.push_back(name);
      else if (std::find(chats.begin(), chats.end(), it->chat) == chats.end()) {  // two names, one chat: sent once
        known.push_back(name);
        chats.push_back(it->chat);
      }
    }
  }
  // The names stay visible in the history, not what was said.
  if (keep) {
    logText = text;
  } else {
    logText = "\xF0\x9F\x94\x92";
    // Only real recipients: anything else is a word of the message.
    for (auto& name : p.names)
      logText += " @" + (std::find(directory.begin(), directory.end(), name) != directory.end() ? name : std::string("?"));
  }
  auto listed = [](const char* f, const std::vector<std::string>& v) {  // "%s" is replaced: 2 bytes more for the names
    std::string n = route::fit(v, mc::MAX_TEXT_LEN - strlen(f) + 2);
    return fmt(f, n.empty() ? "-" : n);
  };
  if (!on || !enabled()) {
    // Its author believes it is private: it must not fall back to the main chat.
    gApp.queueReply(conv, TR(ROUTE_OFF));
  } else if (p.list) {
    gApp.queueReply(conv, listed(TR(ROUTE_LIST), directory));
  } else if (p.names.size() > route::MAX_TARGETS) {
    gApp.queueReply(conv, TR(ROUTE_TOO_MANY));
  } else if (known.empty()) {
    gApp.queueReply(conv, listed(TR(ROUTE_UNKNOWN), directory));
  } else if (p.body.empty()) {
    gApp.queueReply(conv, fmt(TR(ROUTE_EMPTY), known[0]));
  } else {
    for (size_t k = 0; k < known.size(); k++) {
      Out o;
      o.chat = chats[k];
      o.text = "\xF0\x9F\x91\xA4 <b>" + esc(sender) + "</b> : " + esc(p.body);
      o.noThread = true;
      o.route = conv;
      o.routeName = known[k];
      enqueue(o);
      LOGI("telegram: routed from %s to @%s", conv.c_str(), known[k].c_str());
    }
    // The ones that were found still get it: the author would otherwise have to send it all again.
    if (!unknown.empty()) gApp.queueReply(conv, listed(TR(ROUTE_PARTIAL), unknown));
    if (!online_) {
      std::string n = route::fit(known, mc::MAX_TEXT_LEN - strlen(TR(ROUTE_QUEUED)));
      gApp.queueReply(conv, fmt(TR(ROUTE_QUEUED), n.substr(1)));  // the text has the first '@'
    }
  }
  return true;
}

bool TelegramBridge::routeInvite(const std::string& nameIn, std::string& code, std::string& user, std::string& err) {
  std::string name = route::fold(nameIn);
  if (!route::validName(name)) {
    err = "bad_route_name";
    return false;
  }
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    bool known = false;
    for (auto& r : gConfig.tgRoutes) known |= r.name == name;
    if (!known && gConfig.tgRoutes.size() >= route::MAX_ENTRIES) {
      err = "route_full";
      return false;
    }
  }
  {
    std::lock_guard<std::mutex> l(errMtx_);
    user = botUser_;
  }
  if (user.empty()) {  // the link needs the bot's name
    err = "tg_not_online";
    return false;
  }
  char c[20];
  snprintf(c, sizeof(c), "%08lx%08lx", (unsigned long)esp_random(), (unsigned long)esp_random());
  code = c;
  Lock l(mtx_);
  uint32_t now = millis();
  for (auto it = invites_.begin(); it != invites_.end();)
    it = it->name == name || now - it->at > ROUTE_INVITE_MS ? invites_.erase(it) : std::next(it);
  if (invites_.size() >= ROUTE_INVITES) invites_.erase(invites_.begin());
  invites_.push_back({name, code, now});
  return true;
}

void TelegramBridge::routeForget(const std::string& name) {
  Lock l(mtx_);
  for (auto it = invites_.begin(); it != invites_.end();) it = it->name == name ? invites_.erase(it) : std::next(it);
}

// "/start <code>" from an invitation link: this chat becomes a directory entry.
bool TelegramBridge::useInvite(const std::string& code, int64_t chat, int32_t thread, JsonObject msg) {
  std::string name;
  {
    Lock l(mtx_);
    uint32_t now = millis();
    for (auto it = invites_.begin(); it != invites_.end(); ++it) {
      if (it->code != code) continue;
      if (now - it->at <= ROUTE_INVITE_MS) name = it->name;
      invites_.erase(it);
      break;
    }
  }
  if (name.empty()) return false;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    auto& v = gConfig.tgRoutes;
    auto it = std::find_if(v.begin(), v.end(), [&](const TgRoute& r) { return r.name == name; });
    if (it != v.end()) it->chat = chat;
    else if (v.size() < route::MAX_ENTRIES) v.push_back({name, chat});
    else return false;
    gConfig.save();
  }
  LOGI("telegram: @%s linked to chat %lld", name.c_str(), (long long)chat);
  reply(chat, thread, fmt(TR(ROUTE_WELCOME), name));
  JsonDocument ev;
  ev["t"] = "tg_route_linked";
  ev["name"] = name;
  ev["chat"] = chat;
  ev["title"] = msg["chat"]["title"] | (msg["chat"]["first_name"] | "");
  gApp.emit(ev);
  return true;
}

bool TelegramBridge::inDirectory(int64_t chat) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  for (auto& r : gConfig.tgRoutes) if (r.chat == chat) return true;
  return false;
}

std::string TelegramBridge::routeNameOf(int64_t chat) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  if (!gConfig.tgRouteOn) return "";
  for (auto& r : gConfig.tgRoutes) if (r.chat == chat) return r.name;
  return "";
}

// Who a chat of the directory is answering: the author of the message it replies to, or without a
// reply (replyTo 0) the last person who wrote to it.
std::string TelegramBridge::routeConvFor(int64_t chat, int32_t replyTo) {
  Lock l(mtx_);
  for (auto it = routeMsgs_.rbegin(); it != routeMsgs_.rend(); ++it)
    if (it->chat == chat && (!replyTo || it->msgId == replyTo)) return it->conv;
  return "";
}

void TelegramBridge::rememberRoute(int64_t chat, int32_t msgId, const std::string& conv) {
  if (!msgId) return;
  Lock l(mtx_);
  routeMsgs_.push_back({chat, msgId, conv});
  while (routeMsgs_.size() > ROUTE_MSGS) routeMsgs_.pop_front();
  mapsDirty_ = true;
}

bool TelegramBridge::sendRouted(int64_t chat, int32_t msgId, const std::string& conv, const std::string& from, const std::string& text) {
  bool slow;
  {
    Lock l(mtx_);
    uint32_t now = millis();
    auto& r = routeRate_[chat];
    if (now - r.first > 60000) r = {now, 0};
    slow = r.second >= ROUTE_PER_MINUTE;
    if (!slow) r.second++;
  }
  if (slow) {  // outside the lock: reply() takes mtx_ itself
    reply(chat, 0, TR(ROUTE_SLOW), msgId);
    return false;
  }
  std::string line = "[" + from + "] " + text;
  size_t max = gApp.maxTextFor(conv);
  if (line.size() > max) {
    char b[120];
    snprintf(b, sizeof(b), TR(TOO_LONG), (unsigned)line.size(), (unsigned)max);
    reply(chat, 0, b, msgId);
    return false;
  }
  bool keep, approved;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    keep = gConfig.tgRouteKeep;
    auto& v = gConfig.tgRouteFrom;
    approved = std::find(v.begin(), v.end(), conv) != v.end();
  }
  if (!approved) {
    reply(chat, 0, TR(ROUTE_REVOKED), msgId);
    return false;
  }
  std::string hidden = "\xF0\x9F\x94\x92 [" + from + "]";
  SendResult r = gApp.sendText(conv, line, keep ? "telegram" : "route", 0, keep ? nullptr : &hidden);
  if (!r.ok) {
    reply(chat, 0, fmt(TR(UNKNOWN_CONTACT), esc(conv)), msgId);
    return false;
  }
  trackOutgoing(r.id, chat, msgId, true);
  if (!gApp.mesh().ready()) reply(chat, 0, TR(NOT_READY), msgId);
  return true;
}

// Waiting for the network, a message must not go to a chat that left the directory meanwhile.
bool TelegramBridge::routeStillValid(const Out& o) {
  std::lock_guard<std::recursive_mutex> l(gConfigMtx);
  if (!gConfig.tgRouteOn) return false;
  for (auto& r : gConfig.tgRoutes) if (r.name == o.routeName && r.chat == o.chat) return true;
  return false;
}

// A basic group became a supergroup: its id changed.
void TelegramBridge::migrateChat(int64_t from, int64_t to) {
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    bool known = false;
    for (auto& r : gConfig.tgRoutes) if (r.chat == from) { r.chat = to; known = true; }
    for (auto& c : gConfig.tgChats) if (c == from) { c = to; known = true; }
    if (!known) return;  // not ours, or already done: Telegram announces it in both chats
    gConfig.save();
  }
  LOGI("telegram: chat %lld is now %lld", (long long)from, (long long)to);
  Lock l(mtx_);
  for (auto& o : outbox_) if (o.chat == from) o.chat = to;
  for (auto& m : routeMsgs_) if (m.chat == from) m.chat = to;
  mapsDirty_ = true;
}

// ---------------------------------------------------------------- channel commands

static std::string lowerStr(std::string s) {
  for (auto& c : s) c = tolower((unsigned char)c);
  return s;
}

static std::string urlDecode(const std::string& s) {
  std::string o;
  for (size_t i = 0; i < s.size(); i++) {
    if (s[i] == '+') o += ' ';
    else if (s[i] == '%' && i + 2 < s.size() &&isxdigit((unsigned char)s[i + 1]) && isxdigit((unsigned char)s[i + 2])) {
      o += (char)strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
      i += 2;
    } else o += s[i];
  }
  return o;
}

static std::string urlEncode(const std::string& s) {
  static const char* hex = "0123456789ABCDEF";
  std::string o;
  for (unsigned char c : s) {
    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += (char)c;
    else {
      o += '%';
      o += hex[c >> 4];
      o += hex[c & 15];
    }
  }
  return o;
}

void TelegramBridge::channelError(int64_t chat, int32_t thread, const std::string& err) {
  const char* t = err == "not_ready" ? TR(CH_ERR_OFFLINE) : err == "channel_exists" ? TR(CH_ERR_EXISTS)
                : err == "no_free_slot" ? TR(CH_ERR_FULL) : err == "public_channel" ? TR(CH_ERR_PUBLIC)
                : err == "bad_name" ? TR(CH_ERR_NAME) : err == "bad_secret" ? TR(CH_ERR_KEY)
                : err == "channels_loading" ? TR(CH_ERR_RETRY) : nullptr;
  reply(chat, thread, t ? std::string(t) : fmt(TR(CH_REFUSED), esc(err)));
}

// /nouveaucanal #name | name | name <32 hex> | meshcore://channel/add?name=..&secret=..
void TelegramBridge::addChannel(int64_t chat, int32_t thread, const std::string& rawArgs) {
  std::string args = trim(rawArgs), name;
  if (args.empty()) {
    reply(chat, thread, TR(USAGE_ADDCH));
    return;
  }
  uint8_t key[16];
  bool hashtag = false;
  if (args.rfind("meshcore://channel/add", 0) == 0) {
    size_t q = args.find('?');
    bool haveKey = false;
    std::string qs = q == std::string::npos ? "" : args.substr(q + 1);
    for (size_t p = 0; p < qs.size();) {
      size_t e = qs.find('&', p);
      if (e == std::string::npos) e = qs.size();
      std::string kv = qs.substr(p, e - p);
      size_t eq = kv.find('=');
      std::string k = kv.substr(0, eq), v = eq == std::string::npos ? "" : kv.substr(eq + 1);
      if (k == "name") name = trim(urlDecode(v));
      else if (k == "secret") haveKey = mc::fromHex(lowerStr(v), key, 16);
      p = e + 1;
    }
    if (!haveKey) return channelError(chat, thread, "bad_secret");
  } else if (args[0] == '#') {
    name = args;
    hashtag = true;
  } else {
    size_t sp = args.find_last_of(" \t\n");
    if (sp != std::string::npos && args.size() - sp - 1 == 32) {
      if (!mc::fromHex(lowerStr(args.substr(sp + 1)), key, 16)) return channelError(chat, thread, "bad_secret");
      name = trim(args.substr(0, sp));
    } else {
      name = args;
      esp_fill_random(key, sizeof(key));
    }
  }
  std::string ok = fmt(TR(CH_ADDED), esc(name)) + "\n";
  if (hashtag) ok += TR(CH_ADDED_HASHTAG);
  else ok += std::string(TR(CH_ADDED_PRIVATE)) + "\n<code>" +
             esc("meshcore://channel/add?name=" + urlEncode(name) + "&secret=" + mc::toHex(key, 16)) + "</code>";
  uint32_t rid = gApp.newRid();
  trackCommand(rid, chat, thread, ok);  // before sending: the module may answer first
  std::string err;
  if (gApp.setChannel(-1, name, hashtag ? nullptr : key, rid, err) < 0) {
    {
      Lock l(mtx_);
      pendingCmds_.erase(rid);
    }
    channelError(chat, thread, err);
  }
}

// /supprcanal [channel]: asks with a button; the button carries the slot and a key prefix so a
// stale button can't delete another channel that took the slot since.
void TelegramBridge::deleteChannelAsk(int64_t chat, int32_t thread, const std::string& rawArgs) {
  std::string args = trim(rawArgs);
  JsonDocument kb;
  JsonArray rows = kb["inline_keyboard"].to<JsonArray>();
  auto button = [&](const mc::ChannelInfo& ch, const std::string& label) {
    JsonObject b = rows.add<JsonArray>().add<JsonObject>();
    b["text"] = mc::utf8Truncate(label, 60);
    b["callback_data"] = "cd|" + std::to_string(ch.idx) + "|" + mc::toHex(ch.secret, 4);
  };
  std::string text;
  if (args.empty()) {
    for (auto& ch : gApp.mesh().channels())
      if (ch.idx) button(ch, "\xF0\x9F\x97\x91 " + std::string(ch.name));
    if (rows.size() == 0) {
      reply(chat, thread, TR(NO_CHANNELS));
      return;
    }
    text = TR(PICK_DELCH);
  } else {
    ConvRef ref;
    bool amb = false;
    mc::ChannelInfo ch;
    if (!gApp.findConvByName(args, true, ref, amb) || !gApp.mesh().findChannel((uint8_t)atoi(ref.id.c_str() + 3), ch)) {
      reply(chat, thread, amb ? std::string(TR(AMBIGUOUS)) : fmt(TR(UNKNOWN_CHANNEL), esc(mc::utf8Truncate(args, 64))));
      return;
    }
    if (ch.idx == 0) return channelError(chat, thread, "public_channel");
    button(ch, TR(CH_DEL_BUTTON));
    text = fmt(TR(CH_DEL_CONFIRM), esc(ch.name));
  }
  std::string markup;
  serializeJson(kb, markup);
  reply(chat, thread, text, 0, markup);
}

// ---------------------------------------------------------------- maps (topics, reply routing)

void TelegramBridge::loadMaps() {
  File f = LittleFS.open(MAPS, "r");
  if (!f) return;
  JsonDocument d;
  if (deserializeJson(d, f)) {
    f.close();
    return;
  }
  f.close();
  Lock l(mtx_);
  for (JsonPair kv : d["topics"].as<JsonObject>()) topics_[kv.key().c_str()] = kv.value().as<int32_t>();
  for (JsonArray e : d["msgs"].as<JsonArray>()) {
    int32_t id = e[0];
    msgConv_[id] = e[1].as<const char*>();
    msgOrder_.push_back(id);
  }
  for (JsonArray e : d["routes"].as<JsonArray>()) routeMsgs_.push_back({e[0].as<int64_t>(), e[1].as<int32_t>(), e[2] | ""});
  updateOffset_ = d["offset"] | 0LL;
  botId_ = d["bot"] | 0LL;
}

void TelegramBridge::saveMaps() {
  static std::mutex fileMtx;  // poll and send tasks both save
  std::lock_guard<std::mutex> fl(fileMtx);
  JsonDocument d;
  {
    Lock l(mtx_);
    if (!mapsDirty_) return;
    JsonObject t = d["topics"].to<JsonObject>();
    for (auto& kv : topics_) t[kv.first] = kv.second;
    JsonArray m = d["msgs"].to<JsonArray>();
    for (auto id : msgOrder_) {
      auto it = msgConv_.find(id);
      if (it == msgConv_.end()) continue;
      JsonArray e = m.add<JsonArray>();
      e.add(id);
      e.add(it->second);
    }
    JsonArray rt = d["routes"].to<JsonArray>();
    for (auto& r : routeMsgs_) {
      JsonArray e = rt.add<JsonArray>();
      e.add(r.chat);
      e.add(r.msgId);
      e.add(r.conv);
    }
    d["offset"] = updateOffset_;
    d["bot"] = botId_;
    mapsDirty_ = false;
  }
  File f = LittleFS.open(MAPS, "w");
  if (!f) return;
  serializeJson(d, f);
  f.close();
  lastMapSave_ = millis();
}

void TelegramBridge::rememberMapping(int32_t msgId, const std::string& conv) {
  if (!msgId || conv.empty() || conv[0] == '#') return;
  Lock l(mtx_);
  msgConv_[msgId] = conv;
  msgOrder_.push_back(msgId);
  while (msgOrder_.size() > 300) {
    msgConv_.erase(msgOrder_.front());
    msgOrder_.pop_front();
  }
  mapsDirty_ = true;
}

std::string TelegramBridge::convForMessage(int32_t msgId) {
  Lock l(mtx_);
  auto it = msgConv_.find(msgId);
  return it == msgConv_.end() ? std::string() : it->second;
}

std::string TelegramBridge::convForTopic(int32_t thread) {
  Lock l(mtx_);
  for (auto& kv : topics_) if (kv.second == thread) return kv.first;
  return "";
}

int32_t TelegramBridge::topicFor(WiFiClientSecure& cli, int64_t chat, const std::string& conv, const std::string& title, uint32_t gen) {
  {
    Lock l(mtx_);
    auto it = topics_.find(conv);
    if (it != topics_.end()) return it->second;
  }
  // A chat without topics (private chat, group without "Topics"): don't ask for every message.
  if (topicsFailedAt_ && millis() - topicsFailedAt_ < 600000UL) return 0;
  JsonDocument req, resp;
  req["chat_id"] = chat;
  req["name"] = mc::utf8Truncate(title, 120);
  std::string body;
  serializeJson(req, body);
  std::string err;
  int code = call(cli, token(), "createForumTopic", body, resp, 15000, nullptr, &err);
  if (code != 200) {
    topicsFailedAt_ = millis() ? millis() : 1;
    LOGW("telegram: cannot create topic (%s): messages go to the main chat with their conversation name",
         err.c_str());
    return 0;
  }
  topicsFailedAt_ = 0;
  int32_t id = resp["result"]["message_thread_id"] | 0;
  Lock l(mtx_);
  if (gen != forgetGen_) return id;  // conversation deleted meanwhile: don't hand this topic to its successor
  topics_[conv] = id;
  mapsDirty_ = true;
  return id;
}

// Topics are otherwise created by the first relayed message: a channel nobody has written in
// yet would have no topic to write to from Telegram.
void TelegramBridge::seedChannelTopics(WiFiClientSecure& cli) {
  seedTopics_ = false;
  int64_t chat = mainChat();
  if (!chat) return;
  uint32_t gen;
  {
    Lock l(mtx_);
    gen = forgetGen_;
  }
  for (auto& ch : gApp.mesh().channels()) {
    std::string conv = App::convForChannel(ch.idx);
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      if (gConfig.tgMuted(conv)) continue;
    }
    {
      Lock l(mtx_);
      if (topics_.count(conv)) continue;
    }
    if (!topicFor(cli, chat, conv, std::string("\xF0\x9F\x93\xA2 ") + ch.name, gen)) {
      seedTopics_ = true;  // refused: topicFor itself waits 10 min before asking Telegram again
      return;
    }
    LOGI("telegram: topic created for %s", ch.name);
    saveMaps();  // at once: a topic forgotten by a restart would be created twice
    vTaskDelay(pdMS_TO_TICKS(1100));  // stay under the per-chat rate limit
  }
}

// ---------------------------------------------------------------- send task

// Sends the next queued item on the poll task's connection: one TLS session for everything,
// two would not fit in RAM next to BLE and a few hundred contacts on a board without PSRAM.
// Returns false when the queue is empty or the network failed (stop draining for now).
bool TelegramBridge::sendNext(WiFiClientSecure& cli) {
  Out o;
  uint32_t gen;
  {
  Lock l(mtx_);
  if (outbox_.empty()) return false;
  o = outbox_.front();
  gen = forgetGen_;
  }
  if (!o.route.empty() && !routeStillValid(o)) {
    {
      Lock l(mtx_);
      if (!outbox_.empty() && outbox_.front().seq == o.seq) outbox_.pop_front();
    }
    gApp.queueReply(o.route, fmt(TR(ROUTE_FAILED), o.routeName));
    return true;
  }
  // rate limits: 1 message / s per chat, ~25 / s overall
  uint32_t now = millis();
  uint32_t& last = lastSendPerChat_[o.chat];
  if (o.kind == Out::Message && now - last < 1050) {
    vTaskDelay(pdMS_TO_TICKS(1050 - (now - last)));
  }
  if (millis() - lastSendAny_ < 40) vTaskDelay(pdMS_TO_TICKS(40));
  if (!cli.connected()) setupClient(cli);

  JsonDocument req, resp;
  const char* method = "sendMessage";
  req["chat_id"] = o.chat;
  if (o.kind == Out::Message) {
    req["text"] = o.text;
    req["parse_mode"] = "HTML";
    req["link_preview_options"]["is_disabled"] = true;
    int32_t thread = 0;
    if (o.conv.rfind("#thread:", 0) == 0 && !o.noThread) {
      thread = atol(o.conv.c_str() + 8);
    } else if (topicsMode() && !o.conv.empty() && !o.noThread) {
      ConvRef ref;
      std::string title = o.conv;
      if (gApp.resolveConv(o.conv, ref)) {
        const char* icon = ref.kind == 1 ? "\xF0\x9F\x93\xA2 " : ref.kind == 3 ? "\xF0\x9F\x8F\xA0 " : ref.kind == 2 ? "\xF0\x9F\x93\xA1 " : "\xF0\x9F\x91\xA4 ";
        title = icon + ref.name;
      }
      thread = topicFor(cli, o.chat, o.conv, title, gen);
    }
    if (thread) req["message_thread_id"] = thread;
    else if (!o.flat.empty()) req["text"] = o.flat;  // no topic: say which conversation it is
    if (o.replyTo) {
      req["reply_parameters"]["message_id"] = o.replyTo;
      req["reply_parameters"]["allow_sending_without_reply"] = true;
    }
    if (!o.markup.empty()) req["reply_markup"] = serialized(o.markup);
  } else if (o.kind == Out::Reaction) {
    method = "setMessageReaction";
    req["message_id"] = o.msgId;
    JsonObject r = req["reaction"].to<JsonArray>().add<JsonObject>();
    r["type"] = "emoji";
    r["emoji"] = o.text;
  } else {
    method = "answerCallbackQuery";
    req.clear();
    req["callback_query_id"] = o.text;
  }
  std::string body;
  serializeJson(req, body);
  JsonDocument filter;
  filter["ok"] = true;
  filter["description"] = true;
  filter["parameters"]["retry_after"] = true;
  filter["parameters"]["migrate_to_chat_id"] = true;
  filter["result"]["message_id"] = true;
  std::string err;
  int code = call(cli, token(), method, body, resp, 15000, &filter, &err);
  lastSendAny_ = millis();
  if (o.kind == Out::Message) lastSendPerChat_[o.chat] = lastSendAny_;

  bool pop = false, netErr = false;
  if (code == 200) {
    pop = true;
    online_ = true;
    if (o.kind == Out::Message) LOGI("telegram: sent (%s)", o.conv.empty() ? "notice" : o.conv.c_str());
    bool forgotten;
    {
      Lock l(mtx_);
      forgotten = gen != forgetGen_;
    }
    if (o.kind == Out::Message && !forgotten) rememberMapping(resp["result"]["message_id"] | 0, o.conv);
    if (o.kind == Out::Message && !o.route.empty()) rememberRoute(o.chat, resp["result"]["message_id"] | 0, o.route);
  } else if (code == 429) {
    uint32_t ra = resp["parameters"]["retry_after"] | 5;
    LOGW("telegram rate limited, waiting %u s", (unsigned)ra);
    vTaskDelay(pdMS_TO_TICKS(ra * 1000));
  } else if (code == 401 || code == 404) {
    // token revoked or wrong (404: malformed): keep the message, the poll task checks the token again
    tokenOk_ = false;
    reconfig_ = true;
    online_ = false;
    netErr = true;
  } else if (code >= 400 && code < 500) {
    // bad request: deleted topic, bad reaction, blocked bot... don't block the queue
    LOGW("telegram %s refused: %s", method, err.c_str());
    int64_t moved = resp["parameters"]["migrate_to_chat_id"] | 0LL;
    if (moved) {
      migrateChat(o.chat, moved);  // the item stays in front, with its new chat
      return true;
    }
    if (err.find("thread not found") != std::string::npos && !o.noThread) {
      // Topic deleted by the user, or topics not usable in this chat: forget the topic and
      // resend once without it (with the conversation name in front) instead of losing it.
      Lock l(mtx_);
      topics_.erase(o.conv);
      mapsDirty_ = true;
      o.noThread = true;
    } else {
      pop = true;
    }
    if (++o.tries > 2) pop = true;
  } else {
    online_ = false;  // network: keep the message (order preserved), retried after the next poll
    netErr = true;
  }
  {
    Lock l(mtx_);
    // The queue may have dropped this item meanwhile (full): only touch it if it is still in front.
    if (!outbox_.empty() && outbox_.front().seq == o.seq) {
      if (pop) outbox_.pop_front();
      else {
        outbox_.front().tries = o.tries;
        outbox_.front().noThread |= o.noThread;  // never undo a concurrent forgetConversation
      }
    }
  }
  // Blocked bot, left group...: its author must not think it arrived.
  if (pop && code != 200 && !o.route.empty()) gApp.queueReply(o.route, fmt(TR(ROUTE_FAILED), o.routeName));
  return !netErr;
}

// ---------------------------------------------------------------- poll task

void TelegramBridge::pollTaskFn(void* arg) { static_cast<TelegramBridge*>(arg)->pollLoop(); }

// Long-poll session that reconfigure() can cut short: HTTPClient waits for the answer by polling
// connected(), so reporting "disconnected" ends a 30-50 s getUpdates at once, without touching the
// socket from another task. Only once the request is written: before that, HTTPClient would
// reconnect over the live session.
class AbortableClient : public WiFiClientSecure {
 public:
  explicit AbortableClient(const volatile bool* abort) : abort_(abort) {}
  void arm(bool on) {
    armed_ = on;
    sent_ = false;
  }
  uint8_t connected() override { return armed_ && sent_ && *abort_ ? 0 : WiFiClientSecure::connected(); }
  size_t write(const uint8_t* buf, size_t size) override {
    size_t n = WiFiClientSecure::write(buf, size);
    sent_ = true;
    return n;
  }

 private:
  const volatile bool* abort_;
  bool armed_ = false, sent_ = false;
};

void TelegramBridge::registerCommands(WiFiClientSecure& cli) {
  int lg = lang();
  struct C { const char* fr; const char* en; tgs::Id d; };
  static const C cmds[] = {{"msg", "msg", tgs::CMD_MSG},       {"ch", "ch", tgs::CMD_CH},
                           {"public", "public", tgs::CMD_PUBLIC}, {"contacts", "contacts", tgs::CMD_CONTACTS},
                           {"canaux", "channels", tgs::CMD_CHANNELS}, {"nouveaucanal", "addchannel", tgs::CMD_ADDCH},
                           {"supprcanal", "delchannel", tgs::CMD_DELCH}, {"status", "status", tgs::CMD_STATUS},
                           {"advert", "advert", tgs::CMD_ADVERT}, {"login", "login", tgs::CMD_LOGIN},
                           {"sourdine", "mute", tgs::CMD_MUTE},   {"aide", "help", tgs::CMD_HELP}};
  JsonDocument req, resp;
  JsonArray a = req["commands"].to<JsonArray>();
  for (auto& c : cmds) {
    JsonObject o = a.add<JsonObject>();
    o["command"] = lg == 0 ? c.fr : c.en;
    o["description"] = tgs::T[c.d][lg];
  }
  std::string body;
  serializeJson(req, body);
  call(cli, token(), "setMyCommands", body, resp, 15000);
}

// Back-off waits end early when the web app asks for a token test: it gives up after 20 s.
void TelegramBridge::waitOrTest(uint32_t ms) {
  // Only the poll task of small boards runs the test; roomy boards keep their back-off.
  for (uint32_t t = 0; t < ms && (roomy_ || !testPending_); t += 250) vTaskDelay(pdMS_TO_TICKS(250));
}

// Token check from the web app, on its own short-lived TLS session.
void TelegramBridge::runTokenTest() {
  testPending_ = false;
  JsonDocument resp, ev;
  if (!Net::online()) {
    ev["ok"] = false;
    ev["err"] = "no_internet";
  } else {
    std::string tok;
    {
      Lock l(mtx_);
      tok = testToken_;
    }
    WiFiClientSecure tcli;
    setupClient(tcli);
    std::string err;
    int code = call(tcli, tok, "getMe", "", resp, 15000, nullptr, &err);
    tcli.stop();
    ev["ok"] = code == 200;
    if (code == 200) {
      ev["name"] = resp["result"]["first_name"];
      ev["user"] = resp["result"]["username"];
    } else {
      ev["err"] = code == 401 || code == 404 ? "bad_token" : (code < 0 ? "network" : err.c_str());
    }
  }
  {
    Lock l(mtx_);
    test_.seq++;
    test_.busy = testPending_;  // a newer test may already be queued
    test_.ok = ev["ok"] | false;
    test_.name = ev["name"] | "";
    test_.user = ev["user"] | "";
    test_.err = ev["err"] | "";
  }
  ev["t"] = "tg_test";
  gApp.emit(ev);
}

void TelegramBridge::pollLoop() {
  AbortableClient cli{&reconfig_};  // braces: cli() is an Arduino macro
  JsonDocument filter;
  JsonObject fu = filter["result"].add<JsonObject>();
  fu["update_id"] = true;
  JsonObject fm = fu["message"].to<JsonObject>();
  fm["message_id"] = true;
  fm["message_thread_id"] = true;
  fm["is_topic_message"] = true;
  fm["chat"]["id"] = true;
  fm["chat"]["title"] = true;
  fm["chat"]["first_name"] = true;
  fm["chat"]["type"] = true;
  fm["from"]["first_name"] = true;
  fm["migrate_to_chat_id"] = true;
  fm["migrate_from_chat_id"] = true;
  fm["text"] = true;
  fm["caption"] = true;
  fm["reply_to_message"]["message_id"] = true;
  JsonObject fc = fu["callback_query"].to<JsonObject>();
  fc["id"] = true;
  fc["data"] = true;
  fc["message"]["message_id"] = true;
  fc["message"]["message_thread_id"] = true;
  fc["message"]["chat"]["id"] = true;
  filter["ok"] = true;
  filter["description"] = true;

  bool single = false;  // getUpdates one at a time after an update that did not fit in RAM
  uint32_t backoff = 2000;
  for (;;) {
    // small boards: here, after closing the poll session (never two TLS sessions at once);
    // roomy boards: in the send task, which is not stuck in a 30 s long poll
    if (testPending_ && !roomy_) {
      cli.stop();
      runTokenTest();
    }
    if (!enabled() || !Net::online()) {
      online_ = false;
      lastAlive_ = millis();
      if (cli.connected()) cli.stop();
      vTaskDelay(pdMS_TO_TICKS(1000));
      continue;
    }
    // The web app has priority: on a board without PSRAM there is not enough RAM for a TLS session
    // and the pages at the same time. Messages stay queued and go out once the app is idle.
    uint32_t lastWeb = Web::lastRequestMs();
    if (!roomy_ && lastWeb && millis() - lastWeb < WEB_PRIORITY_MS) {
      lastAlive_ = millis();  // paused on purpose, not stuck
      if (cli.connected()) cli.stop();
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    // Fragmentation only gets worse: once the bridge has worked since boot, a restart is better
    // than a Telegram that stays silent for good. Only when RAM is the cause (an Internet outage
    // must not reboot the Hub and drop the queued messages).
    if (everOnline_ && ramStarved_ && millis() - lastAlive_ > LOW_RAM_REBOOT_MS) {
      LOGE("telegram: no RAM for TLS for %u min, restarting", (unsigned)(LOW_RAM_REBOOT_MS / 60000));
      saveMaps();
      vTaskDelay(pdMS_TO_TICKS(500));
      ESP.restart();
    }
    // Roomy boards keep the TLS buffers in PSRAM: only a little internal RAM (sockets) is needed.
    if (!cli.connected() && !(roomy_ ? ESP.getFreeHeap() >= 16 * 1024 : tlsFits())) {
      // TLS would take the last RAM: wait, but say so (this used to block the bridge silently).
      ramStarved_ = true;
      if (mapsDirty_ && millis() - lastMapSave_ > 30000) saveMaps();  // don't lose new topics
      online_ = false;
      setError(lang() == 0 ? "mémoire du Hub insuffisante, nouvel essai en cours" : "Hub low on memory, retrying");
      static uint32_t lastLowLog = 0;
      if (millis() - lastLowLog > 60000UL) {
        lastLowLog = millis();
        size_t q;
        {
          Lock l(mtx_);
          q = outbox_.size();
        }
        LOGW("telegram: waiting for RAM (%u kB free, largest block %u kB), %u message(s) queued",
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024), (unsigned)q);
      }
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    static uint32_t lastGetMe = 0;
    // Topics switched on in @BotFather later: look again every 5 min, on the open session (a full
    // reconfiguration would reconnect and re-register the commands each time).
    if (!reconfig_ && topicsMode() && !topicsEnabled_ && millis() - lastGetMe > 300000UL) {
      lastGetMe = millis();
      JsonDocument r, f;
      f["result"]["has_topics_enabled"] = true;
      if (call(cli, token(), "getMe", "", r, 15000, &f) == 200 && (r["result"]["has_topics_enabled"] | false)) {
        topicsEnabled_ = true;
        topicsFailedAt_ = 0;
        seedTopics_ = true;
        LOGI("telegram: topics in private chat enabled");
      }
    }
    if (reconfig_) {
      lastGetMe = millis();
      reconfig_ = false;
      uint32_t gen = sendGen_;
      cli.stop();
      setupClient(cli);
      JsonDocument resp;
      std::string err;
      int code = call(cli, token(), "getMe", "", resp, 15000, nullptr, &err);
      if (gen != sendGen_) continue;  // settings changed again meanwhile: this answer may be the old bot's
      if (code == 200) {
        // update ids are per bot: an offset saved for another bot would confirm (drop) the new
        // bot's updates, /start included. Topics are kept: still valid in a group, and a topic
        // the new bot cannot use is forgotten by sendNext ("thread not found").
        int64_t id = resp["result"]["id"] | 0LL;
        bool newBot = false;
        {
          Lock l(mtx_);
          if (id && id != botId_) {
            newBot = botId_ != 0;
            if (newBot) {
              updateOffset_ = 0;
              msgConv_.clear();
              msgOrder_.clear();
              routeMsgs_.clear();
            }
            botId_ = id;
            mapsDirty_ = true;
          }
        }
        if (newBot) {
          LOGI("telegram: new bot, update offset and reply map reset");
          saveMaps();
        }
        std::string user;
        {
          std::lock_guard<std::mutex> l(errMtx_);
          botName_ = std::string(resp["result"]["first_name"] | "");
          botUser_ = std::string(resp["result"]["username"] | "");
          lastError_.clear();
          user = botUser_;
        }
        // Bot API 9.3+: topics in the private chat with the bot (enabled in @BotFather)
        topicsEnabled_ = resp["result"]["has_topics_enabled"] | false;
        if (topicsEnabled_) topicsFailedAt_ = 0;
        LOGI("telegram: topics in private chat %s", topicsEnabled_ ? "enabled" : "disabled (enable them in @BotFather)");
        online_ = true;
        // reconfigure() during the call: the answer may be for the old token, check again
        if (gen != sendGen_) reconfig_ = true;
        else tokenOk_ = true;
        registerCommands(cli);
        LOGI("telegram: bot @%s ready", user.c_str());
      } else {
        LOGW("telegram: getMe failed (%d %s) - heap %u kB, largest block %u kB", code, err.c_str(),
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
        ramStarved_ = tlsNoRam_;
        if (code == 401 || code == 404) tokenOk_ = false;
        reconfig_ = true;
        online_ = false;
        waitOrTest(backoff);
        backoff = backoff < 60000 ? backoff * 2 : 60000;
        continue;
      }
    }
    if (!roomy_) {
      // outgoing messages first, on the same TLS connection
      for (int k = 0; k < 20 && sendNext(cli); k++) {
      }
      if (seedTopics_ && topicsEnabled_ && topicsMode() && gApp.mesh().ready() && gApp.mesh().channelsComplete())
        seedChannelTopics(cli);
    }
    if (mapsDirty_ && millis() - lastMapSave_ > 30000) saveMaps();
    size_t pending;
    {
      Lock l(mtx_);
      pending = outbox_.size();
    }
    // Short long-poll on small boards: a MeshCore message waiting to be relayed goes out within
    // POLL_SECONDS. With a separate sender the poll can wait much longer (one request per 30 s,
    // 50 s on 4G only), an incoming Telegram message still arrives at once.
    int wait = roomy_ ? (Net::staConnected() ? 30 : 50) : (pending ? 0 : POLL_SECONDS);
    char q[180];
    snprintf(q, sizeof(q), "getUpdates?offset=%lld&timeout=%d&limit=%d&allowed_updates=%%5B%%22message%%22%%2C%%22callback_query%%22%%5D",
             (long long)updateOffset_, wait, single ? 1 : 10);
    JsonDocument resp;
    std::string err;
    cli.arm(true);
    int code = call(cli, token(), q, "", resp, (wait + 10) * 1000, &filter, &err);
    if (code == 200 && err == "bad json") {
      // An update too big to parse would come back on every poll and block the bridge: fetch them
      // one at a time, and skip (by its id only) the one that still does not fit.
      if (!single) {
        single = true;
        continue;
      }
      JsonDocument idf, ids;
      idf["result"][0]["update_id"] = true;
      snprintf(q, sizeof(q), "getUpdates?offset=%lld&timeout=0&limit=1", (long long)updateOffset_);
      if (call(cli, token(), q, "", ids, 15000, &idf) == 200) {
        int64_t uid = ids["result"][0]["update_id"] | (int64_t)0;
        if (uid >= updateOffset_) {
          LOGW("telegram: update %lld too large for the RAM, skipped", (long long)uid);
          Lock l(mtx_);
          updateOffset_ = uid + 1;
          mapsDirty_ = true;
        }
      }
      continue;
    }
    if (code == 200) single = false;
    cli.arm(false);
    if (code != 200 && reconfig_) continue;  // cut short by reconfigure(): getMe with the new settings
    if (code == 409) {  // a webhook is set: long polling needs it removed
      JsonDocument r2;
      call(cli, token(), "deleteWebhook", "", r2, 15000);
      vTaskDelay(pdMS_TO_TICKS(3000));  // also covers another bot instance polling the same token
      continue;
    }
    if (code != 200) {
      online_ = false;
      ramStarved_ = tlsNoRam_;
      static uint32_t lastFailLog = 0;
      if (millis() - lastFailLog > 60000UL) {  // never fail silently, but don't flood the log
        lastFailLog = millis();
        LOGW("telegram: getUpdates failed (%d %s) - heap %u kB, largest block %u kB", code, err.c_str(),
             (unsigned)(ESP.getFreeHeap() / 1024), (unsigned)(ESP.getMaxAllocHeap() / 1024));
      }
      if (code == 401) {
        LOGE("telegram: invalid token");
        tokenOk_ = false;  // revoked: hold the outbox until getMe succeeds again
        reconfig_ = true;
        waitOrTest(60000);
      } else {
        waitOrTest(backoff);
        backoff = backoff < 60000 ? backoff * 2 : 60000;
      }
      continue;
    }
    online_ = true;
    everOnline_ = true;
    ramStarved_ = false;
    lastAlive_ = millis();
    backoff = 2000;
    size_t got = 0;
    for (JsonObject u : resp["result"].as<JsonArray>()) {
      int64_t uid = u["update_id"].as<int64_t>();
      if (uid >= updateOffset_) updateOffset_ = uid + 1;
      handleUpdate(u);
      got++;
      Lock l(mtx_);
      mapsDirty_ = true;
    }
    // Duty cycle: close the TLS session (~45 kB) and leave the RAM to the web server for a
    // few seconds, unless a conversation is going on. The web app cannot always signal itself
    // (with no RAM left it cannot even accept the request that would ask Telegram to yield).
    size_t queued;
    {
      Lock l(mtx_);
      queued = outbox_.size();
    }
    if (!roomy_ && !got && !queued) {
      cli.stop();
      vTaskDelay(pdMS_TO_TICKS(IDLE_GAP_MS));
    }
  }
}

void TelegramBridge::sendTaskFn(void* arg) { static_cast<TelegramBridge*>(arg)->sendLoop(); }

// Roomy boards: drains the outbox on its own TLS session, as soon as enqueue() wakes it.
void TelegramBridge::sendLoop() {
  WiFiClientSecure cli;
  uint32_t gen = sendGen_;
  uint32_t idleWait = 1000;
  for (;;) {
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(idleWait));
    idleWait = 1000;
    if (testPending_) runTokenTest();  // also while the bridge is off (setup wizard)
    if (gen != sendGen_) {  // token or settings changed
      gen = sendGen_;
      cli.stop();
    }
    // tokenOk_: the poll task's getMe has checked the token and read the bot's topic setting
    if (!enabled() || !Net::online() || !tokenOk_) {
      if (cli.connected()) cli.stop();
      continue;
    }
    bool ok = true;
    for (int k = 0; k < 20 && (ok = sendNext(cli)); k++) {
    }
    size_t left;
    {
      Lock l(mtx_);
      left = outbox_.size();
    }
    if (!ok && left) idleWait = 5000;  // network trouble: retry a bit later, the queue is kept
    else if (left) idleWait = 0;       // more than 20 queued: go on at once
    if (seedTopics_ && topicsEnabled_ && topicsMode() && gApp.mesh().ready() && gApp.mesh().channelsComplete())
      seedChannelTopics(cli);
    if (mapsDirty_ && millis() - lastMapSave_ > 30000) saveMaps();
  }
}

void TelegramBridge::handleUpdate(JsonObject u) {
  if (u["message"].is<JsonObject>()) handleMessage(u["message"]);
  else if (u["callback_query"].is<JsonObject>()) handleCallback(u["callback_query"]);
}

void TelegramBridge::handleMessage(JsonObject msg) {
  int64_t chat = msg["chat"]["id"].as<int64_t>();
  int32_t msgId = msg["message_id"] | 0;
  int32_t thread = (msg["is_topic_message"] | false) ? (msg["message_thread_id"] | 0) : 0;
  std::string text = msg["text"] | (msg["caption"] | "");
  int64_t movedTo = msg["migrate_to_chat_id"] | 0LL, movedFrom = msg["migrate_from_chat_id"] | 0LL;
  if (movedTo || movedFrom) {  // service message: a group became a supergroup
    if (movedTo) migrateChat(chat, movedTo);
    else migrateChat(movedFrom, chat);
    return;
  }
  // Only a plain /start (or the wizard's "/start hub") can link the main chat: an invitation
  // gives a directory entry, never the commands of the Hub, even when it is no longer valid.
  bool claimable = false;
  if (text.rfind("/start", 0) == 0) {
    size_t sp = text.find(' ');
    std::string param = sp == std::string::npos ? "" : trim(text.substr(sp + 1));
    claimable = param.empty() || param == "hub";
    if (!claimable) {
      if (useInvite(param, chat, thread, msg)) return;
      if (route::looksLikeInvite(param)) {
        if (!lastExpiredReply_ || millis() - lastExpiredReply_ > 60000) {
          lastExpiredReply_ = millis();
          reply(chat, thread, TR(ROUTE_EXPIRED), msgId);
        }
        return;
      }
    }
  }
  std::string entry = routeNameOf(chat);
  bool admin = allowed(chat), listed = inDirectory(chat);
  bool priv = strcmp(msg["chat"]["type"] | "", "private") == 0;
  int32_t to = msg["reply_to_message"]["message_id"] | 0;
  // A person of the directory only becomes an admin when the owner asked for a new chat just now.
  bool open;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    open = gConfig.tgChats.empty();
  }
  bool canClaim = !admin && claimable && (claimActive() || (open && !listed));
  if (!entry.empty()) {
    std::string from = entry;
    std::string first = msg["from"]["first_name"] | "";
    if (!priv && !first.empty()) from += "/" + mc::utf8Truncate(first, 16);
    if (!text.empty() && text[0] != '/' && (to || priv)) {
      // In a group only a reply counts: the rest is the group's own conversation.
      std::string conv = (to || !admin) ? routeConvFor(chat, to) : "";
      if (!conv.empty()) {
        sendRouted(chat, msgId, conv, from, text);
        return;
      }
    }
    if (!admin && !canClaim) {  // a directory entry has no other rights than writing to approved people
      std::string bot, cmd, args;
      {
        std::lock_guard<std::mutex> l(errMtx_);
        bot = botUser_;
      }
      if (route::command(text, bot, cmd, args) && cmd == "msg") routeWrite(chat, thread, msgId, from, args);
      else if (priv) reply(chat, thread, TR(ROUTE_HINT), msgId);
      return;
    }
  }
  if (!admin) {
    if (canClaim) {
      {
        // Appended: the wizard asks for a replacement with tg_claim {reset:true}, which empties the list first.
        std::lock_guard<std::recursive_mutex> l(gConfigMtx);
        gConfig.tgChats.push_back(chat);
        gConfig.save();
      }
      claiming_ = false;
      LOGI("telegram: linked to chat %lld", (long long)chat);
      reply(chat, thread, TR(WELCOME));
      JsonDocument ev;
      ev["t"] = "tg_claimed";
      ev["chat"] = chat;
      ev["title"] = msg["chat"]["title"] | (msg["chat"]["first_name"] | "");
      ev["type"] = msg["chat"]["type"] | "";
      gApp.emit(ev);
    } else if (listed && !text.empty()) {
      std::string bot, cmd, args;
      {
        std::lock_guard<std::mutex> l(errMtx_);
        bot = botUser_;
      }
      if (priv || to || (route::command(text, bot, cmd, args) && cmd == "msg"))
        reply(chat, thread, TR(ROUTE_OFF), msgId);  // routing is off: the answer goes nowhere
    }
    return;  // everybody else is ignored
  }
  if (text.empty()) return;
  if (text[0] == '/') {
    size_t sp = text.find(' ');
    std::string cmd = text.substr(1, sp == std::string::npos ? std::string::npos : sp - 1);
    size_t at = cmd.find('@');
    if (at != std::string::npos) cmd.resize(at);
    for (auto& c : cmd) c = tolower(c);
    std::string args = sp == std::string::npos ? "" : text.substr(sp + 1);
    handleCommand(chat, thread, msgId, cmd, args);
    return;
  }
  int32_t replyTo = msg["reply_to_message"]["message_id"] | 0;
  // Message ids are per chat, and these two maps only describe the main chat.
  bool isMain = chat == mainChat();
  std::string conv = replyTo && isMain ? convForMessage(replyTo) : "";
  // A reply to the "write now" prompt wins over the topic it was asked in. In a topic, a plain message
  // may point at the first message of the topic, whose id is the thread id.
  if (conv.empty() && replyTo && replyTo != thread && entry.empty()) {
    Lock l(mtx_);
    auto it = pendingTarget_.find(chat);
    if (it != pendingTarget_.end()) {
      conv = it->second;
      pendingTarget_.erase(it);
    }
  }
  if (conv.empty() && thread && isMain) conv = convForTopic(thread);
  if (conv.empty() && (!replyTo || entry.empty())) {  // a reply to a routed message that is too old: say so
    Lock l(mtx_);
    auto it = pendingTarget_.find(chat);
    if (it != pendingTarget_.end()) {
      conv = it->second;
      pendingTarget_.erase(it);
    }
  }
  if (conv.empty()) {
    reply(chat, thread, thread ? TR(TOPIC_GENERAL_HINT) : TR(REPLY_HINT), msgId);
    return;
  }
  sendToConv(chat, msgId, conv, text);
}

// A directory entry writing first, only to the contacts approved for routing. That person may have
// never written to it: the message says how to answer, a plain answer would reach the main chat.
void TelegramBridge::routeWrite(int64_t chat, int32_t thread, int32_t msgId, const std::string& from, const std::string& args) {
  std::vector<std::string> approved;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    approved = gConfig.tgRouteFrom;
  }
  ConvRef ref;
  std::string body;
  bool amb = false;
  if (!args.empty() && gApp.splitTarget(args, false, ref, body, amb, &approved) && !body.empty()) {
    std::string name = from.substr(0, from.find('/'));  // "equipe/Paul": the directory entry
    // Next plain messages of a private chat, and replies to this one, go to this person.
    if (sendRouted(chat, msgId, ref.id, from, body + fmt(TR(ROUTE_ANSWER), name))) rememberRoute(chat, msgId, ref.id);
    return;
  }
  std::string names;
  gApp.mesh().forEachContact([&](const mc::Contact& c) {
    if (std::find(approved.begin(), approved.end(), App::convForKey(c.pubKey)) == approved.end()) return;
    names += (names.empty() ? "" : ", ") + esc(c.name);
  });
  reply(chat, thread, fmt(TR(ROUTE_WRITE_HELP), names.empty() ? "-" : names), msgId);
}

// Without privacy mode turned off, a bot only sees the replies to it in a group: ask for one, or
// the text never reaches the Hub. Not in a directory chat, where a reply means its routed message.
void TelegramBridge::askText(int64_t chat, int32_t thread, const std::string& name) {
  bool group = chat < 0 && routeNameOf(chat).empty();
  reply(chat, thread, fmt(TR(NOW_WRITE), "<b>" + esc(name) + "</b>"), 0, group ? "{\"force_reply\":true}" : "");
}

void TelegramBridge::sendToConv(int64_t chat, int32_t msgId, const std::string& conv, const std::string& text) {
  size_t max = gApp.maxTextFor(conv);
  if (text.size() > max) {
    char b[120];
    snprintf(b, sizeof(b), TR(TOO_LONG), (unsigned)text.size(), (unsigned)max);
    reply(chat, 0, b, msgId);
    return;
  }
  SendResult r = gApp.sendText(conv, text, "telegram");
  if (!r.ok) {
    reply(chat, 0, fmt(TR(UNKNOWN_CONTACT), esc(conv)), msgId);
    return;
  }
  trackOutgoing(r.id, chat, msgId);
  if (!gApp.mesh().ready()) reply(chat, 0, TR(NOT_READY), msgId);
}

void TelegramBridge::handleCallback(JsonObject cb) {
  int64_t chat = cb["message"]["chat"]["id"].as<int64_t>();
  int32_t thread = cb["message"]["message_thread_id"] | 0;
  Out ans;
  ans.kind = Out::Answer;
  ans.chat = chat;
  ans.text = cb["id"] | "";
  enqueue(ans);
  if (!allowed(chat)) return;
  std::string data = cb["data"] | "";
  if (data.rfind("to|", 0) == 0) {
    std::string conv = data.substr(3);
    ConvRef ref;
    if (!gApp.resolveConv(conv, ref)) return;
    {
      Lock l(mtx_);
      pendingTarget_[chat] = conv;
    }
    askText(chat, thread, ref.name);
  } else if (data.rfind("rs|", 0) == 0) {
    size_t p = data.find('|', 3);
    if (p == std::string::npos) return;
    std::string conv = data.substr(3, p - 3);
    uint32_t id = strtoul(data.c_str() + p + 1, nullptr, 10);
    SendResult r = gApp.resend(conv, id);
    if (r.ok) trackOutgoing(r.id, chat, cb["message"]["message_id"] | 0);
  } else if (data.rfind("cd|", 0) == 0) {  // delete channel: "cd|<slot>|<first 4 key bytes>"
    size_t p = data.find('|', 3);
    if (p == std::string::npos) return;
    int idx = atoi(data.substr(3, p - 3).c_str());
    mc::ChannelInfo ch;
    if (idx <= 0 || idx > 255 || !gApp.mesh().findChannel((uint8_t)idx, ch) || mc::toHex(ch.secret, 4) != data.substr(p + 1)) {
      reply(chat, thread, fmt(TR(UNKNOWN_CHANNEL), std::to_string(idx)));  // already deleted, or slot reused
      return;
    }
    uint32_t rid = gApp.newRid();
    trackCommand(rid, chat, thread, fmt(TR(CH_DELETED), esc(ch.name)));
    std::string err;
    if (!gApp.deleteChannel((uint8_t)idx, rid, err)) {
      {
        Lock l(mtx_);
        pendingCmds_.erase(rid);
      }
      channelError(chat, thread, err);
    }
  }
}

void TelegramBridge::handleCommand(int64_t chat, int32_t thread, int32_t msgId, const std::string& cmd, const std::string& args) {
  auto keyboard = [&](bool channels) {
    auto recent = gApp.recentConversations(12, channels);
    JsonDocument kb;
    JsonArray rows = kb["inline_keyboard"].to<JsonArray>();
    JsonArray row;
    int n = 0;
    for (auto& c : recent) {
      if (channels != (c.kind == 1)) continue;
      if (n % 2 == 0) row = rows.add<JsonArray>();
      JsonObject b = row.add<JsonObject>();
      b["text"] = mc::utf8Truncate(c.name, 30);
      b["callback_data"] = "to|" + c.id;
      n++;
    }
    std::string markup;
    serializeJson(kb, markup);
    if (n == 0) reply(chat, thread, channels ? TR(NO_CHANNELS) : TR(NO_CONTACTS));
    else reply(chat, thread, TR(PICK_CONTACT), 0, markup);
  };

  if (cmd == "start") {
    reply(chat, thread, TR(WELCOME));
  } else if (cmd == "aide" || cmd == "help") {
    reply(chat, thread, TR(HELP));
  } else if (cmd == "msg" || cmd == "ch") {
    bool channels = cmd == "ch";
    if (args.empty()) {
      keyboard(channels);
      return;
    }
    ConvRef ref;
    std::string text;
    bool amb = false;
    if (!gApp.splitTarget(args, channels, ref, text, amb)) {
      reply(chat, thread, amb ? std::string(TR(AMBIGUOUS)) : fmt(channels ? TR(UNKNOWN_CHANNEL) : TR(UNKNOWN_CONTACT), esc(args.substr(0, args.find(' ')))), msgId);
      return;
    }
    if (text.empty()) {
      {
        Lock l(mtx_);
        pendingTarget_[chat] = ref.id;
      }
      askText(chat, thread, ref.name);  // reply() takes mtx_ itself
      return;
    }
    sendToConv(chat, msgId, ref.id, text);
  } else if (cmd == "public") {
    if (args.empty()) reply(chat, thread, TR(USAGE_CH));
    else sendToConv(chat, msgId, App::convForChannel(0), args);
  } else if (cmd == "contacts") {
    reply(chat, thread, gApp.contactsText(lang()));
  } else if (cmd == "canaux" || cmd == "channels") {
    reply(chat, thread, gApp.channelsText(lang()));
  } else if (cmd == "nouveaucanal" || cmd == "addchannel") {
    addChannel(chat, thread, args);
  } else if (cmd == "supprcanal" || cmd == "delchannel") {
    deleteChannelAsk(chat, thread, args);
  } else if (cmd == "status") {
    reply(chat, thread, gApp.statusText(lang()));
  } else if (cmd == "advert") {
    gApp.advert(true);
    reply(chat, thread, TR(ADVERT_SENT));
  } else if (cmd == "login") {
    ConvRef ref;
    std::string pw;
    bool amb = false;
    if (args.empty() || !gApp.splitTarget(args, false, ref, pw, amb) || pw.empty()) {
      reply(chat, thread, TR(USAGE_LOGIN));
      return;
    }
    trackLogin(ref.id, chat);
    gApp.loginRoom(ref.id, pw, true);
    reply(chat, thread, fmt(TR(LOGIN_SENT), esc(ref.name)));
  } else if (cmd == "sourdine" || cmd == "mute") {
    ConvRef ref;
    bool amb = false;
    if (args.empty() || (!gApp.findConvByName(args, false, ref, amb) && !gApp.findConvByName(args, true, ref, amb))) {
      reply(chat, thread, TR(USAGE_MUTE));
      return;
    }
    bool nowMuted;
    {
      std::lock_guard<std::recursive_mutex> l(gConfigMtx);
      auto& v = gConfig.tgMutedConvs;
      auto it = std::find(v.begin(), v.end(), ref.id);
      nowMuted = it == v.end();
      if (nowMuted) v.push_back(ref.id);
      else v.erase(it);
      gConfig.save();
    }
    reply(chat, thread, fmt(nowMuted ? TR(MUTED) : TR(UNMUTED), esc(ref.name)));
  } else {
    reply(chat, thread, TR(HELP));
  }
}
