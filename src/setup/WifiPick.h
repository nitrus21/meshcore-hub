// Which saved Wi-Fi network to join: pure logic, tested on the PC (test/test_protocol).
// Networks are given in priority order, the default one first.
#pragma once
#include <stddef.h>
#include <string>
#include <vector>

namespace Net {

struct WifiCandidate {
  int rssi = 0;         // best signal in the last scan, 0 = not seen
  bool failed = false;  // joining it failed a moment ago
};

// The password to keep for a network sent back by the web app: the settings page only knows a
// mask for the saved ones, and a network moved in the list keeps the password saved under its name.
template <class WifiEntry>
std::string wifiPassFor(const std::vector<WifiEntry>& saved, const std::string& ssid, const std::string& pass) {
  if (pass.find("********") == std::string::npos) return pass;
  for (auto& n : saved)
    if (n.ssid == ssid) return n.pass;
  return "";
}

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

// Connected to `current` without Internet: the first other network in sight, whatever its signal
// (a weak link beats none). -1 to stay: leaving would only cut the local access too.
inline int otherWifi(const std::vector<WifiCandidate>& c, size_t current) {
  for (size_t i = 0; i < c.size(); i++)
    if (i != current && c[i].rssi && !c[i].failed) return (int)i;
  return -1;
}

}  // namespace Net
