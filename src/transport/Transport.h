// Abstract link to a MeshCore Companion radio (BLE or TCP).
// One frame = one complete Companion protocol frame (max 176 bytes).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <functional>

enum class LinkError : uint8_t {
  None = 0,
  NotConfigured,   // no module chosen yet
  NotFound,        // not advertising: out of range or already connected to a phone
  ConnectFailed,
  AuthFailed,      // wrong PIN / pairing rejected
  NoService,       // not a MeshCore Companion (wrong firmware)
  Lost,            // link dropped
};

enum class LinkPhase : uint8_t { Idle, Scanning, Connecting, Securing, Discovering, Up };

class Transport {
 public:
  using FrameCb = std::function<void(const uint8_t*, size_t)>;
  using PhaseCb = std::function<void(LinkPhase)>;
  virtual ~Transport() {}
  // Blocking connect; returns true when frames can be exchanged.
  virtual bool connect() = 0;
  virtual void disconnect() = 0;
  virtual bool isConnected() = 0;
  virtual bool sendFrame(const uint8_t* d, size_t len) = 0;
  // cb may be invoked from another task: implementations must not block in it.
  virtual void onFrame(FrameCb cb) = 0;
  // Called regularly from the mesh task (TCP polls its socket here).
  virtual void poll() {}
  virtual void onPhase(PhaseCb cb) { phaseCb_ = cb; }
  virtual LinkError lastError() const { return err_; }
  virtual const char* name() const = 0;

 protected:
  void setPhase(LinkPhase p) { if (phaseCb_) phaseCb_(p); }
  PhaseCb phaseCb_;
  LinkError err_ = LinkError::None;
};
