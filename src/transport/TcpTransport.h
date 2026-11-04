// TCP link to a MeshCore Companion flashed with the Wi-Fi firmware (default port 5000).
#pragma once
#include <Arduino.h>
#include <WiFi.h>
#include <string>
#include "TcpFramer.h"
#include "Transport.h"

class TcpTransport : public Transport {
 public:
  void configure(const std::string& host, uint16_t port) {
    host_ = host;
    port_ = port ? port : 5000;
  }
  const std::string& host() const { return host_; }
  uint16_t port() const { return port_; }

  bool connect() override;
  void disconnect() override;
  bool isConnected() override;
  bool sendFrame(const uint8_t* d, size_t len) override;
  void onFrame(FrameCb cb) override { frameCb_ = cb; }
  void poll() override;
  const char* name() const override { return "tcp"; }

 private:
  WiFiClient client_;
  TcpFramer framer_;
  std::string host_;
  uint16_t port_ = 5000;
  FrameCb frameCb_;
};
