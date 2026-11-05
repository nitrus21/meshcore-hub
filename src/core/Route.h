// Private routing: a contact writes "@marie text" to the Hub over the mesh and the Hub sends it to
// the Telegram chat called "marie" in its directory, and to nobody else. Plain functions, so that
// the host tests can cover them.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace route {

constexpr size_t MAX_NAME = 20;
constexpr size_t MAX_ENTRIES = 16;   // directory (people and groups)
constexpr size_t MAX_SENDERS = 32;   // contacts allowed to use it
constexpr size_t MAX_TARGETS = 5;    // recipients of one message

// Lower case, accents removed: "Élodie" and "elodie" are the same directory entry.
inline std::string fold(const std::string& s) {
  // UTF-8 0xC3 0x80..0xBF, in order: À Á Â Ã Ä Å Æ Ç È É ... ü ý þ ÿ ('?' : no plain equivalent)
  static const char* const latin1 = "aaaaaaaceeeeiiiidnooooo?ouuuuyts" "aaaaaaaceeeeiiiidnooooo?ouuuuyty";
  std::string o;
  o.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    unsigned char c = (unsigned char)s[i];
    if (c == 0xC3 && i + 1 < s.size() && (unsigned char)s[i + 1] >= 0x80 && (unsigned char)s[i + 1] <= 0xBF) {
      o += latin1[(unsigned char)s[++i] - 0x80];
    } else if (c >= 'A' && c <= 'Z') {
      o += (char)(c + 'a' - 'A');
    } else {
      o += (char)c;
    }
  }
  return o;
}

// A folded directory name: a-z 0-9 - _
inline bool validName(const std::string& n) {
  if (n.empty() || n.size() > MAX_NAME) return false;
  for (char c : n)
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
  return true;
}

// The names that fit in `budget` bytes, cut between two names: "@marie @paul +3"
inline std::string fit(const std::vector<std::string>& names, size_t budget) {
  std::string o;
  for (size_t i = 0; i < names.size(); i++) {
    std::string next = (o.empty() ? "@" : " @") + names[i];
    std::string more = " +" + std::to_string(names.size() - i - 1);
    bool last = i + 1 == names.size();
    if (o.size() + next.size() + (last ? 0 : more.size()) > budget) return o + " +" + std::to_string(names.size() - i);
    o += next;
  }
  return o;
}

// An invitation code, as opposed to any other /start parameter.
inline bool looksLikeInvite(const std::string& s) {
  if (s.size() != 16) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

struct Parsed {
  bool routed = false;   // starts with '@': the message is for the directory
  bool list = false;     // "@" or "@?": asks who can be reached
  std::string name;      // first recipient, folded
  std::vector<std::string> names;  // all of them, folded, without duplicates
  std::string body;
};

// "@marie text", "@marie: text" or "@[Marie] text" (the mention format of the MeshCore apps).
// Several recipients: "@marie @paul text", "@marie, @paul: text".
inline Parsed parse(const std::string& text) {
  Parsed p;
  auto space = [](char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; };
  size_t i = 0, n = text.size();
  while (i < n && space(text[i])) i++;
  if (i >= n || text[i] != '@') return p;
  p.routed = true;
  // A '@' followed by nothing is part of the text, except at the very start ("@" asks for the list).
  while (i < n && text[i] == '@' && (p.names.empty() || (i + 1 < n && !space(text[i + 1])))) {
    i++;
    std::string name;
    size_t a = i;
    if (i < n && text[i] == '[') {
      a = ++i;
      while (i < n && text[i] != ']') i++;
      name = fold(text.substr(a, i - a));
      if (i < n) i++;
    } else {
      while (i < n && !space(text[i]) && text[i] != ':' && text[i] != ',') i++;
      name = fold(text.substr(a, i - a));
    }
    bool dup = false;
    for (auto& x : p.names) dup |= x == name;
    if (!dup) p.names.push_back(name);
    while (i < n && space(text[i])) i++;
    bool colon = i < n && text[i] == ':';  // ends the recipients: "@marie: @paul viendra" is for Marie
    if (colon || (i < n && text[i] == ',')) i++;
    while (i < n && space(text[i])) i++;
    if (colon) break;
  }
  size_t e = n;
  while (e > i && space(text[e - 1])) e--;
  p.body = i < e ? text.substr(i, e - i) : "";
  p.name = p.names[0];
  p.list = p.name.empty() || p.name == "?";
  return p;
}

}  // namespace route
