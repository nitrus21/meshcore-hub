// Where MeshClient keeps the module's contact list.
// A module can hold 350+ contacts (~92 bytes each): on a board without PSRAM that is too much RAM
// next to Wi-Fi, BLE and a TLS session, so the firmware stores them on flash (FileContactStore)
// with a small RAM index. The host tests use the in-memory version below.
#pragma once
#include <deque>
#include <string.h>
#include "../protocol/Frames.h"

namespace mc {

class ContactStore {
 public:
  virtual ~ContactStore() {}
  virtual void clear() = 0;
  virtual void add(const Contact& c) = 0;
  virtual size_t size() = 0;
  virtual bool get(size_t i, Contact& out) = 0;
  virtual bool put(size_t i, const Contact& c) = 0;
  virtual void removeAt(size_t i) = 0;            // order is not preserved
  virtual int find(const uint8_t* key, size_t n) = 0;  // index of the first key prefix match, -1 if none
  // "Heard again": frequent, so stores may keep it in RAM instead of rewriting the record.
  virtual void touchAdvert(size_t i, uint32_t t) {
    Contact c;
    if (get(i, c)) {
      c.lastAdvert = t;
      put(i, c);
    }
  }
};

class MemoryContactStore : public ContactStore {
 public:
  void clear() override { v_.clear(); }
  void add(const Contact& c) override { v_.push_back(c); }
  size_t size() override { return v_.size(); }
  bool get(size_t i, Contact& out) override {
    if (i >= v_.size()) return false;
    out = v_[i];
    return true;
  }
  bool put(size_t i, const Contact& c) override {
    if (i >= v_.size()) return false;
    v_[i] = c;
    return true;
  }
  void removeAt(size_t i) override {
    if (i >= v_.size()) return;
    v_[i] = v_.back();
    v_.pop_back();
  }
  int find(const uint8_t* key, size_t n) override {
    for (size_t i = 0; i < v_.size(); i++)
      if (memcmp(v_[i].pubKey, key, n) == 0) return (int)i;
    return -1;
  }

 private:
  std::deque<Contact> v_;
};

}  // namespace mc
