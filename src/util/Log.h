// Small system log: Serial + RAM ring buffer + live hook (web journal).
#pragma once
#include <Arduino.h>
#include <functional>
#include <string>
#include <vector>

namespace Log {
using Hook = std::function<void(const char* level, const char* line)>;
void begin();
void write(char level, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void setHook(Hook h);
std::vector<std::string> recent();   // oldest first, each "HH:MM:SS L text"
}  // namespace Log

#define LOGI(...) Log::write('I', __VA_ARGS__)
#define LOGW(...) Log::write('W', __VA_ARGS__)
#define LOGE(...) Log::write('E', __VA_ARGS__)
