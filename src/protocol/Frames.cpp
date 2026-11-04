#include "Frames.h"

namespace mc {

// ---------- little-endian reader / writer ----------
namespace {

struct Rd {
  const uint8_t* d;
  size_t n, i = 0;
  Rd(const uint8_t* d_, size_t n_, size_t start = 0) : d(d_), n(n_), i(start) {}
  bool has(size_t k) const { return i + k <= n; }
  size_t left() const { return i < n ? n - i : 0; }
  uint8_t u8() { return i < n ? d[i++] : 0; }
  int8_t i8() { return (int8_t)u8(); }
  uint16_t u16() { uint16_t v = 0; if (has(2)) { v = d[i] | (d[i + 1] << 8); } i += 2; return v; }
  int16_t i16() { return (int16_t)u16(); }
  uint32_t u32() {
    uint32_t v = 0;
    if (has(4)) v = (uint32_t)d[i] | ((uint32_t)d[i + 1] << 8) | ((uint32_t)d[i + 2] << 16) | ((uint32_t)d[i + 3] << 24);
    i += 4;
    return v;
  }
  int32_t i32() { return (int32_t)u32(); }
  void bytes(uint8_t* out, size_t k) {
    for (size_t j = 0; j < k; j++) out[j] = (i + j < n) ? d[i + j] : 0;
    i += k;
  }
  // fixed-size, NUL padded string field
  std::string fixedStr(size_t k) {
    std::string s;
    for (size_t j = 0; j < k && i + j < n; j++) {
      if (d[i + j] == 0) break;
      s += (char)d[i + j];
    }
    i += k;
    return s;
  }
  std::string rest() {
    std::string s;
    if (i < n) s.assign((const char*)d + i, n - i);
    // firmware strings may contain trailing NULs
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
    i = n;
    return s;
  }
};

struct Wr {
  Bytes b;
  explicit Wr(uint8_t code) { b.reserve(32); b.push_back(code); }
  Wr& u8(uint8_t v) { b.push_back(v); return *this; }
  Wr& u16(uint16_t v) { b.push_back(v & 0xFF); b.push_back(v >> 8); return *this; }
  Wr& u32(uint32_t v) {
    for (int k = 0; k < 4; k++) b.push_back((v >> (8 * k)) & 0xFF);
    return *this;
  }
  Wr& i32(int32_t v) { return u32((uint32_t)v); }
  Wr& raw(const uint8_t* p, size_t k) { b.insert(b.end(), p, p + k); return *this; }
  Wr& str(const std::string& s) { b.insert(b.end(), s.begin(), s.end()); return *this; }
  Wr& fixedStr(const std::string& s, size_t k) {
    for (size_t j = 0; j < k; j++) b.push_back(j < s.size() && j < k - 1 ? (uint8_t)s[j] : 0);
    return *this;
  }
  Bytes done() {
    if (b.size() > MAX_FRAME_SIZE) b.resize(MAX_FRAME_SIZE);
    return std::move(b);
  }
};

}  // namespace

// ---------- encoders ----------
namespace enc {

Bytes deviceQuery(uint8_t ver) { return Wr(CMD_DEVICE_QUERY).u8(ver).done(); }

Bytes appStart(const char* appName) {
  // [1, ver, 6 reserved, app name]; firmware ignores bytes 1..7
  Wr w(CMD_APP_START);
  w.u8(PROTOCOL_VERSION);
  for (int k = 0; k < 6; k++) w.u8(' ');
  w.str(appName);
  return w.done();
}

Bytes setDeviceTime(uint32_t t) { return Wr(CMD_SET_DEVICE_TIME).u32(t).done(); }
Bytes getDeviceTime() { return Wr(CMD_GET_DEVICE_TIME).done(); }

Bytes getContacts(uint32_t since) {
  Wr w(CMD_GET_CONTACTS);
  if (since) w.u32(since);
  return w.done();
}

Bytes getChannel(uint8_t idx) { return Wr(CMD_GET_CHANNEL).u8(idx).done(); }

Bytes setChannel(uint8_t idx, const std::string& name, const uint8_t secret[16]) {
  return Wr(CMD_SET_CHANNEL).u8(idx).fixedStr(name, 32).raw(secret, 16).done();
}

Bytes getBattAndStorage() { return Wr(CMD_GET_BATT_AND_STORAGE).done(); }
Bytes syncNextMessage() { return Wr(CMD_SYNC_NEXT_MESSAGE).done(); }

Bytes sendTxtMsg(uint8_t txtType, uint8_t attempt, uint32_t ts, const uint8_t prefix[PREFIX_SIZE], const std::string& text) {
  return Wr(CMD_SEND_TXT_MSG).u8(txtType).u8(attempt).u32(ts).raw(prefix, PREFIX_SIZE)
      .str(utf8Truncate(text, MAX_TEXT_LEN)).done();
}

Bytes sendChannelTxtMsg(uint8_t channelIdx, uint32_t ts, const std::string& text) {
  return Wr(CMD_SEND_CHANNEL_TXT_MSG).u8(TXT_PLAIN).u8(channelIdx).u32(ts)
      .str(utf8Truncate(text, MAX_TEXT_LEN)).done();
}

Bytes sendLogin(const uint8_t pub[PUB_KEY_SIZE], const std::string& password) {
  return Wr(CMD_SEND_LOGIN).raw(pub, PUB_KEY_SIZE).str(password.substr(0, 15)).done();
}

Bytes logout(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_LOGOUT).raw(pub, PUB_KEY_SIZE).done(); }
Bytes statusReq(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_SEND_STATUS_REQ).raw(pub, PUB_KEY_SIZE).done(); }

Bytes telemetryReq(const uint8_t pub[PUB_KEY_SIZE]) {
  // [39, reserved x3, pub_key]
  return Wr(CMD_SEND_TELEMETRY_REQ).u8(0).u8(0).u8(0).raw(pub, PUB_KEY_SIZE).done();
}

Bytes tracePath(uint32_t tag, uint32_t auth, uint8_t flags, const Bytes& path) {
  return Wr(CMD_SEND_TRACE_PATH).u32(tag).u32(auth).u8(flags).raw(path.data(), path.size()).done();
}

Bytes pathDiscoveryReq(const uint8_t pub[PUB_KEY_SIZE]) {
  return Wr(CMD_SEND_PATH_DISCOVERY_REQ).u8(0).raw(pub, PUB_KEY_SIZE).done();
}

Bytes selfAdvert(bool flood) { return Wr(CMD_SEND_SELF_ADVERT).u8(flood ? 1 : 0).done(); }
Bytes setAdvertName(const std::string& name) { return Wr(CMD_SET_ADVERT_NAME).str(utf8Truncate(name, 31)).done(); }
Bytes setAdvertLatLon(int32_t lat, int32_t lon) { return Wr(CMD_SET_ADVERT_LATLON).i32(lat).i32(lon).done(); }

Bytes setRadioParams(uint32_t freq, uint32_t bw, uint8_t sf, uint8_t cr, bool repeat) {
  return Wr(CMD_SET_RADIO_PARAMS).u32(freq).u32(bw).u8(sf).u8(cr).u8(repeat ? 1 : 0).done();
}

Bytes setTxPower(int8_t dbm) { return Wr(CMD_SET_RADIO_TX_POWER).u8((uint8_t)dbm).done(); }
Bytes setDevicePin(uint32_t pin) { return Wr(CMD_SET_DEVICE_PIN).u32(pin).done(); }

Bytes setOtherParams(uint8_t manualAdd, uint8_t telemetryModes, uint8_t advertLocPolicy, uint8_t multiAcks) {
  return Wr(CMD_SET_OTHER_PARAMS).u8(manualAdd).u8(telemetryModes).u8(advertLocPolicy).u8(multiAcks).done();
}

Bytes setAutoAddConfig(uint8_t cfg, uint8_t maxHops) { return Wr(CMD_SET_AUTOADD_CONFIG).u8(cfg).u8(maxHops).done(); }
Bytes getAutoAddConfig() { return Wr(CMD_GET_AUTOADD_CONFIG).done(); }
Bytes setPathHashMode(uint8_t mode) { return Wr(CMD_SET_PATH_HASH_MODE).u8(0).u8(mode).done(); }
Bytes reboot() { return Wr(CMD_REBOOT).str("reboot").done(); }
Bytes getStats(uint8_t type) { return Wr(CMD_GET_STATS).u8(type).done(); }
Bytes resetPath(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_RESET_PATH).raw(pub, PUB_KEY_SIZE).done(); }
Bytes removeContact(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_REMOVE_CONTACT).raw(pub, PUB_KEY_SIZE).done(); }
Bytes shareContact(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_SHARE_CONTACT).raw(pub, PUB_KEY_SIZE).done(); }

Bytes exportContact(const uint8_t* pub) {
  Wr w(CMD_EXPORT_CONTACT);
  if (pub) w.raw(pub, PUB_KEY_SIZE);
  return w.done();
}

Bytes importContact(const Bytes& pkt) { return Wr(CMD_IMPORT_CONTACT).raw(pkt.data(), pkt.size()).done(); }

Bytes addUpdateContact(const Contact& c) {
  Wr w(CMD_ADD_UPDATE_CONTACT);
  // Only for contacts whose route is unknown or fully cached (see Contact::pathCached).
  w.raw(c.pubKey, PUB_KEY_SIZE).u8(c.type).u8(c.flags).u8(c.outPathLen).raw(c.outPath, CONTACT_PATH_CACHE);
  for (size_t k = CONTACT_PATH_CACHE; k < MAX_PATH_SIZE; k++) w.u8(0);
  w.fixedStr(c.name, 32).u32(c.lastAdvert).i32(c.lat).i32(c.lon).u32(c.lastmod);
  return w.done();
}

Bytes getContactByKey(const uint8_t pub[PUB_KEY_SIZE]) { return Wr(CMD_GET_CONTACT_BY_KEY).raw(pub, PUB_KEY_SIZE).done(); }

}  // namespace enc

// ---------- decoders ----------
namespace dec {

bool deviceInfo(const uint8_t* d, size_t n, DeviceInfo& o) {
  if (n < 2 || d[0] != RESP_DEVICE_INFO) return false;
  Rd r(d, n, 1);
  o.fwVer = r.u8();
  if (r.left() >= 2) {  // v3+
    o.maxContacts = (uint16_t)r.u8() * 2;
    o.maxChannels = r.u8();
  }
  if (r.left() >= 4) o.blePin = r.u32();
  if (r.left() >= 12) o.buildDate = r.fixedStr(12);
  if (r.left() >= 40) o.manufacturer = r.fixedStr(40);
  if (r.left() >= 20) o.fwVersion = r.fixedStr(20);
  if (r.left() >= 1) o.repeat = r.u8() != 0;    // v9+
  if (r.left() >= 1) o.pathHashMode = r.u8();   // v10+
  return true;
}

bool selfInfo(const uint8_t* d, size_t n, SelfInfo& o) {
  if (n < 1 + 3 + PUB_KEY_SIZE || d[0] != RESP_SELF_INFO) return false;
  Rd r(d, n, 1);
  o.advType = r.u8();
  o.txPower = r.i8();
  o.maxTxPower = r.i8();
  r.bytes(o.pubKey, PUB_KEY_SIZE);
  o.lat = r.i32();
  o.lon = r.i32();
  o.multiAcks = r.u8();
  o.advertLocPolicy = r.u8();
  o.telemetryModes = r.u8();
  o.manualAddContacts = r.u8();
  o.freq = r.u32();
  o.bw = r.u32();
  o.sf = r.u8();
  o.cr = r.u8();
  o.name = r.rest();
  return true;
}

bool contact(const uint8_t* d, size_t n, Contact& o) {
  // code + pub(32) + type + flags + path_len + path(64) + name(32) + last_advert(4) = 136
  if (n < 136 || (d[0] != RESP_CONTACT && d[0] != PUSH_NEW_ADVERT)) return false;
  Rd r(d, n, 1);
  r.bytes(o.pubKey, PUB_KEY_SIZE);
  o.type = r.u8();
  o.flags = r.u8();
  o.outPathLen = r.u8();
  r.bytes(o.outPath, CONTACT_PATH_CACHE);
  r.i += MAX_PATH_SIZE - CONTACT_PATH_CACHE;
  std::string nm = utf8Truncate(r.fixedStr(32), sizeof(o.name) - 1);
  memset(o.name, 0, sizeof(o.name));
  memcpy(o.name, nm.data(), nm.size());
  o.lastAdvert = r.u32();
  o.lat = r.left() >= 4 ? r.i32() : 0;
  o.lon = r.left() >= 4 ? r.i32() : 0;
  o.lastmod = r.left() >= 4 ? r.u32() : 0;
  return true;
}

bool contactsStart(const uint8_t* d, size_t n, uint32_t& count) {
  if (n < 1 || d[0] != RESP_CONTACTS_START) return false;
  Rd r(d, n, 1);
  count = r.left() >= 4 ? r.u32() : 0;
  return true;
}

bool channelInfo(const uint8_t* d, size_t n, ChannelInfo& o) {
  if (n < 2 + 32 || d[0] != RESP_CHANNEL_INFO) return false;
  Rd r(d, n, 1);
  o.idx = r.u8();
  std::string nm = r.fixedStr(32);
  memset(o.name, 0, sizeof(o.name));
  memcpy(o.name, nm.data(), nm.size() < 32 ? nm.size() : 32);
  r.bytes(o.secret, 16);
  return true;
}

bool message(const uint8_t* d, size_t n, RecvMsg& o) {
  if (n < 1) return false;
  uint8_t code = d[0];
  Rd r(d, n, 1);
  o = RecvMsg();
  if (code == RESP_CONTACT_MSG_RECV_V3 || code == RESP_CHANNEL_MSG_RECV_V3) {
    if (n < 4) return false;
    o.hasSnr = true;
    o.snr = r.i8() / 4.0f;
    r.u8();  // reserved
    r.u8();  // reserved
  } else if (code != RESP_CONTACT_MSG_RECV && code != RESP_CHANNEL_MSG_RECV) {
    return false;
  }
  if (code == RESP_CONTACT_MSG_RECV || code == RESP_CONTACT_MSG_RECV_V3) {
    if (r.left() < PREFIX_SIZE + 1 + 1 + 4) return false;
    o.isChannel = false;
    r.bytes(o.prefix, PREFIX_SIZE);
  } else {
    if (r.left() < 1 + 1 + 1 + 4) return false;
    o.isChannel = true;
    o.channelIdx = r.u8();
  }
  o.pathLen = r.u8();
  o.txtType = r.u8();
  o.senderTs = r.u32();
  if (!o.isChannel && o.txtType == TXT_SIGNED_PLAIN && r.left() >= 4) {
    o.hasAuthor = true;
    r.bytes(o.author, 4);
  }
  o.text = r.rest();
  return true;
}

bool sent(const uint8_t* d, size_t n, SentInfo& o) {
  if (n < 10 || d[0] != RESP_SENT) return false;
  Rd r(d, n, 1);
  o.flood = r.u8() != 0;
  o.tag = r.u32();
  o.timeoutMs = r.u32();
  return true;
}

bool sendConfirmed(const uint8_t* d, size_t n, SendConfirmed& o) {
  if (n < 5 || d[0] != PUSH_SEND_CONFIRMED) return false;
  Rd r(d, n, 1);
  o.ack = r.u32();
  o.tripMs = r.left() >= 4 ? r.u32() : 0;
  return true;
}

bool login(const uint8_t* d, size_t n, LoginResult& o) {
  if (n < 2 + PREFIX_SIZE || (d[0] != PUSH_LOGIN_SUCCESS && d[0] != PUSH_LOGIN_FAIL)) return false;
  Rd r(d, n, 1);
  o.ok = d[0] == PUSH_LOGIN_SUCCESS;
  o.permissions = r.u8();
  r.bytes(o.prefix, PREFIX_SIZE);
  if (r.left() >= 4) {
    o.hasServerTs = true;
    o.serverTs = r.u32();
  }
  if (r.left() >= 1) o.acl = r.u8();
  if (r.left() >= 1) o.fwLevel = r.u8();
  return true;
}

bool status(const uint8_t* d, size_t n, RepeaterStatus& o) {
  // [0x87, reserved, prefix(6), status fields(52 or 56)]
  if (n < 8 + 52 || d[0] != PUSH_STATUS_RESPONSE) return false;
  Rd r(d, n, 2);
  r.bytes(o.prefix, PREFIX_SIZE);
  o.battMv = r.u16();
  o.txQueue = r.u16();
  o.noiseFloor = r.i16();
  o.lastRssi = r.i16();
  o.nRecv = r.u32();
  o.nSent = r.u32();
  o.airtime = r.u32();
  o.uptime = r.u32();
  o.sentFlood = r.u32();
  o.sentDirect = r.u32();
  o.recvFlood = r.u32();
  o.recvDirect = r.u32();
  o.fullEvents = r.u16();
  o.lastSnr = r.i16() / 4.0f;
  o.directDups = r.u16();
  o.floodDups = r.u16();
  o.rxAirtime = r.u32();
  if (r.left() >= 4) {
    o.hasRecvErrors = true;
    o.recvErrors = r.u32();
  }
  return true;
}

bool telemetry(const uint8_t* d, size_t n, TelemetryResp& o) {
  if (n < 2 + PREFIX_SIZE || d[0] != PUSH_TELEMETRY_RESPONSE) return false;
  Rd r(d, n, 2);
  r.bytes(o.prefix, PREFIX_SIZE);
  o.lpp.assign(d + r.i, d + n);
  return true;
}

bool trace(const uint8_t* d, size_t n, TraceData& o) {
  if (n < 12 || d[0] != PUSH_TRACE_DATA) return false;
  Rd r(d, n, 2);
  o.pathLen = r.u8();
  o.flags = r.u8();
  o.tag = r.u32();
  o.auth = r.u32();
  uint8_t pathSz = o.flags & 0x03;
  size_t nHashes = o.pathLen;
  size_t nSnrs = o.pathLen >> pathSz;
  if (!r.has(nHashes + nSnrs)) return false;
  o.hashes.assign(d + r.i, d + r.i + nHashes);
  r.i += nHashes;
  o.snrs.clear();
  for (size_t k = 0; k < nSnrs; k++) o.snrs.push_back(r.i8() / 4.0f);
  if (r.left() >= 1) o.snrs.push_back(r.i8() / 4.0f);  // final SNR (to this node)
  return true;
}

bool pathDiscovery(const uint8_t* d, size_t n, PathDiscovery& o) {
  if (n < 2 + PREFIX_SIZE + 2 || d[0] != PUSH_PATH_DISCOVERY_RESPONSE) return false;
  Rd r(d, n, 2);
  r.bytes(o.prefix, PREFIX_SIZE);
  o.outPathLen = r.u8();
  size_t ol = pathByteLen(o.outPathLen);
  if (!r.has(ol + 1)) return false;
  o.outPath.assign(d + r.i, d + r.i + ol);
  r.i += ol;
  o.inPathLen = r.u8();
  size_t il = pathByteLen(o.inPathLen);
  if (!r.has(il)) return false;
  o.inPath.assign(d + r.i, d + r.i + il);
  return true;
}

bool battStorage(const uint8_t* d, size_t n, BattStorage& o) {
  if (n < 3 || d[0] != RESP_BATT_AND_STORAGE) return false;
  Rd r(d, n, 1);
  o.mv = r.u16();
  o.usedKb = r.left() >= 4 ? r.u32() : 0;
  o.totalKb = r.left() >= 4 ? r.u32() : 0;
  return true;
}

bool stats(const uint8_t* d, size_t n, Stats& o) {
  if (n < 2 || d[0] != RESP_STATS) return false;
  Rd r(d, n, 1);
  o.type = r.u8();
  if (o.type == STATS_CORE) {
    if (!r.has(9)) return false;
    o.battMv = r.u16(); o.uptime = r.u32(); o.errFlags = r.u16(); o.queueLen = r.u8();
  } else if (o.type == STATS_RADIO) {
    if (!r.has(12)) return false;
    o.noiseFloor = r.i16(); o.lastRssi = r.i8(); o.lastSnr = r.i8() / 4.0f; o.txAir = r.u32(); o.rxAir = r.u32();
  } else if (o.type == STATS_PACKETS) {
    if (!r.has(24)) return false;
    o.recv = r.u32(); o.sent = r.u32(); o.sentFlood = r.u32(); o.sentDirect = r.u32();
    o.recvFlood = r.u32(); o.recvDirect = r.u32();
    o.recvErrors = r.left() >= 4 ? r.u32() : 0;
  } else {
    return false;
  }
  return true;
}

bool currTime(const uint8_t* d, size_t n, uint32_t& t) {
  if (n < 5 || d[0] != RESP_CURR_TIME) return false;
  Rd r(d, n, 1);
  t = r.u32();
  return true;
}

bool pubKeyPush(const uint8_t* d, size_t n, uint8_t pub[PUB_KEY_SIZE]) {
  if (n < 1 + PUB_KEY_SIZE) return false;
  memcpy(pub, d + 1, PUB_KEY_SIZE);
  return true;
}

bool error(const uint8_t* d, size_t n, uint8_t& err) {
  if (n < 1 || d[0] != RESP_ERR) return false;
  err = n >= 2 ? d[1] : 0;
  return true;
}

}  // namespace dec

// ---------- helpers ----------

bool Contact::pathCached() const { return pathByteLen(outPathLen) <= (int)CONTACT_PATH_CACHE; }

std::string RecvMsg::channelSender() const {
  size_t p = text.find(": ");
  return p == std::string::npos ? std::string() : text.substr(0, p);
}

std::string RecvMsg::channelBody() const {
  size_t p = text.find(": ");
  return p == std::string::npos ? text : text.substr(p + 2);
}

std::string toHex(const uint8_t* d, size_t n) {
  static const char* hx = "0123456789abcdef";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; i++) {
    s += hx[d[i] >> 4];
    s += hx[d[i] & 15];
  }
  return s;
}

static int hexNib(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

bool fromHex(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != n * 2) return false;
  for (size_t i = 0; i < n; i++) {
    int a = hexNib(s[2 * i]), b = hexNib(s[2 * i + 1]);
    if (a < 0 || b < 0) return false;
    out[i] = (uint8_t)((a << 4) | b);
  }
  return true;
}

Bytes fromHexVar(const std::string& s) {
  Bytes b;
  if (s.size() % 2) return b;
  b.resize(s.size() / 2);
  if (!fromHex(s, b.data(), b.size())) b.clear();
  return b;
}

const char* errorName(uint8_t err) {
  switch (err) {
    case ERR_UNSUPPORTED_CMD: return "unsupported";
    case ERR_NOT_FOUND: return "not_found";
    case ERR_TABLE_FULL: return "table_full";
    case ERR_BAD_STATE: return "bad_state";
    case ERR_FILE_IO: return "file_io";
    case ERR_ILLEGAL_ARG: return "illegal_arg";
    default: return "unknown";
  }
}

std::string utf8Truncate(const std::string& s, size_t maxBytes) {
  if (s.size() <= maxBytes) return s;
  size_t cut = maxBytes;
  // step back over continuation bytes (10xxxxxx) so we never split a code point
  while (cut > 0 && ((uint8_t)s[cut] & 0xC0) == 0x80) cut--;
  return s.substr(0, cut);
}

// ---------- SHA-256 (compact, FIPS 180-4) ----------
namespace {
const uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
inline uint32_t rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
void block(uint32_t h[8], const uint8_t* p) {
  uint32_t w[64];
  for (int i = 0; i < 16; i++) w[i] = (p[4 * i] << 24) | (p[4 * i + 1] << 16) | (p[4 * i + 2] << 8) | p[4 * i + 3];
  for (int i = 16; i < 64; i++) {
    uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }
  uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = hh + (rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}
}  // namespace

void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
  uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
  size_t i = 0;
  for (; i + 64 <= len; i += 64) block(h, data + i);
  uint8_t buf[128] = {0};
  size_t rem = len - i;
  memcpy(buf, data + i, rem);
  buf[rem] = 0x80;
  size_t total = rem + 1 + 8 <= 64 ? 64 : 128;
  uint64_t bits = (uint64_t)len * 8;
  for (int k = 0; k < 8; k++) buf[total - 1 - k] = (uint8_t)(bits >> (8 * k));
  block(h, buf);
  if (total == 128) block(h, buf + 64);
  for (int k = 0; k < 8; k++) {
    out[4 * k] = (uint8_t)(h[k] >> 24); out[4 * k + 1] = (uint8_t)(h[k] >> 16);
    out[4 * k + 2] = (uint8_t)(h[k] >> 8); out[4 * k + 3] = (uint8_t)h[k];
  }
}

void hashtagSecret(const std::string& name, uint8_t out[16]) {
  std::string n = (!name.empty() && name[0] == '#') ? name : "#" + name;
  uint8_t full[32];
  sha256((const uint8_t*)n.data(), n.size(), full);
  memcpy(out, full, 16);
}

}  // namespace mc
