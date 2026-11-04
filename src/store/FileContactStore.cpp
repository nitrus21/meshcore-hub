#include "FileContactStore.h"
#include <LittleFS.h>
#include "../util/Log.h"

using mc::Contact;

static const char* PATH = "/contacts.bin";

bool FileContactStore::begin() {
  clear();
  return (bool)f_;
}

void FileContactStore::clear() {
  if (f_) f_.close();
  f_ = LittleFS.open(PATH, "w+");  // truncate; one handle for reads and writes
  if (!f_) LOGE("contacts: cannot open %s", PATH);
  else f_.setBufferSize(0);  // unbuffered: a full disk is reported on the record that fails
  keys_.clear();
  keys_.shrink_to_fit();
}

bool FileContactStore::writeAt(size_t i, const Contact& c) {
  if (!f_ || !f_.seek(i * REC)) return false;
  return f_.write((const uint8_t*)&c, REC) == REC;
}

void FileContactStore::add(const Contact& c) {
  if (!writeAt(keys_.size(), c)) {
    LOGE("contacts: write failed (storage full?)");
    return;
  }
  Entry e;
  memcpy(e.key, c.pubKey, KEY);
  e.lastAdvert = c.lastAdvert;
  keys_.push_back(e);
}

bool FileContactStore::get(size_t i, Contact& out) {
  if (i >= keys_.size() || !f_ || !f_.seek(i * REC)) return false;
  if (f_.read((uint8_t*)&out, REC) != REC) return false;
  if (keys_[i].lastAdvert > out.lastAdvert) out.lastAdvert = keys_[i].lastAdvert;
  return true;
}

void FileContactStore::touchAdvert(size_t i, uint32_t t) {
  if (i < keys_.size()) keys_[i].lastAdvert = t;
}

bool FileContactStore::put(size_t i, const Contact& c) {
  if (i >= keys_.size() || !writeAt(i, c)) return false;
  memcpy(keys_[i].key, c.pubKey, KEY);
  keys_[i].lastAdvert = c.lastAdvert;
  return true;
}

void FileContactStore::removeAt(size_t i) {
  if (i >= keys_.size()) return;
  size_t last = keys_.size() - 1;
  if (i != last) {
    Contact c;
    // move the last record into the hole (the stale tail is ignored); if that fails the
    // deleted contact would survive, so drop the last one instead of corrupting the index
    if (!get(last, c) || !put(i, c)) LOGE("contacts: remove failed, list may be stale until next sync");
  }
  keys_.pop_back();
}

int FileContactStore::find(const uint8_t* key, size_t n) {
  size_t m = n < KEY ? n : KEY;
  for (size_t i = 0; i < keys_.size(); i++) {
    if (memcmp(keys_[i].key, key, m) != 0) continue;
    if (n <= KEY) return (int)i;
    Contact c;  // longer key: confirm on the full record
    if (get(i, c) && memcmp(c.pubKey, key, n) == 0) return (int)i;
  }
  return -1;
}
