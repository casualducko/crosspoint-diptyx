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
  // Sets the UART up again on the same pins. A cable joined while the lines were noisy has been seen to leave the
  // receiver deaf until the units were joined a second time; doing this while nobody answers heals that.
  void restart();
  // Waits until everything written has left the UART (before the unit goes to sleep).
  void flush();

  // Bytes sent and received since begin(), for the diagnostics line of the Link screen.
  uint32_t bytesSent() const { return sentBytes; }
  uint32_t bytesReceived() const { return receivedBytes; }

  // Radio build only (no-ops on the wire). Locks the link to one peer address: frames then go to it alone and nothing
  // from other addresses is accepted. lastSender() is the address of the newest packet heard while unlocked.
  void lockPeer(const uint8_t* mac);
  void unlockPeer();
  bool peerLocked() const;
  bool lastSender(uint8_t* out) const;
  // The radio was taken down under the link (a send failed because ESP-NOW is gone); restart() brings it back.
  bool radioDown() const;

  size_t write(const uint8_t* data, size_t len) override;
  int read() override;

 private:
  void openPort();

  uint32_t sentBytes = 0;
  uint32_t receivedBytes = 0;
  bool running = false;
  int rxPin = -1;
  int txPin = -1;
};
