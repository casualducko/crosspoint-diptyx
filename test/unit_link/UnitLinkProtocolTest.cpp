#include <gtest/gtest.h>

#include <vector>

#include "UnitLinkProtocol.h"

using namespace unitlink;

namespace {
int feed(Parser& p, const uint8_t* d, size_t n, std::vector<Frame>& out) {
  int got = 0;
  for (size_t i = 0; i < n; i++) {
    if (p.push(d[i])) {
      out.push_back(p.frame());
      got++;
    }
  }
  return got;
}
}  // namespace

TEST(UnitLinkProtocolTest, MessagesRoundTrip) {
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD];
  Parser p;
  std::vector<Frame> fr;

  Hello h;
  h.role = Role::Right;
  h.bookId = 0xDEADBEEF;
  h.layoutHash = 0x01020304;
  h.linked = true;
  ASSERT_EQ(feed(p, buf, encodeHello(h, buf), fr), 1);
  Hello g;
  ASSERT_TRUE(decodeHello(fr.back(), g));
  EXPECT_EQ(g.role, Role::Right);
  EXPECT_EQ(g.bookId, 0xDEADBEEFu);
  EXPECT_EQ(g.layoutHash, 0x01020304u);
  EXPECT_TRUE(g.linked);

  PageState s;
  s.seq = 65535;
  s.spine = 12;
  s.page = 345;
  ASSERT_EQ(feed(p, buf, encodePageState(s, buf), fr), 1);
  PageState t;
  ASSERT_TRUE(decodePageState(fr.back(), t));
  EXPECT_EQ(t.seq, 65535);
  EXPECT_EQ(t.spine, 12);
  EXPECT_EQ(t.page, 345);

  ASSERT_EQ(feed(p, buf, encodeAck(7, buf), fr), 1);
  uint16_t a = 0;
  ASSERT_TRUE(decodeAck(fr.back(), a));
  EXPECT_EQ(a, 7);

  const size_t n = encode(Type::Sleep, nullptr, 0, buf);
  EXPECT_EQ(n, FRAME_OVERHEAD);
  ASSERT_EQ(feed(p, buf, n, fr), 1);
  EXPECT_EQ(fr.back().type, Type::Sleep);
  EXPECT_EQ(fr.back().len, 0);
}

TEST(UnitLinkProtocolTest, PayloadLimit) {
  uint8_t big[MAX_PAYLOAD + 1] = {};
  uint8_t buf[MAX_PAYLOAD + FRAME_OVERHEAD + 1];
  EXPECT_EQ(encode(Type::State, big, sizeof(big), buf), 0u);
  Parser p;
  std::vector<Frame> fr;
  const size_t n = encode(Type::Book, big, MAX_PAYLOAD, buf);  // Book is the one message with a variable size
  EXPECT_EQ(n, MAX_PAYLOAD + FRAME_OVERHEAD);
  EXPECT_EQ(feed(p, buf, n, fr), 1);
}

TEST(UnitLinkProtocolTest, WrongLengthPayloadIsRejectedByDecoders) {
  Frame f;
  f.type = Type::Hello;
  f.len = 3;
  Hello h;
  EXPECT_FALSE(decodeHello(f, h));
  f.type = Type::State;
  PageState s;
  EXPECT_FALSE(decodePageState(f, s));
  f.type = Type::Ack;
  uint16_t a;
  EXPECT_FALSE(decodeAck(f, a));
}

TEST(UnitLinkProtocolTest, ResyncsAfterGarbageAndCorruptFrame) {
  Parser p;
  std::vector<Frame> fr;
  uint8_t good[MAX_PAYLOAD + FRAME_OVERHEAD], bad[MAX_PAYLOAD + FRAME_OVERHEAD];
  PageState g1;
  g1.seq = 1;
  g1.spine = 2;
  g1.page = 3;
  PageState b1;
  b1.seq = 9;
  b1.spine = 9;
  b1.page = 9;
  const size_t gn = encodePageState(g1, good);
  const size_t bn = encodePageState(b1, bad);
  bad[bn - 1] ^= 0x55;
  std::vector<uint8_t> stream = {0x00, 0xFF, 0x13, SOF, 0x77, 0xFF};
  stream.insert(stream.end(), bad, bad + bn);
  stream.insert(stream.end(), good, good + gn);
  int total = 0;
  for (uint8_t c : stream) {
    if (p.push(c)) {
      total++;
      fr.push_back(p.frame());
    }
  }
  ASSERT_EQ(total, 1);
  PageState s;
  ASSERT_TRUE(decodePageState(fr.back(), s));
  EXPECT_EQ(s.seq, 1);
  EXPECT_EQ(s.page, 3);
  EXPECT_GE(p.crcErrors(), 2u);
}

TEST(UnitLinkProtocolTest, FrameBehindFalseStartIsNotLost) {
  Parser p;
  uint8_t good[MAX_PAYLOAD + FRAME_OVERHEAD];
  const size_t gn = encodeAck(42, good);
  std::vector<uint8_t> stream = {SOF, SOF, 0xF0};  // SOF, then a "type" that is SOF, then an impossible length
  stream.insert(stream.end(), good, good + gn);
  int total = 0;
  uint16_t a = 0;
  for (uint8_t c : stream) {
    if (p.push(c)) {
      total++;
      ASSERT_TRUE(decodeAck(p.frame(), a));
    }
  }
  EXPECT_EQ(total, 1);
  EXPECT_EQ(a, 42);
}

TEST(UnitLinkProtocolTest, IncompatibleReasons) {
  Hello l, r;
  l.role = Role::Left;
  r.role = Role::Right;
  l.bookId = r.bookId = 5;
  l.layoutHash = r.layoutHash = 6;
  EXPECT_EQ(incompatibleReasons(l, r), 0);
  r.layoutHash = 7;
  EXPECT_EQ(incompatibleReasons(l, r), INCOMPATIBLE_LAYOUT);
  r.bookId = 8;
  r.version = PROTOCOL_VERSION + 1;
  EXPECT_EQ(incompatibleReasons(l, r), INCOMPATIBLE_LAYOUT | INCOMPATIBLE_BOOK | INCOMPATIBLE_VERSION);
  r = l;
  EXPECT_EQ(incompatibleReasons(l, r), INCOMPATIBLE_ROLE);
}

TEST(UnitLinkProtocolTest, Fnv1aMatchesKnownValuesAndChains) {
  EXPECT_EQ(fnv1a("", 0), 2166136261u);
  EXPECT_EQ(fnv1a("a", 1), 0xE40C292Cu);
  EXPECT_EQ(fnv1a("foobar", 6), 0xBF9CF968u);
  // Feeding the pieces in turn gives the same result as hashing the whole.
  EXPECT_EQ(fnv1a("bar", 3, fnv1a("foo", 3)), fnv1a("foobar", 6));
}

TEST(UnitLinkProtocolTest, FalseStartWithLongLengthDoesNotSwallowTheNextFrame) {
  Hello h;
  uint8_t good[MAX_PAYLOAD + FRAME_OVERHEAD];
  const size_t n = encodeHello(h, good);
  Parser p;
  // SOF, a type, a length of 150: the parser now waits for 150 payload bytes unless it can replay them.
  const uint8_t junk[] = {SOF, 1, 150};
  for (uint8_t c : junk) EXPECT_FALSE(p.push(c));
  bool got = false;
  for (size_t i = 0; i < n; i++) got = p.push(good[i]) || got;
  // The real frame is shorter than the claimed payload, so only a replaying parser can have found it by the time it
  // is complete plus the missing bytes arrive.
  for (int i = 0; i < 200 && !got; i++) got = p.push(0);
  EXPECT_TRUE(got);
}
