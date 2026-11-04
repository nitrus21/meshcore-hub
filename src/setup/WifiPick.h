// Which saved Wi-Fi network to join: pure logic, tested on the PC (test/test_protocol).
// Networks are given in priority order, the default one first.
#pragma once
#include <stddef.h>
#include <vector>

namespace Net {

struct WifiCandidate {
  int rssi = 0;         // best signal in the last scan, 0 = not seen
  bool failed = false;  // joining it failed a moment ago
};

// Not connected: the first network in sight that did not just fail, else the first in sight.
// Nothing in sight (hidden networks, missed scan): try them blindly in turn, `turn` being a
// counter the caller increments. -1 if there is no network.
inline int pickWifi(const std::vector<WifiCandidate>& c, size_t turn) {
  for (size_t i = 0; i < c.size(); i++)
    if (c[i].rssi && !c[i].failed) return (int)i;
  for (size_t i = 0; i < c.size(); i++)
    if (c[i].rssi) return (int)i;
  return c.empty() ? -1 : (int)(turn % c.size());
}

// Connected to network `current` (c.size() if it is not a saved one): a network of higher
// priority worth moving to, -1 to stay. A weak signal is not worth leaving a working link.
inline int betterWifi(const std::vector<WifiCandidate>& c, size_t current, int minRssi) {
  for (size_t i = 0; i < current && i < c.size(); i++)
    if (c[i].rssi && !c[i].failed && c[i].rssi >= minRssi) return (int)i;
  return -1;
}

}  // namespace Net
