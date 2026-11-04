#include "Log.h"
#include <mutex>
#include <stdarg.h>
#include <string.h>
#include <time.h>

namespace Log {

static const size_t RING = 40, LINE = 230;
// Fixed storage, so the log never takes more RAM over time (growing std::strings in a ring
// looked like a slow leak in the heap reports).
static char ring_[RING][LINE];
static size_t count_ = 0, head_ = 0;
static std::mutex mtx_;
static Hook hook_;

void begin() {}

void setHook(Hook h) {
  std::lock_guard<std::mutex> l(mtx_);
  hook_ = h;
}

void write(char level, const char* fmt, ...) {
  char msg[200];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);

  char ts[12];
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm tmv;
    localtime_r(&now, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
  } else {
    uint32_t s = millis() / 1000;
    snprintf(ts, sizeof(ts), "+%lu", (unsigned long)s);
  }
  char line[LINE];
  snprintf(line, sizeof(line), "%s %c %s", ts, level, msg);
  Serial.println(line);

  Hook h;
  {
    std::lock_guard<std::mutex> l(mtx_);
    strlcpy(ring_[(head_ + count_) % RING], line, LINE);
    if (count_ < RING) count_++;
    else head_ = (head_ + 1) % RING;
    h = hook_;
  }
  if (h) {
    char lv[2] = {level, 0};
    h(lv, line);
  }
}

std::vector<std::string> recent() {
  std::lock_guard<std::mutex> l(mtx_);
  std::vector<std::string> out;
  out.reserve(count_);
  for (size_t i = 0; i < count_; i++) out.push_back(ring_[(head_ + i) % RING]);
  return out;
}

}  // namespace Log
