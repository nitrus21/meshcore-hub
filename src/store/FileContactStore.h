// Contact list on LittleFS (/contacts.bin, fixed-size records) with an 8-byte key index in RAM:
// ~8 bytes of RAM per contact instead of ~92. Rebuilt from the module at every connection.
// Not thread safe on its own: MeshClient calls it under its lock.
#pragma once
#include <FS.h>
#include <vector>
#include "../core/ContactStore.h"

class FileContactStore : public mc::ContactStore {
 public:
  bool begin();
  void clear() override;
  void add(const mc::Contact& c) override;
  size_t size() override { return keys_.size(); }
  bool get(size_t i, mc::Contact& out) override;
  bool put(size_t i, const mc::Contact& c) override;
  void removeAt(size_t i) override;
  int find(const uint8_t* key, size_t n) override;
  void touchAdvert(size_t i, uint32_t t) override;

 private:
  static constexpr size_t KEY = 8;
  static constexpr size_t REC = sizeof(mc::Contact);
  struct Entry {
    uint8_t key[KEY];
    uint32_t lastAdvert;  // RAM overlay: adverts are frequent, rewriting flash for each is not
  };
  bool writeAt(size_t i, const mc::Contact& c);
  fs::File f_;
  std::vector<Entry> keys_;
};
