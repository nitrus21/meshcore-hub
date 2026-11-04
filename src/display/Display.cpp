#include "Display.h"

#ifndef HUB_DISPLAY_TDISPLAY_S3

namespace Display {
void begin() {}
void onEvent(JsonDocument&) {}
}  // namespace Display

#else

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <WiFi.h>
#include <time.h>
#include <deque>
#include <mutex>
#include <string>
#include "../App.h"
#include "../config/Config.h"
#include "../setup/Cellular.h"
#include "../setup/Network.h"
#include "../store/History.h"
#include "../telegram/TelegramBridge.h"
#include "../util/Log.h"

namespace Display {
namespace {

// LilyGO T-Display-S3: ST7789 (240x320 memory, 170x320 visible) on the ESP32-S3 8-bit i80 bus.
class Lcd : public lgfx::LGFX_Device {
  lgfx::Panel_ST7789 panel_;
  lgfx::Bus_Parallel8 bus_;
  lgfx::Light_PWM light_;

 public:
  Lcd() {
    auto b = bus_.config();
    b.freq_write = 20000000;
    b.pin_wr = 8;
    b.pin_rd = 9;
    b.pin_rs = 7;
    b.pin_d0 = 39;
    b.pin_d1 = 40;
    b.pin_d2 = 41;
    b.pin_d3 = 42;
    b.pin_d4 = 45;
    b.pin_d5 = 46;
    b.pin_d6 = 47;
    b.pin_d7 = 48;
    bus_.config(b);
    panel_.setBus(&bus_);
    auto p = panel_.config();
    p.pin_cs = 6;
    p.pin_rst = 5;
    p.pin_busy = -1;
    p.memory_width = 240;
    p.memory_height = 320;
    p.panel_width = 170;
    p.panel_height = 320;
    p.offset_x = 35;
    p.offset_y = 0;
    p.readable = false;
    p.invert = true;
    p.rgb_order = false;
    p.dlen_16bit = false;
    p.bus_shared = false;
    panel_.config(p);
    auto l = light_.config();
    l.pin_bl = 38;
    l.invert = false;
    l.freq = 2000;
    l.pwm_channel = 7;
    light_.config(l);
    panel_.setLight(&light_);
    setPanel(&panel_);
  }
};

const int W = 320, H = 170;
const int POWER_PIN = 15;                   // LCD power on battery
const uint8_t BRIGHT = 180, DIM = 12;
const uint32_t DIM_AFTER_MS = 3UL * 60 * 1000;
const size_t MAX_MSGS = 6;

const uint16_t C_BG = lgfx::color565(11, 17, 32), C_BAR = lgfx::color565(22, 32, 56),
               C_TEXT = lgfx::color565(226, 232, 240), C_MUTED = lgfx::color565(148, 163, 184),
               C_ACCENT = lgfx::color565(96, 145, 255), C_OK = lgfx::color565(34, 197, 94),
               C_WARN = lgfx::color565(245, 158, 11), C_ERR = lgfx::color565(239, 68, 68);

enum Str {
  S_MODULE, S_WIFI, S_TELEGRAM, S_CONTACTS, S_LOOKING, S_RELEASED, S_NO_MODULE, S_CONNECTING, S_ONLINE, S_OFFLINE, S_DISABLED,
  S_NOT_CONNECTED, S_ACCESS_POINT, S_UNREAD, S_LAST_MSGS, S_NO_MSGS, S_WEB_APP, S_SCAN_APP, S_SETUP, S_SETUP_1, S_SETUP_2, S_SETUP_SCAN, S_FROM_HOME, S_COUNT
};
const char* const STR[S_COUNT][2] = {
    {"Module", "Module"},
    {"Wi-Fi", "Wi-Fi"},
    {"Telegram", "Telegram"},
    {"Contacts", "Contacts"},
    {"recherche de %s", "looking for %s"},
    {"libéré pour le téléphone", "released for the phone"},
    {"aucun module", "no module"},
    {"connexion...", "connecting..."},
    {"connecté", "connected"},
    {"hors ligne", "offline"},
    {"désactivé", "disabled"},
    {"non connecté", "not connected"},
    {"point d'accès", "access point"},
    {"%u · %u non lus", "%u · %u unread"},
    {"Derniers messages", "Latest messages"},
    {"Aucun message pour l'instant", "No message yet"},
    {"Appli web", "Web app"},
    {"Scanne pour l'appli", "Scan for the app"},
    {"Configuration", "Setup"},
    {"1. Connecte-toi au Wi-Fi", "1. Join the Wi-Fi"},
    {"2. Ouvre", "2. Open"},
    {"ou scanne ce code", "or scan this code"},
    {"depuis la box :", "from home Wi-Fi:"},
};

struct Msg {
  std::string where, who, text;
  uint32_t ts;
};

struct Snap {
  int lang = 0;
  bool setup = false, sta = false, ap = false, tgEnabled = false, tgOnline = false, meshReady = false;
  std::string apSsid, apPass, apIp, ip, host, node, linkName, cellOp, ssid;
  bool cell = false;  // Internet through the 4G modem
  mc::LinkState link = mc::LinkState::Disabled;
  unsigned contacts = 0, unread = 0;
  char clock[8] = "";
};

std::mutex mtx_;            // msgs_, dirty_, wake_ (written by App events from any task)
std::deque<Msg> msgs_;      // newest first
bool dirty_ = true, wake_ = false;
Lcd* lcd_ = nullptr;
LGFX_Sprite* spr_ = nullptr;

// The DejaVu fonts only have ASCII glyphs: keep the text readable rather than print boxes.
std::string ascii(const std::string& s, size_t maxChars = 160) {
  static const char* const LATIN1[64] = {
      "A", "A", "A", "A", "A", "A", "AE", "C", "E", "E", "E", "E", "I", "I", "I", "I",
      "D", "N", "O", "O", "O", "O", "O", "x", "O", "U", "U", "U", "U", "Y", "Th", "ss",
      "a", "a", "a", "a", "a", "a", "ae", "c", "e", "e", "e", "e", "i", "i", "i", "i",
      "d", "n", "o", "o", "o", "o", "o", "/", "o", "u", "u", "u", "u", "y", "th", "y"};
  std::string o;
  for (size_t i = 0; i < s.size() && o.size() < maxChars;) {
    uint8_t c = s[i];
    uint32_t cp;
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > s.size()) break;
    cp = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; k++) cp = (cp << 6) | (s[i + k] & 0x3F);
    i += n;
    if (cp < 0x20) o += ' ';
    else if (cp < 0x7F) o += (char)cp;
    else if (cp >= 0xC0 && cp <= 0xFF) o += LATIN1[cp - 0xC0];
    else if (cp == 0x152) o += "OE";
    else if (cp == 0x153) o += "oe";
    else if (cp == 0x2018 || cp == 0x2019) o += '\'';
    else if (cp == 0x201C || cp == 0x201D || cp == 0xAB || cp == 0xBB) o += '"';
    else if (cp == 0x2013 || cp == 0x2014) o += '-';
    else if (cp == 0x2026) o += "...";
    else if (cp == 0xA0) o += ' ';
    else if (cp == 0xB7 || cp == 0x2022) o += '-';
    else if (cp == 0xB0) o += "deg";
    // anything else (emoji, other scripts) is left out
  }
  return o;
}

std::string fit(LGFX_Sprite& s, std::string t, int w) {
  if (s.textWidth(t.c_str()) <= w) return t;
  while (!t.empty() && s.textWidth((t + "...").c_str()) > w) t.pop_back();
  return t + "...";
}

const char* tr(const Snap& s, Str id) { return STR[id][s.lang]; }

Snap snapshot() {
  Snap s;
  {
    std::lock_guard<std::recursive_mutex> l(gConfigMtx);
    s.lang = gConfig.lang == "en" ? 1 : 0;
    s.setup = !gConfig.setupDone;
    s.tgEnabled = gConfig.tgEnabled && !gConfig.tgToken.empty() && !gConfig.apOnly;
    s.linkName = gConfig.link == "tcp" ? gConfig.tcpHost : gConfig.bleName;
    s.host = gConfig.hostname;
    s.apPass = gConfig.apPass;
  }
  s.sta = Net::staConnected();
  s.ap = Net::apActive();
  if (s.sta) s.ssid = WiFi.SSID().c_str();
  s.cell = Cellular::online();
  if (s.cell) {
    JsonDocument c;
    Cellular::statusJson(c.to<JsonObject>());
    s.cellOp = std::string(c["operator"] | "");
  }
  s.apSsid = Net::apSsid();
  s.apIp = WiFi.softAPIP().toString().c_str();
  // the web app is reachable on the home network, or on the Hub's own access point
  s.ip = s.sta ? WiFi.localIP().toString().c_str() : s.apIp;
  auto& mesh = gApp.mesh();
  s.link = mesh.state();
  s.meshReady = mesh.ready();
  if (s.meshReady) s.node = mesh.selfInfo().name;
  s.contacts = mesh.contactCount();
  s.unread = gHistory.totalUnread();
  s.tgOnline = gTelegram.online();
  time_t now = time(nullptr);
  if (now > 1700000000) {
    struct tm t;
    localtime_r(&now, &t);
    strftime(s.clock, sizeof(s.clock), "%H:%M", &t);
  }
  return s;
}

void header(LGFX_Sprite& g, const Snap& s, const std::string& title) {
  g.fillRect(0, 0, W, 26, C_BAR);
  g.setFont(&lgfx::fonts::DejaVu18);
  g.setTextColor(C_TEXT);
  g.drawString(fit(g, ascii(title), 250).c_str(), 8, 4);
  g.setTextColor(C_MUTED);
  g.drawRightString(s.clock, W - 8, 4);
}

void pageDots(LGFX_Sprite& g, int page) {
  for (int i = 0; i < 3; i++) g.fillCircle(W - 32 + i * 10, H - 8, 3, i == page ? C_ACCENT : C_BAR);
}

void row(LGFX_Sprite& g, int y, uint16_t dot, const char* label, const std::string& value) {
  g.fillCircle(12, y + 9, 5, dot);
  g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_MUTED);
  g.drawString(fit(g, ascii(label), 76).c_str(), 24, y + 4);
  g.setFont(&lgfx::fonts::DejaVu18);
  g.setTextColor(C_TEXT);
  g.drawString(fit(g, ascii(value), W - 104 - 8).c_str(), 104, y);
}

void drawHome(LGFX_Sprite& g, const Snap& s, const std::deque<Msg>& msgs) {
  header(g, s, "MeshCore Hub");
  char buf[64];
  // module
  uint16_t c = C_WARN;
  std::string v;
  switch (s.link) {
    case mc::LinkState::Ready: c = C_OK; v = s.node; break;
    case mc::LinkState::Disabled: c = C_MUTED; v = tr(s, S_NO_MODULE); break;
    case mc::LinkState::Released: c = C_ACCENT; v = tr(s, S_RELEASED); break;
    case mc::LinkState::Connecting:
    case mc::LinkState::Handshake: v = tr(s, S_CONNECTING); break;
    default: snprintf(buf, sizeof(buf), tr(s, S_LOOKING), s.linkName.c_str()); v = buf;
  }
  row(g, 34, c, tr(s, S_MODULE), v);
  if (!s.sta && s.cell) row(g, 62, C_OK, "4G", s.cellOp.empty() ? "4G" : s.cellOp);  // no Wi-Fi, Internet by the modem
  else row(g, 62, s.sta ? C_OK : s.ap ? C_ACCENT : C_ERR, s.sta ? s.ssid.c_str() : tr(s, S_WIFI),  // which network: there may be several

           s.sta ? s.ip : s.ap ? std::string(tr(s, S_ACCESS_POINT)) + " " + s.apIp : std::string(tr(s, S_NOT_CONNECTED)));
  row(g, 90, !s.tgEnabled ? C_MUTED : s.tgOnline ? C_OK : C_ERR, tr(s, S_TELEGRAM),
      !s.tgEnabled ? tr(s, S_DISABLED) : s.tgOnline ? tr(s, S_ONLINE) : tr(s, S_OFFLINE));
  snprintf(buf, sizeof(buf), tr(s, S_UNREAD), s.contacts, s.unread);
  row(g, 118, s.unread ? C_ACCENT : C_MUTED, tr(s, S_CONTACTS), buf);
  if (!msgs.empty()) {
    g.setFont(&lgfx::fonts::DejaVu12);
    g.setTextColor(C_MUTED);
    const Msg& m = msgs.front();
    std::string line = m.where + " - " + (m.who.empty() ? "" : m.who + ": ") + m.text;
    g.drawString(fit(g, ascii(line), W - 50).c_str(), 8, H - 16);
  }
  pageDots(g, 0);
}

void drawMessages(LGFX_Sprite& g, const Snap& s, const std::deque<Msg>& msgs) {
  header(g, s, tr(s, S_LAST_MSGS));
  if (msgs.empty()) {
    g.setFont(&lgfx::fonts::DejaVu12);
    g.setTextColor(C_MUTED);
    g.drawCenterString(ascii(tr(s, S_NO_MSGS)).c_str(), W / 2, 80);
  }
  int y = 30;
  for (size_t i = 0; i < msgs.size() && i < 4; i++, y += 34) {
    const Msg& m = msgs[i];
    char when[8] = "";
    time_t t = m.ts;
    if (t > 1700000000) {
      struct tm tm;
      localtime_r(&t, &tm);
      strftime(when, sizeof(when), "%H:%M", &tm);
    }
    g.setFont(&lgfx::fonts::DejaVu12);
    g.setTextColor(C_ACCENT);
    g.drawString(fit(g, ascii(m.where) + (when[0] ? std::string(" - ") + when : ""), W - 16).c_str(), 8, y);
    g.setFont(&lgfx::fonts::DejaVu18);
    g.setTextColor(C_TEXT);
    int w = i == 3 ? W - 50 : W - 16;  // the last line shares its row with the page dots
    g.drawString(fit(g, ascii((m.who.empty() ? "" : m.who + ": ") + m.text), w).c_str(), 8, y + 13);
  }
  pageDots(g, 1);
}

void drawApp(LGFX_Sprite& g, const Snap& s) {
  header(g, s, tr(s, S_WEB_APP));
  if (!s.sta && !s.ap) {
    g.setFont(&lgfx::fonts::DejaVu18);
    g.setTextColor(C_ERR);
    g.drawCenterString(ascii(std::string(tr(s, S_WIFI)) + " : " + tr(s, S_NOT_CONNECTED)).c_str(), W / 2, 80);
    pageDots(g, 2);
    return;
  }
  std::string url = "http://" + s.ip + "/";
  g.fillRect(6, 30, 136, 136, TFT_WHITE);  // quiet zone: dark themes confuse some scanners
  g.qrcode(url.c_str(), 10, 34, 128);
  g.setFont(&lgfx::fonts::DejaVu18);
  g.setTextColor(C_TEXT);
  g.drawString("http://", 154, 44);
  g.drawString(fit(g, s.ip, W - 158).c_str(), 154, 66);
  g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_MUTED);
  if (s.sta) g.drawString(fit(g, s.host + ".local", W - 158).c_str(), 154, 92);
  else g.drawString(fit(g, ascii(std::string(tr(s, S_ACCESS_POINT)) + " " + s.apSsid), W - 158).c_str(), 154, 92);
  g.drawString(fit(g, ascii(tr(s, S_SCAN_APP)), W - 158).c_str(), 154, 120);
  pageDots(g, 2);
}

std::string wifiQrEscape(const std::string& v) {
  std::string o;
  for (char c : v) {
    if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') o += '\\';
    o += c;
  }
  return o;
}

void drawSetup(LGFX_Sprite& g, const Snap& s) {
  header(g, s, tr(s, S_SETUP));
  const int textW = 180;
  g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_MUTED);
  g.drawString(ascii(tr(s, S_SETUP_1)).c_str(), 8, 34);
  g.setFont(&lgfx::fonts::DejaVu18);
  if (g.textWidth(s.apSsid.c_str()) > textW) g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_ACCENT);
  g.drawString(fit(g, s.apSsid, textW).c_str(), 8, 50);
  g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_MUTED);
  g.drawString(ascii(tr(s, S_SETUP_2)).c_str(), 8, 84);
  g.setFont(&lgfx::fonts::DejaVu18);
  g.setTextColor(C_TEXT);
  g.drawString(fit(g, "http://" + s.apIp, textW).c_str(), 8, 100);  // always the access point's address
  g.setFont(&lgfx::fonts::DejaVu12);
  g.setTextColor(C_MUTED);
  if (s.sta) {  // the wizard can also go on from the home network (the test moves the AP channel)
    g.drawString(ascii(tr(s, S_FROM_HOME)).c_str(), 8, 126);
    g.setTextColor(C_TEXT);
    g.drawString(fit(g, "http://" + s.ip, textW).c_str(), 8, 142);
  } else {
    g.drawString(ascii(tr(s, S_SETUP_SCAN)).c_str(), 8, 140);
  }
  g.fillRect(186, 32, 130, 130, TFT_WHITE);
  // same rule as Net::startAp(): the access point is protected when apPass has 8+ characters
  std::string qr = s.apPass.size() >= 8
                       ? "WIFI:T:WPA;S:" + wifiQrEscape(s.apSsid) + ";P:" + wifiQrEscape(s.apPass) + ";;"
                       : "WIFI:T:nopass;S:" + wifiQrEscape(s.apSsid) + ";;";
  g.qrcode(qr.c_str(), 189, 35, 124);
}

void task(void*) {
  pinMode(POWER_PIN, OUTPUT);
  digitalWrite(POWER_PIN, HIGH);
  lcd_->init();
  lcd_->setRotation(1);  // landscape, 320x170
  lcd_->setBrightness(BRIGHT);
  spr_->setColorDepth(16);
  spr_->setPsram(true);
  if (!spr_->createSprite(W, H)) {
    LOGE("display: no memory for the frame buffer");
    vTaskDelete(nullptr);
    return;
  }
#ifdef DISPLAY_BUTTON_PIN
  pinMode(DISPLAY_BUTTON_PIN, INPUT_PULLUP);
  bool btnWas = false;
  uint32_t btnAt = 0;
#endif
  int page = 0;
  bool dimmed = false;
  uint32_t lastActivity = millis(), lastDraw = 0;
  for (;;) {
    uint32_t now = millis();
    bool redraw = now - lastDraw >= 1000;
    {
      std::lock_guard<std::mutex> l(mtx_);
      if (wake_) {
        wake_ = false;
        lastActivity = now;
      }
      if (dirty_) {
        dirty_ = false;
        redraw = true;
      }
    }
#ifdef DISPLAY_BUTTON_PIN
    bool btn = digitalRead(DISPLAY_BUTTON_PIN) == LOW;
    if (btn && !btnWas && now - btnAt > 150) {
      btnAt = now;
      if (!dimmed) page = (page + 1) % 3;  // a press on a dimmed screen only wakes it up
      lastActivity = now;
      redraw = true;
    }
    btnWas = btn;
#endif
    bool wantDim = now - lastActivity > DIM_AFTER_MS;
    if (wantDim != dimmed) {
      dimmed = wantDim;
      lcd_->setBrightness(dimmed ? DIM : BRIGHT);
    }
    if (redraw) {
      lastDraw = now;
      Snap s = snapshot();  // no display lock held: these take the mesh / history / config locks
      std::deque<Msg> msgs;
      {
        std::lock_guard<std::mutex> l(mtx_);
        msgs = msgs_;
      }
      LGFX_Sprite& g = *spr_;
      g.fillScreen(C_BG);
      if (s.setup) drawSetup(g, s);
      else if (page == 1) drawMessages(g, s, msgs);
      else if (page == 2) drawApp(g, s);
      else drawHome(g, s, msgs);
      spr_->pushSprite(0, 0);
    }
    vTaskDelay(pdMS_TO_TICKS(30));
  }
}

}  // namespace

void begin() {
  lcd_ = new Lcd();
  spr_ = new LGFX_Sprite(lcd_);
  xTaskCreatePinnedToCore(task, "display", 8192, nullptr, 1, nullptr, ARDUINO_RUNNING_CORE);
}

void onEvent(JsonDocument& d) {
  if (!lcd_ || d["t"] != "msg" || (d["muted"] | false)) return;  // muted conversations stay quiet
  JsonObject m = d["m"];
  if ((m["d"] | 0) == 1 || (m["y"] | 0) == 1) return;  // our own messages, CLI replies
  Msg x;
  x.where = std::string(d["name"] | "");
  x.who = std::string(m["a"] | "");
  x.text = std::string(m["x"] | "");
  x.ts = m["t"] | 0u;
  if (x.text.size() > 200) x.text.resize(200);
  std::lock_guard<std::mutex> l(mtx_);
  msgs_.push_front(std::move(x));
  while (msgs_.size() > MAX_MSGS) msgs_.pop_back();
  dirty_ = wake_ = true;
}

}  // namespace Display

#endif
