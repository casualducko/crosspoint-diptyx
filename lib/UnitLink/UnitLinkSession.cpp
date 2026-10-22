#include "UnitLinkSession.h"

#include <algorithm>
#include <cstring>

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
  endRequested = false;
  bookRequested = false;
  marksPending = false;
  marksIn.clear();
  marksReady = false;
  haveApplied = false;
  keyOutCount = 0;
  keyInCount = 0;
  haveKeySeq = false;
  gotoOutPending = false;
  gotoInReady = false;
  haveGotoSeq = false;
  closeOutLeft = 0;
  closeReady = false;
  layoutUnacked = false;
  layoutReady = false;
  haveAppliedLayout = false;
  sendHello(nowMs);
}

void Session::setIdentity(uint32_t bookId, uint32_t layoutHash, uint32_t nowMs) {
  if (currentStatus == Status::Idle || (cfg.bookId == bookId && cfg.layoutHash == layoutHash)) return;
  cfg.bookId = bookId;
  cfg.layoutHash = layoutHash;
  currentStatus = Status::Pairing;
  reasons = 0;
  lastRx = nowMs;
  marksIn.clear();
  haveApplied = false;
  keyOutCount = 0;
  keyInCount = 0;
  haveKeySeq = false;
  gotoOutPending = false;
  gotoInReady = false;
  haveGotoSeq = false;
  peerDisplayKnown = false;
  displayPending = false;
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
  h.display = cfg.display;
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
    if (parser.push(static_cast<uint8_t>(c))) {
      framesOk++;
      handleFrame(parser.frame(), nowMs);
    }
  }

  if (currentStatus == Status::Linked && nowMs - lastRx >= LOST_AFTER_MS) currentStatus = Status::Lost;

  const uint32_t helloEvery = currentStatus == Status::Linked ? HELLO_LINKED_MS : HELLO_PAIRING_MS;
  if (nowMs - lastHelloTx >= helloEvery) {
    sendHello(nowMs);
    // The right unit has another book (or none) open: tell it which one this unit has.
    if (offerBlocked && nowMs - offerBlockedAt >= OFFER_BLOCK_MS) offerBlocked = false;
    const bool mayOffer = !offerBlocked && (cfg.role == Role::Left || peerBookId == 0);
    if (mayOffer && currentStatus == Status::Incompatible && (reasons & INCOMPATIBLE_BOOK) && !bookPath.empty()) {
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encode(Type::Book, reinterpret_cast<const uint8_t*>(bookPath.data()), bookPath.size(), buf));
    }
  }

  if (haveSent && !acked && canSendState() && nowMs - lastStateTx >= RESEND_MS) sendPageState(nowMs);

  // A press that waited out a lost link would turn a page long after it was meant to.
  if (currentStatus != Status::Linked) keyOutCount = 0;
  if (keyOutCount && nowMs - lastKeyTx >= RESEND_MS) sendKeyFrame(nowMs);
  if (closeOutLeft && nowMs - lastCloseTx >= CLOSE_REPEAT_MS) {
    uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
    sendBytes(buf, encodeClose(closeOutId, buf));
    lastCloseTx = nowMs;
    closeOutLeft--;
  }
  if (currentStatus != Status::Linked) gotoOutPending = false;
  if (gotoOutPending && nowMs - lastGotoTx >= RESEND_MS) sendGotoFrame(nowMs);
  if (layoutUnacked) {
    if (nowMs - layoutSince >= LAYOUT_GIVE_UP_MS) {
      layoutUnacked = false;
    } else if (nowMs - lastLayoutTx >= RESEND_MS) {
      sendLayoutFrame(nowMs);
    }
  }

  if (marksPending && currentStatus == Status::Linked) {
    const bool sentAll = marksCursor >= marksOut.size();
    if (nowMs - lastMarksTx >= (sentAll ? MARKS_RESEND_MS : MARKS_CHUNK_MS)) sendMarksChunk(nowMs);
  }
}

void Session::sendLayoutFrame(uint32_t nowMs) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodeLayout(layoutOutCrc, layoutOut, buf));
  lastLayoutTx = nowMs;
}

bool Session::publishLayout(const uint8_t* blob, uint32_t nowMs) {
  if (currentStatus == Status::Idle) return false;
  memcpy(layoutOut, blob, LAYOUT_BLOB_LEN);
  layoutOutCrc = crc16(layoutOut, LAYOUT_BLOB_LEN);
  layoutUnacked = true;
  layoutSince = nowMs;
  haveAppliedLayout = false;  // the peer's settings may come back round to what this unit last adopted
  sendLayoutFrame(nowMs);
  return true;
}

bool Session::takeLayout(uint8_t* out) {
  if (!layoutReady) return false;
  layoutReady = false;
  memcpy(out, layoutIn, LAYOUT_BLOB_LEN);
  return true;
}

void Session::sendKeyFrame(uint32_t nowMs) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodeKey(keySeq, keyOut[0], buf));
  lastKeyTx = nowMs;
}

bool Session::sendKey(KeyAction action, uint32_t nowMs) {
  if (currentStatus != Status::Linked || cfg.role != Role::Right || keyOutCount >= KEY_QUEUE) return false;
  keyOut[keyOutCount++] = action;
  if (keyOutCount == 1) {
    keySeq++;
    sendKeyFrame(nowMs);
  }
  return true;
}

void Session::sendClose(uint32_t bookId, uint32_t nowMs) {
  if (currentStatus == Status::Idle || bookId == 0) return;
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodeClose(bookId, buf));
  closeOutId = bookId;
  closeOutLeft = CLOSE_REPEATS;
  lastCloseTx = nowMs;
}

bool Session::takeClose() {
  const bool ready = closeReady;
  closeReady = false;
  return ready;
}

void Session::sendGotoFrame(uint32_t nowMs) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encodeGoto(gotoSeq, gotoOut, buf));
  lastGotoTx = nowMs;
}

bool Session::sendGoto(const JumpRequest& jump, uint32_t nowMs) {
  if (currentStatus != Status::Linked || cfg.role != Role::Right) return false;
  gotoOut = jump;
  gotoOutPending = true;
  gotoSeq++;
  sendGotoFrame(nowMs);
  return true;
}

bool Session::takeGoto(JumpRequest& jump) {
  if (!gotoInReady) return false;
  gotoInReady = false;
  jump = gotoIn;
  return true;
}

bool Session::takeKey(KeyAction& action) {
  if (!keyInCount) return false;
  action = keyIn[0];
  for (size_t i = 1; i < keyInCount; i++) keyIn[i - 1] = keyIn[i];
  keyInCount--;
  return true;
}

void Session::sendMarksChunk(uint32_t nowMs) {
  if (marksCursor >= marksOut.size()) marksCursor = 0;  // nobody confirmed the whole file: start over
  const size_t n = std::min(MARKS_CHUNK_DATA, marksOut.size() - marksCursor);
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf,
            encodeMarksChunk(marksOutCrc, static_cast<uint16_t>(marksCursor), static_cast<uint16_t>(marksOut.size()),
                             reinterpret_cast<const uint8_t*>(marksOut.data()) + marksCursor, n, buf));
  marksCursor += n;
  lastMarksTx = nowMs;
}

bool Session::publishMarks(const std::string& data, uint32_t nowMs) {
  if (data.empty() || data.size() > MARKS_MAX_TOTAL) return false;
  marksOut = data;
  marksOutCrc = crc16(reinterpret_cast<const uint8_t*>(data.data()), data.size());
  marksCursor = 0;
  marksPending = true;
  lastMarksTx = nowMs - MARKS_CHUNK_MS;  // the first chunk goes out on the next update
  return true;
}

void Session::handleMarks(const Frame& f, uint32_t nowMs) {
  if (cfg.role != Role::Right || f.len <= MARKS_HEADER_LEN) return;
  lastRx = nowMs;
  const uint16_t crc = get16(f.payload);
  const uint16_t offset = get16(f.payload + 2);
  const uint16_t total = get16(f.payload + 4);
  const size_t n = f.len - MARKS_HEADER_LEN;
  if (total == 0 || total > MARKS_MAX_TOTAL || offset + n > total) return;
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  // The same file again: the earlier confirmation may have been lost.
  if (haveApplied && crc == appliedCrc) {
    if (offset + n == total) sendBytes(buf, encodeAck(crc, buf, Type::MarksAck));
    return;
  }
  const char* data = reinterpret_cast<const char*>(f.payload) + MARKS_HEADER_LEN;
  if (offset == 0) {
    marksIn.assign(data, n);
    marksInCrc = crc;
    marksInTotal = total;
  } else if (crc == marksInCrc && total == marksInTotal && offset == marksIn.size()) {
    marksIn.append(data, n);
  } else {
    return;  // a chunk was missed: wait for the file to come round again
  }
  if (marksIn.size() < marksInTotal) return;
  if (crc16(reinterpret_cast<const uint8_t*>(marksIn.data()), marksIn.size()) != marksInCrc) {
    marksIn.clear();
    return;
  }
  marksDone = std::move(marksIn);
  marksIn.clear();
  marksReady = true;
  haveApplied = true;
  appliedCrc = crc;
  sendBytes(buf, encodeAck(crc, buf, Type::MarksAck));
}

bool Session::takeMarks(std::string& out) {
  if (!marksReady) return false;
  marksReady = false;
  out = std::move(marksDone);
  marksDone.clear();
  return true;
}

void Session::handleFrame(const Frame& f, uint32_t nowMs) {
  // Any valid State, Ack or Sleep frame can only come from a peer that already passed the Hello check.
  if (currentStatus == Status::Lost && f.type != Type::Hello) currentStatus = Status::Linked;
  switch (f.type) {
    case Type::Hello: {
      Hello peer;
      if (!decodeHello(f, peer)) return;
      // A peer that is not linked yet has just started: its state counts as a first contact, where the left unit wins.
      if (!peer.linked) peerDisplayKnown = false;
      if (!peerDisplayKnown) {
        peerDisplayKnown = true;
        peerDisplay = peer.display;
        if (cfg.role == Role::Right && peer.display != cfg.display) {
          displayPending = true;
          displayPendingValue = peer.display;
        }
      } else if (peer.display != peerDisplay) {
        peerDisplay = peer.display;
        if (peer.display != cfg.display) {
          displayPending = true;
          displayPendingValue = peer.display;
        }
      }
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
      if (!peer.linked) {
        peerFresh++;
        haveKeySeq = false;  // a restarted right unit counts its presses from the start again
        haveGotoSeq = false;
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
    case Type::End:
      lastRx = nowMs;
      endRequested = true;
      return;
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
    case Type::Marks:
      handleMarks(f, nowMs);
      return;
    case Type::MarksAck: {
      uint16_t crc;
      if (!decodeAck(f, crc, Type::MarksAck)) return;
      lastRx = nowMs;
      if (marksPending && crc == marksOutCrc) marksPending = false;
      return;
    }
    case Type::Key: {
      uint16_t seq;
      KeyAction action;
      if (cfg.role != Role::Left || !decodeKey(f, seq, action)) return;
      lastRx = nowMs;
      if (!haveKeySeq || seq != lastKeySeq) {
        haveKeySeq = true;
        lastKeySeq = seq;
        if (keyInCount < KEY_QUEUE) keyIn[keyInCount++] = action;
      }
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encodeAck(seq, buf, Type::KeyAck));  // repeats too: the earlier confirmation may have been lost
      return;
    }
    case Type::Layout: {
      if (f.len != LAYOUT_LEN) return;
      lastRx = nowMs;
      const uint16_t crc = get16(f.payload);
      if (crc16(f.payload + 2, LAYOUT_BLOB_LEN) != crc) return;
      // Both units changed at once: the left unit's settings win, so it does not take the right unit's.
      if (cfg.role == Role::Left && layoutUnacked) return;
      if (cfg.role == Role::Right) layoutUnacked = false;
      if (!haveAppliedLayout || crc != appliedLayoutCrc) {
        haveAppliedLayout = true;
        appliedLayoutCrc = crc;
        memcpy(layoutIn, f.payload + 2, LAYOUT_BLOB_LEN);
        layoutReady = true;
      }
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encodeAck(crc, buf, Type::LayoutAck));
      return;
    }
    case Type::LayoutAck: {
      uint16_t crc;
      if (!decodeAck(f, crc, Type::LayoutAck)) return;
      lastRx = nowMs;
      if (layoutUnacked && crc == layoutOutCrc) layoutUnacked = false;
      return;
    }
    case Type::Close: {
      uint32_t id;
      if (!decodeClose(f, id)) return;
      lastRx = nowMs;
      if (cfg.bookId != 0 && id == cfg.bookId) {
        closeReady = true;
        offerBlocked = true;  // this unit's own book must not drag the peer straight back in
        offerBlockedAt = nowMs;
      }
      return;
    }
    case Type::Goto: {
      uint16_t seq;
      JumpRequest jump;
      if (cfg.role != Role::Left || !decodeGoto(f, seq, jump)) return;
      lastRx = nowMs;
      if (!haveGotoSeq || seq != lastGotoSeq) {
        haveGotoSeq = true;
        lastGotoSeq = seq;
        gotoIn = jump;
        gotoInReady = true;
      }
      uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
      sendBytes(buf, encodeAck(seq, buf, Type::GotoAck));
      return;
    }
    case Type::GotoAck: {
      uint16_t seq;
      if (!decodeAck(f, seq, Type::GotoAck)) return;
      lastRx = nowMs;
      if (gotoOutPending && seq == gotoSeq) gotoOutPending = false;
      return;
    }
    case Type::KeyAck: {
      uint16_t seq;
      if (!decodeAck(f, seq, Type::KeyAck)) return;
      lastRx = nowMs;
      if (keyOutCount && seq == keySeq) {
        for (size_t i = 1; i < keyOutCount; i++) keyOut[i - 1] = keyOut[i];
        keyOutCount--;
        if (keyOutCount) {
          keySeq++;
          sendKeyFrame(nowMs);
        }
      }
      return;
    }
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

void Session::setDisplay(const DisplayState& state, uint32_t nowMs) {
  if (cfg.display == state) return;
  cfg.display = state;
  if (currentStatus != Status::Idle) sendHello(nowMs);
}

bool Session::takeDisplay(DisplayState& out) {
  if (!displayPending) return false;
  displayPending = false;
  out = displayPendingValue;
  return true;
}

void Session::sendSleep() {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  const size_t n = encode(Type::Sleep, nullptr, 0, buf);
  // No ack for this one: send it a few times so one corrupted frame does not leave the peer awake.
  for (int i = 0; i < 3; i++) sendBytes(buf, n);
}

void Session::sendEnd() {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  sendBytes(buf, encode(Type::End, nullptr, 0, buf));
}

bool Session::takeEndRequest() {
  const bool r = endRequested && (currentStatus == Status::Linked || currentStatus == Status::Lost);
  endRequested = false;
  return r;
}

bool Session::takeSleepRequest() {
  const bool r = sleepRequested;
  sleepRequested = false;
  return r;
}

}  // namespace unitlink
