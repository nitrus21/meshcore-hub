// Stream framing used by MeshCore serial / Wi-Fi companions.
//   app -> radio : '<' len_lo len_hi payload
//   radio -> app : '>' len_lo len_hi payload
// Receive state machine mirrors ArduinoSerialInterface::checkRecvFrame.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "../protocol/Codes.h"

class TcpFramer {
 public:
  // Writes the 3-byte header for an outgoing frame.
  static void header(uint8_t hdr[3], size_t len) {
    hdr[0] = '<';
    hdr[1] = len & 0xFF;
    hdr[2] = (len >> 8) & 0xFF;
  }

  // Feeds one byte; returns frame length when a frame is complete (data in buf()).
  size_t push(uint8_t c) {
    switch (state_) {
      case IDLE:
        if (c == '>') state_ = HDR;
        break;
      case HDR:
        len_ = c;
        state_ = LEN1;
        break;
      case LEN1:
        len_ |= (uint16_t)c << 8;
        rx_ = 0;
        state_ = len_ > 0 ? BODY : IDLE;
        break;
      case BODY:
        if (rx_ < mc::MAX_FRAME_SIZE) buf_[rx_] = c;  // excess bytes are dropped
        rx_++;
        if (rx_ >= len_) {
          state_ = IDLE;
          return len_ > mc::MAX_FRAME_SIZE ? mc::MAX_FRAME_SIZE : len_;
        }
        break;
    }
    return 0;
  }

  const uint8_t* buf() const { return buf_; }
  void reset() { state_ = IDLE; rx_ = 0; len_ = 0; }

 private:
  enum State { IDLE, HDR, LEN1, BODY } state_ = IDLE;
  uint16_t len_ = 0, rx_ = 0;
  uint8_t buf_[mc::MAX_FRAME_SIZE];
};
