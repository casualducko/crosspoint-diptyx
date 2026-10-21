#pragma once
#include <UnitLinkSession.h>

// UART link to a second unit over the pogo data pads (X4 Pro only; every other board reports unsupported).
//
// The pads are the USB D+/D- lines, so starting the link takes the unit off USB: the Mac loses it and the USB console
// is gone until the unit restarts. Callers end a link by restarting, never by trying to hand the pads back live. On a
// straight cable one unit must send on D+ and the other on D-, so the role also picks the pins.
class HalLink : public unitlink::Port {
 public:
  static bool isSupported();

  // Switches the USB pad off and starts the UART. Returns false if unsupported or already active.
  bool begin(unitlink::Role role);
  // Stops the UART and gives the pins and the USB pad back; USB itself only returns after a restart.
  void end();
  bool active() const { return running; }

  size_t write(const uint8_t* data, size_t len) override;
  int read() override;

 private:
  bool running = false;
  int rxPin = -1;
  int txPin = -1;
};
