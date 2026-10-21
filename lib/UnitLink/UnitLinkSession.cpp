#include "UnitLinkSession.h"

namespace unitlink {

namespace {
// True when a is newer than b in 16-bit serial-number order.
bool seqNewer(uint16_t a, uint16_t b) { return static_cast<int16_t>(static_cast<uint16_t>(a - b)) > 0; }
}  // namespace

void Session::start(const Config& config, uint32_t nowMs) {
  cfg = config;
  parser.reset();
  currentStatus = Status::Pairing;
  reasons = 0;
  lastRx = nowMs;
  haveSent = false;
  acked = false;
  haveRemote = false;
  remoteDelivered = true;
  sleepRequested = false;
  bookRequested = false;
  sendHello(nowMs);
}

void Session::setIdentity(uint32_t bookId, uint32_t layoutHash, uint32_t nowMs) {
  if (currentStatus == Status::Idle || (cfg.bookId == bookId && cfg.layoutHash == layoutHash)) return;
  cfg.bookId = bookId;
  cfg.layoutHash = layoutHash;
  currentStatus = Status::Pairing;
  reasons = 0;
  lastRx = nowMs;
  sendHello(nowMs);
}

bool Session::canSendState() const { return currentStatus == Status::Linked || currentStatus == Status::Lost; }

void Session::stop() { currentStatus = Status::Idle; }

void Session::sendBytes(const uint8_t* data, size_t len) {
  if (len) port.write(data, len);
}

void Session::sendHello(uint32_t nowMs) {
  Hello h;
  h.role = cfg.role;
  h.bookId = cfg.bookId;
  h.layoutHash = cfg.layoutHash;
  h.linked = currentStatus == Status::Linked;
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodeHello(h, buf));
  lastHelloTx = nowMs;
}

void Session::sendPageState(uint32_t nowMs) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodePageState(sent, buf));
  lastStateTx = nowMs;
}

void Session::update(uint32_t nowMs) {
  if (currentStatus == Status::Idle) return;

  int c;
  while ((c = port.read()) >= 0) {
    if (parser.push(static_cast<uint8_t>(c))) handleFrame(parser.frame(), nowMs);
  }

  if (currentStatus == Status::Linked && nowMs - lastRx >= LOST_AFTER_MS) currentStatus = Status::Lost;

  const uint32_t helloEvery = currentStatus == Status::Linked ? HELLO_LINKED_MS : HELLO_PAIRING_MS;
  if (nowMs - lastHelloTx >= helloEvery) {
    sendHello(nowMs);
    // The right unit has another book (or none) open: tell it which one this unit has.
    const bool mayOffer = cfg.role == Role::Left || peerBookId == 0;
    if (mayOffer && currentStatus == Status::Incompatible && (reasons & INCOMPATIBLE_BOOK) && !bookPath.empty()) {
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encode(Type::Book, reinterpret_cast<const uint8_t*>(bookPath.data()), bookPath.size(), buf));
    }
  }

  if (haveSent && !acked && canSendState() && nowMs - lastStateTx >= RESEND_MS) sendPageState(nowMs);
}

void Session::handleFrame(const Frame& f, uint32_t nowMs) {
  // Any valid State, Ack or Sleep frame can only come from a peer that already passed the Hello check.
  if (currentStatus == Status::Lost && f.type != Type::Hello) currentStatus = Status::Linked;
  switch (f.type) {
    case Type::Hello: {
      Hello peer;
      if (!decodeHello(f, peer)) return;
      Hello self;
      self.role = cfg.role;
      self.bookId = cfg.bookId;
      self.layoutHash = cfg.layoutHash;
      reasons = unitlink::incompatibleReasons(self, peer);
      peerBookId = peer.bookId;
      lastRx = nowMs;
      if (reasons) {
        currentStatus = Status::Incompatible;
        return;
      }
      const bool wasLinked = currentStatus == Status::Linked;
      currentStatus = Status::Linked;
      // Answer at once so the peer links without waiting for its next heartbeat.
      if (!wasLinked) sendHello(nowMs);
      // A peer that does not consider itself linked has just started or restarted: send it the position again.
      if (!peer.linked && haveSent) {
        acked = false;
        sendPageState(nowMs);
      }
      return;
    }
    case Type::State: {
      PageState s;
      if (cfg.role != Role::Right || !decodePageState(f, s)) return;
      lastRx = nowMs;
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encodeAck(s.seq, buf));  // ack duplicates too: the earlier ack may have been lost
      if (!haveRemote || seqNewer(s.seq, remote.seq)) {
        remote = s;
        haveRemote = true;
        remoteDelivered = false;
      }
      return;
    }
    case Type::Ack: {
      uint16_t seq;
      if (!decodeAck(f, seq)) return;
      lastRx = nowMs;
      if (haveSent && seq == sent.seq) acked = true;
      return;
    }
    case Type::Sleep:
      lastRx = nowMs;
      sleepRequested = true;
      return;
    case Type::Book:
      if (f.len == 0) return;
      lastRx = nowMs;
      requestedPath.assign(reinterpret_cast<const char*>(f.payload), f.len);
      bookRequested = true;
      return;
    case Type::Key:
      return;
  }
}

void Session::publish(uint16_t spine, uint16_t page, uint32_t nowMs) {
  sent.seq = static_cast<uint16_t>(sent.seq + 1);
  sent.spine = spine;
  sent.page = page;
  haveSent = true;
  acked = false;
  if (canSendState()) sendPageState(nowMs);
}

void Session::setBookPath(const std::string& path) { bookPath = path.size() <= MAX_PAYLOAD ? path : std::string(); }

bool Session::takeBookRequest(std::string& path) {
  if (!bookRequested) return false;
  bookRequested = false;
  path = requestedPath;
  return true;
}

bool Session::takeRemote(PageState& out) {
  if (!haveRemote || remoteDelivered) return false;
  out = remote;
  remoteDelivered = true;
  return true;
}

void Session::sendSleep() {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  const size_t n = encode(Type::Sleep, nullptr, 0, buf);
  // No ack for this one: send it a few times so one corrupted frame does not leave the peer awake.
  for (int i = 0; i < 3; i++) sendBytes(buf, n);
}

bool Session::takeSleepRequest() {
  const bool r = sleepRequested;
  sleepRequested = false;
  return r;
}

}  // namespace unitlink
