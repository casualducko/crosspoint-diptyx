#include "BookTransfer.h"

#include <algorithm>

namespace unitlink {

void BookReceiver::begin(Session& session, uint32_t nowMs) {
  current = State::Receiving;
  fileTotal = 0;
  next = 0;
  reqStart = 0;
  reqCount = FILE_WINDOW;
  lastActivity = nowMs;
  lastAsk = nowMs;
  stalls = 0;
  session.requestFile(0, reqCount);
}

BookReceiver::State BookReceiver::update(Session& session, Sink& sink, uint32_t nowMs) {
  if (current != State::Receiving) return current;
  FileChunk chunk;
  bool gap = false;
  while (session.takeFileChunk(chunk)) {
    if (chunk.offset != next) {  // a repeat or a gap: the next request starts from `next`
      if (chunk.offset > next) gap = true;
      continue;
    }
    if (fileTotal == 0) {
      if (chunk.total > MAX_BYTES) return current = State::Failed;
      fileTotal = chunk.total;
    } else if (chunk.total != fileTotal) {
      return current = State::Failed;
    }
    if (!sink.write(chunk.data, chunk.len)) return current = State::Failed;
    next += chunk.len;
    lastActivity = nowMs;
    stalls = 0;
  }
  if (fileTotal != 0 && next >= fileTotal) return current = State::Done;

  const uint32_t windowEnd = reqStart + static_cast<uint32_t>(reqCount) * FILE_CHUNK_DATA;
  const bool windowDone = fileTotal != 0 && next >= windowEnd;
  const bool stalled = nowMs - lastActivity >= STALL_MS;
  const bool reask = gap && nowMs - lastAsk >= GAP_REASK_MS;
  if (!windowDone && !stalled && !reask) return current;
  if (stalled && ++stalls > MAX_STALLS) return current = State::Failed;
  reqStart = next;
  reqCount = FILE_WINDOW;
  lastActivity = nowMs;
  lastAsk = nowMs;
  session.requestFile(reqStart, reqCount);
  return current;
}

bool BookServer::update(Session& session, Source& source, uint32_t bookId) {
  bool worked = false;
  FileRequest req;
  if (session.takeFileRequest(req)) {
    active = false;
    if (bookId != 0 && req.bookId == bookId) {
      total = source.size();
      offset = req.offset;
      left = req.count;
      active = offset < total;
      worked = true;
    }
  }
  if (!active) return worked;
  uint8_t buf[FILE_CHUNK_DATA];
  for (uint8_t i = 0; i < PER_UPDATE && left > 0 && offset < total; i++) {
    const uint32_t n = std::min<uint32_t>(FILE_CHUNK_DATA, total - offset);
    if (!source.read(offset, buf, n)) {
      active = false;
      return worked;
    }
    // A chunk the radio did not take is asked for again by the receiver after its stall timeout.
    if (!session.sendFileChunk(total, offset, buf, n)) return worked;
    offset += n;
    left--;
    worked = true;
  }
  if (left == 0 || offset >= total) active = false;
  return worked;
}

}  // namespace unitlink
