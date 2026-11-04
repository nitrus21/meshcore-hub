// Encoding / decoding of MeshCore Companion frames.
// Pure C++ (no Arduino dependency) so it can be unit tested on the host.
#pragma once
#include <stdint.h>
#include <string.h>
#include <string>
#include <vector>
#include "Codes.h"

namespace mc {

using Bytes = std::vector<uint8_t>;

// ---------- data model ----------

struct DeviceInfo {
  uint8_t fwVer = 0;
  uint16_t maxContacts = 0;
  uint8_t maxChannels = 0;
  uint32_t blePin = 0;
  std::string buildDate, manufacturer, fwVersion;
  bool repeat = false;
  uint8_t pathHashMode = 0;
};

struct SelfInfo {
  uint8_t advType = 0;
  int8_t txPower = 0;
  int8_t maxTxPower = 0;
  uint8_t pubKey[PUB_KEY_SIZE] = {0};
  int32_t lat = 0, lon = 0;            // degrees * 1e6
  uint8_t multiAcks = 0;
  uint8_t advertLocPolicy = 0;
  uint8_t telemetryModes = 0;
  uint8_t manualAddContacts = 0;
  uint32_t freq = 0;                   // kHz (MHz * 1000)
  uint32_t bw = 0;                     // Hz/1000 * 1000 (kHz * 1000)
  uint8_t sf = 0, cr = 0;
  std::string name;
};

// Contacts are kept in RAM by the hundreds (a module can hold 350+), so they are
// compact: only the first CONTACT_PATH_CACHE bytes of the route are stored, which
// covers 16 hops with 1-byte hashes (8 with 2-byte hashes). pathCached() says whether the route is complete.
constexpr size_t CONTACT_PATH_CACHE = 16;

struct Contact {
  uint8_t pubKey[PUB_KEY_SIZE] = {0};
  uint8_t type = 0;
  uint8_t flags = 0;
  uint8_t outPathLen = OUT_PATH_UNKNOWN;
  uint8_t outPath[CONTACT_PATH_CACHE] = {0};
  char name[32] = {0};                 // 31 chars + NUL (the frame field is 32 bytes)
  uint32_t lastAdvert = 0;
  int32_t lat = 0, lon = 0;
  uint32_t lastmod = 0;
  bool samePrefix(const uint8_t* p, size_t n = PREFIX_SIZE) const { return memcmp(pubKey, p, n) == 0; }
  bool pathCached() const;
};

struct ChannelInfo {
  uint8_t idx = 0;
  char name[33] = {0};
  uint8_t secret[16] = {0};
};

struct RecvMsg {
  bool isChannel = false;
  bool hasSnr = false;
  float snr = 0;
  uint8_t prefix[PREFIX_SIZE] = {0};   // private: sender (or room) pubkey prefix
  uint8_t channelIdx = 0;              // channel messages
  uint8_t pathLen = OUT_PATH_UNKNOWN;  // 0xFF = direct
  uint8_t txtType = TXT_PLAIN;
  uint32_t senderTs = 0;
  bool hasAuthor = false;              // room (signed) messages
  uint8_t author[4] = {0};
  std::string text;
  // For channels the text is "Sender: message"; split helpers below.
  std::string channelSender() const;
  std::string channelBody() const;
};

struct SentInfo {
  bool flood = false;
  uint32_t tag = 0;          // expected_ack for messages, tag for requests
  uint32_t timeoutMs = 0;
};

struct SendConfirmed {
  uint32_t ack = 0;
  uint32_t tripMs = 0;
};

struct LoginResult {
  bool ok = false;
  uint8_t permissions = 0;
  uint8_t prefix[PREFIX_SIZE] = {0};
  bool hasServerTs = false;
  uint32_t serverTs = 0;
  uint8_t acl = 0;
  uint8_t fwLevel = 0;
};

struct RepeaterStatus {
  uint8_t prefix[PREFIX_SIZE] = {0};
  uint16_t battMv = 0, txQueue = 0;
  int16_t noiseFloor = 0, lastRssi = 0;
  uint32_t nRecv = 0, nSent = 0, airtime = 0, uptime = 0;
  uint32_t sentFlood = 0, sentDirect = 0, recvFlood = 0, recvDirect = 0;
  uint16_t fullEvents = 0;
  float lastSnr = 0;
  uint16_t directDups = 0, floodDups = 0;
  uint32_t rxAirtime = 0;
  bool hasRecvErrors = false;
  uint32_t recvErrors = 0;
};

struct TelemetryResp {
  uint8_t prefix[PREFIX_SIZE] = {0};
  Bytes lpp;                 // Cayenne LPP payload
};

struct TraceData {
  uint8_t pathLen = 0, flags = 0;
  uint32_t tag = 0, auth = 0;
  Bytes hashes;              // path_len bytes
  std::vector<float> snrs;   // one per hop (+ final SNR appended)
};

struct PathDiscovery {
  uint8_t prefix[PREFIX_SIZE] = {0};
  uint8_t outPathLen = 0, inPathLen = 0;
  Bytes outPath, inPath;
};

struct BattStorage {
  uint16_t mv = 0;
  uint32_t usedKb = 0, totalKb = 0;
};

struct Stats {
  uint8_t type = 0;
  // core
  uint16_t battMv = 0; uint32_t uptime = 0; uint16_t errFlags = 0; uint8_t queueLen = 0;
  // radio
  int16_t noiseFloor = 0; int8_t lastRssi = 0; float lastSnr = 0; uint32_t txAir = 0, rxAir = 0;
  // packets
  uint32_t recv = 0, sent = 0, sentFlood = 0, sentDirect = 0, recvFlood = 0, recvDirect = 0, recvErrors = 0;
};

// ---------- encoding ----------
namespace enc {
Bytes deviceQuery(uint8_t ver = PROTOCOL_VERSION);
Bytes appStart(const char* appName);
Bytes setDeviceTime(uint32_t unixSecs);
Bytes getDeviceTime();
Bytes getContacts(uint32_t since = 0);
Bytes getChannel(uint8_t idx);
Bytes setChannel(uint8_t idx, const std::string& name, const uint8_t secret[16]);
Bytes getBattAndStorage();
Bytes syncNextMessage();
Bytes sendTxtMsg(uint8_t txtType, uint8_t attempt, uint32_t ts, const uint8_t prefix[PREFIX_SIZE], const std::string& text);
Bytes sendChannelTxtMsg(uint8_t channelIdx, uint32_t ts, const std::string& text);
Bytes sendLogin(const uint8_t pub[PUB_KEY_SIZE], const std::string& password);
Bytes logout(const uint8_t pub[PUB_KEY_SIZE]);
Bytes statusReq(const uint8_t pub[PUB_KEY_SIZE]);
Bytes telemetryReq(const uint8_t pub[PUB_KEY_SIZE]);
Bytes tracePath(uint32_t tag, uint32_t auth, uint8_t flags, const Bytes& path);
Bytes pathDiscoveryReq(const uint8_t pub[PUB_KEY_SIZE]);
Bytes selfAdvert(bool flood);
Bytes setAdvertName(const std::string& name);
Bytes setAdvertLatLon(int32_t lat, int32_t lon);
Bytes setRadioParams(uint32_t freq, uint32_t bw, uint8_t sf, uint8_t cr, bool repeat);
Bytes setTxPower(int8_t dbm);
Bytes setDevicePin(uint32_t pin);
Bytes setOtherParams(uint8_t manualAdd, uint8_t telemetryModes, uint8_t advertLocPolicy, uint8_t multiAcks);
Bytes setAutoAddConfig(uint8_t cfg, uint8_t maxHops);
Bytes getAutoAddConfig();
Bytes setPathHashMode(uint8_t mode);
Bytes reboot();
Bytes getStats(uint8_t type);
Bytes resetPath(const uint8_t pub[PUB_KEY_SIZE]);
Bytes removeContact(const uint8_t pub[PUB_KEY_SIZE]);
Bytes shareContact(const uint8_t pub[PUB_KEY_SIZE]);
Bytes exportContact(const uint8_t* pubOrNull);   // null = export self
Bytes importContact(const Bytes& advertPacket);
Bytes addUpdateContact(const Contact& c);
Bytes getContactByKey(const uint8_t pub[PUB_KEY_SIZE]);
}  // namespace enc

// ---------- decoding ----------
// All decoders return false when the frame is too short or has the wrong code.
namespace dec {
bool deviceInfo(const uint8_t* d, size_t n, DeviceInfo& out);
bool selfInfo(const uint8_t* d, size_t n, SelfInfo& out);
bool contact(const uint8_t* d, size_t n, Contact& out);          // RESP_CONTACT or PUSH_NEW_ADVERT
bool contactsStart(const uint8_t* d, size_t n, uint32_t& count);
bool channelInfo(const uint8_t* d, size_t n, ChannelInfo& out);
bool message(const uint8_t* d, size_t n, RecvMsg& out);           // codes 7, 8, 16, 17
bool sent(const uint8_t* d, size_t n, SentInfo& out);
bool sendConfirmed(const uint8_t* d, size_t n, SendConfirmed& out);
bool login(const uint8_t* d, size_t n, LoginResult& out);         // 0x85 / 0x86
bool status(const uint8_t* d, size_t n, RepeaterStatus& out);     // 0x87
bool telemetry(const uint8_t* d, size_t n, TelemetryResp& out);   // 0x8B
bool trace(const uint8_t* d, size_t n, TraceData& out);           // 0x89
bool pathDiscovery(const uint8_t* d, size_t n, PathDiscovery& out);  // 0x8D
bool battStorage(const uint8_t* d, size_t n, BattStorage& out);
bool stats(const uint8_t* d, size_t n, Stats& out);
bool currTime(const uint8_t* d, size_t n, uint32_t& out);
bool pubKeyPush(const uint8_t* d, size_t n, uint8_t pub[PUB_KEY_SIZE]);  // 0x80, 0x81, 0x8F
bool error(const uint8_t* d, size_t n, uint8_t& err);
}  // namespace dec

// ---------- helpers ----------
std::string toHex(const uint8_t* d, size_t n);
bool fromHex(const std::string& s, uint8_t* out, size_t n);   // exact length
Bytes fromHexVar(const std::string& s);                        // any even length; empty on error
const char* errorName(uint8_t err);
// UTF-8 safe truncation to at most maxBytes bytes.
std::string utf8Truncate(const std::string& s, size_t maxBytes);
// Hashtag channel secret: first 16 bytes of SHA-256("#name").
void hashtagSecret(const std::string& name, uint8_t out[16]);
void sha256(const uint8_t* data, size_t len, uint8_t out[32]);

// Maximum text payload, mirroring BaseChatMesh.h MAX_TEXT_LEN (10 * CIPHER_BLOCK_SIZE).
// Channel messages also carry "<node name>: " inside that budget.
constexpr size_t MAX_TEXT_LEN = 160;
inline size_t maxChannelText(size_t nodeNameLen) {
  return nodeNameLen + 2 >= MAX_TEXT_LEN ? 0 : MAX_TEXT_LEN - nodeNameLen - 2;
}

}  // namespace mc
