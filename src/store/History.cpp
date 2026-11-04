#include "History.h"
#include <LittleFS.h>
#include <algorithm>
#include <deque>
#include <time.h>
#include "../config/Config.h"
#include "../protocol/Frames.h"
#include "../util/Log.h"

History gHistory;

using Lock = std::lock_guard<std::recursive_mutex>;
static const char* IDX = "/idx.json";
static const char* ROOT = "/h";

bool History::begin() {
  Lock l(mtx_);
  if (!LittleFS.exists(ROOT)) LittleFS.mkdir(ROOT);
  loadIndex();
  return true;
}

std::string History::monthOf(uint32_t ts) {
  if (ts < 1600000000UL) return "000000";
  time_t t = ts;
  struct tm tmv;
  gmtime_r(&t, &tmv);
  char b[16];
  snprintf(b, sizeof(b), "%04d%02d", (tmv.tm_year + 1900) % 10000, (tmv.tm_mon + 1) % 100);
  return b;
}

uint32_t History::nextId() {
  Lock l(mtx_);
  dirty_ = true;
  return next_++;
}

// ---------------------------------------------------------------- index

void History::loadIndex() {
  idx_.clear();
  File f = LittleFS.open(IDX, "r");
  if (!f) return;
  JsonDocument d;
  DeserializationError e = deserializeJson(d, f);
  f.close();
  if (e) {
    LOGW("history index unreadable (%s), rebuilding", e.c_str());
    rebuildIndexFromFiles();
    return;
  }
  next_ = (d["next"] | 1u) + 50;  // margin in case ids were used after the last flush
  for (JsonObject o : d["convs"].as<JsonArray>()) {
    ConvMeta m;
    m.name = o["n"] | "";
    m.kind = o["k"] | 0;
    m.lastTs = o["t"] | 0u;
    m.lastId = o["l"] | 0u;
    m.preview = o["p"] | "";
    m.lastAuthor = o["a"] | "";
    m.lastOut = o["o"] | false;
    m.unread = o["u"] | 0;
    m.pinned = o["pi"] | false;
    m.muted = o["mu"] | false;
    m.archived = o["ar"] | false;
    idx_[o["id"] | ""] = m;
  }
  dirty_ = true;
}

void History::flush() {
  Lock l(mtx_);
  if (!dirty_) return;
  JsonDocument d;
  d["next"] = next_;
  JsonArray a = d["convs"].to<JsonArray>();
  for (auto& kv : idx_) {
    JsonObject o = a.add<JsonObject>();
    const ConvMeta& m = kv.second;
    o["id"] = kv.first;
    o["n"] = m.name;
    o["k"] = m.kind;
    o["t"] = m.lastTs;
    o["l"] = m.lastId;
    o["p"] = m.preview;
    if (!m.lastAuthor.empty()) o["a"] = m.lastAuthor;
    if (m.lastOut) o["o"] = true;
    if (m.unread) o["u"] = m.unread;
    if (m.pinned) o["pi"] = true;
    if (m.muted) o["mu"] = true;
    if (m.archived) o["ar"] = true;
  }
  File f = LittleFS.open("/idx.tmp", "w");
  if (!f) {
    LOGE("cannot write history index");
    return;
  }
  serializeJson(d, f);
  f.close();
  LittleFS.remove(IDX);
  LittleFS.rename("/idx.tmp", IDX);
  dirty_ = false;
  lastFlush_ = millis();
}

void History::rebuildIndexFromFiles() {
  Lock l(mtx_);
  File root = LittleFS.open(ROOT);
  if (!root) return;
  for (File dir = root.openNextFile(); dir; dir = root.openNextFile()) {
    if (!dir.isDirectory()) continue;
    std::string conv = dir.name();
    dir.close();
    auto ms = months(conv);
    if (ms.empty()) continue;
    JsonDocument doc;
    JsonArray arr = doc.to<JsonArray>();
    read(conv, 0, 1, arr);
    ConvMeta& m = idx_[conv];
    m.kind = conv.rfind("ch_", 0) == 0 ? 1 : m.kind;
    if (arr.size()) {
      JsonObject last = arr[0];
      m.lastTs = last["t"] | 0u;
      m.lastId = last["i"] | 0u;
      m.preview = mc::utf8Truncate(last["x"] | "", 90);
      m.lastAuthor = last["a"] | "";
      m.lastOut = last["d"] | 0;
      if (m.lastId >= next_) next_ = m.lastId + 1;
    }
  }
  dirty_ = true;
  flush();
}

// ---------------------------------------------------------------- files

std::vector<std::string> History::months(const std::string& conv) {
  std::vector<std::string> out;
  std::string dirPath = std::string(ROOT) + "/" + conv;
  File dir = LittleFS.open(dirPath.c_str());
  if (!dir || !dir.isDirectory()) return out;
  for (File f = dir.openNextFile(); f; f = dir.openNextFile()) {
    std::string n = f.name();
    f.close();
    if (n.size() == 9 && n.compare(6, 3, ".jl") == 0) out.push_back(n.substr(0, 6));
  }
  std::sort(out.begin(), out.end());
  return out;
}

bool History::appendLine(const std::string& path, const std::string& line) {
  File f = LittleFS.open(path.c_str(), "a");
  if (!f) {
    maintenance();  // maybe the disk is full
    f = LittleFS.open(path.c_str(), "a");
    if (!f) return false;
  }
  f.print(line.c_str());
  f.print('\n');
  f.close();
  return true;
}

std::vector<std::string> History::files() {
  Lock l(mtx_);
  std::vector<std::string> out;
  File root = LittleFS.open(ROOT);
  if (!root) return out;
  std::vector<std::string> convs;
  for (File dir = root.openNextFile(); dir; dir = root.openNextFile()) {
    if (dir.isDirectory()) convs.push_back(dir.name());
    dir.close();
  }
  for (auto& c : convs)
    for (auto& m : months(c)) out.push_back(std::string(ROOT) + "/" + c + "/" + m + ".jl");
  return out;
}

void History::fsInfo(size_t& used, size_t& total) {
  used = LittleFS.usedBytes();
  total = LittleFS.totalBytes();
}

// ---------------------------------------------------------------- records

void History::toJson(const MsgRecord& m, JsonObject o, bool compact) {
  (void)compact;
  o["i"] = m.id;
  o["t"] = m.ts;
  if (m.senderTs) o["st"] = m.senderTs;
  o["d"] = m.out ? 1 : 0;
  if (!m.author.empty()) o["a"] = m.author;
  if (!m.authorKey.empty()) o["ak"] = m.authorKey;
  o["x"] = m.text;
  if (!isnan(m.snr)) o["s"] = round(m.snr * 4) / 4.0;
  if (m.hops != -2) o["h"] = m.hops;
  if (m.txtType) o["y"] = m.txtType;
  if (m.out) o["k"] = m.status;
  if (m.rtt) o["r"] = m.rtt;
  if (!m.via.empty()) o["v"] = m.via;
}

bool History::parseLine(JsonDocument& d, MsgRecord& m) {
  if (!d["i"].is<uint32_t>()) return false;
  m.id = d["i"];
  m.ts = d["t"] | 0u;
  m.senderTs = d["st"] | 0u;
  m.out = (d["d"] | 0) == 1;
  m.author = d["a"] | "";
  m.authorKey = d["ak"] | "";
  m.text = d["x"] | "";
  m.snr = d["s"].is<float>() ? d["s"].as<float>() : NAN;
  m.hops = d["h"] | -2;
  m.txtType = d["y"] | 0;
  m.status = d["k"] | 0;
  m.rtt = d["r"] | 0u;
  m.via = d["v"] | "";
  return true;
}

uint32_t History::append(const std::string& conv, const ConvMeta& hint, MsgRecord& m) {
  Lock l(mtx_);
  if (!m.id) m.id = next_++;
  if (!m.ts) m.ts = time(nullptr);
  std::string month = monthOf(m.ts);
  std::string dir = std::string(ROOT) + "/" + conv;
  if (!LittleFS.exists(dir.c_str())) LittleFS.mkdir(dir.c_str());
  JsonDocument d;
  toJson(m, d.to<JsonObject>(), true);
  std::string line;
  serializeJson(d, line);
  if (!appendLine(dir + "/" + month + ".jl", line)) LOGE("history write failed for %s", conv.c_str());

  ConvMeta& meta = idx_[conv];
  if (!hint.name.empty()) meta.name = hint.name;
  meta.kind = hint.kind;
  meta.lastTs = m.ts;
  meta.lastId = m.id;
  meta.preview = mc::utf8Truncate(m.text, 90);
  meta.lastAuthor = m.author;
  meta.lastOut = m.out;
  if (!m.out && m.txtType != 1) meta.unread++;
  if (meta.archived && !m.out) meta.archived = false;  // a new message brings it back
  dirty_ = true;

  if (m.out) {
    recentOut_[m.id] = {conv, month};
    while (recentOut_.size() > 200) recentOut_.erase(recentOut_.begin());
  }
  if (millis() - lastFlush_ > 10000) flush();
  return m.id;
}

void History::updateStatus(const std::string& conv, uint32_t id, uint8_t status, uint32_t rtt) {
  Lock l(mtx_);
  std::string month;
  auto it = recentOut_.find(id);
  if (it != recentOut_.end()) month = it->second.second;
  else month = monthOf(time(nullptr));
  char line[64];
  if (rtt) snprintf(line, sizeof(line), "{\"u\":%lu,\"k\":%u,\"r\":%lu}", (unsigned long)id, (unsigned)status, (unsigned long)rtt);
  else snprintf(line, sizeof(line), "{\"u\":%lu,\"k\":%u}", (unsigned long)id, (unsigned)status);
  appendLine(std::string(ROOT) + "/" + conv + "/" + month + ".jl", line);
}

std::string History::convOf(uint32_t id) {
  Lock l(mtx_);
  auto it = recentOut_.find(id);
  return it == recentOut_.end() ? std::string() : it->second.first;
}

void History::read(const std::string& conv, uint32_t beforeId, int limit, JsonArray out) {
  Lock l(mtx_);
  if (limit <= 0) limit = 50;
  if (limit > 100) limit = 100;
  auto ms = months(conv);
  std::deque<std::string> result;  // oldest first
  std::map<uint32_t, std::pair<uint8_t, uint32_t>> upd;
  for (auto it = ms.rbegin(); it != ms.rend() && (int)result.size() < limit; ++it) {
    std::string path = std::string(ROOT) + "/" + conv + "/" + *it + ".jl";
    File f = LittleFS.open(path.c_str(), "r");
    if (!f) continue;
    std::deque<std::string> window;
    size_t want = limit - result.size();
    while (f.available()) {
      String s = f.readStringUntil('\n');
      if (s.length() < 8) continue;
      if (s.startsWith("{\"u\":")) {
        JsonDocument d;
        if (!deserializeJson(d, s)) upd[d["u"].as<uint32_t>()] = {d["k"] | 0, d["r"] | 0u};
        continue;
      }
      if (!s.startsWith("{\"i\":")) continue;
      uint32_t id = strtoul(s.c_str() + 5, nullptr, 10);
      if (beforeId && id >= beforeId) continue;
      window.emplace_back(s.c_str());
      if (window.size() > want) window.pop_front();
    }
    f.close();
    result.insert(result.begin(), window.begin(), window.end());
  }
  for (auto& line : result) {
    JsonDocument d;
    if (deserializeJson(d, line)) continue;
    MsgRecord m;
    if (!parseLine(d, m)) continue;
    auto u = upd.find(m.id);
    if (u != upd.end()) {
      m.status = u->second.first;
      if (u->second.second) m.rtt = u->second.second;
    }
    toJson(m, out.add<JsonObject>(), false);
  }
}

bool History::find(const std::string& conv, uint32_t id, MsgRecord& out) {
  JsonDocument doc;
  JsonArray a = doc.to<JsonArray>();
  read(conv, id + 1, 1, a);
  if (!a.size() || (a[0]["i"] | 0u) != id) return false;
  JsonDocument d;
  d.set(a[0]);
  return parseLine(d, out);
}

// ---------------------------------------------------------------- conversations

void History::listConversations(JsonArray out) {
  Lock l(mtx_);
  for (auto& kv : idx_) {
    const ConvMeta& m = kv.second;
    JsonObject o = out.add<JsonObject>();
    o["id"] = kv.first;
    o["name"] = m.name;
    o["kind"] = m.kind;
    o["lastTs"] = m.lastTs;
    o["lastId"] = m.lastId;
    o["preview"] = m.preview;
    if (!m.lastAuthor.empty()) o["lastAuthor"] = m.lastAuthor;
    o["lastOut"] = m.lastOut;
    o["unread"] = m.unread;
    o["pinned"] = m.pinned;
    o["muted"] = m.muted;
    o["archived"] = m.archived;
  }
}

bool History::meta(const std::string& conv, ConvMeta& out) {
  Lock l(mtx_);
  auto it = idx_.find(conv);
  if (it == idx_.end()) return false;
  out = it->second;
  return true;
}

void History::setName(const std::string& conv, const std::string& name, uint8_t kind) {
  Lock l(mtx_);
  auto it = idx_.find(conv);
  if (it == idx_.end()) return;  // only conversations that already have messages
  if (it->second.name != name || it->second.kind != kind) {
    it->second.name = name;
    it->second.kind = kind;
    dirty_ = true;
  }
}

void History::setFlags(const std::string& conv, int pinned, int muted, int archived) {
  Lock l(mtx_);
  ConvMeta& m = idx_[conv];
  if (pinned >= 0) m.pinned = pinned;
  if (muted >= 0) m.muted = muted;
  if (archived >= 0) m.archived = archived;
  dirty_ = true;
}

void History::markRead(const std::string& conv) {
  Lock l(mtx_);
  auto it = idx_.find(conv);
  if (it != idx_.end() && it->second.unread) {
    it->second.unread = 0;
    dirty_ = true;
  }
}

uint32_t History::totalUnread() {
  Lock l(mtx_);
  uint32_t n = 0;
  for (auto& kv : idx_) if (!kv.second.muted) n += kv.second.unread;
  return n;
}

void History::removeConversation(const std::string& conv) {
  Lock l(mtx_);
  for (auto& m : months(conv)) LittleFS.remove((std::string(ROOT) + "/" + conv + "/" + m + ".jl").c_str());
  LittleFS.rmdir((std::string(ROOT) + "/" + conv).c_str());
  idx_.erase(conv);
  dirty_ = true;
  flush();
}

void History::wipe() {
  Lock l(mtx_);
  std::vector<std::string> convs;
  for (auto& kv : idx_) convs.push_back(kv.first);
  for (auto& f : files()) LittleFS.remove(f.c_str());
  File root = LittleFS.open(ROOT);
  std::vector<std::string> dirs;
  if (root)
    for (File d = root.openNextFile(); d; d = root.openNextFile()) {
      dirs.push_back(std::string(ROOT) + "/" + d.name());
      d.close();
    }
  for (auto& d : dirs) LittleFS.rmdir(d.c_str());
  idx_.clear();
  recentOut_.clear();
  dirty_ = true;
  flush();
}

// ---------------------------------------------------------------- maintenance

void History::maintenance() {
  Lock l(mtx_);
  uint16_t days;
  uint8_t pct;
  {
    std::lock_guard<std::recursive_mutex> cl(gConfigMtx);
    days = gConfig.historyDays;
    pct = gConfig.historyMaxPct;
  }
  auto all = files();
  // retention: drop whole months older than the cutoff
  uint32_t now = time(nullptr);
  if (days && now > 1600000000UL) {
    std::string cutoff = monthOf(now - (uint32_t)days * 86400UL);
    for (auto& f : all) {
      std::string month = f.substr(f.size() - 9, 6);
      if (month < cutoff) {
        LittleFS.remove(f.c_str());
        LOGI("history: removed %s (retention)", f.c_str());
      }
    }
    all = files();
  }
  // free space: delete the globally oldest month until under the limit
  size_t total = LittleFS.totalBytes();
  while (total && LittleFS.usedBytes() > total * pct / 100 && !all.empty()) {
    auto oldest = std::min_element(all.begin(), all.end(), [](const std::string& a, const std::string& b) {
      return a.substr(a.size() - 9, 6) < b.substr(b.size() - 9, 6);
    });
    LOGW("history: storage over %u%%, removing %s", (unsigned)pct, oldest->c_str());
    LittleFS.remove(oldest->c_str());
    all.erase(oldest);
  }
  flush();
}

bool History::importLine(const std::string& conv, const std::string& month, const std::string& json) {
  Lock l(mtx_);
  if (conv.empty() || conv.find('/') != std::string::npos || month.size() != 6) return false;
  std::string dir = std::string(ROOT) + "/" + conv;
  if (!LittleFS.exists(dir.c_str())) LittleFS.mkdir(dir.c_str());
  return appendLine(dir + "/" + month + ".jl", json);
}
