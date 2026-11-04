// BLE central link to a MeshCore Companion (Nordic UART Service, passkey + bonding).
#pragma once
#include <Arduino.h>
#include <NimBLEDevice.h>
#include <atomic>
#include <functional>
#include <string>
#include <vector>
#include "Transport.h"

class BleTransport : public Transport, public NimBLEClientCallbacks {
 public:
  struct Found {
    std::string addr;
    uint8_t addrType = 0;
    std::string name;
    int rssi = 0;
    bool meshcore = false;  // advertises the NUS service or a MeshCore- name
  };

  bool begin(const char* deviceName);
  void configure(const std::string& addr, uint8_t addrType, uint32_t pin);
  const std::string& address() const { return addr_; }

  bool connect() override;
  void disconnect() override;
  bool isConnected() override;
  bool sendFrame(const uint8_t* d, size_t len) override;
  void onFrame(FrameCb cb) override { frameCb_ = cb; }
  const char* name() const override { return "ble"; }

  // Blocking scan (call from the mesh task only).
  std::vector<Found> scan(uint32_t ms);
  void forget(const std::string& addr, uint8_t addrType);
  void forgetAll();
  void resetStack();   // deinit + init NimBLE (recovery when the stack is stuck)
  int rssi();
  int lastHeard() const { return lastHeard_; }  // devices heard by the last scan, -1 = scan failed
  uint16_t mtu();

  // NimBLEClientCallbacks
  void onConnect(NimBLEClient* c) override;
  void onDisconnect(NimBLEClient* c, int reason) override;
  void onPassKeyEntry(NimBLEConnInfo& info) override;
  void onAuthenticationComplete(NimBLEConnInfo& info) override;
  void onConfirmPasskey(NimBLEConnInfo& info, uint32_t pin) override;

 private:
  void abortLink();   // leaves the client fully disconnected, whatever state NimBLE is in
  NimBLEClient* client_ = nullptr;
  NimBLERemoteCharacteristic* rx_ = nullptr;  // we write here
  NimBLERemoteCharacteristic* tx_ = nullptr;  // module notifies here
  std::string addr_;
  uint8_t addrType_ = 0;
  uint32_t pin_ = 123456;
  std::string devName_;
  int lastHeard_ = -1;
  std::atomic<bool> up_{false};
  std::atomic<bool> authFailed_{false};
  std::atomic<bool> encrypted_{false};
  std::atomic<bool> passkeyAsked_{false};
  std::atomic<int> lastDiscReason_{0};
  FrameCb frameCb_;
};
