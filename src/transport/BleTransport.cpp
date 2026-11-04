#include "BleTransport.h"
#include <algorithm>
#include "../util/Log.h"
#include <esp_task_wdt.h>

static const char* NUS_SERVICE = "6E400001-B5A3-F393-E0A9-E50E24DCCA9E";
static const char* NUS_RX = "6E400002-B5A3-F393-E0A9-E50E24DCCA9E";  // write (hub -> module)
static const char* NUS_TX = "6E400003-B5A3-F393-E0A9-E50E24DCCA9E";  // notify (module -> hub)

bool BleTransport::begin(const char* deviceName) {
  devName_ = deviceName;
  if (!NimBLEDevice::init(deviceName)) {
    LOGE("BLE: NimBLE init failed (%u kB free)", (unsigned)(ESP.getFreeHeap() / 1024));
    return false;
  }
  NimBLEDevice::setPower(9);
  NimBLEDevice::setMTU(247);
  // Companion characteristics require an encrypted, MITM-protected link (passkey).
  NimBLEDevice::setSecurityAuth(true /*bonding*/, true /*MITM*/, true /*secure connections*/);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_KEYBOARD_ONLY);
  return true;
}

void BleTransport::configure(const std::string& addr, uint8_t addrType, uint32_t pin) {
  addr_ = addr;
  addrType_ = addrType;
  pin_ = pin ? pin : 123456;
}

bool BleTransport::connect() {
  err_ = LinkError::None;
  if (addr_.empty()) {
    err_ = LinkError::NotConfigured;
    return false;
  }
  NimBLEAddress peer(addr_, addrType_);
  if (!client_) {
    client_ = NimBLEDevice::createClient();
    client_->setClientCallbacks(this, false);
    client_->setConnectTimeout(6000);
    client_->setConnectRetries(0);  // no hidden retries behind our back: our own backoff handles it
    // Moderate interval: leaves air time to Wi-Fi on the shared radio.
    client_->setConnectionParams(24, 48, 0, 400);
  }
  if (client_->isConnected()) abortLink();  // orphan link from an interrupted attempt
  rx_ = tx_ = nullptr;
  authFailed_ = false;
  encrypted_ = false;
  passkeyAsked_ = false;
  lastDiscReason_ = 0;

  // Every step is asynchronous with its own deadline (like meshcore-p4): NimBLE's blocking
  // secureConnection() waits forever if the encryption event is missed.
  auto waitFor = [this](uint32_t ms, std::function<bool()> done) {
    uint32_t t0 = millis();
    while (millis() - t0 < ms) {
      if (done()) return true;
      if (!client_->isConnected()) return false;
      esp_task_wdt_reset();  // long but bounded waits (pairing) in the watched mesh task
      delay(20);
    }
    return done();
  };

  setPhase(LinkPhase::Connecting);
  uint32_t t0 = millis();
  LOGI("BLE: connecting to %s (type %u)", addr_.c_str(), (unsigned)addrType_);
  if (!client_->connect(peer, true, true /*async*/, true)) {
    LOGW("BLE: connect request refused (rc=%d)", client_->getLastError());
    abortLink();
    err_ = LinkError::ConnectFailed;
    setPhase(LinkPhase::Idle);
    return false;
  }
  uint32_t until = millis() + 9000;
  while (!client_->isConnected() && (int32_t)(until - millis()) > 0) {
    esp_task_wdt_reset();
    delay(20);
  }
  if (!client_->isConnected()) {
    // Not advertising: out of range, powered off, or already connected to a phone.
    abortLink();
    LOGW("BLE: no connection after %u ms", (unsigned)(millis() - t0));
    err_ = LinkError::NotFound;
    setPhase(LinkPhase::Idle);
    return false;
  }
  LOGI("BLE: link up in %u ms", (unsigned)(millis() - t0));

  setPhase(LinkPhase::Securing);
  bool bonded = NimBLEDevice::isBonded(peer);
  t0 = millis();
  if (!client_->getConnInfo().isEncrypted()) client_->secureConnection(true /*async*/);
  // PIN entry is answered by onPassKeyEntry; a bonded module re-encrypts without PIN.
  bool ok = waitFor(30000, [this] { return encrypted_ || authFailed_ || client_->getConnInfo().isEncrypted(); });
  ok = ok && !authFailed_ && client_->getConnInfo().isEncrypted();
  if (!ok) {
    int reason = lastDiscReason_;
    // HCI 0x05 authentication failure, 0x06 PIN or key missing, 0x3D MIC failure (stale keys)
    bool authProblem = authFailed_ || reason == BLE_HS_HCI_ERR(0x05) || reason == BLE_HS_HCI_ERR(0x06) ||
                       reason == BLE_HS_HCI_ERR(0x3D) || (client_->isConnected() && passkeyAsked_);
    LOGW("BLE: encryption failed after %u ms (bonded=%d, pin asked=%d, auth=%d, disconnect reason 0x%x)",
         (unsigned)(millis() - t0), bonded, (int)passkeyAsked_, (int)authProblem, reason);
    abortLink();
    // A stale bond (module re-flashed, or bond removed on the module) fails here: drop ours so
    // the next attempt pairs again with the PIN. A plain radio drop keeps the (valid) bond.
    if (bonded && authProblem) NimBLEDevice::deleteBond(peer);
    err_ = authProblem ? LinkError::AuthFailed : LinkError::ConnectFailed;
    setPhase(LinkPhase::Idle);
    return false;
  }
  LOGI("BLE: encrypted in %u ms (bonded=%d, pin asked=%d)", (unsigned)(millis() - t0), bonded, (int)passkeyAsked_);

  setPhase(LinkPhase::Discovering);
  NimBLERemoteService* svc = client_->getService(NUS_SERVICE);
  if (svc) {
    rx_ = svc->getCharacteristic(NUS_RX);
    tx_ = svc->getCharacteristic(NUS_TX);
  }
  if (!rx_ || !tx_ || !tx_->canNotify()) {
    LOGW("BLE: Nordic UART service not found (is it a Companion firmware?)");
    abortLink();
    err_ = LinkError::NoService;
    setPhase(LinkPhase::Idle);
    return false;
  }
  bool subscribed = tx_->subscribe(true, [this](NimBLERemoteCharacteristic*, uint8_t* data, size_t len, bool) {
    if (frameCb_ && len) frameCb_(data, len);  // one notification = one frame
  });
  if (!subscribed) {
    abortLink();
    err_ = authFailed_ ? LinkError::AuthFailed : LinkError::ConnectFailed;
    setPhase(LinkPhase::Idle);
    return false;
  }
  LOGI("BLE connected to %s, MTU %u", addr_.c_str(), (unsigned)client_->getMTU());
  up_ = true;
  setPhase(LinkPhase::Up);
  return true;
}

void BleTransport::disconnect() {
  up_ = false;
  if (client_ && client_->isConnected()) client_->disconnect();
}

void BleTransport::abortLink() {
  up_ = false;
  if (!client_) return;
  client_->cancelConnect();  // harmless when no attempt is pending
  if (client_->isConnected()) client_->disconnect();
  for (int i = 0; i < 50 && client_->isConnected(); i++) delay(20);  // let DISCONNECT complete
}

bool BleTransport::isConnected() { return up_ && client_ && client_->isConnected(); }

bool BleTransport::sendFrame(const uint8_t* d, size_t len) {
  if (!isConnected() || !rx_) return false;
  return rx_->writeValue(d, len, true);
}

std::vector<BleTransport::Found> BleTransport::scan(uint32_t ms) {
  std::vector<Found> out;
  lastHeard_ = -1;
  if (!NimBLEDevice::isInitialized()) {
    LOGE("BLE scan: stack not initialised");
    return out;
  }
  NimBLEScan* s = NimBLEDevice::getScan();
  s->setActiveScan(true);   // the "MeshCore-..." name is only in the scan response
  // Wi-Fi and BLE share the radio (software coexistence): listen 90 % of the time.
  s->setInterval(100);
  s->setWindow(90);
  uint32_t t0 = millis();
  NimBLEScanResults res = s->getResults(ms, false);
  uint32_t took = millis() - t0;
  if (took + 500 < ms) LOGW("BLE scan stopped early (%u ms): start failed or scan busy", (unsigned)took);
  NimBLEUUID nus(NUS_SERVICE);
  lastHeard_ = res.getCount();
  LOGI("BLE scan: %d device(s) heard in %u ms", lastHeard_, (unsigned)took);
  for (int i = 0; i < res.getCount(); i++) {
    const NimBLEAdvertisedDevice* dev = res.getDevice(i);
    Found f;
    f.name = dev->getName();
    f.addr = dev->getAddress().toString();
    f.addrType = dev->getAddress().getType();
    f.rssi = dev->getRSSI();
    bool hasNus = dev->isAdvertisingService(nus);
    bool named = f.name.rfind("MeshCore", 0) == 0;
    f.meshcore = hasNus || named;
    if (i < 20) {
      // raw advertising data (+ scan response) to identify radios that don't match
      const std::vector<uint8_t>& pl = dev->getPayload();
      char hex[2 * 62 + 1];
      size_t n = pl.size() < 62 ? pl.size() : 62;
      for (size_t k = 0; k < n; k++) snprintf(hex + 2 * k, 3, "%02x", pl[k]);
      hex[2 * n] = 0;
      LOGI("  %s %s t%u %d dBm%s%s uuids=%u \"%s\" %s", f.meshcore ? "*" : " ", f.addr.c_str(), (unsigned)f.addrType,
           f.rssi, hasNus ? " NUS" : "", dev->isConnectable() ? " conn" : "", (unsigned)dev->getServiceUUIDCount(),
           f.name.c_str(), hex);
    }
    // Unnamed non-MeshCore devices (beacons, phones...) are useless in the picker.
    if (f.meshcore || !f.name.empty()) out.push_back(f);
  }
  s->clearResults();
  std::sort(out.begin(), out.end(), [](const Found& a, const Found& b) {
    return a.meshcore != b.meshcore ? a.meshcore : a.rssi > b.rssi;
  });
  if (out.size() > 25) out.resize(25);
  return out;
}

void BleTransport::forget(const std::string& addr, uint8_t addrType) {
  if (addr.empty()) return;
  NimBLEDevice::deleteBond(NimBLEAddress(addr, addrType));
}

void BleTransport::forgetAll() { NimBLEDevice::deleteAllBonds(); }

void BleTransport::resetStack() {
  LOGW("BLE: resetting the Bluetooth stack");
  disconnect();
  NimBLEDevice::deinit(true);  // also deletes our client
  client_ = nullptr;
  rx_ = tx_ = nullptr;
  delay(200);
  begin(devName_.c_str());
}

int BleTransport::rssi() { return isConnected() ? client_->getRssi() : 0; }
uint16_t BleTransport::mtu() { return isConnected() ? client_->getMTU() : 0; }

// ---------------------------------------------------------------- callbacks (NimBLE host task)

void BleTransport::onConnect(NimBLEClient*) {}

void BleTransport::onDisconnect(NimBLEClient*, int reason) {
  lastDiscReason_ = reason;
  if (up_) LOGW("BLE disconnected (reason 0x%x)", reason);
  up_ = false;
}

void BleTransport::onPassKeyEntry(NimBLEConnInfo& info) {
  passkeyAsked_ = true;
  bool ok = NimBLEDevice::injectPassKey(info, pin_);
  LOGI("BLE: module asked for the PIN, sent (%s)", ok ? "ok" : "inject failed");
}

void BleTransport::onConfirmPasskey(NimBLEConnInfo& info, uint32_t) {
  NimBLEDevice::injectConfirmPasskey(info, true);
}

void BleTransport::onAuthenticationComplete(NimBLEConnInfo& info) {
  if (!info.isEncrypted()) {
    authFailed_ = true;
    LOGW("BLE: authentication failed (wrong PIN?)");
  } else {
    encrypted_ = true;
  }
}
