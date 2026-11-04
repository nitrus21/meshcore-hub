// App: glue between the MeshCore client, the history, the web app and Telegram.
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "core/MeshClient.h"
#include "store/History.h"

struct SendResult {
  bool ok = false;
  uint32_t id = 0;
  std::string err;       // i18n key for the web app
  std::string target;    // display name
};

struct ConvRef {
  std::string id, name;
  uint8_t kind = 0;      // 0 chat, 1 channel, 2 repeater, 3 room, 4 sensor
};

class App : public mc::MeshListener {
 public:
  void begin();
  void meshLoop();       // mesh task body (one iteration)
  void housekeeping();   // periodic, from the mesh task

  mc::MeshClient& mesh() { return mesh_; }
  void applyLinkConfig();               // (re)select BLE / TCP from the config

  // ---- conversations
  static std::string convForKey(const uint8_t* pub);   // "c_<12 hex>"
  static std::string convForChannel(uint8_t idx);      // "ch_<idx>"
  bool resolveConv(const std::string& conv, ConvRef& out);
  bool findConvByName(const std::string& query, bool channels, ConvRef& out, bool& ambiguous);
  // "name rest of text" -> longest matching contact / channel name
  bool splitTarget(const std::string& args, bool channels, ConvRef& out, std::string& text, bool& ambiguous);
  std::vector<ConvRef> recentConversations(size_t n, bool includeChannels);

  // ---- actions (thread safe; used by the web API and Telegram)
  SendResult sendText(const std::string& conv, const std::string& text, const char* via, uint8_t txtType = 0);
  SendResult resend(const std::string& conv, uint32_t id);
  bool loginRoom(const std::string& conv, const std::string& password, bool remember, uint32_t rid = 0);
  void advert(bool flood);
  size_t maxTextFor(const std::string& conv);
  // Channels. idx < 0 adds in the first free slot; secret == nullptr makes a hashtag channel
  // (secret derived from "#name"). Returns the slot, or -1 with err set (i18n key "err.<err>").
  int setChannel(int idx, std::string name, const uint8_t* secret, uint32_t rid, std::string& err);
  // Removes a channel from the module; once the module confirms, its history and Telegram topic go too.
  bool deleteChannel(uint8_t idx, uint32_t rid, std::string& err);

  // ---- BLE scan (runs in the mesh task)
  void requestBleScan();
  void bleForget(const std::string& addr, uint8_t type);
  bool bleScanBusy() const { return scanRequested_ || scanning_; }

  // ---- JSON views
  void statusJson(JsonObject o);
  void contactsJson(JsonArray a);
  void channelsJson(JsonArray a);
  static void contactJson(const mc::Contact& c, JsonObject o);
  void selfJson(JsonObject o);

  // ---- text summaries (Telegram)
  std::string statusText(int lang);
  std::string contactsText(int lang);
  std::string channelsText(int lang);

  // event sink (web socket). type + payload
  using EventSink = std::function<void(JsonDocument&)>;
  void setEventSink(EventSink s) { sink_ = s; }
  void emit(JsonDocument& d);
  uint32_t newRid() {  // web and Telegram tasks both ask; 0 means "no rid"
    uint32_t r;
    do r = ++rid_; while (!r);
    return r;
  }

  // time
  bool timeValid() const;
  void onNtpSynced();

  // MeshListener
  void onLinkState(mc::LinkState, LinkPhase, LinkError) override;
  void onDeviceInfo(const mc::DeviceInfo&) override;
  void onSelfInfo(const mc::SelfInfo&) override;
  void onContactsLoaded() override;
  void onContact(const mc::Contact&, bool isNew) override;
  void onContactRemoved(const uint8_t* pub) override;
  void onChannelsLoaded() override;
  void onMessage(const mc::RecvMsg&) override;
  void onMsgStatus(uint32_t id, mc::MsgStatus, uint32_t rttMs, uint8_t attempt) override;
  void onBattery(const mc::BattStorage&) override;
  void onLogin(const mc::LoginResult&) override;
  void onRepeaterStatus(const mc::RepeaterStatus&) override;
  void onTelemetry(const mc::TelemetryResp&) override;
  void onTrace(const mc::TraceData&) override;
  void onPathDiscovery(const mc::PathDiscovery&) override;
  void onStats(const mc::Stats&) override;
  void onContactsFull() override;
  void onModuleTime(uint32_t t) override;
  void onResult(uint32_t rid, bool ok, uint8_t err, const uint8_t* data, size_t len) override;
  void onLog(const char* msg) override;

  uint32_t lastScanMs = 0;
  std::string scanJson = "[]";
  int scanHeard = -1;   // raw devices heard by the last scan (-1 = scan failed)

 private:
  void autoLoginRooms();
  void forgetConversation(const std::string& conv);   // history, Telegram topic, mute flag
  void runPendingForgets();
  std::string authorName(const uint8_t* prefix, size_t n);

  mc::MeshClient mesh_{[] { return (uint32_t)millis(); },
                       [] { time_t t = time(nullptr); return (uint32_t)(t > 1700000000 ? t : 0); }};
  EventSink sink_;
  std::atomic<uint32_t> rid_{0};
  volatile bool scanRequested_ = false;
  volatile bool scanning_ = false;
  bool wasReady_ = false;
  uint32_t lastHousekeep_ = 0, lastMaint_ = 0;
  std::map<uint32_t, std::string> loginRids_;   // rid -> conv (room logins from the web)
  std::mutex chMtx_;                            // web and Telegram tasks both edit channels
  std::map<uint32_t, std::string> channelDeletes_;  // rid -> conv of the channel being removed
  struct Reserved { uint32_t at; std::string name; uint8_t key[16]; };
  std::map<int, Reserved> slotReserved_;        // added, not yet re-read from the module (30 s max)
  std::vector<std::string> pendingForgets_;     // run from meshLoop, outside the MeshClient lock
};

extern App gApp;
