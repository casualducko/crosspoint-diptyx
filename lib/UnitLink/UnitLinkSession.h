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
  DisplayState display;  // this unit's night mode and frontlight
};

class Session {
 public:
  static constexpr uint32_t HELLO_PAIRING_MS = 500;
  static constexpr uint32_t HELLO_LINKED_MS = 1000;
  static constexpr uint32_t LOST_AFTER_MS = 3500;
  static constexpr uint32_t RESEND_MS = 400;
  static constexpr size_t KEY_QUEUE = 4;
  static constexpr uint32_t OFFER_BLOCK_MS = 3000;
  static constexpr uint32_t CLOSE_REPEAT_MS = 150;
  static constexpr uint8_t CLOSE_REPEATS = 4;
  static constexpr uint32_t LAYOUT_GIVE_UP_MS = 10000;
  static constexpr uint32_t MARKS_CHUNK_MS = 25;     // one 200-byte frame takes 17 ms on the wire
  static constexpr uint32_t MARKS_RESEND_MS = 2000;  // the whole file goes out again if nobody confirmed it

  explicit Session(Port& portRef) : port(portRef) {}

  void start(const Config& config, uint32_t nowMs);
  void stop();
  void update(uint32_t nowMs);  // call often (every loop pass)

  Status status() const { return currentStatus; }
  uint8_t incompatibleReasons() const { return reasons; }
  bool isLinked() const { return currentStatus == Status::Linked; }
  Role role() const { return cfg.role; }
  // When a valid frame from the peer last arrived (the start time before any did).
  uint32_t lastHeardMs() const { return lastRx; }
  // For diagnostics: frames that passed the CRC and frames that failed it.
  uint32_t framesReceived() const { return framesOk; }
  uint32_t frameErrors() const { return parser.crcErrors(); }

  // The book or the text layout changed (the reader opened or closed a book, or its settings moved). Pairing starts
  // again with the new values, so the peer re-checks them.
  void setIdentity(uint32_t bookId, uint32_t layoutHash, uint32_t nowMs);

  // Left unit: announce the position both units should show. Resent until acked or replaced by a newer one.
  void publish(uint16_t spine, uint16_t page, uint32_t nowMs);
  bool publishedIsAcked() const { return !haveSent || acked; }

  // Right unit: true once for each new position from the left unit.
  bool takeRemote(PageState& out);

  // The path of the open book ("" when none). While the peer reports a different book the path is sent again with
  // every heartbeat, so it can open the same file; a path that does not fit in a frame is not sent. The left unit
  // always sends; the right unit sends only to a left unit that has no book open, so two different books settle on the
  // left's.
  void setBookPath(const std::string& path);
  // True once per Book frame received (they repeat until this unit has the book open).
  bool takeBookRequest(std::string& path);

  // Left unit: send the bookmark file's content to the right unit, chunk by chunk, until it confirms (at most
  // MARKS_MAX_TOTAL bytes, never empty; a newer file replaces one still on its way).
  bool publishMarks(const std::string& data, uint32_t nowMs);
  bool marksDelivered() const { return !marksPending; }
  // Right unit: true once for each complete, CRC-checked file from the left unit.
  bool takeMarks(std::string& out);
  // Counts the peers' Hello frames that say it has just started or restarted; a change means "send it everything
  // again".
  uint32_t peerFreshCount() const { return peerFresh; }

  // Right unit: ask the left unit to turn the page. One press is in flight at a time and is resent until the left unit
  // confirms it; presses wait their turn (at most KEY_QUEUE of them). False when the link is not up or the queue is
  // full.
  bool sendKey(KeyAction action, uint32_t nowMs);
  // Left unit: the next pending press from the right unit, oldest first.
  bool takeKey(KeyAction& action);

  // Right unit: ask the left unit to jump to a place (a bookmark or chapter chosen here). One request is in flight; a
  // newer one replaces it. Resent until the left unit confirms; dropped if the link goes down first.
  bool sendGoto(const JumpRequest& jump, uint32_t nowMs);
  // Left unit: true once for each request from the right unit.
  bool takeGoto(JumpRequest& jump);

  // Either unit: this unit left the book `bookId` (Home). The peer, if it has the same book open, leaves too. Sent a
  // few times, because nothing confirms it; the peer ignores it unless the book matches, so a stale copy does no harm.
  void sendClose(uint32_t bookId, uint32_t nowMs);
  // True once when the peer left the book this unit has open.
  bool takeClose();

  // Either unit: hand the text settings to the peer, which adopts them. Resent until confirmed (for at most
  // LAYOUT_GIVE_UP_MS). If both units send at once the left unit's wins: it ignores the right unit's while its own is
  // unconfirmed, and the right unit drops its own when the left unit's arrives.
  bool publishLayout(const uint8_t* blob, uint32_t nowMs);
  bool layoutUnconfirmed() const { return layoutUnacked; }
  // True once for each new settings blob from the peer (LAYOUT_BLOB_LEN bytes are written to out).
  bool takeLayout(uint8_t* out);

  // Night mode and frontlight ride on the Hello heartbeat. setDisplay() is called when the local state changes;
  // takeDisplay() is true once for each change of the peer's (and, on the right unit, once at first contact if the
  // left unit's differs: the left unit wins there). The caller applies it and calls setDisplay() with the result.
  void setDisplay(const DisplayState& state, uint32_t nowMs);
  const DisplayState& display() const { return cfg.display; }
  // `parts` (DisplayPart bits) says which parts of `out` are news; the others are this unit's own and stay.
  bool takeDisplay(DisplayState& out, uint8_t* parts = nullptr);

  // Book transfer. The unit that lacks the book asks the other for it, a window of chunks at a time (see FileRequest);
  // the unit that has it answers from the file it has open.
  // The book id the peer reports in its Hello (0 until one arrived or while it has no book open).
  uint32_t peerBook() const { return peerBookId; }
  // Receiver: ask for `count` chunks (1..FILE_WINDOW) starting at `offset`. False when the link is not up.
  bool requestFile(uint32_t offset, uint8_t count);
  // Sender: the newest request from the peer, once.
  bool takeFileRequest(FileRequest& out);
  // Sender: one chunk. False when the radio did not take it (the receiver asks again after its timeout).
  bool sendFileChunk(uint32_t total, uint32_t offset, const uint8_t* data, size_t n);
  // Receiver: the next chunk, oldest first. Chunks beyond FILE_WINDOW waiting are dropped.
  bool takeFileChunk(FileChunk& out);

  // Either unit: the user ended the link here; the peer ends it too. One frame per call (the caller repeats it, as
  // nothing confirms it). takeEndRequest() is true once on the receiving side, and only while the link is up.
  void sendEnd();
  bool takeEndRequest();

  // Either unit: ask the peer to sleep too. takeSleepRequest() is true once on the receiving side.
  void sendSleep();
  bool takeSleepRequest();

 private:
  void handleFrame(const Frame& f, uint32_t nowMs);
  void sendHello(uint32_t nowMs);
  void sendBytes(const uint8_t* data, size_t len);
  void sendPageState(uint32_t nowMs);
  bool canSendState() const;
  void sendMarksChunk(uint32_t nowMs);
  void handleMarks(const Frame& f, uint32_t nowMs);
  void sendKeyFrame(uint32_t nowMs);
  void sendGotoFrame(uint32_t nowMs);
  void sendLayoutFrame(uint32_t nowMs);

  Port& port;
  Parser parser;
  Config cfg;
  Status currentStatus = Status::Idle;
  uint8_t reasons = 0;
  uint32_t framesOk = 0;
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
  bool endRequested = false;

  std::string bookPath;       // the open book
  std::string requestedPath;  // the newest path received
  uint32_t peerBookId = 0;
  bool bookRequested = false;
  uint32_t peerFresh = 0;

  // The text settings being handed over, and the newest ones received.
  uint8_t layoutOut[LAYOUT_BLOB_LEN] = {};
  uint16_t layoutOutCrc = 0;
  bool layoutUnacked = false;
  uint32_t layoutSince = 0;
  uint32_t lastLayoutTx = 0;
  uint8_t layoutIn[LAYOUT_BLOB_LEN] = {};
  bool layoutReady = false;
  bool haveAppliedLayout = false;
  bool peerDisplayKnown = false;
  DisplayState peerDisplay;
  bool displayPending = false;
  DisplayState displayPendingValue;
  uint8_t displayPendingParts = 0;
  uint16_t appliedLayoutCrc = 0;

  // Right side: presses waiting to be confirmed (the first one is in flight).
  KeyAction keyOut[KEY_QUEUE] = {};
  size_t keyOutCount = 0;
  uint16_t keySeq = 0;
  uint32_t lastKeyTx = 0;
  // Left side: presses received and not yet taken, and the seq of the last one (a repeat is only confirmed again).
  KeyAction keyIn[KEY_QUEUE] = {};
  size_t keyInCount = 0;
  uint16_t lastKeySeq = 0;
  bool haveKeySeq = false;

  // Close: sent a few times; received once, and a book offer is held back for a while after it.
  uint32_t closeOutId = 0;
  uint8_t closeOutLeft = 0;
  uint32_t lastCloseTx = 0;
  bool closeReady = false;
  bool offerBlocked = false;
  uint32_t offerBlockedAt = 0;

  // Goto: right side one request in flight; left side the last one received and not yet taken.
  JumpRequest gotoOut;
  bool gotoOutPending = false;
  uint16_t gotoSeq = 0;
  uint32_t lastGotoTx = 0;
  JumpRequest gotoIn;
  bool gotoInReady = false;
  uint16_t lastGotoSeq = 0;
  bool haveGotoSeq = false;

  // Book transfer: the request waiting to be served, and the chunks waiting to be written.
  FileRequest fileReq;
  bool fileReqReady = false;
  FileChunk fileIn[FILE_WINDOW];
  size_t fileInHead = 0;
  size_t fileInCount = 0;

  // Left side: the bookmark file being sent.
  std::string marksOut;
  uint16_t marksOutCrc = 0;
  size_t marksCursor = 0;
  bool marksPending = false;
  uint32_t lastMarksTx = 0;

  // Right side: the file being assembled, the newest complete one, and the crc of the one handed over last.
  std::string marksIn;
  uint16_t marksInCrc = 0;
  uint16_t marksInTotal = 0;
  std::string marksDone;
  bool marksReady = false;
  bool haveApplied = false;
  uint16_t appliedCrc = 0;
};

}  // namespace unitlink
