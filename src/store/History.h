// Message history on LittleFS.
//   /h/<conv>/<YYYYMM>.jl   one JSON object per line (messages + status updates)
//   /idx.json               conversation index (unread, last message, flags)
// Conversation ids: "c_<12 hex>" (contact / room / repeater by pubkey prefix), "ch_<idx>" (channel).
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <map>
#include <mutex>
#include <string>
#include <vector>

struct ConvMeta {
  std::string name;
  uint8_t kind = 0;          // 0 chat, 1 channel, 2 repeater, 3 room, 4 sensor
  uint32_t lastTs = 0;       // unix time of last message
  uint32_t lastId = 0;
  std::string preview;
  std::string lastAuthor;
  bool lastOut = false;
  uint16_t unread = 0;
  bool pinned = false, muted = false, archived = false;
};

struct MsgRecord {
  uint32_t id = 0;
  uint32_t ts = 0;           // hub time (received / sent)
  uint32_t senderTs = 0;
  bool out = false;
  std::string author;        // display name of the sender (channel / room / own)
  std::string authorKey;     // hex prefix when known
  std::string text;
  float snr = NAN;
  int hops = -2;             // -1 direct, -2 unknown
  uint8_t txtType = 0;
  uint8_t status = 0;        // MsgStatus for outgoing
  uint32_t rtt = 0;
  std::string via;           // "web" | "telegram" for outgoing
};

class History {
 public:
  bool begin();
  uint32_t nextId();
  // Appends a message and updates the index. Returns the record id.
  uint32_t append(const std::string& conv, const ConvMeta& hint, MsgRecord& m);
  void updateStatus(const std::string& conv, uint32_t id, uint8_t status, uint32_t rtt);
  // Messages older than `beforeId` (0 = newest), oldest first, at most `limit`.
  void read(const std::string& conv, uint32_t beforeId, int limit, JsonArray out);
  bool find(const std::string& conv, uint32_t id, MsgRecord& out);
  std::string convOf(uint32_t id);   // recent outgoing ids only (status routing)

  void listConversations(JsonArray out);
  bool meta(const std::string& conv, ConvMeta& out);
  void setName(const std::string& conv, const std::string& name, uint8_t kind);
  void setFlags(const std::string& conv, int pinned, int muted, int archived);  // -1 = unchanged
  void markRead(const std::string& conv);
  void removeConversation(const std::string& conv);
  uint32_t totalUnread();

  void flush();        // writes the index if dirty
  void maintenance();  // retention + free space rotation
  void wipe();
  void fsInfo(size_t& used, size_t& total);

  // Backup helpers
  std::vector<std::string> files();   // all history files
  bool importLine(const std::string& conv, const std::string& month, const std::string& json);
  void rebuildIndexFromFiles();

  static void toJson(const MsgRecord& m, JsonObject o, bool compact);
  static std::string monthOf(uint32_t ts);

 private:
  void loadIndex();
  bool parseLine(JsonDocument& d, MsgRecord& m);
  std::vector<std::string> months(const std::string& conv);  // sorted ascending
  bool appendLine(const std::string& path, const std::string& line);

  std::recursive_mutex mtx_;
  std::map<std::string, ConvMeta> idx_;
  uint32_t next_ = 1;
  bool dirty_ = false;
  uint32_t lastFlush_ = 0;
  // id -> conv for recent outgoing messages (status updates)
  std::map<uint32_t, std::pair<std::string, std::string>> recentOut_;  // id -> (conv, month)
};

extern History gHistory;
