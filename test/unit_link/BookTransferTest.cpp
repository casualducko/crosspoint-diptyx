#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <deque>
#include <vector>

#include "BookTransfer.h"

using namespace unitlink;

namespace {

struct Pipe {
  std::deque<uint8_t> bytes;
  uint32_t lossPercent = 0;
  uint32_t rng = 4242;
  uint32_t next() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 16;
  }
  void put(const uint8_t* d, size_t n) {
    if (next() % 100 < lossPercent) return;
    for (size_t i = 0; i < n; i++) bytes.push_back(d[i]);
  }
};

class End : public Port {
 public:
  End(Pipe& out, Pipe& in) : outPipe(out), inPipe(in) {}
  size_t write(const uint8_t* d, size_t n) override {
    outPipe.put(d, n);
    return n;
  }
  int read() override {
    if (inPipe.bytes.empty()) return -1;
    const int c = inPipe.bytes.front();
    inPipe.bytes.pop_front();
    return c;
  }

 private:
  Pipe& outPipe;
  Pipe& inPipe;
};

struct MemSource : Source {
  std::vector<uint8_t> data;
  uint32_t size() override { return static_cast<uint32_t>(data.size()); }
  bool read(uint32_t offset, uint8_t* out, size_t n) override {
    if (offset + n > data.size()) return false;
    memcpy(out, data.data() + offset, n);
    return true;
  }
};

struct MemSink : Sink {
  std::vector<uint8_t> data;
  size_t capacity = SIZE_MAX;
  bool write(const uint8_t* d, size_t n) override {
    if (data.size() + n > capacity) return false;
    data.insert(data.end(), d, d + n);
    return true;
  }
};

// The sender has book 7 open, the receiver has none: the book ids differ, so the pair is Incompatible, as it is when a
// transfer starts.
struct Rig {
  Pipe l2r, r2l;
  End leftEnd{l2r, r2l};
  End rightEnd{r2l, l2r};
  Session sender{leftEnd};
  Session receiver{rightEnd};
  uint32_t now = 1000;
  MemSource source;
  MemSink sink;
  BookReceiver rx;
  BookServer server;

  explicit Rig(size_t size) {
    uint32_t x = 99;
    for (size_t i = 0; i < size; i++) {
      x = x * 1664525u + 1013904223u;
      source.data.push_back(static_cast<uint8_t>(x >> 24));
    }
    Config l;
    l.role = Role::Left;
    l.bookId = 7;
    l.layoutHash = 9;
    Config r = l;
    r.role = Role::Right;
    r.bookId = 0;
    sender.start(l, now);
    receiver.start(r, now);
    for (int i = 0; i < 20; i++) step(50);
  }

  void step(uint32_t ms) {
    now += ms;
    sender.update(now);
    receiver.update(now);
    server.update(sender, source, 7);
    if (rx.state() == BookReceiver::State::Receiving) rx.update(receiver, sink, now);
  }

  BookReceiver::State run(uint32_t maxMs) {
    rx.begin(receiver, now);
    for (uint32_t t = 0; t < maxMs && rx.state() == BookReceiver::State::Receiving; t += 5) step(5);
    return rx.state();
  }
};

}  // namespace

TEST(BookTransferProtocol, RequestAndChunkRoundTrip) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  Parser parser;
  FileRequest req;
  req.bookId = 0xDEADBEEF;
  req.offset = 123456;
  req.count = 5;
  const size_t n = encodeFileRequest(req, buf);
  bool got = false;
  for (size_t i = 0; i < n; i++) got = parser.push(buf[i]);
  ASSERT_TRUE(got);
  FileRequest back;
  ASSERT_TRUE(decodeFileRequest(parser.frame(), back));
  EXPECT_EQ(back.bookId, req.bookId);
  EXPECT_EQ(back.offset, req.offset);
  EXPECT_EQ(back.count, 5);

  uint8_t data[FILE_CHUNK_DATA];
  for (size_t i = 0; i < sizeof(data); i++) data[i] = static_cast<uint8_t>(i);
  const size_t m = encodeFileChunk(1000000, 192, data, sizeof(data), buf);
  ASSERT_GT(m, 0u);
  got = false;
  for (size_t i = 0; i < m; i++) got = parser.push(buf[i]);
  ASSERT_TRUE(got);
  FileChunk chunk;
  ASSERT_TRUE(decodeFileChunk(parser.frame(), chunk));
  EXPECT_EQ(chunk.total, 1000000u);
  EXPECT_EQ(chunk.offset, 192u);
  EXPECT_EQ(chunk.len, FILE_CHUNK_DATA);
  EXPECT_EQ(memcmp(chunk.data, data, sizeof(data)), 0);
}

TEST(BookTransferProtocol, RejectsBadRequestsAndChunks) {
  Frame f;
  f.type = Type::FileReq;
  f.len = FILE_REQ_LEN;
  FileRequest r;
  EXPECT_FALSE(decodeFileRequest(f, r));  // count 0
  f.payload[8] = FILE_WINDOW + 1;
  EXPECT_FALSE(decodeFileRequest(f, r));
  f.type = Type::FileData;
  f.len = FILE_HEADER_LEN + 4;
  put32(f.payload, 10);     // total
  put32(f.payload + 4, 8);  // offset 8 + 4 bytes runs past the end
  FileChunk c;
  EXPECT_FALSE(decodeFileChunk(f, c));
  put32(f.payload + 4, 10);  // offset at the end
  EXPECT_FALSE(decodeFileChunk(f, c));
  EXPECT_FALSE(lengthAllowed(Type::FileData, FILE_HEADER_LEN));
  EXPECT_FALSE(lengthAllowed(Type::FileReq, FILE_REQ_LEN + 1));
}

TEST(BookTransfer, CleanLink) {
  Rig rig(100000);
  ASSERT_EQ(rig.run(20000), BookReceiver::State::Done);
  EXPECT_EQ(rig.sink.data, rig.source.data);
}

TEST(BookTransfer, SizesAroundAChunk) {
  for (size_t size : {size_t{1}, FILE_CHUNK_DATA - 1, FILE_CHUNK_DATA, FILE_CHUNK_DATA + 1,
                      FILE_CHUNK_DATA * FILE_WINDOW, FILE_CHUNK_DATA * FILE_WINDOW + 1}) {
    Rig rig(size);
    ASSERT_EQ(rig.run(20000), BookReceiver::State::Done) << size;
    EXPECT_EQ(rig.sink.data, rig.source.data) << size;
  }
}

TEST(BookTransfer, LossyLinkStillDeliversTheExactFile) {
  Rig rig(60000);
  rig.l2r.lossPercent = 25;
  rig.r2l.lossPercent = 25;
  ASSERT_EQ(rig.run(120000), BookReceiver::State::Done);
  EXPECT_EQ(rig.sink.data, rig.source.data);
}

TEST(BookTransfer, SilentSenderFails) {
  Rig rig(5000);
  rig.l2r.lossPercent = 100;
  EXPECT_EQ(rig.run(60000), BookReceiver::State::Failed);
}

TEST(BookTransfer, FullCardFails) {
  Rig rig(5000);
  rig.sink.capacity = 1000;
  EXPECT_EQ(rig.run(20000), BookReceiver::State::Failed);
}

TEST(BookTransfer, RequestForAnotherBookIsIgnored) {
  Rig rig(5000);
  rig.server = BookServer();
  // The server serves book 8 only; the receiver asks for the sender's book 7.
  rig.rx.begin(rig.receiver, rig.now);
  for (int i = 0; i < 400; i++) {
    rig.now += 5;
    rig.sender.update(rig.now);
    rig.receiver.update(rig.now);
    rig.server.update(rig.sender, rig.source, 8);
    rig.rx.update(rig.receiver, rig.sink, rig.now);
  }
  EXPECT_TRUE(rig.sink.data.empty());
}

TEST(BookTransfer, ServerWithNoBookServesNothing) {
  Rig rig(5000);
  rig.rx.begin(rig.receiver, rig.now);
  for (int i = 0; i < 200; i++) {
    rig.now += 5;
    rig.sender.update(rig.now);
    rig.receiver.update(rig.now);
    rig.server.update(rig.sender, rig.source, 0);
    rig.rx.update(rig.receiver, rig.sink, rig.now);
  }
  EXPECT_TRUE(rig.sink.data.empty());
}
