#include <gtest/gtest.h>

#include <deque>

#include "UnitLinkSession.h"

using namespace unitlink;

namespace {

// One direction of the cable: whole frames can be lost or have a byte flipped, deterministically.
struct Pipe {
  std::deque<uint8_t> bytes;
  uint32_t lossPercent = 0;
  uint32_t corruptPercent = 0;
  uint32_t rng = 12345;

  uint32_t next() {
    rng = rng * 1664525u + 1013904223u;
    return rng >> 16;
  }
  void put(const uint8_t* d, size_t n) {
    if (next() % 100 < lossPercent) return;
    const bool corrupt = next() % 100 < corruptPercent;
    for (size_t i = 0; i < n; i++) bytes.push_back(corrupt && i == n / 2 ? static_cast<uint8_t>(d[i] ^ 0x5A) : d[i]);
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

struct Rig {
  Pipe l2r, r2l;
  End leftEnd{l2r, r2l};
  End rightEnd{r2l, l2r};
  Session left{leftEnd};
  Session right{rightEnd};
  uint32_t now = 1000;

  Config cfg(Role role, uint32_t book = 7, uint32_t layout = 9) {
    Config c;
    c.role = role;
    c.bookId = book;
    c.layoutHash = layout;
    return c;
  }
  void startBoth() {
    left.start(cfg(Role::Left), now);
    right.start(cfg(Role::Right), now);
  }
  void run(uint32_t ms) {
    for (uint32_t t = 0; t < ms; t += 10) {
      now += 10;
      left.update(now);
      right.update(now);
    }
  }
};

}  // namespace

TEST(UnitLinkSessionTest, PairsWithinAFewHundredMilliseconds) {
  Rig r;
  r.startBoth();
  EXPECT_EQ(r.left.status(), Status::Pairing);
  r.run(200);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, PairsWhenOneUnitStartsLater) {
  Rig r;
  r.left.start(r.cfg(Role::Left), r.now);
  r.run(3000);
  EXPECT_EQ(r.left.status(), Status::Pairing);
  r.right.start(r.cfg(Role::Right), r.now);
  r.run(1000);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, DifferentBookDoesNotPair) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 1), r.now);
  r.right.start(r.cfg(Role::Right, 2), r.now);
  r.run(1000);
  EXPECT_EQ(r.left.status(), Status::Incompatible);
  EXPECT_EQ(r.right.status(), Status::Incompatible);
  EXPECT_EQ(r.left.incompatibleReasons(), INCOMPATIBLE_BOOK);
}

TEST(UnitLinkSessionTest, DifferentLayoutAndSameSideDoNotPair) {
  Rig r;
  r.left.start(r.cfg(Role::Left, 7, 1), r.now);
  r.right.start(r.cfg(Role::Right, 7, 2), r.now);
  r.run(1000);
  EXPECT_EQ(r.right.incompatibleReasons(), INCOMPATIBLE_LAYOUT);

  Rig s;
  s.left.start(s.cfg(Role::Left), s.now);
  s.right.start(s.cfg(Role::Left), s.now);
  s.run(1000);
  EXPECT_EQ(s.left.incompatibleReasons(), INCOMPATIBLE_ROLE);
}

TEST(UnitLinkSessionTest, PositionIsDeliveredAckedAndTakenOnce) {
  Rig r;
  r.startBoth();
  r.run(200);
  r.left.publish(3, 41, r.now);
  r.run(100);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 3);
  EXPECT_EQ(s.page, 41);
  EXPECT_FALSE(r.right.takeRemote(s));
  EXPECT_TRUE(r.left.publishedIsAcked());
}

TEST(UnitLinkSessionTest, NewerPositionReplacesOlder) {
  Rig r;
  r.startBoth();
  r.run(200);
  r.left.publish(1, 10, r.now);
  r.left.publish(1, 12, r.now);
  r.run(100);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.page, 12);
  EXPECT_FALSE(r.right.takeRemote(s));
}

TEST(UnitLinkSessionTest, PositionPublishedBeforePairingArrivesAfterPairing) {
  Rig r;
  r.left.start(r.cfg(Role::Left), r.now);
  r.left.publish(2, 5, r.now);
  r.run(1500);
  r.right.start(r.cfg(Role::Right), r.now);
  r.run(1500);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 2);
  EXPECT_EQ(s.page, 5);
  EXPECT_TRUE(r.left.publishedIsAcked());
}

TEST(UnitLinkSessionTest, SurvivesLossyAndNoisyCable) {
  Rig r;
  r.l2r.lossPercent = r.r2l.lossPercent = 40;
  r.l2r.corruptPercent = r.r2l.corruptPercent = 10;
  r.startBoth();
  r.run(8000);
  ASSERT_TRUE(r.left.isLinked());
  ASSERT_TRUE(r.right.isLinked());
  uint16_t lastSeen = 0;
  for (uint16_t page = 1; page <= 20; page++) {
    r.left.publish(1, page, r.now);
    PageState s;
    // Delivered and acked within ten seconds even though almost half of the frames never arrive intact.
    for (int step = 0; step < 100 && !(lastSeen == page && r.left.publishedIsAcked()); step++) {
      r.run(100);
      while (r.right.takeRemote(s)) lastSeen = s.page;
    }
    EXPECT_EQ(lastSeen, page);
    EXPECT_TRUE(r.left.publishedIsAcked());
  }
}

TEST(UnitLinkSessionTest, SilentPeerMarksLinkLostAndItRecovers) {
  Rig r;
  r.startBoth();
  r.run(500);
  ASSERT_TRUE(r.left.isLinked());
  // The right unit stops answering (no update calls) for longer than the timeout.
  for (uint32_t t = 0; t < 4000; t += 10) {
    r.now += 10;
    r.left.update(r.now);
  }
  EXPECT_EQ(r.left.status(), Status::Lost);
  r.r2l.bytes.clear();
  r.run(3000);
  EXPECT_TRUE(r.left.isLinked());
  EXPECT_TRUE(r.right.isLinked());
}

TEST(UnitLinkSessionTest, RestartedRightUnitGetsThePositionAgain) {
  Rig r;
  r.startBoth();
  r.run(300);
  r.left.publish(4, 77, r.now);
  r.run(300);
  PageState s;
  ASSERT_TRUE(r.right.takeRemote(s));
  r.right.start(r.cfg(Role::Right), r.now);  // the right unit rebooted and forgot everything
  r.run(2000);
  ASSERT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.spine, 4);
  EXPECT_EQ(s.page, 77);
}

TEST(UnitLinkSessionTest, SleepRequestReachesThePeerOnce) {
  Rig r;
  r.startBoth();
  r.run(300);
  r.left.sendSleep();
  r.run(100);
  EXPECT_TRUE(r.right.takeSleepRequest());
  EXPECT_FALSE(r.right.takeSleepRequest());
  EXPECT_FALSE(r.left.takeSleepRequest());
}

TEST(UnitLinkSessionTest, LineGarbageDoesNotBreakTheLink) {
  Rig r;
  r.startBoth();
  r.run(300);
  for (int i = 0; i < 300; i++) r.l2r.bytes.push_back(static_cast<uint8_t>(r.l2r.next()));
  r.left.publish(1, 2, r.now);
  r.run(500);
  PageState s;
  EXPECT_TRUE(r.right.takeRemote(s));
  EXPECT_EQ(s.page, 2);
}

TEST(UnitLinkSessionTest, SequenceNumberWrapsAround) {
  Rig r;
  r.startBoth();
  r.run(300);
  PageState s;
  uint16_t last = 0;
  for (uint32_t i = 1; i <= 70000; i++) {
    r.left.publish(1, static_cast<uint16_t>(i & 0xFFFF), r.now);
    r.run(20);
    while (r.right.takeRemote(s)) last = s.page;
  }
  EXPECT_EQ(last, static_cast<uint16_t>(70000 & 0xFFFF));
}
