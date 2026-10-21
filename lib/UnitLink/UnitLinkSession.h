#pragma once
#include <cstdint>
#include <string>

#include "UnitLinkProtocol.h"

// Pairing and page sync for two units. Hardware-free: the byte transport and the clock are passed in, so the logic runs
// the same on the device and in host tests.
//
// Both units send Hello until they hear a compatible one. The left unit owns the position: publish() sends it and keeps
// resending until the right unit acks; the right unit picks new positions up with takeRemote().

namespace unitlink {

class Port {
 public:
  virtual ~Port() = default;
  virtual size_t write(const uint8_t* data, size_t len) = 0;
  virtual int read() = 0;  // next received byte, -1 when none is waiting
};

enum class Status : uint8_t {
  Idle,          // not started
  Pairing,       // sending Hello, nothing compatible heard yet
  Linked,        // a compatible peer answers
  Incompatible,  // a peer answers but cannot pair (see incompatibleReasons())
  Lost,          // was linked, then went silent; keeps trying
};

struct Config {
  Role role = Role::Left;
  uint32_t bookId = 0;
  uint32_t layoutHash = 0;
};

class Session {
 public:
  static constexpr uint32_t HELLO_PAIRING_MS = 500;
  static constexpr uint32_t HELLO_LINKED_MS = 1000;
  static constexpr uint32_t LOST_AFTER_MS = 3500;
  static constexpr uint32_t RESEND_MS = 400;

  explicit Session(Port& portRef) : port(portRef) {}

  void start(const Config& config, uint32_t nowMs);
  void stop();
  void update(uint32_t nowMs);  // call often (every loop pass)

  Status status() const { return currentStatus; }
  uint8_t incompatibleReasons() const { return reasons; }
  bool isLinked() const { return currentStatus == Status::Linked; }
  Role role() const { return cfg.role; }

  // The book or the text layout changed (the reader opened or closed a book, or its settings moved). Pairing starts
  // again with the new values, so the peer re-checks them.
  void setIdentity(uint32_t bookId, uint32_t layoutHash, uint32_t nowMs);

  // Left unit: announce the position both units should show. Resent until acked or replaced by a newer one.
  void publish(uint16_t spine, uint16_t page, uint32_t nowMs);
  bool publishedIsAcked() const { return !haveSent || acked; }

  // Right unit: true once for each new position from the left unit.
  bool takeRemote(PageState& out);

  // Left unit: the path of the open book ("" when none). While the right unit reports a different book the path is sent
  // again with every heartbeat, so it can open the same file; a path that does not fit in a frame is not sent.
  void setBookPath(const std::string& path);
  // Right unit: true once per Book frame received (they repeat until the right unit has the book open).
  bool takeBookRequest(std::string& path);

  // Either unit: ask the peer to sleep too. takeSleepRequest() is true once on the receiving side.
  void sendSleep();
  bool takeSleepRequest();

 private:
  void handleFrame(const Frame& f, uint32_t nowMs);
  void sendHello(uint32_t nowMs);
  void sendBytes(const uint8_t* data, size_t len);
  void sendPageState(uint32_t nowMs);
  bool canSendState() const;

  Port& port;
  Parser parser;
  Config cfg;
  Status currentStatus = Status::Idle;
  uint8_t reasons = 0;
  uint32_t lastHelloTx = 0;
  uint32_t lastRx = 0;
  uint32_t lastStateTx = 0;

  // Left side: the position being published.
  PageState sent;
  bool haveSent = false;
  bool acked = false;

  // Right side: the newest position received.
  PageState remote;
  bool haveRemote = false;
  bool remoteDelivered = true;
  bool sleepRequested = false;

  std::string bookPath;       // left side: the open book
  std::string requestedPath;  // right side: the newest path received
  bool bookRequested = false;
};

}  // namespace unitlink
