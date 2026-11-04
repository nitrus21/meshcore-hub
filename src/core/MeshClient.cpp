#include "MeshClient.h"
#include <stdarg.h>
#include <stdio.h>
#include <algorithm>
#include <memory>

namespace mc {

const char* linkStateName(LinkState s) {
  switch (s) {
    case LinkState::Disabled: return "disabled";
    case LinkState::Waiting: return "waiting";
    case LinkState::Connecting: return "connecting";
    case LinkState::Handshake: return "handshake";
    case LinkState::Ready: return "ready";
    case LinkState::Released: return "released";
  }
  return "?";
}

const char* linkErrorName(LinkError e) {
  switch (e) {
    case LinkError::None: return "";
    case LinkError::NotConfigured: return "not_configured";
    case LinkError::NotFound: return "not_found";
    case LinkError::ConnectFailed: return "connect_failed";
    case LinkError::AuthFailed: return "auth_failed";
    case LinkError::NoService: return "no_service";
    case LinkError::Lost: return "lost";
  }
  return "?";
}

const char* linkPhaseName(LinkPhase p) {
  switch (p) {
    case LinkPhase::Idle: return "idle";
    case LinkPhase::Scanning: return "scanning";
    case LinkPhase::Connecting: return "connecting";
    case LinkPhase::Securing: return "securing";
    case LinkPhase::Discovering: return "discovering";
    case LinkPhase::Up: return "up";
  }
  return "?";
}

const char* msgStatusName(MsgStatus s) {
  switch (s) {
    case MsgStatus::Sending: return "sending";
    case MsgStatus::Sent: return "sent";
    case MsgStatus::Delivered: return "delivered";
    case MsgStatus::Failed: return "failed";
  }
  return "?";
}

using Lock = std::lock_guard<std::recursive_mutex>;

MeshClient::MeshClient(MillisFn millis, UnixFn unixTime) : millis_(millis), unix_(unixTime) {}

void MeshClient::log(const char* fmt, ...) {
  if (!listener_) return;
  char buf[160];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  listener_->onLog(buf);
}

void MeshClient::setTransport(Transport* t) {
  Lock l(mtx_);
  if (transport_ && transport_->isConnected()) transport_->disconnect();
  transport_ = t;
  stopped_ = false;
  releaseForever_ = false;
  backoffMs_ = 2000;
  nextAttempt_ = millis_();
  if (t) {
    t->onFrame([this](const uint8_t* d, size_t n) { pushFrame(d, n); });
    t->onPhase([this](LinkPhase p) {
      phase_ = p;
      if (listener_) listener_->onLinkState(state_, phase_, lastErr_);
    });
    setState(LinkState::Waiting);
  } else {
    setState(LinkState::Disabled);
  }
}

void MeshClient::setState(LinkState s) {
  if (s == state_) return;
  state_ = s;
  if (s != LinkState::Connecting && s != LinkState::Handshake) phase_ = s == LinkState::Ready ? LinkPhase::Up : LinkPhase::Idle;
  if (listener_) listener_->onLinkState(state_, phase_, lastErr_);
}

// ---------------------------------------------------------------- control

void MeshClient::release(uint32_t minutes) {
  Lock l(mtx_);
  releaseForever_ = minutes == 0;
  releaseUntil_ = millis_() + minutes * 60000UL;
  if (transport_ && transport_->isConnected()) transport_->disconnect();
  onLinkDown(LinkError::None);
  setState(LinkState::Released);
  log("module released for %u min", (unsigned)minutes);
}

void MeshClient::reconnect() {
  Lock l(mtx_);
  stopped_ = false;
  releaseForever_ = false;
  releaseUntil_ = 0;
  backoffMs_ = 2000;
  nextAttempt_ = millis_();
  if (transport_ && transport_->isConnected()) {
    transport_->disconnect();
    onLinkDown(LinkError::None);
  }
  setState(transport_ ? LinkState::Waiting : LinkState::Disabled);
}

void MeshClient::disconnect() {
  Lock l(mtx_);
  stopped_ = true;
  if (transport_ && transport_->isConnected()) transport_->disconnect();
  onLinkDown(LinkError::None);
  setState(LinkState::Disabled);
}

void MeshClient::pushFrame(const uint8_t* d, size_t n) {
  if (!n) return;
  std::lock_guard<std::mutex> l(inboxMtx_);
  if (inbox_.size() >= 64) inbox_.pop_front();
  inbox_.emplace_back(d, d + n);
}

// ---------------------------------------------------------------- loop

void MeshClient::loop() {
  std::unique_lock<std::recursive_mutex> lk(mtx_);
  uint32_t now = millis_();
  if (!transport_ || stopped_) return;

  if (state_ == LinkState::Released) {
    if (releaseForever_ || (int32_t)(now - releaseUntil_) < 0) return;
    log("release period over, reconnecting");
    nextAttempt_ = now;
    setState(LinkState::Waiting);
  }

  if (!transport_->isConnected()) {
    if (state_ == LinkState::Ready || state_ == LinkState::Handshake) {
      onLinkDown(LinkError::Lost);
      setState(LinkState::Waiting);
      nextAttempt_ = now + backoffMs_;
      log("link lost");
      return;
    }
    if (state_ == LinkState::Waiting && (int32_t)(now - nextAttempt_) >= 0) {
      lastErr_ = LinkError::None;
      setState(LinkState::Connecting);
      Transport* t = transport_;
      lk.unlock();  // connect() blocks for seconds; keep the API responsive
      bool ok = t->connect();
      lk.lock();
      if (t != transport_ || stopped_ || state_ != LinkState::Connecting) {
        // reconnect / release / new module requested while connecting: drop this link,
        // otherwise it would stay up with no handshake (or hold the radio for nothing).
        if (ok) t->disconnect();
        return;
      }
      if (ok) {
        backoffMs_ = 2000;
        onLinkUp();
      } else {
        lastErr_ = t->lastError();
        nextAttempt_ = millis_() + backoffMs_;
        log("connect failed (%s), retry in %u s", linkErrorName(lastErr_), (unsigned)(backoffMs_ / 1000));
        backoffMs_ = std::min<uint32_t>(backoffMs_ * 2, 60000);
        setState(LinkState::Waiting);
      }
    }
    return;
  }

  transport_->poll();

  for (;;) {
    Bytes f;
    {
      std::lock_guard<std::mutex> il(inboxMtx_);
      if (inbox_.empty()) break;
      f = std::move(inbox_.front());
      inbox_.pop_front();
    }
    handleFrame(f.data(), f.size());
  }

  now = millis_();
  pumpQueue(now);
  checkOutTimeouts(now);

  if (state_ == LinkState::Ready) {
    if (now - lastSync_ >= SYNC_INTERVAL_MS) syncMessages();
    if (now - lastBatt_ >= BATT_INTERVAL_MS) {
      lastBatt_ = now;
      enqueue({enc::getBattAndStorage(), [this](const uint8_t* d, size_t n) {
                 if (d && dec::battStorage(d, n, batt_) && listener_) listener_->onBattery(batt_);
                 return true;
               }});
    }
    if (now - lastTimeSet_ >= TIME_INTERVAL_MS && unix_() > 0) setTimeNow();
    if (!busy_ && queue_.empty() && now - lastRx_ >= KEEPALIVE_MS) syncMessages();  // keep-alive
    if (now - lastRxStats_ >= RX_STATS_MS) {
      lastRxStats_ = now;
      char pushes[96] = "";
      size_t o = 0;
      for (int i = 0; i < 16; i++)
        if (rxPush_[i] && o < sizeof(pushes) - 12) o += snprintf(pushes + o, sizeof(pushes) - o, " %02X:%u", 0x80 + i, (unsigned)rxPush_[i]);
      log("from module in %u min: %u replies, %u messages, pushes%s", (unsigned)(RX_STATS_MS / 60000),
          (unsigned)rxResp_, (unsigned)rxMsgs_, o ? pushes : " none");
      memset(rxPush_, 0, sizeof(rxPush_));
      rxResp_ = rxMsgs_ = 0;
    }
  }
}

void MeshClient::onLinkUp() {
  log("link up (%s)", transport_->name());
  {
    std::lock_guard<std::mutex> il(inboxMtx_);
    inbox_.clear();
  }
  lastRx_ = millis_();
  consecutiveTimeouts_ = 0;
  setState(LinkState::Handshake);
  startHandshake();
}

void MeshClient::onLinkDown(LinkError e) {
  if (e != LinkError::None) lastErr_ = e;
  // Fail queued commands so callers get a result; queued messages stay "sending"
  // and are transmitted again once the link is back.
  dropping_ = true;
  if (busy_ && current_.handler) current_.handler(nullptr, 0);
  busy_ = false;
  for (auto& c : queue_) if (c.handler) c.handler(nullptr, 0);
  for (auto& c : reqQueue_) if (c.handler) c.handler(nullptr, 0);
  queue_.clear();
  reqQueue_.clear();
  dropping_ = false;
  syncing_ = false;
  syncAgain_ = false;
  reqGateUntil_ = 0;
  hsStep_ = -1;
  for (auto& m : out_) {
    if (m.status == MsgStatus::Sending) m.deadline = 0;
  }
}

// ---------------------------------------------------------------- queue

void MeshClient::enqueue(Cmd c, bool front) {
  auto& q = c.isRequest ? reqQueue_ : queue_;
  if (q.size() >= 48) {  // protect RAM if the radio stops answering
    log("command queue full, dropping");
    if (c.handler) c.handler(nullptr, 0);
    return;
  }
  if (front) q.push_front(std::move(c));
  else q.push_back(std::move(c));
}

void MeshClient::pumpQueue(uint32_t now) {
  if (busy_ && (int32_t)(now - cmdDeadline_) >= 0) {
    Handler h = current_.handler;
    uint8_t code = current_.frame.empty() ? 0 : current_.frame[0];
    busy_ = false;
    consecutiveTimeouts_++;
    log("command %u timed out", (unsigned)code);
    if (h) h(nullptr, 0);
    if (consecutiveTimeouts_ >= 3) {  // radio unresponsive: watchdog reconnect
      log("no answer from module, reconnecting");
      transport_->disconnect();
      onLinkDown(LinkError::Lost);
      setState(LinkState::Waiting);
      nextAttempt_ = now + 2000;
      return;
    }
  }
  if (busy_) return;
  if (!queue_.empty()) {
    current_ = std::move(queue_.front());
    queue_.pop_front();
  } else if (!reqQueue_.empty() && (int32_t)(now - reqGateUntil_) >= 0 && state_ == LinkState::Ready) {
    current_ = std::move(reqQueue_.front());
    reqQueue_.pop_front();
  } else {
    return;
  }
  busy_ = true;
  cmdDeadline_ = now + CMD_TIMEOUT_MS;
  if (!transport_->sendFrame(current_.frame.data(), current_.frame.size())) {
    log("write failed");
    transport_->disconnect();
    onLinkDown(LinkError::Lost);
    setState(LinkState::Waiting);
    nextAttempt_ = now + backoffMs_;
  }
}

void MeshClient::handleFrame(const uint8_t* d, size_t n) {
  if (!n) return;
  lastRx_ = millis_();
  consecutiveTimeouts_ = 0;
  if (isPush(d[0]) && d[0] < 0x90) rxPush_[d[0] - 0x80]++;
  else rxResp_++;
  if (isPush(d[0])) {
    handlePush(d, n);
    return;
  }
  if (!busy_) {
    log("unexpected response %u", (unsigned)d[0]);
    return;
  }
  cmdDeadline_ = millis_() + CMD_TIMEOUT_MS;  // multi-frame replies (contacts) extend the deadline
  Handler h = current_.handler;
  bool done = h ? h(d, n) : true;
  if (done) busy_ = false;
}

MeshClient::Handler MeshClient::resultHandler(uint32_t rid) {
  return [this, rid](const uint8_t* d, size_t n) {
    if (!rid || !listener_) return true;
    if (!d) listener_->onResult(rid, false, 0xFF, nullptr, 0);
    else if (d[0] == RESP_ERR) listener_->onResult(rid, false, n > 1 ? d[1] : 0, d, n);
    else listener_->onResult(rid, true, 0, d, n);
    return true;
  };
}

// ---------------------------------------------------------------- handshake

void MeshClient::startHandshake() {
  hsStep_ = 0;
  hsChannel_ = 0;
  handshakeStep();
}

void MeshClient::handshakeStep() {
  auto next = [this]() { hsStep_++; handshakeStep(); };
  switch (hsStep_) {
    case 0:
      enqueue({enc::deviceQuery(), [this, next](const uint8_t* d, size_t n) {
                 if (!d) {
                   if (dropping_) return true;
                   log("module did not answer DEVICE_QUERY");
                   transport_->disconnect();
                   return true;  // loop() notices the disconnect
                 }
                 if (dec::deviceInfo(d, n, dev_) && listener_) listener_->onDeviceInfo(dev_);
                 next();
                 return true;
               }}, true);
      break;
    case 1:
      enqueue({enc::appStart(appName_.c_str()), [this, next](const uint8_t* d, size_t n) {
                 if (!d) { if (!dropping_) next(); return true; }
                 if (dec::selfInfo(d, n, self_) && listener_) listener_->onSelfInfo(self_);
                 next();
                 return true;
               }}, true);
      break;
    case 2:
      if (unix_() > 0) {
        lastTimeSet_ = millis_();
        // The module refuses a time earlier than its own clock: the error is ignored.
        enqueue({enc::setDeviceTime(unix_()), [this, next](const uint8_t* d, size_t) {
                   if (d || !dropping_) next();
                   return true;
                 }}, true);
      } else {
        enqueue({enc::getDeviceTime(), [this, next](const uint8_t* d, size_t n) {
                   uint32_t t;
                   if (!d) { if (!dropping_) next(); return true; }
                   if (dec::currTime(d, n, t) && listener_) listener_->onModuleTime(t);
                   next();
                   return true;
                 }}, true);
      }
      break;
    case 3:
      enqueue({enc::getContacts(0), [this, next](const uint8_t* d, size_t n) {
                 if (!d) { if (!dropping_) next(); return true; }
                 if (d[0] == RESP_CONTACTS_START) {
                   store_->clear();  // filled in place: no second copy of the list
                   return false;
                 }
                 if (d[0] == RESP_CONTACT) {
                   Contact c;
                   if (dec::contact(d, n, c)) store_->add(c);
                   return false;
                 }
                 if (d[0] == RESP_END_OF_CONTACTS) {
                   log("%u contacts", (unsigned)store_->size());
                   if (listener_) listener_->onContactsLoaded();
                 }
                 next();
                 return true;
               }}, true);
      break;
    case 4: {
      int maxCh = dev_.maxChannels ? dev_.maxChannels : 8;
      if (hsChannel_ == 0) {
        channelsTmp_.clear();
        channelsTmpComplete_ = true;
      }
      if (hsChannel_ >= maxCh) {
        channels_.swap(channelsTmp_);
        channelsTmp_.clear();
        channelsComplete_ = channelsTmpComplete_;
        if (listener_) listener_->onChannelsLoaded();
        next();
        break;
      }
      enqueue({enc::getChannel((uint8_t)hsChannel_), [this](const uint8_t* d, size_t n) {
                 if (!d && dropping_) return true;
                 ChannelInfo ch;
                 // An error answer (some firmwares, for an empty slot) is an answer too.
                 bool info = d && dec::channelInfo(d, n, ch) && ch.idx == hsChannel_;
                 if (!info && !(d && n >= 1 && d[0] == RESP_ERR)) channelsTmpComplete_ = false;
                 if (info && ch.name[0]) channelsTmp_.push_back(ch);
                 hsChannel_++;
                 handshakeStep();
                 return true;
               }}, true);
      break;
    }
    case 5:
      lastBatt_ = millis_();
      enqueue({enc::getBattAndStorage(), [this, next](const uint8_t* d, size_t n) {
                 if (!d) { if (!dropping_) next(); return true; }
                 if (dec::battStorage(d, n, batt_) && listener_) listener_->onBattery(batt_);
                 next();
                 return true;
               }}, true);
      break;
    case 6:
      hsStep_ = -1;
      setState(LinkState::Ready);
      log("ready: %s", self_.name.c_str());
      syncMessages();
      for (auto& m : out_)
        if (m.status == MsgStatus::Sending && m.deadline == 0) transmitOut(m);
      break;
    default:
      break;
  }
}

void MeshClient::refreshContacts() {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) return;
  enqueue({enc::getContacts(0), [this](const uint8_t* d, size_t n) {
             if (!d) return true;
             if (d[0] == RESP_CONTACTS_START) {
               store_->clear();
               return false;
             }
             if (d[0] == RESP_CONTACT) {
               Contact c;
               if (dec::contact(d, n, c)) store_->add(c);
               return false;
             }
             if (d[0] == RESP_END_OF_CONTACTS && listener_) listener_->onContactsLoaded();
             return true;
           }});
}

void MeshClient::refreshChannels() {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) return;
  int maxCh = dev_.maxChannels ? dev_.maxChannels : 8;
  auto tmp = std::make_shared<std::vector<ChannelInfo>>();
  auto complete = std::make_shared<bool>(true);
  for (int i = 0; i < maxCh; i++) {
    bool last = i == maxCh - 1;
    enqueue({enc::getChannel((uint8_t)i), [this, tmp, complete, last, i](const uint8_t* d, size_t n) {
               ChannelInfo ch;
               bool info = d && dec::channelInfo(d, n, ch) && ch.idx == i;
               if (!info && !(d && n >= 1 && d[0] == RESP_ERR)) *complete = false;  // timeout, or another slot's answer
               if (info && ch.name[0]) tmp->push_back(ch);
               if (last && !dropping_) {
                 channels_ = *tmp;
                 channelsComplete_ = *complete;
                 if (listener_) listener_->onChannelsLoaded();
               }
               return true;
             }});
  }
}

void MeshClient::refreshSelf() {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) return;
  enqueue({enc::appStart(appName_.c_str()), [this](const uint8_t* d, size_t n) {
             if (d && dec::selfInfo(d, n, self_) && listener_) listener_->onSelfInfo(self_);
             return true;
           }});
  enqueue({enc::deviceQuery(), [this](const uint8_t* d, size_t n) {
             if (d && dec::deviceInfo(d, n, dev_) && listener_) listener_->onDeviceInfo(dev_);
             return true;
           }});
}

void MeshClient::setTimeNow() {
  Lock l(mtx_);
  uint32_t t = unix_();
  if (!t || state_ != LinkState::Ready) return;
  lastTimeSet_ = millis_();
  enqueue({enc::setDeviceTime(t), [](const uint8_t*, size_t) { return true; }});
}

// ---------------------------------------------------------------- message sync

void MeshClient::syncMessages() {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) {
    syncAgain_ = true;
    return;
  }
  lastSync_ = millis_();
  if (syncing_) {
    syncAgain_ = true;
    return;
  }
  syncing_ = true;
  enqueue({enc::syncNextMessage(), [this](const uint8_t* d, size_t n) {
             if (!d) {
               syncing_ = false;
               return true;
             }
             if (d[0] == RESP_NO_MORE_MESSAGES) {
               syncing_ = false;
               lastSyncUnix_ = unix_();
               if (syncAgain_) {
                 syncAgain_ = false;
                 syncMessages();
               }
               return true;
             }
             RecvMsg m;
             if (dec::message(d, n, m)) {
               rxMsgs_++;
               if (listener_) listener_->onMessage(m);
             } else {
               log("skipping queued frame %u", (unsigned)d[0]);  // e.g. channel data: not handled
             }
             syncing_ = false;
             syncMessages();  // fetch the next one
             return true;
           }});
}

// ---------------------------------------------------------------- outgoing messages

uint32_t MeshClient::nextTs() {
  uint32_t t = unix_();
  if (!t) t = lastTs_ ? lastTs_ + 1 : 1;
  if (t <= lastTs_) t = lastTs_ + 1;
  lastTs_ = t;
  return t;
}

bool MeshClient::sendDirect(uint32_t id, const uint8_t prefix[PREFIX_SIZE], const std::string& text, uint8_t txtType) {
  Lock l(mtx_);
  out_.erase(std::remove_if(out_.begin(), out_.end(), [id](const OutMsg& m) { return m.id == id; }), out_.end());
  OutMsg m;
  m.id = id;
  m.channel = false;
  memcpy(m.prefix, prefix, PREFIX_SIZE);
  m.txtType = txtType;
  m.text = utf8Truncate(text, MAX_TEXT_LEN);
  m.ts = nextTs();
  out_.push_back(m);
  if (out_.size() > 64) out_.erase(out_.begin());
  if (state_ == LinkState::Ready) transmitOut(out_.back());
  return true;
}

bool MeshClient::sendChannel(uint32_t id, uint8_t channelIdx, const std::string& text) {
  Lock l(mtx_);
  out_.erase(std::remove_if(out_.begin(), out_.end(), [id](const OutMsg& m) { return m.id == id; }), out_.end());
  OutMsg m;
  m.id = id;
  m.channel = true;
  m.chIdx = channelIdx;
  m.text = utf8Truncate(text, maxChannelText(self_.name.size()));
  m.ts = nextTs();
  out_.push_back(m);
  if (out_.size() > 64) out_.erase(out_.begin());
  if (state_ == LinkState::Ready) transmitOut(out_.back());
  return true;
}

void MeshClient::emitStatus(OutMsg& m, MsgStatus st, uint32_t rtt) {
  m.status = st;
  if (st == MsgStatus::Delivered || st == MsgStatus::Failed || (st == MsgStatus::Sent && m.deadline == 0))
    m.finishedAt = millis_();
  if (listener_) listener_->onMsgStatus(m.id, st, rtt, m.attempt);
}

void MeshClient::transmitOut(OutMsg& m) {
  uint32_t id = m.id;
  m.deadline = 0;
  m.status = MsgStatus::Sending;
  if (m.channel) {
    enqueue({enc::sendChannelTxtMsg(m.chIdx, m.ts, m.text), [this, id](const uint8_t* d, size_t n) {
               auto it = std::find_if(out_.begin(), out_.end(), [id](const OutMsg& o) { return o.id == id; });
               if (it == out_.end() || !d) return true;  // dropped: retransmitted on reconnect
               if (d[0] == RESP_OK) emitStatus(*it, MsgStatus::Sent, 0);  // no ACK on channels
               else {
                 log("channel send error %s", n > 1 ? errorName(d[1]) : "?");
                 emitStatus(*it, MsgStatus::Failed, 0);
               }
               return true;
             }});
    return;
  }
  enqueue({enc::sendTxtMsg(m.txtType, m.attempt, m.ts, m.prefix, m.text), [this, id](const uint8_t* d, size_t n) {
             auto it = std::find_if(out_.begin(), out_.end(), [id](const OutMsg& o) { return o.id == id; });
             if (it == out_.end()) return true;
             OutMsg& m = *it;
             if (!d) {
               if (dropping_) return true;
               m.deadline = millis_();  // command timeout counts as a failed attempt
               m.status = MsgStatus::Sent;
               return true;
             }
             SentInfo si;
             if (dec::sent(d, n, si)) {
               if (si.tag == 0) {  // CLI data: no ACK expected
                 m.deadline = 0;
                 emitStatus(m, MsgStatus::Sent, 0);
                 return true;
               }
               m.acks.push_back(si.tag);
               m.sentAt = millis_();
               uint32_t to = si.timeoutMs < 3000 ? 3000 : si.timeoutMs;
               if (to > 60000) to = 60000;
               m.deadline = m.sentAt + to + 1000;
               bool first = m.attempt == 0;
               m.status = MsgStatus::Sent;
               if (first && listener_) listener_->onMsgStatus(m.id, MsgStatus::Sent, 0, m.attempt);
               return true;
             }
             uint8_t err = n > 1 ? d[1] : 0;
             log("send error %s", errorName(err));
             if (err == ERR_TABLE_FULL) {  // radio busy: retry after a short delay
               m.status = MsgStatus::Sent;
               m.deadline = millis_() + 2000;
             } else {
               emitStatus(m, MsgStatus::Failed, 0);
             }
             return true;
           }});
}

void MeshClient::checkOutTimeouts(uint32_t now) {
  for (auto& m : out_) {
    if (m.status != MsgStatus::Sent || m.deadline == 0 || (int32_t)(now - m.deadline) < 0) continue;
    if (state_ != LinkState::Ready) continue;
    if (m.attempt + 1 < MAX_ATTEMPTS) {
      m.attempt++;
      log("no ACK for msg %u, attempt %u", (unsigned)m.id, (unsigned)(m.attempt + 1));
      // Last attempt: like the official app, forget the direct path so it floods.
      if (m.attempt == MAX_ATTEMPTS - 1) {
        Contact c;
        int i = store_->find(m.prefix, PREFIX_SIZE);
        if (i >= 0 && store_->get(i, c) && c.outPathLen != OUT_PATH_UNKNOWN) {
          enqueue({enc::resetPath(c.pubKey), [](const uint8_t*, size_t) { return true; }});
          c.outPathLen = OUT_PATH_UNKNOWN;
          store_->put(i, c);
        }
      }
      transmitOut(m);
    } else {
      m.deadline = 0;
      emitStatus(m, MsgStatus::Failed, 0);
    }
  }
  // forget finished messages after an hour
  out_.erase(std::remove_if(out_.begin(), out_.end(),
                            [now](const OutMsg& m) { return m.finishedAt && now - m.finishedAt > 3600000UL; }),
             out_.end());
}

// ---------------------------------------------------------------- pushes

void MeshClient::handlePush(const uint8_t* d, size_t n) {
  uint8_t pub[PUB_KEY_SIZE];
  switch (d[0]) {
    case PUSH_MSG_WAITING:
      syncMessages();
      break;
    case PUSH_SEND_CONFIRMED: {
      SendConfirmed sc;
      if (!dec::sendConfirmed(d, n, sc)) break;
      for (auto& m : out_) {
        if (m.status == MsgStatus::Delivered) continue;
        if (std::find(m.acks.begin(), m.acks.end(), sc.ack) != m.acks.end()) {
          m.deadline = 0;
          emitStatus(m, MsgStatus::Delivered, sc.tripMs);
          break;
        }
      }
      break;
    }
    case PUSH_ADVERT:
      if (dec::pubKeyPush(d, n, pub)) {
        Contact c;
        int i = store_->find(pub, PUB_KEY_SIZE);
        if (i >= 0) {
          uint32_t t = unix_();
          if (t) store_->touchAdvert(i, t);
          if (store_->get(i, c) && listener_) listener_->onContact(c, false);
        }
      }
      break;
    case PUSH_NEW_ADVERT: {
      Contact c;
      if (!dec::contact(d, n, c)) break;
      int i = store_->find(c.pubKey, PUB_KEY_SIZE);
      bool isNew = i < 0;
      if (isNew) store_->add(c);
      else store_->put(i, c);
      if (listener_) listener_->onContact(c, isNew);
      break;
    }
    case PUSH_PATH_UPDATED:
      if (dec::pubKeyPush(d, n, pub)) {
        enqueue({enc::getContactByKey(pub), [this](const uint8_t* d2, size_t n2) {
                   Contact c;
                   if (d2 && dec::contact(d2, n2, c)) {
                     int i = store_->find(c.pubKey, PUB_KEY_SIZE);
                     if (i >= 0) store_->put(i, c);
                     if (listener_) listener_->onContact(c, false);
                   }
                   return true;
                 }});
      }
      break;
    case PUSH_CONTACT_DELETED:
      if (dec::pubKeyPush(d, n, pub)) {
        int i = store_->find(pub, PUB_KEY_SIZE);
        if (i >= 0) store_->removeAt(i);
        if (listener_) listener_->onContactRemoved(pub);
      }
      break;
    case PUSH_CONTACTS_FULL:
      if (listener_) listener_->onContactsFull();
      break;
    case PUSH_LOGIN_SUCCESS:
    case PUSH_LOGIN_FAIL: {
      reqGateUntil_ = 0;
      LoginResult r;
      if (dec::login(d, n, r) && listener_) listener_->onLogin(r);
      break;
    }
    case PUSH_STATUS_RESPONSE: {
      reqGateUntil_ = 0;
      RepeaterStatus s;
      if (dec::status(d, n, s) && listener_) listener_->onRepeaterStatus(s);
      break;
    }
    case PUSH_TELEMETRY_RESPONSE: {
      reqGateUntil_ = 0;
      TelemetryResp t;
      if (dec::telemetry(d, n, t) && listener_) listener_->onTelemetry(t);
      break;
    }
    case PUSH_PATH_DISCOVERY_RESPONSE: {
      reqGateUntil_ = 0;
      PathDiscovery p;
      if (dec::pathDiscovery(d, n, p) && listener_) listener_->onPathDiscovery(p);
      break;
    }
    case PUSH_BINARY_RESPONSE:
      reqGateUntil_ = 0;
      break;
    case PUSH_TRACE_DATA: {
      TraceData t;
      if (dec::trace(d, n, t) && listener_) listener_->onTrace(t);
      break;
    }
    default:
      break;  // log/raw/control data and future codes are ignored
  }
}

// ---------------------------------------------------------------- generic commands

void MeshClient::command(const Bytes& frame, uint32_t rid) {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) {
    if (rid && listener_) listener_->onResult(rid, false, ERR_BAD_STATE, nullptr, 0);
    return;
  }
  enqueue({frame, resultHandler(rid)});
}

void MeshClient::request(const Bytes& frame, uint32_t rid) {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) {
    if (rid && listener_) listener_->onResult(rid, false, ERR_BAD_STATE, nullptr, 0);
    return;
  }
  Cmd c;
  c.frame = frame;
  c.isRequest = true;
  c.handler = [this, rid](const uint8_t* d, size_t n) {
    SentInfo si;
    if (d && dec::sent(d, n, si)) {
      uint32_t to = si.timeoutMs + 2000;
      reqGateUntil_ = millis_() + (to > 30000 ? 30000 : to);
    }
    return resultHandler(rid)(d, n);
  };
  enqueue(std::move(c));
}

void MeshClient::login(const uint8_t pub[PUB_KEY_SIZE], const std::string& password, uint32_t rid) {
  request(enc::sendLogin(pub, password), rid);
}

// ---------------------------------------------------------------- snapshots

void MeshClient::forEachContact(const std::function<void(const Contact&)>& fn) {
  Lock l(mtx_);
  Contact c;
  size_t n = store_->size();
  for (size_t i = 0; i < n; i++)
    if (store_->get(i, c)) fn(c);
}

size_t MeshClient::contactCount() { Lock l(mtx_); return store_->size(); }

bool MeshClient::contactAt(size_t i, Contact& out) {
  Lock l(mtx_);
  return store_->get(i, out);
}

void MeshClient::setFavorite(const uint8_t pub[PUB_KEY_SIZE], bool fav, uint32_t rid) {
  Lock l(mtx_);
  if (state_ != LinkState::Ready) {
    if (rid && listener_) listener_->onResult(rid, false, ERR_BAD_STATE, nullptr, 0);
    return;
  }
  // RESP_CONTACT and CMD_ADD_UPDATE_CONTACT share the same layout after the code byte.
  enqueue({enc::getContactByKey(pub), [this, fav, rid](const uint8_t* d, size_t n) {
             if (!d || d[0] != RESP_CONTACT || n < 36) return resultHandler(rid)(d, n);
             Bytes f(d, d + n);
             f[0] = CMD_ADD_UPDATE_CONTACT;
             f[1 + PUB_KEY_SIZE + 1] = fav ? (f[34] | 1) : (f[34] & ~1);  // flags byte
             enqueue({f, resultHandler(rid)}, true);
             return true;
           }});
  refreshContacts();
}
std::vector<ChannelInfo> MeshClient::channels() { Lock l(mtx_); return channels_; }
bool MeshClient::channelsComplete() { Lock l(mtx_); return channelsComplete_; }
DeviceInfo MeshClient::deviceInfo() { Lock l(mtx_); return dev_; }
SelfInfo MeshClient::selfInfo() { Lock l(mtx_); return self_; }
BattStorage MeshClient::battery() { Lock l(mtx_); return batt_; }

bool MeshClient::findContact(const uint8_t* prefix, size_t n, Contact& out) {
  Lock l(mtx_);
  int i = store_->find(prefix, n);
  return i >= 0 && store_->get(i, out);
}

static std::string lower(const std::string& s) {
  std::string r = s;
  for (auto& ch : r) if (ch >= 'A' && ch <= 'Z') ch += 32;
  return r;
}

bool MeshClient::findContactByName(const std::string& name, Contact& out) {
  Lock l(mtx_);
  std::string q = lower(name);
  Contact c, partial;
  int partialCount = 0;
  size_t n = store_->size();
  for (size_t i = 0; i < n; i++) {
    if (!store_->get(i, c)) continue;
    std::string cn = lower(c.name);
    if (cn == q) { out = c; return true; }
    if (cn.find(q) == 0) { partial = c; partialCount++; }
  }
  if (partialCount == 1) { out = partial; return true; }
  return false;
}

bool MeshClient::findChannel(uint8_t idx, ChannelInfo& out) {
  Lock l(mtx_);
  for (auto& c : channels_)
    if (c.idx == idx) { out = c; return true; }
  return false;
}

bool MeshClient::findChannelByName(const std::string& name, ChannelInfo& out) {
  Lock l(mtx_);
  std::string q = lower(name);
  if (!q.empty() && q[0] == '#') q.erase(0, 1);
  for (auto& c : channels_) {
    std::string cn = lower(c.name);
    if (!cn.empty() && cn[0] == '#') cn.erase(0, 1);
    if (cn == q) { out = c; return true; }
  }
  return false;
}

}  // namespace mc
