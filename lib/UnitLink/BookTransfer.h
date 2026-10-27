#pragma once
#include <cstdint>

#include "UnitLinkSession.h"

// Moving a book from one unit to the other over the session. Hardware-free: the file is reached through Sink and
// Source, so the same code runs on the device and in host tests.
//
// The receiver asks for a window of chunks from the first byte it lacks and asks again when the window is in or when
// nothing has arrived for STALL_MS; the sender keeps no state between requests, so a lost frame costs one repeated
// request and nothing else.

namespace unitlink {

class Sink {
 public:
  virtual ~Sink() = default;
  virtual bool write(const uint8_t* data, size_t n) = 0;  // false: the card is full or gone
};

class Source {
 public:
  virtual ~Source() = default;
  virtual uint32_t size() = 0;
  virtual bool read(uint32_t offset, uint8_t* out, size_t n) = 0;
};

class BookReceiver {
 public:
  enum class State : uint8_t { Idle, Receiving, Done, Failed };
  static constexpr uint32_t MAX_BYTES = 64u * 1024 * 1024;
  static constexpr uint32_t STALL_MS = 500;
  static constexpr uint32_t GAP_REASK_MS =
      100;  // a chunk past the missing one arrived: ask again at once, not more often
  static constexpr uint8_t MAX_STALLS = 20;

  void begin(Session& session, uint32_t nowMs);
  // Call every loop pass while Receiving; writes arrived chunks to `sink`. Done and Failed are final until begin().
  State update(Session& session, Sink& sink, uint32_t nowMs);
  State state() const { return current; }
  uint32_t received() const { return next; }
  uint32_t total() const { return fileTotal; }

 private:
  State current = State::Idle;
  uint32_t fileTotal = 0;
  uint32_t next = 0;  // first byte not written yet
  uint32_t reqStart = 0;
  uint8_t reqCount = 0;
  uint32_t lastActivity = 0;
  uint32_t lastAsk = 0;
  uint8_t stalls = 0;
};

class BookServer {
 public:
  static constexpr uint8_t PER_UPDATE = FILE_WINDOW;  // the radio's send queue pushes back when it is full

  // `bookId`: the id of the book that is open here (0: none, every request is ignored).
  // True when this call took a request or sent a chunk.
  bool update(Session& session, Source& source, uint32_t bookId);
  void cancel() { active = false; }
  bool busy() const { return active; }

 private:
  bool active = false;
  uint32_t total = 0;
  uint32_t offset = 0;
  uint8_t left = 0;
};

}  // namespace unitlink
