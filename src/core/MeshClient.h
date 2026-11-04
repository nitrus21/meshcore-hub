// MeshClient: drives a MeshCore Companion radio over a Transport.
//  - connection handshake (device query, self info, time, contacts, channels, sync)
//  - one-command-at-a-time queue with 5 s timeout
//  - message sync loop, delivery tracking with retries (like the official app)
//  - "request" gate: login/status/telemetry/discovery share one pending slot in
//    the firmware (clearPendingReqs), so they are serialised separately.
// Platform independent: unit tested on the host with a fake transport.
#pragma once
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../protocol/Frames.h"
#include "ContactStore.h"
#include "../transport/Transport.h"

namespace mc {

enum class LinkState : uint8_t {
  Disabled,    // no module configured
  Waiting,     // backoff before next attempt
  Connecting,  // transport connect in progress
  Handshake,   // reading profile / contacts / channels
  Ready,
  Released,    // user released the module for the phone app
};
const char* linkStateName(LinkState s);
const char* linkErrorName(LinkError e);
const char* linkPhaseName(LinkPhase p);

enum class MsgStatus : uint8_t { Sending, Sent, Delivered, Failed };
const char* msgStatusName(MsgStatus s);

class MeshListener {
 public:
  virtual ~MeshListener() {}
  virtual void onLinkState(LinkState, LinkPhase, LinkError) {}
  virtual void onDeviceInfo(const DeviceInfo&) {}
  virtual void onSelfInfo(const SelfInfo&) {}
  virtual void onContactsLoaded() {}
  virtual void onContact(const Contact&, bool isNew) {}
  virtual void onContactRemoved(const uint8_t* pub) {}
  virtual void onChannelsLoaded() {}
  virtual void onMessage(const RecvMsg&) {}
  virtual void onMsgStatus(uint32_t id, MsgStatus, uint32_t rttMs, uint8_t attempt) {}
  virtual void onBattery(const BattStorage&) {}
  virtual void onLogin(const LoginResult&) {}
  virtual void onRepeaterStatus(const RepeaterStatus&) {}
  virtual void onTelemetry(const TelemetryResp&) {}
  virtual void onTrace(const TraceData&) {}
  virtual void onPathDiscovery(const PathDiscovery&) {}
  virtual void onStats(const Stats&) {}
  virtual void onContactsFull() {}
  virtual void onModuleTime(uint32_t unixSecs) {}
  // Result of an API call made with a request id (rid != 0).
  virtual void onResult(uint32_t rid, bool ok, uint8_t err, const uint8_t* data, size_t len) {}
  virtual void onLog(const char* msg) {}
};

class MeshClient {
 public:
  using MillisFn = std::function<uint32_t()>;
  using UnixFn = std::function<uint32_t()>;  // 0 when unknown

  static constexpr uint32_t CMD_TIMEOUT_MS = 5000;
  static constexpr uint32_t SYNC_INTERVAL_MS = 60000;
  static constexpr uint32_t BATT_INTERVAL_MS = 300000;
  static constexpr uint32_t TIME_INTERVAL_MS = 6UL * 3600UL * 1000UL;
  static constexpr uint32_t KEEPALIVE_MS = 25000;
  static constexpr uint32_t RX_STATS_MS = 120000;
  static constexpr uint8_t MAX_ATTEMPTS = 3;

  MeshClient(MillisFn millis, UnixFn unixTime);

  void setListener(MeshListener* l) { listener_ = l; }
  void setTransport(Transport* t);   // nullptr = disabled
  Transport* transport() const { return transport_; }
  void setAppName(const std::string& n) { appName_ = n; }
  // Must be set before the first connection (default: in RAM).
  void setContactStore(ContactStore* s) { store_ = s ? s : &memStore_; }

  // Driven by the mesh task. May block inside transport->connect().
  void loop();

  // ---- connection control (thread safe)
  void release(uint32_t minutes);    // 0 = until reconnect() is called
  void reconnect();                  // cancel release / backoff, connect now
  void disconnect();                 // stop and stay disabled until reconnect()
  LinkState state() const { return state_; }
  LinkPhase phase() const { return phase_; }
  LinkError lastError() const { return lastErr_; }
  uint32_t releasedUntilMs() const { return releaseUntil_; }
  uint32_t lastRxMs() const { return lastRx_; }
  bool ready() const { return state_ == LinkState::Ready; }

  // ---- incoming frames (thread safe, called from the transport callback)
  void pushFrame(const uint8_t* d, size_t n);

  // ---- messaging (thread safe). `id` is the caller's message id.
  bool sendDirect(uint32_t id, const uint8_t prefix[PREFIX_SIZE], const std::string& text,
                  uint8_t txtType = TXT_PLAIN);
  bool sendChannel(uint32_t id, uint8_t channelIdx, const std::string& text);
  // Calling sendDirect/sendChannel again with an existing id restarts it (resend).

  // ---- generic commands (thread safe). rid != 0 -> MeshListener::onResult
  void command(const Bytes& frame, uint32_t rid = 0);
  void request(const Bytes& frame, uint32_t rid = 0);   // gated: login/status/telemetry/discovery
  void login(const uint8_t pub[PUB_KEY_SIZE], const std::string& password, uint32_t rid = 0);
  void refreshContacts();
  void refreshChannels();
  void refreshSelf();
  void syncMessages();
  void setTimeNow();

  // ---- snapshots (thread safe copies)
  // Contacts are never copied as a whole (a module can hold 350+ of them).
  void forEachContact(const std::function<void(const Contact&)>& fn);
  size_t contactCount();
  bool contactAt(size_t i, Contact& out);
  // Toggles the favourite flag: re-reads the full contact from the module first, so a
  // long route (beyond the RAM cache) is written back unchanged.
  void setFavorite(const uint8_t pub[PUB_KEY_SIZE], bool fav, uint32_t rid);
  std::vector<ChannelInfo> channels();
  bool findContact(const uint8_t* prefix, size_t n, Contact& out);
  bool findContactByName(const std::string& name, Contact& out);
  bool findChannel(uint8_t idx, ChannelInfo& out);
  bool findChannelByName(const std::string& name, ChannelInfo& out);
  // false when a slot of the last channel read got no (or a mismatched) answer: a slot missing from
  // channels() may then be in use, so nothing must be written into a "free" slot.
  bool channelsComplete();
  DeviceInfo deviceInfo();
  SelfInfo selfInfo();
  BattStorage battery();
  uint32_t lastSyncUnix() const { return lastSyncUnix_; }

  std::recursive_mutex& mutex() { return mtx_; }

 private:
  using Handler = std::function<bool(const uint8_t*, size_t)>;  // nullptr data = timeout; true = done
  struct Cmd {
    Bytes frame;
    Handler handler;
    bool isRequest = false;
  };
  struct OutMsg {
    uint32_t id = 0;
    bool channel = false;
    uint8_t prefix[PREFIX_SIZE] = {0};
    uint8_t chIdx = 0;
    uint8_t txtType = TXT_PLAIN;
    std::string text;
    uint32_t ts = 0;
    uint8_t attempt = 0;
    std::vector<uint32_t> acks;
    uint32_t sentAt = 0;
    uint32_t deadline = 0;   // 0 = not waiting for an ACK
    MsgStatus status = MsgStatus::Sending;
    uint32_t finishedAt = 0;
  };

  void setState(LinkState s);
  void onLinkUp();
  void onLinkDown(LinkError e);
  void handleFrame(const uint8_t* d, size_t n);
  void handlePush(const uint8_t* d, size_t n);
  void enqueue(Cmd c, bool front = false);
  void pumpQueue(uint32_t now);
  void startHandshake();
  void handshakeStep();
  void transmitOut(OutMsg& m);
  void checkOutTimeouts(uint32_t now);
  void emitStatus(OutMsg& m, MsgStatus st, uint32_t rtt);
  Handler resultHandler(uint32_t rid);
  uint32_t nextTs();
  void log(const char* fmt, ...);

  MillisFn millis_;
  UnixFn unix_;
  MeshListener* listener_ = nullptr;
  Transport* transport_ = nullptr;
  std::string appName_ = "MeshCoreHub";
  mutable std::recursive_mutex mtx_;

  LinkState state_ = LinkState::Disabled;
  LinkPhase phase_ = LinkPhase::Idle;
  LinkError lastErr_ = LinkError::None;
  uint32_t backoffMs_ = 2000;
  uint32_t nextAttempt_ = 0;
  uint32_t releaseUntil_ = 0;
  bool releaseForever_ = false;
  bool stopped_ = false;
  bool linkUpPending_ = false;

  std::mutex inboxMtx_;   // separate lock: the BLE callback must never wait on mtx_
  std::deque<Bytes> inbox_;
  bool dropping_ = false;
  std::deque<Cmd> queue_;
  std::deque<Cmd> reqQueue_;
  bool busy_ = false;
  Cmd current_;
  uint32_t cmdDeadline_ = 0;
  uint8_t consecutiveTimeouts_ = 0;
  uint32_t reqGateUntil_ = 0;

  int hsStep_ = -1;
  int hsChannel_ = 0;
  MemoryContactStore memStore_;
  ContactStore* store_ = &memStore_;
  std::vector<ChannelInfo> channels_, channelsTmp_;
  bool channelsComplete_ = false, channelsTmpComplete_ = true;
  DeviceInfo dev_;
  SelfInfo self_;
  BattStorage batt_;
  bool syncing_ = false;
  bool syncAgain_ = false;

  std::vector<OutMsg> out_;
  uint32_t lastTs_ = 0;

  uint32_t lastRx_ = 0, lastSync_ = 0, lastBatt_ = 0, lastTimeSet_ = 0;
  // traffic seen from the module, logged every RX_STATS_MS (is the radio hearing anything?)
  uint16_t rxPush_[16] = {}, rxResp_ = 0, rxMsgs_ = 0;
  uint32_t lastRxStats_ = 0;
  uint32_t lastSyncUnix_ = 0;
};

}  // namespace mc
