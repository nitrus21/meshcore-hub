// MeshCore Companion protocol constants.
// Source of truth: meshcore-dev/MeshCore, examples/companion_radio/MyMesh.cpp
// (FIRMWARE_VER_CODE 14). Values are copied verbatim; do not renumber.
#pragma once
#include <stdint.h>

namespace mc {

constexpr uint8_t PROTOCOL_VERSION = 3;      // app_target_ver sent in CMD_DEVICE_QUERY
constexpr size_t MAX_FRAME_SIZE = 176;       // BaseSerialInterface.h
constexpr size_t PUB_KEY_SIZE = 32;
constexpr size_t PREFIX_SIZE = 6;            // contacts are referenced by 6-byte prefix
constexpr size_t MAX_PATH_SIZE = 64;
constexpr uint8_t OUT_PATH_UNKNOWN = 0xFF;   // also "direct" marker in received msgs

// ---- Commands (app -> radio)
enum Cmd : uint8_t {
  CMD_APP_START = 1,
  CMD_SEND_TXT_MSG = 2,
  CMD_SEND_CHANNEL_TXT_MSG = 3,
  CMD_GET_CONTACTS = 4,
  CMD_GET_DEVICE_TIME = 5,
  CMD_SET_DEVICE_TIME = 6,
  CMD_SEND_SELF_ADVERT = 7,
  CMD_SET_ADVERT_NAME = 8,
  CMD_ADD_UPDATE_CONTACT = 9,
  CMD_SYNC_NEXT_MESSAGE = 10,
  CMD_SET_RADIO_PARAMS = 11,
  CMD_SET_RADIO_TX_POWER = 12,
  CMD_RESET_PATH = 13,
  CMD_SET_ADVERT_LATLON = 14,
  CMD_REMOVE_CONTACT = 15,
  CMD_SHARE_CONTACT = 16,
  CMD_EXPORT_CONTACT = 17,
  CMD_IMPORT_CONTACT = 18,
  CMD_REBOOT = 19,
  CMD_GET_BATT_AND_STORAGE = 20,
  CMD_SET_TUNING_PARAMS = 21,
  CMD_DEVICE_QUERY = 22,
  CMD_EXPORT_PRIVATE_KEY = 23,
  CMD_IMPORT_PRIVATE_KEY = 24,
  CMD_SEND_RAW_DATA = 25,
  CMD_SEND_LOGIN = 26,
  CMD_SEND_STATUS_REQ = 27,
  CMD_HAS_CONNECTION = 28,
  CMD_LOGOUT = 29,
  CMD_GET_CONTACT_BY_KEY = 30,
  CMD_GET_CHANNEL = 31,
  CMD_SET_CHANNEL = 32,
  CMD_SEND_TRACE_PATH = 36,
  CMD_SET_DEVICE_PIN = 37,
  CMD_SET_OTHER_PARAMS = 38,
  CMD_SEND_TELEMETRY_REQ = 39,
  CMD_GET_ADVERT_PATH = 42,
  CMD_GET_TUNING_PARAMS = 43,
  CMD_SEND_BINARY_REQ = 50,
  CMD_FACTORY_RESET = 51,
  CMD_SEND_PATH_DISCOVERY_REQ = 52,
  CMD_GET_STATS = 56,
  CMD_SET_AUTOADD_CONFIG = 58,
  CMD_GET_AUTOADD_CONFIG = 59,
  CMD_SET_PATH_HASH_MODE = 61,
};

// ---- Responses (radio -> app, reply to the current command)
enum Resp : uint8_t {
  RESP_OK = 0,
  RESP_ERR = 1,
  RESP_CONTACTS_START = 2,
  RESP_CONTACT = 3,
  RESP_END_OF_CONTACTS = 4,
  RESP_SELF_INFO = 5,
  RESP_SENT = 6,
  RESP_CONTACT_MSG_RECV = 7,      // legacy (< v3)
  RESP_CHANNEL_MSG_RECV = 8,      // legacy (< v3)
  RESP_CURR_TIME = 9,
  RESP_NO_MORE_MESSAGES = 10,
  RESP_EXPORT_CONTACT = 11,
  RESP_BATT_AND_STORAGE = 12,
  RESP_DEVICE_INFO = 13,
  RESP_PRIVATE_KEY = 14,
  RESP_DISABLED = 15,
  RESP_CONTACT_MSG_RECV_V3 = 16,
  RESP_CHANNEL_MSG_RECV_V3 = 17,
  RESP_CHANNEL_INFO = 18,
  RESP_TUNING_PARAMS = 23,
  RESP_STATS = 24,
  RESP_AUTOADD_CONFIG = 25,
  RESP_CHANNEL_DATA_RECV = 27,
  RESP_CLI_REPLY = 29,
};

// ---- Pushes (radio -> app, unsolicited)
enum Push : uint8_t {
  PUSH_ADVERT = 0x80,
  PUSH_PATH_UPDATED = 0x81,
  PUSH_SEND_CONFIRMED = 0x82,
  PUSH_MSG_WAITING = 0x83,
  PUSH_RAW_DATA = 0x84,
  PUSH_LOGIN_SUCCESS = 0x85,
  PUSH_LOGIN_FAIL = 0x86,
  PUSH_STATUS_RESPONSE = 0x87,
  PUSH_LOG_RX_DATA = 0x88,
  PUSH_TRACE_DATA = 0x89,
  PUSH_NEW_ADVERT = 0x8A,
  PUSH_TELEMETRY_RESPONSE = 0x8B,
  PUSH_BINARY_RESPONSE = 0x8C,
  PUSH_PATH_DISCOVERY_RESPONSE = 0x8D,
  PUSH_CONTROL_DATA = 0x8E,
  PUSH_CONTACT_DELETED = 0x8F,
  PUSH_CONTACTS_FULL = 0x90,
};

enum Err : uint8_t {
  ERR_UNSUPPORTED_CMD = 1,
  ERR_NOT_FOUND = 2,
  ERR_TABLE_FULL = 3,
  ERR_BAD_STATE = 4,
  ERR_FILE_IO = 5,
  ERR_ILLEGAL_ARG = 6,
};

enum TxtType : uint8_t {
  TXT_PLAIN = 0,
  TXT_CLI_DATA = 1,
  TXT_SIGNED_PLAIN = 2,   // room messages: 4-byte author prefix precedes the text
  TXT_CLI_COMMAND = 3,
};

enum AdvType : uint8_t {
  ADV_NONE = 0,
  ADV_CHAT = 1,
  ADV_REPEATER = 2,
  ADV_ROOM = 3,
  ADV_SENSOR = 4,
};

enum StatsType : uint8_t { STATS_CORE = 0, STATS_RADIO = 1, STATS_PACKETS = 2 };

// Auto-add bitmask (CMD_SET_AUTOADD_CONFIG)
constexpr uint8_t AUTO_ADD_OVERWRITE_OLDEST = 1 << 0;
constexpr uint8_t AUTO_ADD_CHAT = 1 << 1;
constexpr uint8_t AUTO_ADD_REPEATER = 1 << 2;
constexpr uint8_t AUTO_ADD_ROOM = 1 << 3;
constexpr uint8_t AUTO_ADD_SENSOR = 1 << 4;

inline bool isPush(uint8_t code) { return code >= 0x80; }

// path_len encoding (Packet.cpp): low 6 bits = hop count, high 2 bits = hash size - 1.
inline int pathHops(uint8_t pathLen) { return pathLen == OUT_PATH_UNKNOWN ? -1 : (pathLen & 63); }
inline int pathHashSize(uint8_t pathLen) { return (pathLen >> 6) + 1; }
inline int pathByteLen(uint8_t pathLen) {
  if (pathLen == OUT_PATH_UNKNOWN) return 0;
  int n = pathHops(pathLen) * pathHashSize(pathLen);
  return n > (int)MAX_PATH_SIZE ? (int)MAX_PATH_SIZE : n;
}

}  // namespace mc
