#if defined(UNITLINK_RADIO) && defined(ARDUINO_ARCH_ESP32)
#include "HalLink.h"

#include <BoardConfig.h>
#include <Logging.h>
#include <WiFi.h>
#include <esp_now.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>

#include <atomic>
#include <cstring>

// Radio build of the unit link (env x4pro-radio): ESP-NOW between the two units. USB and the pads are left alone, so
// charging and flashing work while linked. Until a peer is locked the units talk by broadcast and accept any sender;
// once the session is linked the service locks the sender's address, after which frames go to that unit only (with
// the MAC-level acknowledgement and retries of a unicast) and everything from other addresses is dropped.
namespace {
constexpr const char* TAG = "LINK";
constexpr uint8_t RADIO_CHANNEL = 1;
constexpr size_t RX_RING = 2048;
constexpr uint32_t FLUSH_MAX_MS = 150;

uint8_t rxRing[RX_RING];
size_t rxHead = 0;
size_t rxTail = 0;
portMUX_TYPE rxLock = portMUX_INITIALIZER_UNLOCKED;

const uint8_t BROADCAST[ESP_NOW_ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
uint8_t peerMac[ESP_NOW_ETH_ALEN] = {};
bool peerIsLocked = false;
uint8_t senderMac[ESP_NOW_ETH_ALEN] = {};
bool haveSender = false;
std::atomic<int> sendsInFlight{0};
std::atomic<bool> radioBroken{false};

// Runs on the Wi-Fi task. A packet that does not fit whole is dropped (the session resends), never cut in two.
void onReceive(const esp_now_recv_info_t* info, const uint8_t* data, int len) {
  if (len <= 0 || info == nullptr || info->src_addr == nullptr) return;
  portENTER_CRITICAL(&rxLock);
  if (!peerIsLocked || memcmp(info->src_addr, peerMac, ESP_NOW_ETH_ALEN) == 0) {
    const size_t used = (rxHead + RX_RING - rxTail) % RX_RING;
    if (static_cast<size_t>(len) < RX_RING - used) {
      for (int i = 0; i < len; i++) {
        rxRing[rxHead] = data[i];
        rxHead = (rxHead + 1) % RX_RING;
      }
      if (!peerIsLocked) {
        memcpy(senderMac, info->src_addr, ESP_NOW_ETH_ALEN);
        haveSender = true;
      }
    }
  }
  portEXIT_CRITICAL(&rxLock);
}

void onSent(const esp_now_send_info_t*, esp_now_send_status_t) {
  if (sendsInFlight.load() > 0) sendsInFlight--;
}

bool addPeer(const uint8_t* mac) {
  if (esp_now_is_peer_exist(mac)) return true;
  esp_now_peer_info_t peer = {};
  memcpy(peer.peer_addr, mac, ESP_NOW_ETH_ALEN);
  peer.channel = 0;  // whatever channel the radio is on
  peer.ifidx = WIFI_IF_STA;
  peer.encrypt = false;
  return esp_now_add_peer(&peer) == ESP_OK;
}

// Brings the radio and ESP-NOW up if anything (a Wi-Fi screen turning Wi-Fi off, a stopped radio) has taken them down.
// Safe to call at any time: it changes nothing that is already in order.
bool ensureRadio() {
  const wifi_mode_t mode = WiFi.getMode();
  if (mode != WIFI_MODE_STA && mode != WIFI_MODE_APSTA) WiFi.mode(WIFI_STA);
  // Both units must share a channel. While joined to an access point the channel is the access point's.
  if (WiFi.status() != WL_CONNECTED) esp_wifi_set_channel(RADIO_CHANNEL, WIFI_SECOND_CHAN_NONE);
  if (!esp_now_is_peer_exist(BROADCAST)) {
    esp_now_deinit();
    if (esp_now_init() != ESP_OK) return false;
    esp_now_register_recv_cb(onReceive);
    esp_now_register_send_cb(onSent);
    sendsInFlight = 0;
    if (!addPeer(BROADCAST)) return false;
    if (peerIsLocked) addPeer(peerMac);
  }
  radioBroken = false;
  return true;
}
}  // namespace

bool HalLink::isSupported() { return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro; }

bool HalLink::begin(unitlink::Role role) {
  if (running || !isSupported()) return false;
  portENTER_CRITICAL(&rxLock);
  rxHead = rxTail = 0;
  haveSender = false;
  portEXIT_CRITICAL(&rxLock);
  if (!ensureRadio()) {
    LOG_ERR(TAG, "Radio link could not start");
    return false;
  }
  sentBytes = 0;
  receivedBytes = 0;
  running = true;
  LOG_INF(TAG, "Radio link started, role=%s, %s, free heap %u", role == unitlink::Role::Left ? "left" : "right",
          peerIsLocked ? "peer locked" : "peer not known yet", static_cast<unsigned>(ESP.getFreeHeap()));
  return true;
}

void HalLink::restart() {
  if (!running) return;
  if (!ensureRadio()) LOG_ERR(TAG, "Radio could not be brought back");
}

void HalLink::flush() {
  if (!running) return;
  const uint32_t start = millis();
  while (sendsInFlight.load() > 0 && millis() - start < FLUSH_MAX_MS) delay(2);
}

void HalLink::end() {
  if (!running) return;
  flush();
  esp_now_deinit();
  WiFi.mode(WIFI_OFF);
  running = false;
}

size_t HalLink::write(const uint8_t* data, size_t len) {
  if (!running || len == 0 || len > ESP_NOW_MAX_DATA_LEN) return 0;
  sendsInFlight++;
  const esp_err_t err = esp_now_send(peerIsLocked ? peerMac : BROADCAST, data, len);
  if (err != ESP_OK) {
    sendsInFlight--;
    if (err == ESP_ERR_ESPNOW_NOT_INIT || err == ESP_ERR_ESPNOW_IF) radioBroken = true;  // restart() fixes it
    return 0;
  }
  sentBytes += len;
  return len;
}

int HalLink::read() {
  if (!running) return -1;
  int c = -1;
  portENTER_CRITICAL(&rxLock);
  if (rxTail != rxHead) {
    c = rxRing[rxTail];
    rxTail = (rxTail + 1) % RX_RING;
  }
  portEXIT_CRITICAL(&rxLock);
  if (c >= 0) receivedBytes++;
  return c;
}

void HalLink::lockPeer(const uint8_t* mac) {
  portENTER_CRITICAL(&rxLock);
  memcpy(peerMac, mac, ESP_NOW_ETH_ALEN);
  peerIsLocked = true;
  portEXIT_CRITICAL(&rxLock);
  if (running) addPeer(peerMac);
}

void HalLink::unlockPeer() {
  portENTER_CRITICAL(&rxLock);
  peerIsLocked = false;
  haveSender = false;
  portEXIT_CRITICAL(&rxLock);
}

bool HalLink::peerLocked() const { return peerIsLocked; }

bool HalLink::lastSender(uint8_t* out) const {
  bool have;
  portENTER_CRITICAL(&rxLock);
  have = haveSender;
  if (have) memcpy(out, senderMac, ESP_NOW_ETH_ALEN);
  portEXIT_CRITICAL(&rxLock);
  return have;
}

bool HalLink::radioDown() const { return radioBroken.load(); }

#else
#include <BoardConfig.h>
#include <Logging.h>

#include "HalLink.h"

#if defined(ARDUINO_ARCH_ESP32)
#include <sdkconfig.h>
#endif

#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S3
#include <Arduino.h>
#include <soc/usb_serial_jtag_reg.h>
#endif

namespace {
constexpr const char* TAG = "LINK";
constexpr uint32_t LINK_BAUD = 115200;
// ESP32-S3 USB Serial/JTAG pads: D- = GPIO19, D+ = GPIO20.
constexpr int PIN_D_MINUS = 19;
constexpr int PIN_D_PLUS = 20;
}  // namespace

bool HalLink::isSupported() {
#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S3
  return BoardConfig::ACTIVE.board == BoardConfig::Board::XteinkX4Pro;
#else
  return false;
#endif
}

#if defined(ARDUINO_ARCH_ESP32) && CONFIG_IDF_TARGET_ESP32S3

bool HalLink::begin(unitlink::Role role) {
  if (running || !isSupported()) return false;
  // Left sends on D+ and listens on D-, right the other way round, so the straight cable joins one TX to the other RX.
  txPin = role == unitlink::Role::Left ? PIN_D_PLUS : PIN_D_MINUS;
  rxPin = role == unitlink::Role::Left ? PIN_D_MINUS : PIN_D_PLUS;
  logSerial.flush();
  CLEAR_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  openPort();
  sentBytes = 0;
  receivedBytes = 0;
  running = true;
  LOG_INF(TAG, "Link started, role=%s tx=%d rx=%d", role == unitlink::Role::Left ? "left" : "right", txPin, rxPin);
  return true;
}

void HalLink::openPort() {
  // The first Serial1.begin on a USB pin does not take the pin; claiming both as GPIO first does.
  pinMode(rxPin, INPUT_PULLUP);
  pinMode(txPin, OUTPUT);
  Serial1.begin(LINK_BAUD, SERIAL_8N1, rxPin, txPin);
}

void HalLink::restart() {
  if (!running) return;
  Serial1.end();
  openPort();
}

void HalLink::flush() {
  if (running) Serial1.flush();
}

void HalLink::end() {
  if (!running) return;
  Serial1.end();
  pinMode(rxPin, INPUT);
  pinMode(txPin, INPUT);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  running = false;
}

size_t HalLink::write(const uint8_t* data, size_t len) {
  if (!running) return 0;
  const size_t n = Serial1.write(data, len);
  sentBytes += n;
  return n;
}

int HalLink::read() {
  if (!running) return -1;
  const int c = Serial1.read();
  if (c >= 0) receivedBytes++;
  return c;
}

#else

bool HalLink::begin(unitlink::Role) { return false; }
void HalLink::end() {}
void HalLink::restart() {}
void HalLink::flush() {}
size_t HalLink::write(const uint8_t*, size_t) { return 0; }
int HalLink::read() { return -1; }

#endif

// Peer locking only exists on the radio; a wire has exactly one peer.
void HalLink::lockPeer(const uint8_t*) {}
void HalLink::unlockPeer() {}
bool HalLink::peerLocked() const { return false; }
bool HalLink::lastSender(uint8_t*) const { return false; }
bool HalLink::radioDown() const { return false; }

#endif  // UNITLINK_RADIO
