// Telegram bot bridge (Bot API over HTTPS, long polling, no third-party library).
//  - one task and one TLS session: rate-limited outbox (1 msg/s per chat, retry_after on 429,
//    kept while offline) drained before each short long-poll (getUpdates?timeout=4)
#pragma once
#include <Arduino.h>
#include <ArduinoJson.h>
#include <WiFiClientSecure.h>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include "../store/History.h"

class TelegramBridge {
 public:
  void begin();
  void reconfigure();   // token / chats / mode changed

  // MeshCore -> Telegram
  void relayIncoming(const std::string& conv, const ConvMeta& meta, const MsgRecord& m);
  void relayOwn(const std::string& conv, const ConvMeta& meta, const MsgRecord& m);
  // Private routing. True when the message is taken care of here: it must not reach the main
  // chat, and logText is what the history keeps.
  bool routeIncoming(const std::string& conv, const std::string& sender, const std::string& text, std::string& logText);
  // Invitation link for a directory entry: t.me/<user>?start=<code> (or ?startgroup=<code>)
  bool routeInvite(const std::string& name, std::string& code, std::string& user, std::string& err);
  void routeForget(const std::string& name);   // entry removed: its pending link must die too
  void notify(const std::string& html);
  // delivery feedback for messages sent from Telegram
  void trackOutgoing(uint32_t id, int64_t chat, int32_t msgId, bool noResend = false);
  void onOutStatus(uint32_t id, uint8_t status, const std::string& conv, const std::string& convName);

  // Wizard helpers (non blocking; results via the App event sink)
  void testToken(const std::string& token);
  void startClaim();    // accept the next chat that sends /start, for a few minutes
  void statusJson(JsonObject o);
  bool online() const { return online_; }
  void loginResult(const std::string& conv, bool ok, const std::string& name);
  void commandResult(uint32_t rid, bool ok, const std::string& err);   // module answer to a tracked command
  void forgetConversation(const std::string& conv);                    // conversation deleted: drop its topic
  void channelsChanged() { seedTopics_ = true; }                       // give every channel its topic

 private:
  struct Out {
    enum Kind : uint8_t { Message, Reaction, Answer } kind = Message;
    int64_t chat = 0;
    std::string conv;      // for topic routing and reply mapping
    std::string text;      // html (Message) or emoji (Reaction) or callback id (Answer)
    int32_t replyTo = 0;
    int32_t msgId = 0;     // reaction target
    std::string markup;    // reply_markup JSON
    std::string flat;      // single-chat wording, used when no forum topic can be created
    uint8_t tries = 0;
    bool noThread = false;  // topic refused: send to the main chat with the flat wording
    uint32_t seq = 0;       // identifies the item across the unlocked send (front may change)
    std::string route;      // routed message: conversation of its author (replies go back there)
    std::string routeName;  // and the directory entry it is for
  };
  struct Origin { int64_t chat; int32_t msgId; bool noResend; };
  struct RouteMsg { int64_t chat; int32_t msgId; std::string conv; };
  struct Invite { std::string name, code; uint32_t at; };

  static void pollTaskFn(void*);
  static void sendTaskFn(void*);
  void pollLoop();
  void sendLoop();
  void runTokenTest();
  void setError(const std::string& e);
  std::string error();
  void waitOrTest(uint32_t ms);
  bool sendNext(WiFiClientSecure& cli);
  int call(WiFiClientSecure& cli, const std::string& token, const char* method, const std::string& body,
           JsonDocument& resp, uint32_t timeoutMs, JsonDocument* filter = nullptr, std::string* err = nullptr);
  void handleUpdate(JsonObject u);
  void handleMessage(JsonObject msg);
  void handleCallback(JsonObject cb);
  void handleCommand(int64_t chat, int32_t thread, int32_t msgId, const std::string& cmd, const std::string& args);
  void sendToConv(int64_t chat, int32_t msgId, const std::string& conv, const std::string& text);
  bool sendRouted(int64_t chat, int32_t msgId, const std::string& conv, const std::string& from, const std::string& text);
  bool useInvite(const std::string& code, int64_t chat, int32_t thread, JsonObject msg);
  std::string routeNameOf(int64_t chat);   // "": not in the directory, or routing off
  std::string routeConvFor(int64_t chat, int32_t replyTo);
  void rememberRoute(int64_t chat, int32_t msgId, const std::string& conv);
  void migrateChat(int64_t from, int64_t to);
  bool routeStillValid(const Out& o);
  bool inDirectory(int64_t chat);          // even while routing is off
  bool claimActive();
  void enqueue(Out o);
  void reply(int64_t chat, int32_t thread, const std::string& html, int32_t replyTo = 0, const std::string& markup = "");
  void askText(int64_t chat, int32_t thread, const std::string& name);
  void routeWrite(int64_t chat, int32_t thread, int32_t msgId, const std::string& from, const std::string& args);
  bool allowed(int64_t chat);
  int lang();
  int64_t mainChat();
  bool topicsMode();
  int32_t topicFor(WiFiClientSecure& cli, int64_t chat, const std::string& conv, const std::string& title, uint32_t gen);
  std::string convForTopic(int32_t thread);
  void rememberMapping(int32_t msgId, const std::string& conv);
  std::string convForMessage(int32_t msgId);
  void trackLogin(const std::string& conv, int64_t chat);
  void trackCommand(uint32_t rid, int64_t chat, int32_t thread, const std::string& okHtml);
  void addChannel(int64_t chat, int32_t thread, const std::string& args);
  void deleteChannelAsk(int64_t chat, int32_t thread, const std::string& args);
  void channelError(int64_t chat, int32_t thread, const std::string& err);
  void seedChannelTopics(WiFiClientSecure& cli);
  void loadMaps();
  void saveMaps();
  void setupClient(WiFiClientSecure& c);
  void registerCommands(WiFiClientSecure& cli);
  std::string format(const std::string& conv, const ConvMeta& meta, const MsgRecord& m, bool own, bool topics);

  std::mutex mtx_;
  std::deque<Out> outbox_;
  uint32_t outSeq_ = 0;
  std::map<int32_t, std::string> msgConv_;         // telegram message id -> conversation
  std::deque<int32_t> msgOrder_;
  std::map<std::string, int32_t> topics_;          // conversation -> forum topic id
  std::map<uint32_t, Origin> origin_;              // mesh msg id -> telegram chat/msg
  std::deque<RouteMsg> routeMsgs_;                 // routed messages, oldest first: who to answer
  std::vector<Invite> invites_;                    // kept in RAM: a restart cancels them
  std::map<int64_t, std::pair<uint32_t, uint8_t>> routeRate_;  // chat -> start of the minute, messages
  std::map<int64_t, std::string> pendingTarget_;   // chat -> conversation picked via keyboard
  std::map<std::string, int64_t> pendingLogins_;   // conv -> chat
  struct PendingCmd { int64_t chat; int32_t thread; std::string okHtml; };
  std::map<uint32_t, PendingCmd> pendingCmds_;     // rid -> where to say how it went
  bool mapsDirty_ = false;
  uint32_t lastMapSave_ = 0;

  volatile bool reconfig_ = true;
  volatile bool claiming_ = false;
  uint32_t claimAt_ = 0;
  uint32_t lastExpiredReply_ = 0;  // answers to dead invitations: anyone can ask, so one a minute
  std::string testToken_;
  volatile bool testPending_ = false;
  // last token test, also in statusJson for clients without WebSocket (guarded by mtx_)
  struct TestResult { uint32_t seq = 0; bool busy = false, ok = false; std::string name, user, err; } test_;
  std::string botName_, botUser_, lastError_;  // guarded by errMtx_ (poll and send tasks)
  std::mutex errMtx_;
  // Boards with PSRAM keep two TLS sessions open (buffers in PSRAM): one long-polls, the other
  // sends at once. No handshake every few seconds: matters on a metered 4G plan.
  bool roomy_ = false;
  TaskHandle_t sendTask_ = nullptr;
  volatile uint32_t sendGen_ = 0;  // bumped by reconfigure(): the send session must be redone
  volatile bool tokenOk_ = false;  // getMe succeeded with the current settings (gates the send task)
  volatile bool online_ = false;
  volatile bool topicsEnabled_ = false;
  volatile bool seedTopics_ = true;  // create the missing channel topics (so a quiet channel can be written to)
  bool everOnline_ = false;       // polled successfully since boot (gates the low-RAM restart)
  uint32_t lastAlive_ = 0;        // last successful poll, or deliberate pause (web app, no Wi-Fi)
  bool ramStarved_ = false;       // the last failure to reach Telegram was for lack of RAM
  bool tlsNoRam_ = false;         // set by call(): the TLS connection failed on an allocation
  int64_t updateOffset_ = 0;
  int64_t botId_ = 0;             // bot the saved offset and reply map belong to (guarded by mtx_)
  std::map<int64_t, uint32_t> lastSendPerChat_;
  uint32_t lastSendAny_ = 0;
  uint32_t forgetGen_ = 0;        // bumped by forgetConversation: a send in flight must not re-map a forgotten conv
  uint32_t topicsFailedAt_ = 0;   // createForumTopic refused: retry only after a while
  TaskHandle_t pollTask_ = nullptr;
};

extern TelegramBridge gTelegram;
