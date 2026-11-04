#include "TcpTransport.h"
#include "../util/Log.h"

bool TcpTransport::connect() {
  err_ = LinkError::None;
  if (host_.empty()) {
    err_ = LinkError::NotConfigured;
    return false;
  }
  if (WiFi.status() != WL_CONNECTED && WiFi.getMode() != WIFI_AP && WiFi.getMode() != WIFI_AP_STA) {
    err_ = LinkError::ConnectFailed;
    return false;
  }
  setPhase(LinkPhase::Connecting);
  framer_.reset();
  if (!client_.connect(host_.c_str(), port_, 4000)) {
    err_ = LinkError::NotFound;
    setPhase(LinkPhase::Idle);
    return false;
  }
  client_.setNoDelay(true);
  LOGI("TCP connected to %s:%u", host_.c_str(), (unsigned)port_);
  setPhase(LinkPhase::Up);
  return true;
}

void TcpTransport::disconnect() { client_.stop(); }

bool TcpTransport::isConnected() { return client_.connected(); }

bool TcpTransport::sendFrame(const uint8_t* d, size_t len) {
  if (!client_.connected()) return false;
  uint8_t hdr[3];
  TcpFramer::header(hdr, len);
  uint8_t buf[3 + mc::MAX_FRAME_SIZE];
  if (len > mc::MAX_FRAME_SIZE) return false;
  memcpy(buf, hdr, 3);
  memcpy(buf + 3, d, len);
  return client_.write(buf, len + 3) == len + 3;  // single write: one TCP segment per frame
}

void TcpTransport::poll() {
  int budget = 1024;  // bounded so the mesh loop stays responsive
  while (budget-- > 0 && client_.available()) {
    int c = client_.read();
    if (c < 0) break;
    size_t n = framer_.push((uint8_t)c);
    if (n && frameCb_) frameCb_(framer_.buf(), n);
  }
}
