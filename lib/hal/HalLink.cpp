#include "HalLink.h"

#include <BoardConfig.h>
#include <Logging.h>

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

void HalLink::end() {
  if (!running) return;
  Serial1.end();
  pinMode(rxPin, INPUT);
  pinMode(txPin, INPUT);
  SET_PERI_REG_MASK(USB_SERIAL_JTAG_CONF0_REG, USB_SERIAL_JTAG_USB_PAD_ENABLE);
  running = false;
}

size_t HalLink::write(const uint8_t* data, size_t len) { return running ? Serial1.write(data, len) : 0; }

int HalLink::read() { return running ? Serial1.read() : -1; }

#else

bool HalLink::begin(unitlink::Role) { return false; }
void HalLink::end() {}
void HalLink::restart() {}
size_t HalLink::write(const uint8_t*, size_t) { return 0; }
int HalLink::read() { return -1; }

#endif
